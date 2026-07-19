#include "./arm_runtime_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unicorn/unicorn.h>

#include "./header/debug.h"
#include "./header/utils.h"
#include "./header/vmrp.h"

typedef struct UnicornBackend {
    uc_engine *uc;
    struct UnicornCodeHook *code_hooks;
    FILE *trace_file;
    uint64_t trace_count;
    uint64_t trace_limit;
    uint64_t run_count;
} UnicornBackend;

typedef struct UnicornCodeHook {
    ArmRuntime *runtime;
    ArmCodeHook callback;
    void *user_data;
    uc_hook handle;
    struct UnicornCodeHook *next;
} UnicornCodeHook;

static int unicorn_register(ArmRegister reg) {
    static const int registers[] = {
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
        UC_ARM_REG_CPSR, UC_ARM_REG_FPSCR,
    };
    return (unsigned)reg < sizeof(registers) / sizeof(registers[0]) ? registers[reg] : UC_ARM_REG_INVALID;
}

static UnicornBackend *backend(ArmRuntime *runtime) { return arm_runtime_backend_data(runtime); }

void *arm_runtime_unicorn_native_handle(ArmRuntime *runtime) {
    UnicornBackend *state = backend(runtime);
    return state ? state->uc : NULL;
}

#ifdef DEBUG
static void unicorn_trace_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    (void)uc;
    (void)user_data;
    printf(">>> Tracing basic block at 0x%" PRIx64 ", block size = 0x%x\n", address, size);
}

static void unicorn_trace_memory(uc_engine *uc, uc_mem_type type, uint64_t address,
                                 int size, int64_t value, void *user_data) {
    (void)user_data;
    printf(">>> Tracing mem_valid mem_type:%s at 0x%" PRIx64
           ", size:0x%x, value:0x%" PRIx64 "\n",
           memTypeStr(type), address, size, (uint64_t)value);
    if (type == UC_MEM_READ && size <= 4) {
        uint32_t data = 0;
        uint32_t pc = 0;
        uc_mem_read(uc, address, &data, size);
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        printf("PC:0x%X,read:0x%X\n", pc, data);
    }
}
#endif

static bool unicorn_invalid_memory(uc_engine *uc, uc_mem_type type, uint64_t address,
                                   int size, int64_t value, void *user_data) {
    (void)user_data;
    printf(">>> Tracing mem_invalid mem_type:%s at 0x%" PRIx64
           ", size:0x%x, value:0x%" PRIx64 "\n",
           memTypeStr(type), address, size, (uint64_t)value);
    dumpREG(uc);
    return false;
}

static bool unicorn_invalid_instruction(uc_engine *uc, void *user_data) {
    (void)user_data;
    uint32_t pc = 0, lr = 0, sp = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    printf(">>> Tracing insn_invalid at PC=0x%X, LR=0x%X, SP=0x%X\n", pc, lr, sp);
    dumpREG(uc);
    if (pc >= START_ADDRESS && pc < END_ADDRESS) {
        uint32_t data[4] = {0};
        uc_mem_read(uc, pc, data, sizeof(data));
        printf("    mem@PC:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
    }
    if (lr >= START_ADDRESS && lr < END_ADDRESS) {
        uint32_t data[8] = {0};
        uc_mem_read(uc, lr & ~3u, data, sizeof(data));
        printf("    mem@LR-:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
        printf("    mem@LR+:  %08X %08X %08X %08X\n", data[4], data[5], data[6], data[7]);
    }
    if (sp >= START_ADDRESS && sp < END_ADDRESS) {
        uint32_t data[16] = {0};
        uc_mem_read(uc, sp, data, sizeof(data));
        printf("    mem@SP:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
        printf("            %08X %08X %08X %08X\n", data[4], data[5], data[6], data[7]);
        printf("            %08X %08X %08X %08X\n", data[8], data[9], data[10], data[11]);
        printf("            %08X %08X %08X %08X\n", data[12], data[13], data[14], data[15]);
    }
    return false;
}

static void unicorn_lockstep_trace(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    (void)address;
    (void)size;
    UnicornBackend *state = user_data;
    uint32_t regs[16] = {0};
    uint32_t cpsr = 0;
    static const int native_regs[16] = {
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
    };
    for (size_t i = 0; i < 16; ++i) uc_reg_read(uc, native_regs[i], &regs[i]);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    fprintf(state->trace_file,
            "%08X %08X %08X %08X %08X %08X %08X %08X %08X "
            "%08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
            regs[15], cpsr, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
            regs[6], regs[7], regs[8], regs[9], regs[10], regs[11], regs[12],
            regs[13], regs[14], regs[15]);
    if (++state->trace_count == state->trace_limit && state->trace_limit != 0) {
        fflush(state->trace_file);
        exit(0);
    }
}

static bool unicorn_install_diagnostics(UnicornBackend *state) {
    uc_hook hook;
    const char *trace_path = getenv("VMRP_LOCKSTEP_LOG");
    if (trace_path) {
        state->trace_file = fopen(trace_path, "w");
        const char *limit = getenv("VMRP_LOCKSTEP_LIMIT");
        if (limit) state->trace_limit = strtoull(limit, NULL, 10);
        if (!state->trace_file ||
            uc_hook_add(state->uc, &hook, UC_HOOK_CODE, unicorn_lockstep_trace,
                        state, 1, 0) != UC_ERR_OK) {
            return false;
        }
    }
#ifdef DEBUG
    if (uc_hook_add(state->uc, &hook, UC_HOOK_BLOCK, unicorn_trace_block, NULL, 1, 0) != UC_ERR_OK ||
        uc_hook_add(state->uc, &hook, UC_HOOK_MEM_VALID, unicorn_trace_memory, NULL, 1, 0) != UC_ERR_OK ||
        uc_hook_add(state->uc, &hook, UC_HOOK_CODE, hook_code_trace, NULL, 1, 0) != UC_ERR_OK) {
        return false;
    }
#endif
    return uc_hook_add(state->uc, &hook, UC_HOOK_MEM_INVALID, unicorn_invalid_memory, NULL, 1, 0) == UC_ERR_OK &&
           uc_hook_add(state->uc, &hook, UC_HOOK_INSN_INVALID, unicorn_invalid_instruction, NULL, 1, 0) == UC_ERR_OK;
}

static void unicorn_destroy(ArmRuntime *runtime) {
    UnicornBackend *state = backend(runtime);
    UnicornCodeHook *hook = state->code_hooks;
    while (hook) {
        UnicornCodeHook *next = hook->next;
        free(hook);
        hook = next;
    }
    if (state->trace_file) fclose(state->trace_file);
    if (state->uc) uc_close(state->uc);
    free(state);
}

static void unicorn_code_hook(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    (void)uc;
    UnicornCodeHook *hook = user_data;
    hook->callback(hook->runtime, (GuestAddr)address, size, hook->user_data);
}

static bool unicorn_add_code_hook(ArmRuntime *runtime, GuestAddr begin, GuestAddr end,
                                  ArmCodeHook callback, void *user_data) {
    UnicornBackend *state = backend(runtime);
    UnicornCodeHook *hook = calloc(1, sizeof(*hook));
    if (!hook) return false;
    hook->runtime = runtime;
    hook->callback = callback;
    hook->user_data = user_data;
    uc_err error = uc_hook_add(state->uc, &hook->handle, UC_HOOK_CODE, unicorn_code_hook,
                               hook, begin, end);
    if (error != UC_ERR_OK) {
        free(hook);
        return false;
    }
    hook->next = state->code_hooks;
    state->code_hooks = hook;
    return true;
}

static bool unicorn_reg_read(ArmRuntime *runtime, ArmRegister reg, uint32_t *value) {
    int native_reg = unicorn_register(reg);
    return native_reg != UC_ARM_REG_INVALID && uc_reg_read(backend(runtime)->uc, native_reg, value) == UC_ERR_OK;
}

static bool unicorn_reg_write(ArmRuntime *runtime, ArmRegister reg, uint32_t value) {
    int native_reg = unicorn_register(reg);
    return native_reg != UC_ARM_REG_INVALID && uc_reg_write(backend(runtime)->uc, native_reg, &value) == UC_ERR_OK;
}

static bool unicorn_mem_read(ArmRuntime *runtime, GuestAddr address, void *data, size_t size) {
    return uc_mem_read(backend(runtime)->uc, address, data, size) == UC_ERR_OK;
}

static bool unicorn_mem_write(ArmRuntime *runtime, GuestAddr address, const void *data, size_t size) {
    return uc_mem_write(backend(runtime)->uc, address, data, size) == UC_ERR_OK;
}

static ArmRunResult unicorn_run(ArmRuntime *runtime, const ArmRunOptions *options) {
    UnicornBackend *state = backend(runtime);
    if (state->trace_file) {
        fprintf(state->trace_file, "RUN %" PRIu64 " %08X %08X %u\n",
                ++state->run_count, options->start, options->stop, options->thumb ? 1u : 0u);
    }
    uint32_t lr = options->stop;
    uint64_t start = options->thumb ? options->start | 1u : options->start;
    uc_err error = uc_reg_write(state->uc, UC_ARM_REG_LR, &lr);
    if (error == UC_ERR_OK) {
        error = uc_emu_start(state->uc, start, options->stop, options->timeout_us, options->instruction_limit);
    }
    uint32_t pc = 0;
    uc_reg_read(state->uc, UC_ARM_REG_PC, &pc);
    return (ArmRunResult){
        .reason = error == UC_ERR_OK ? ARM_STOP_RETURN : ARM_STOP_ERROR,
        .pc = pc,
        .backend_error = error,
        .message = error == UC_ERR_OK ? NULL : uc_strerror(error),
    };
}

static const ArmRuntimeOps unicorn_ops = {
    .destroy = unicorn_destroy,
    .reg_read = unicorn_reg_read,
    .reg_write = unicorn_reg_write,
    .mem_read = unicorn_mem_read,
    .mem_write = unicorn_mem_write,
    .run = unicorn_run,
    .add_code_hook = unicorn_add_code_hook,
};

ArmRuntime *arm_runtime_create_unicorn(GuestMemory *memory) {
    UnicornBackend *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &state->uc) != UC_ERR_OK) {
        free(state);
        return NULL;
    }
    if (uc_mem_map_ptr(state->uc, memory->low.base, memory->low.size, UC_PROT_ALL, memory->low.host) != UC_ERR_OK ||
        uc_mem_map_ptr(state->uc, memory->main.base, memory->main.size, UC_PROT_ALL, memory->main.host) != UC_ERR_OK) {
        uc_close(state->uc);
        free(state);
        return NULL;
    }
    if (!unicorn_install_diagnostics(state)) {
        uc_close(state->uc);
        free(state);
        return NULL;
    }
    return arm_runtime_alloc(&unicorn_ops, ARM_BACKEND_UNICORN, memory, state);
}
