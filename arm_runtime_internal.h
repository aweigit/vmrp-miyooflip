#ifndef VMRP_ARM_RUNTIME_INTERNAL_H
#define VMRP_ARM_RUNTIME_INTERNAL_H

#include "./header/arm_runtime.h"

typedef struct ArmRuntimeOps {
    void (*destroy)(ArmRuntime *runtime);
    bool (*reg_read)(ArmRuntime *runtime, ArmRegister reg, uint32_t *value);
    bool (*reg_write)(ArmRuntime *runtime, ArmRegister reg, uint32_t value);
    bool (*mem_read)(ArmRuntime *runtime, GuestAddr address, void *data, size_t size);
    bool (*mem_write)(ArmRuntime *runtime, GuestAddr address, const void *data, size_t size);
    ArmRunResult (*run)(ArmRuntime *runtime, const ArmRunOptions *options);
    bool (*add_code_hook)(ArmRuntime *runtime, GuestAddr begin, GuestAddr end,
                          ArmCodeHook callback, void *user_data);
} ArmRuntimeOps;

#ifdef __cplusplus
extern "C" {
#endif
ArmRuntime *arm_runtime_alloc(const void *ops, ArmBackendKind kind, GuestMemory *memory, void *backend);
void *arm_runtime_backend_data(ArmRuntime *runtime);
#ifdef __cplusplus
}
#endif

#endif
