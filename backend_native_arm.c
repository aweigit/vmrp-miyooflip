#include "./arm_runtime_internal.h"

#if !defined(__arm__)
#error "backend_native_arm.c requires an ARM32 compiler"
#endif

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <sys/mman.h>

typedef struct NativeHook {
    GuestAddr begin, end;
    ArmCodeHook callback;
    void *user_data;
    GuestAddr dispatch_address;
    struct NativeHook *next;
} NativeHook;

typedef struct NativeContext {
    uint32_t regs[16];
    uint32_t cpsr;
    uint32_t host_sp;
    uint32_t start;
} NativeContext;

typedef struct NativeBackend {
    NativeContext context;
    NativeHook *hooks;
    sigjmp_buf escape;
    ArmRuntime *runtime;
    volatile sig_atomic_t running;
    ArmRunResult result;
    struct sigaction old_ill, old_segv, old_bus;
    uint32_t *stub_area;
    size_t stub_count;
    size_t stub_capacity;
} NativeBackend;

NativeBackend *active_backend;
extern void vmrp_native_arm_call(NativeContext *context, uint32_t start);

static NativeHook *find_hook(NativeBackend *backend, uint32_t pc) {
    for (NativeHook *hook = backend->hooks; hook; hook = hook->next)
        if (pc >= hook->begin && pc <= hook->end) return hook;
    return NULL;
}

static void context_from_signal(NativeContext *ctx, const ucontext_t *uc) {
    ctx->regs[0] = uc->uc_mcontext.arm_r0; ctx->regs[1] = uc->uc_mcontext.arm_r1;
    ctx->regs[2] = uc->uc_mcontext.arm_r2; ctx->regs[3] = uc->uc_mcontext.arm_r3;
    ctx->regs[4] = uc->uc_mcontext.arm_r4; ctx->regs[5] = uc->uc_mcontext.arm_r5;
    ctx->regs[6] = uc->uc_mcontext.arm_r6; ctx->regs[7] = uc->uc_mcontext.arm_r7;
    ctx->regs[8] = uc->uc_mcontext.arm_r8; ctx->regs[9] = uc->uc_mcontext.arm_r9;
    ctx->regs[10] = uc->uc_mcontext.arm_r10; ctx->regs[11] = uc->uc_mcontext.arm_fp;
    ctx->regs[12] = uc->uc_mcontext.arm_ip; ctx->regs[13] = uc->uc_mcontext.arm_sp;
    ctx->regs[14] = uc->uc_mcontext.arm_lr; ctx->regs[15] = uc->uc_mcontext.arm_pc;
    ctx->cpsr = uc->uc_mcontext.arm_cpsr;
}

static void context_to_signal(const NativeContext *ctx, ucontext_t *uc) {
    uc->uc_mcontext.arm_r0 = ctx->regs[0]; uc->uc_mcontext.arm_r1 = ctx->regs[1];
    uc->uc_mcontext.arm_r2 = ctx->regs[2]; uc->uc_mcontext.arm_r3 = ctx->regs[3];
    uc->uc_mcontext.arm_r4 = ctx->regs[4]; uc->uc_mcontext.arm_r5 = ctx->regs[5];
    uc->uc_mcontext.arm_r6 = ctx->regs[6]; uc->uc_mcontext.arm_r7 = ctx->regs[7];
    uc->uc_mcontext.arm_r8 = ctx->regs[8]; uc->uc_mcontext.arm_r9 = ctx->regs[9];
    uc->uc_mcontext.arm_r10 = ctx->regs[10]; uc->uc_mcontext.arm_fp = ctx->regs[11];
    uc->uc_mcontext.arm_ip = ctx->regs[12]; uc->uc_mcontext.arm_sp = ctx->regs[13];
    uc->uc_mcontext.arm_lr = ctx->regs[14]; uc->uc_mcontext.arm_pc = ctx->regs[15];
    uc->uc_mcontext.arm_cpsr = ctx->cpsr;
}

/* Some ARM kernels keep page zero inaccessible even when a fixed low mapping
 * was requested. Unicorn/Dynarmic expose this area as zero-filled guest RAM.
 * Emulate the common Thumb immediate loads used by old MRP code so their
 * observable behavior remains the same without weakening kernel protection. */
static bool emulate_thumb_low_read(NativeBackend *backend, ucontext_t *uc,
                                   uint32_t pc, uintptr_t fault_address) {
    if (!(uc->uc_mcontext.arm_cpsr & (1u << 5)) || fault_address >= 0x80000u) return false;
    uint16_t insn = *(const volatile uint16_t *)(uintptr_t)pc;
    unsigned long *regs[] = {
        &uc->uc_mcontext.arm_r0, &uc->uc_mcontext.arm_r1,
        &uc->uc_mcontext.arm_r2, &uc->uc_mcontext.arm_r3,
        &uc->uc_mcontext.arm_r4, &uc->uc_mcontext.arm_r5,
        &uc->uc_mcontext.arm_r6, &uc->uc_mcontext.arm_r7,
    };
    const unsigned op = insn & 0xF800u;
    const unsigned rn = (insn >> 3) & 7u;
    const unsigned rt = insn & 7u;
    uint32_t address;
    size_t size;
    if (op == 0x6800u) {          /* LDR Rt, [Rn, #imm5*4] */
        address = *regs[rn] + ((insn >> 4) & 0x7Cu); size = 4;
    } else if (op == 0x7800u) {   /* LDRB Rt, [Rn, #imm5] */
        address = *regs[rn] + ((insn >> 6) & 0x1Fu); size = 1;
    } else if (op == 0x8800u) {   /* LDRH Rt, [Rn, #imm5*2] */
        address = *regs[rn] + ((insn >> 5) & 0x3Eu); size = 2;
    } else {
        return false;
    }
    if (address != fault_address || address + size > 0x80000u) return false;
    /* Low guest RAM is zero-filled. Address zero cannot be dereferenced by
     * this kernel, but reads from it have the same zero result. */
    *regs[rt] = 0;
    uc->uc_mcontext.arm_pc = pc + 2;
    context_from_signal(&backend->context, uc);
    return true;
}

static bool emulate_arm_low_read(NativeBackend *backend, ucontext_t *uc,
                                 uint32_t pc, uintptr_t fault_address) {
    if ((uc->uc_mcontext.arm_cpsr & (1u << 5)) || fault_address >= 0x80000u) return false;
    const uint32_t insn = *(const volatile uint32_t *)(uintptr_t)pc;
    /* ARM A1 immediate single-data-transfer: LDR/LDRB, pre-indexed without
     * writeback. This covers the old-runtime null-string probes seen in MRP. */
    if ((insn & 0x0E100000u) != 0x04100000u || /* bits 27:26=01, I=0, L=1 */
        !(insn & (1u << 24)) ||               /* P=1 */
        (insn & (1u << 21))) {                /* W=0 */
        return false;
    }
    unsigned long *regs[] = {
        &uc->uc_mcontext.arm_r0, &uc->uc_mcontext.arm_r1,
        &uc->uc_mcontext.arm_r2, &uc->uc_mcontext.arm_r3,
        &uc->uc_mcontext.arm_r4, &uc->uc_mcontext.arm_r5,
        &uc->uc_mcontext.arm_r6, &uc->uc_mcontext.arm_r7,
        &uc->uc_mcontext.arm_r8, &uc->uc_mcontext.arm_r9,
        &uc->uc_mcontext.arm_r10, &uc->uc_mcontext.arm_fp,
        &uc->uc_mcontext.arm_ip, &uc->uc_mcontext.arm_sp,
        &uc->uc_mcontext.arm_lr, &uc->uc_mcontext.arm_pc,
    };
    const unsigned rn = (insn >> 16) & 15u;
    const unsigned rt = (insn >> 12) & 15u;
    const uint32_t offset = insn & 0xFFFu;
    const uint32_t base = rn == 15 ? pc + 8 : (uint32_t)*regs[rn];
    const uint32_t address = (insn & (1u << 23)) ? base + offset : base - offset;
    const size_t size = (insn & (1u << 22)) ? 1 : 4;
    if (rt == 15 || address != fault_address || address + size > 0x80000u) return false;
    *regs[rt] = 0;
    uc->uc_mcontext.arm_pc = pc + 4;
    context_from_signal(&backend->context, uc);
    return true;
}

static void native_signal(int sig, siginfo_t *info, void *opaque) {
    NativeBackend *backend = active_backend;
    ucontext_t *uc = opaque;
    if (!backend || !backend->running) return;
    const uint32_t pc = uc->uc_mcontext.arm_pc;
    if (sig == SIGSEGV) {
        const uintptr_t fault_address = (uintptr_t)info->si_addr;
        if (emulate_thumb_low_read(backend, uc, pc, fault_address) ||
            emulate_arm_low_read(backend, uc, pc, fault_address)) {
            return;
        }
    }
    context_from_signal(&backend->context, uc);
    NativeHook *hook = sig == SIGILL ? find_hook(backend, pc) : NULL;
    if (hook) {
        const GuestAddr dispatch = hook->dispatch_address ? hook->dispatch_address : pc;
        if (hook->callback(backend->runtime, dispatch, 4, hook->user_data)) {
            context_to_signal(&backend->context, uc);
            return;
        }
    }
    backend->result.reason = sig == SIGILL ? ARM_STOP_INVALID_INSTRUCTION : ARM_STOP_INVALID_MEMORY;
    backend->result.pc = pc;
    backend->result.fault_address = (GuestAddr)(uintptr_t)info->si_addr;
    backend->result.backend_error = sig;
    backend->result.message = sig == SIGILL ? "native ARM illegal instruction" : "native ARM memory fault";
    uint32_t instruction = 0;
    guest_memory_read(arm_runtime_memory(backend->runtime), pc & ~3u,
                      &instruction, sizeof(instruction));
    fprintf(stdout,
            "Native ARM fault: signal=%d pc=%08X address=%08X instruction=%08X "
            "cpsr=%08X sp=%08X lr=%08X\n",
            sig, pc, backend->result.fault_address, instruction,
            backend->context.cpsr, backend->context.regs[13], backend->context.regs[14]);
    fflush(stdout);
    backend->running = 0;
    siglongjmp(backend->escape, 1);
}

static void Destroy(ArmRuntime *runtime) {
    NativeBackend *b = arm_runtime_backend_data(runtime);
    sigaction(SIGILL, &b->old_ill, NULL); sigaction(SIGSEGV, &b->old_segv, NULL);
    sigaction(SIGBUS, &b->old_bus, NULL);
    while (b->hooks) { NativeHook *next = b->hooks->next; free(b->hooks); b->hooks = next; }
    if (active_backend == b) active_backend = NULL;
    if (b->stub_area) munmap(b->stub_area, b->stub_capacity * sizeof(uint32_t));
    free(b);
}

static bool RegRead(ArmRuntime *r, ArmRegister reg, uint32_t *v) {
    NativeBackend *b = arm_runtime_backend_data(r); if (!v) return false;
    if (reg <= ARM_RUNTIME_PC) *v = b->context.regs[reg];
    else if (reg == ARM_RUNTIME_CPSR) *v = b->context.cpsr;
    else if (reg == ARM_RUNTIME_FPSCR) *v = 0;
    else return false;
    return true;
}
static bool RegWrite(ArmRuntime *r, ArmRegister reg, uint32_t v) {
    NativeBackend *b = arm_runtime_backend_data(r);
    if (reg <= ARM_RUNTIME_PC) b->context.regs[reg] = v;
    else if (reg == ARM_RUNTIME_CPSR) b->context.cpsr = v;
    else if (reg != ARM_RUNTIME_FPSCR) return false;
    return true;
}
static bool MemRead(ArmRuntime *r, GuestAddr a, void *d, size_t n) { return guest_memory_read(arm_runtime_memory(r), a, d, n); }
static bool MemWrite(ArmRuntime *r, GuestAddr a, const void *d, size_t n) {
    NativeBackend *b = arm_runtime_backend_data(r);
    uint32_t word = 0;
    if (n == 4) memcpy(&word, d, 4);
    NativeHook *range = n == 4 && word == a ? find_hook(b, a) : NULL;
    if (range) {
        if (!b->stub_area || b->stub_count >= b->stub_capacity) return false;
        uint32_t *stub = &b->stub_area[b->stub_count++];
        *stub = 0xE7F000F0u;
        __builtin___clear_cache((char *)stub, (char *)(stub + 1));
        NativeHook *alias = calloc(1, sizeof(*alias));
        if (!alias) return false;
        alias->begin = alias->end = (GuestAddr)(uintptr_t)stub;
        alias->dispatch_address = a;
        alias->callback = range->callback;
        alias->user_data = range->user_data;
        alias->next = b->hooks;
        b->hooks = alias;
        word = (uint32_t)(uintptr_t)stub;
    }
    bool ok = guest_memory_write(arm_runtime_memory(r), a, range ? &word : d, n);
    if (ok) __builtin___clear_cache((char *)(uintptr_t)a, (char *)(uintptr_t)(a + n));
    return ok;
}
static ArmRunResult Run(ArmRuntime *r, const ArmRunOptions *o) {
    NativeBackend *b = arm_runtime_backend_data(r);
    b->result = (ArmRunResult){.reason = ARM_STOP_RETURN, .pc = o->start};
    b->running = 1;
    if (sigsetjmp(b->escape, 1) == 0) {
        uint32_t start = o->start | (o->thumb ? 1u : 0u);
        vmrp_native_arm_call(&b->context, start);
        b->result.pc = b->context.regs[15];
    }
    b->running = 0;
    return b->result;
}
static bool AddHook(ArmRuntime *r, GuestAddr begin, GuestAddr end, ArmCodeHook cb, void *u) {
    NativeBackend *b = arm_runtime_backend_data(r); NativeHook *h = calloc(1, sizeof(*h));
    if (!h) return false;
    h->begin = begin; h->end = end; h->dispatch_address = 0;
    h->callback = cb; h->user_data = u;
    h->next = b->hooks; b->hooks = h; return true;
}
static const ArmRuntimeOps ops = {Destroy, RegRead, RegWrite, MemRead, MemWrite, Run, AddHook};

ArmRuntime *arm_runtime_create_native(GuestMemory *memory) {
    NativeBackend *b = calloc(1, sizeof(*b)); if (!b) return NULL;
    ArmRuntime *r = arm_runtime_alloc(&ops, ARM_BACKEND_NATIVE, memory, b); if (!r) { free(b); return NULL; }
    b->runtime = r; b->context.cpsr = 0x400001D3; active_backend = b;
    b->stub_capacity = 4096;
    b->stub_area = mmap(NULL, b->stub_capacity * sizeof(uint32_t),
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b->stub_area == MAP_FAILED) {
        b->stub_area = NULL;
        arm_runtime_destroy(r);
        return NULL;
    }
    struct sigaction sa = {.sa_sigaction = native_signal, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &b->old_ill); sigaction(SIGSEGV, &sa, &b->old_segv); sigaction(SIGBUS, &sa, &b->old_bus);
    return r;
}
