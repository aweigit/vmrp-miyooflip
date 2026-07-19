#include "./arm_runtime_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

namespace {

constexpr std::uint64_t unlimited_tick_slice = 1'000'000;
constexpr GuestAddr return_sentinel = 0x1000;
constexpr std::uint32_t return_svc = 0xEF0000FF;

struct CodeHookEntry {
    GuestAddr begin;
    GuestAddr end;
    ArmCodeHook callback;
    void* user_data;
};

class DynarmicCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    using PageTable = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

    explicit DynarmicCallbacks(GuestMemory* memory_) : memory(memory_) {}

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t address) override {
        if (FindHook(address)) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        if (!guest_memory_read(memory, address, &value, sizeof(value))) {
            return std::nullopt;
        }
        /* Code fetches do not use the data page table. Once a page has been
         * translated, route its data writes through callbacks so generated or
         * patched ARM/Thumb code can invalidate only that page's JIT blocks. */
        if (page_table) {
            const std::uint32_t page = address >> Dynarmic::A32::UserConfig::PAGE_BITS;
            executable_pages[page] = true;
            (*page_table)[page] = nullptr;
        }
        return value;
    }

    std::uint8_t MemoryRead8(std::uint32_t address) override { return Read<std::uint8_t>(address); }
    std::uint16_t MemoryRead16(std::uint32_t address) override { return Read<std::uint16_t>(address); }
    std::uint32_t MemoryRead32(std::uint32_t address) override { return Read<std::uint32_t>(address); }
    std::uint64_t MemoryRead64(std::uint32_t address) override { return Read<std::uint64_t>(address); }

    void MemoryWrite8(std::uint32_t address, std::uint8_t value) override { Write(address, value); }
    void MemoryWrite16(std::uint32_t address, std::uint16_t value) override { Write(address, value); }
    void MemoryWrite32(std::uint32_t address, std::uint32_t value) override { Write(address, value); }
    void MemoryWrite64(std::uint32_t address, std::uint64_t value) override { Write(address, value); }

    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return Exclusive(a, v, e); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return Exclusive(a, v, e); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return Exclusive(a, v, e); }
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override { return Exclusive(a, v, e); }

    void InterpreterFallback(std::uint32_t pc, size_t) override {
        pending_address = pc;
        stop_reason = ARM_STOP_INVALID_INSTRUCTION;
        Halt();
    }

    void CallSVC(std::uint32_t swi) override {
        if (diagnostics && (swi != 0xFF || memory_fault)) {
            std::fprintf(stderr, "Dynarmic SVC pc=%08X swi=%08X cpsr=%08X\n",
                         jit ? jit->Regs()[15] : 0, swi, jit ? jit->Cpsr() : 0);
        }
        if (!memory_fault) {
            stop_reason = swi == 0xFF ? ARM_STOP_RETURN : ARM_STOP_INVALID_INSTRUCTION;
        }
        Halt();
    }

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (diagnostics && !FindHook(pc)) {
            std::uint32_t instruction = 0;
            guest_memory_read(memory, pc, &instruction, sizeof(instruction));
            std::fprintf(stderr, "Dynarmic exception pc=%08X type=%u instruction=%08X cpsr=%08X lr=%08X\n",
                         pc, static_cast<unsigned>(exception), instruction,
                         jit ? jit->Cpsr() : 0, jit ? jit->Regs()[14] : 0);
        }
        pending_address = pc;
        if (exception == Dynarmic::A32::Exception::NoExecuteFault) {
            if (pc == stop_address) {
                stop_reason = ARM_STOP_RETURN;
            } else if (FindHook(pc)) {
                pending_bridge = true;
                stop_reason = ARM_STOP_RETURN;
            } else {
                fault_address = pc;
                stop_reason = ARM_STOP_INVALID_MEMORY;
            }
        } else {
            stop_reason = ARM_STOP_INVALID_INSTRUCTION;
        }
        Halt();
    }

    void AddTicks(std::uint64_t ticks) override {
        ticks_remaining = ticks >= ticks_remaining ? 0 : ticks_remaining - ticks;
    }
    std::uint64_t GetTicksRemaining() override { return ticks_remaining; }

    CodeHookEntry* FindHook(GuestAddr address) {
        auto it = std::find_if(hooks.begin(), hooks.end(), [address](const auto& hook) {
            return address >= hook.begin && address <= hook.end;
        });
        return it == hooks.end() ? nullptr : &*it;
    }

    void Halt() {
        if (jit) jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }

    GuestMemory* memory;
    ArmRuntime* runtime{};
    Dynarmic::A32::Jit* jit{};
    std::vector<CodeHookEntry> hooks;
    GuestAddr stop_address{};
    GuestAddr pending_address{};
    GuestAddr fault_address{};
    ArmStopReason stop_reason{ARM_STOP_ERROR};
    bool pending_bridge{};
    bool memory_fault{};
    std::uint64_t ticks_remaining{unlimited_tick_slice};
    bool diagnostics{std::getenv("VMRP_DYNARMIC_DIAGNOSTICS") != nullptr};
    PageTable* page_table{};
    std::array<bool, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> executable_pages{};

    bool IsExecutable(std::uint32_t address) const {
        return executable_pages[address >> Dynarmic::A32::UserConfig::PAGE_BITS];
    }

private:
    template<typename T>
    T Read(std::uint32_t address) {
        T value{};
        if (!guest_memory_read(memory, address, &value, sizeof(value))) {
            fault_address = address;
            memory_fault = true;
            stop_reason = ARM_STOP_INVALID_MEMORY;
            if (diagnostics) {
                std::fprintf(stderr, "Dynarmic data read fault address=%08X pc=%08X cpsr=%08X\n",
                             address, jit ? jit->Regs()[15] : 0, jit ? jit->Cpsr() : 0);
            }
            Halt();
        }
        return value;
    }

    template<typename T>
    void Write(std::uint32_t address, T value) {
        if (!guest_memory_write(memory, address, &value, sizeof(value))) {
            fault_address = address;
            memory_fault = true;
            stop_reason = ARM_STOP_INVALID_MEMORY;
            if (diagnostics) {
                std::fprintf(stderr, "Dynarmic data write fault address=%08X pc=%08X cpsr=%08X\n",
                             address, jit ? jit->Regs()[15] : 0, jit ? jit->Cpsr() : 0);
            }
            Halt();
        } else if (jit && IsExecutable(address)) {
            /* MRP modules generate and patch executable code in their heap.
             * Keep translated blocks coherent with guest data writes. */
            jit->InvalidateCacheRange(address, sizeof(value));
        }
    }

    template<typename T>
    bool Exclusive(std::uint32_t address, T value, T expected) {
        T current{};
        if (!guest_memory_read(memory, address, &current, sizeof(current)) || current != expected) return false;
        return guest_memory_write(memory, address, &value, sizeof(value));
    }
};

struct DynarmicBackend {
    explicit DynarmicBackend(GuestMemory* memory)
            : callbacks(memory), jit(MakeConfig(callbacks, page_table, memory)) {
        callbacks.jit = &jit;
        callbacks.page_table = &page_table;
        /* Match Unicorn's default ARM state (SVC mode, IRQ/FIQ masked, Z set). */
        jit.SetCpsr(0x400001D3);
        const char* trace_path = std::getenv("VMRP_LOCKSTEP_LOG");
        if (trace_path) trace_file = std::fopen(trace_path, "w");
        const char* trace_limit_text = std::getenv("VMRP_LOCKSTEP_LIMIT");
        if (trace_limit_text) trace_limit = std::strtoull(trace_limit_text, nullptr, 10);
    }

    ~DynarmicBackend() {
        if (trace_file) std::fclose(trace_file);
    }

    using PageTable = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

    static void MapRegion(PageTable& page_table, const GuestMemoryRegion& region) {
        constexpr std::uint32_t page_size = 1u << Dynarmic::A32::UserConfig::PAGE_BITS;
        const std::uint32_t first = region.base / page_size;
        const std::uint32_t pages = region.size / page_size;
        for (std::uint32_t page = 0; page < pages; ++page) {
            page_table[first + page] = region.host + static_cast<size_t>(page) * page_size;
        }
    }

    static Dynarmic::A32::UserConfig MakeConfig(DynarmicCallbacks& callbacks,
                                                 PageTable& page_table,
                                                 GuestMemory* memory) {
        page_table.fill(nullptr);
        MapRegion(page_table, memory->low);
        MapRegion(page_table, memory->main);
        Dynarmic::A32::UserConfig config;
        config.callbacks = &callbacks;
        config.arch_version = Dynarmic::A32::ArchVersion::v7;
        config.always_little_endian = true;
        config.enable_cycle_counting = false;
        config.code_cache_size = 64 * 1024 * 1024;
        config.page_table = std::getenv("VMRP_DYNARMIC_SAFE_MEMORY") ? nullptr : &page_table;
        config.absolute_offset_page_table = false;
        config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64;
        return config;
    }

    PageTable page_table{};
    DynarmicCallbacks callbacks;
    Dynarmic::A32::Jit jit;
    std::FILE* trace_file{};
    std::uint64_t trace_count{};
    std::uint64_t trace_limit{};
    std::uint64_t run_count{};
};

DynarmicBackend* Backend(ArmRuntime* runtime) {
    return static_cast<DynarmicBackend*>(arm_runtime_backend_data(runtime));
}

void Destroy(ArmRuntime* runtime) { delete Backend(runtime); }

bool RegRead(ArmRuntime* runtime, ArmRegister reg, std::uint32_t* value) {
    if (!value) return false;
    auto* backend = Backend(runtime);
    if (reg <= ARM_RUNTIME_PC) {
        *value = backend->jit.Regs()[static_cast<size_t>(reg)];
        return true;
    }
    if (reg == ARM_RUNTIME_CPSR) *value = backend->jit.Cpsr();
    else if (reg == ARM_RUNTIME_FPSCR) *value = backend->jit.Fpscr();
    else return false;
    return true;
}

bool RegWrite(ArmRuntime* runtime, ArmRegister reg, std::uint32_t value) {
    auto* backend = Backend(runtime);
    if (reg <= ARM_RUNTIME_PC) backend->jit.Regs()[static_cast<size_t>(reg)] = value;
    else if (reg == ARM_RUNTIME_CPSR) backend->jit.SetCpsr(value);
    else if (reg == ARM_RUNTIME_FPSCR) backend->jit.SetFpscr(value);
    else return false;
    return true;
}

bool MemRead(ArmRuntime* runtime, GuestAddr address, void* data, size_t size) {
    return guest_memory_read(arm_runtime_memory(runtime), address, data, size);
}

bool MemWrite(ArmRuntime* runtime, GuestAddr address, const void* data, size_t size) {
    auto* backend = Backend(runtime);
    if (size == 0) return true;
    if (!guest_memory_write(arm_runtime_memory(runtime), address, data, size)) return false;
    constexpr size_t page_bits = Dynarmic::A32::UserConfig::PAGE_BITS;
    const std::uint32_t first = address >> page_bits;
    const std::uint32_t last = static_cast<std::uint32_t>(address + size - 1) >> page_bits;
    for (std::uint32_t page = first; page <= last; ++page) {
        if (backend->callbacks.executable_pages[page]) {
            backend->jit.InvalidateCacheRange(address, size);
            break;
        }
    }
    return true;
}

ArmRunResult Run(ArmRuntime* runtime, const ArmRunOptions* options) {
    auto* backend = Backend(runtime);
    auto& cb = backend->callbacks;
    if (backend->trace_file) {
        std::fprintf(backend->trace_file, "RUN %llu %08X %08X %u\n",
                     static_cast<unsigned long long>(++backend->run_count),
                     options->start, options->stop, options->thumb ? 1u : 0u);
    }
    std::array<std::uint32_t, 9> callee_saved{};
    for (size_t i = 0; i < 8; ++i) callee_saved[i] = backend->jit.Regs()[4 + i];
    callee_saved[8] = backend->jit.Regs()[13];
    cb.stop_address = return_sentinel;
    cb.pending_bridge = false;
    cb.memory_fault = false;
    cb.fault_address = 0;
    cb.stop_reason = ARM_STOP_ERROR;
    cb.ticks_remaining = options->instruction_limit ? options->instruction_limit
                                                   : unlimited_tick_slice;
    backend->jit.Regs()[15] = options->start & ~1u;
    backend->jit.Regs()[14] = return_sentinel;
    std::uint32_t cpsr = backend->jit.Cpsr();
    backend->jit.SetCpsr(options->thumb ? cpsr | (1u << 5) : cpsr & ~(1u << 5));

    Dynarmic::HaltReason last_halt_reason{};
    const bool trace_slices = std::getenv("VMRP_DYNARMIC_TRACE_SLICES") != nullptr;
    std::uint64_t slice_count = 0;
    for (;;) {
        backend->jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
        backend->jit.ClearHalt(Dynarmic::HaltReason::CacheInvalidation);
        if (backend->trace_file) {
            const auto& regs = backend->jit.Regs();
            std::fprintf(backend->trace_file,
                         "%08X %08X %08X %08X %08X %08X %08X %08X %08X "
                         "%08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
                         regs[15], backend->jit.Cpsr(), regs[0], regs[1], regs[2], regs[3],
                         regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10],
                         regs[11], regs[12], regs[13], regs[14], regs[15]);
            if (++backend->trace_count == backend->trace_limit && backend->trace_limit != 0) {
                std::fflush(backend->trace_file);
                std::exit(0);
            }
        }
        Dynarmic::HaltReason halt_reason = backend->trace_file ? backend->jit.Step()
                                                               : backend->jit.Run();
        last_halt_reason = halt_reason;
        if (cb.pending_bridge) {
            cb.pending_bridge = false;
            CodeHookEntry* hook = cb.FindHook(cb.pending_address);
            if (!hook || !hook->callback(runtime, cb.pending_address, 0, hook->user_data)) {
                cb.stop_reason = ARM_STOP_ERROR;
                break;
            }
            /* NoExecuteFault is only used to transfer control to a host bridge.
             * The guest call has not returned from Run(), so discard the
             * temporary halt status before resuming at the bridge return PC. */
            if (cb.memory_fault) break;
            cb.stop_reason = ARM_STOP_ERROR;
            continue;
        }
        if (cb.stop_reason != ARM_STOP_ERROR) {
            break;
        }
        if (Dynarmic::Has(halt_reason, Dynarmic::HaltReason::CacheInvalidation)) {
            backend->jit.ClearHalt(Dynarmic::HaltReason::CacheInvalidation);
            continue;
        }
        if (Dynarmic::Has(halt_reason, Dynarmic::HaltReason::Step)) {
            backend->jit.ClearHalt(Dynarmic::HaltReason::Step);
            continue;
        }
        if (!halt_reason && cb.ticks_remaining == 0 && options->instruction_limit == 0) {
            if (trace_slices) {
                std::fprintf(stderr, "Dynarmic slice=%llu PC=%08X LR=%08X SP=%08X\n",
                             static_cast<unsigned long long>(++slice_count),
                             backend->jit.Regs()[15], backend->jit.Regs()[14], backend->jit.Regs()[13]);
            }
            cb.ticks_remaining = unlimited_tick_slice;
            continue;
        }
        if (cb.ticks_remaining == 0 && cb.stop_reason == ARM_STOP_ERROR) cb.stop_reason = ARM_STOP_TIMEOUT;
        break;
    }

    if (cb.stop_reason == ARM_STOP_RETURN) {
        for (size_t i = 0; i < 8; ++i) backend->jit.Regs()[4 + i] = callee_saved[i];
        backend->jit.Regs()[13] = callee_saved[8];
    }

    return ArmRunResult{
        cb.stop_reason,
        backend->jit.Regs()[15],
        cb.fault_address,
        static_cast<int>(last_halt_reason),
        cb.stop_reason == ARM_STOP_ERROR ? "Dynarmic execution halted unexpectedly" : nullptr,
    };
}

bool AddCodeHook(ArmRuntime* runtime, GuestAddr begin, GuestAddr end,
                 ArmCodeHook callback, void* user_data) {
    Backend(runtime)->callbacks.hooks.push_back({begin, end, callback, user_data});
    Backend(runtime)->jit.InvalidateCacheRange(begin, static_cast<size_t>(end - begin) + 1);
    return true;
}

const ArmRuntimeOps ops{
    Destroy, RegRead, RegWrite, MemRead, MemWrite, Run, AddCodeHook,
};

}  // namespace

extern "C" ArmRuntime* arm_runtime_create_dynarmic(GuestMemory* memory) {
    if (!memory) return nullptr;
    auto backend = std::make_unique<DynarmicBackend>(memory);
    if (!guest_memory_write(memory, return_sentinel, &return_svc, sizeof(return_svc))) {
        return nullptr;
    }
    ArmRuntime* runtime = arm_runtime_alloc(&ops, ARM_BACKEND_DYNARMIC, memory, backend.get());
    if (!runtime) return nullptr;
    backend->callbacks.runtime = runtime;
    backend.release();
    return runtime;
}
