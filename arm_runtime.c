#include "./arm_runtime_internal.h"

#include <stdlib.h>

#ifndef VMRP_ENABLE_NATIVE_ARM
void *arm_runtime_unicorn_native_handle(ArmRuntime *runtime);
#endif

struct ArmRuntime {
    const ArmRuntimeOps *ops;
    ArmBackendKind kind;
    GuestMemory *memory;
    void *backend;
};

/* Backend constructors use this internal allocation entry point. */
ArmRuntime *arm_runtime_alloc(const void *ops, ArmBackendKind kind, GuestMemory *memory, void *backend) {
    ArmRuntime *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return NULL;
    runtime->ops = ops;
    runtime->kind = kind;
    runtime->memory = memory;
    runtime->backend = backend;
    return runtime;
}

void *arm_runtime_backend_data(ArmRuntime *runtime) {
    return runtime ? runtime->backend : NULL;
}

void arm_runtime_destroy(ArmRuntime *runtime) {
    if (!runtime) return;
    runtime->ops->destroy(runtime);
    free(runtime);
}

ArmBackendKind arm_runtime_backend(const ArmRuntime *runtime) { return runtime->kind; }
GuestMemory *arm_runtime_memory(ArmRuntime *runtime) { return runtime ? runtime->memory : NULL; }
void *arm_runtime_native_handle(ArmRuntime *runtime) {
    if (!runtime) return NULL;
#ifndef VMRP_ENABLE_NATIVE_ARM
    if (runtime->kind == ARM_BACKEND_UNICORN) return arm_runtime_unicorn_native_handle(runtime);
#endif
    return NULL;
}
bool arm_runtime_reg_read(ArmRuntime *runtime, ArmRegister reg, uint32_t *value) { return runtime && runtime->ops->reg_read(runtime, reg, value); }
bool arm_runtime_reg_write(ArmRuntime *runtime, ArmRegister reg, uint32_t value) { return runtime && runtime->ops->reg_write(runtime, reg, value); }
bool arm_runtime_mem_read(ArmRuntime *runtime, GuestAddr address, void *data, size_t size) { return runtime && runtime->ops->mem_read(runtime, address, data, size); }
bool arm_runtime_mem_write(ArmRuntime *runtime, GuestAddr address, const void *data, size_t size) { return runtime && runtime->ops->mem_write(runtime, address, data, size); }
ArmRunResult arm_runtime_run(ArmRuntime *runtime, const ArmRunOptions *options) {
    if (!runtime || !options) return (ArmRunResult){.reason = ARM_STOP_ERROR, .message = "invalid runtime or run options"};
    return runtime->ops->run(runtime, options);
}

bool arm_runtime_add_code_hook(ArmRuntime *runtime, GuestAddr begin, GuestAddr end,
                               ArmCodeHook callback, void *user_data) {
    return runtime && callback && runtime->ops->add_code_hook &&
           runtime->ops->add_code_hook(runtime, begin, end, callback, user_data);
}
