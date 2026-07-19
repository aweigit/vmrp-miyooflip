#ifndef VMRP_ARM_RUNTIME_H
#define VMRP_ARM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "guest_memory.h"

typedef enum ArmBackendKind {
    ARM_BACKEND_UNICORN,
    ARM_BACKEND_DYNARMIC,
    ARM_BACKEND_NATIVE,
} ArmBackendKind;

typedef enum ArmRegister {
    ARM_RUNTIME_R0,
    ARM_RUNTIME_R1,
    ARM_RUNTIME_R2,
    ARM_RUNTIME_R3,
    ARM_RUNTIME_R4,
    ARM_RUNTIME_R5,
    ARM_RUNTIME_R6,
    ARM_RUNTIME_R7,
    ARM_RUNTIME_R8,
    ARM_RUNTIME_R9,
    ARM_RUNTIME_R10,
    ARM_RUNTIME_R11,
    ARM_RUNTIME_R12,
    ARM_RUNTIME_SP,
    ARM_RUNTIME_LR,
    ARM_RUNTIME_PC,
    ARM_RUNTIME_CPSR,
    ARM_RUNTIME_FPSCR,
} ArmRegister;

typedef enum ArmStopReason {
    ARM_STOP_RETURN,
    ARM_STOP_INVALID_MEMORY,
    ARM_STOP_INVALID_INSTRUCTION,
    ARM_STOP_TIMEOUT,
    ARM_STOP_ERROR,
} ArmStopReason;

typedef struct ArmRunOptions {
    GuestAddr start;
    GuestAddr stop;
    bool thumb;
    uint64_t timeout_us;
    uint64_t instruction_limit;
} ArmRunOptions;

typedef struct ArmRunResult {
    ArmStopReason reason;
    GuestAddr pc;
    GuestAddr fault_address;
    int backend_error;
    const char *message;
} ArmRunResult;

typedef struct ArmRuntime ArmRuntime;
typedef bool (*ArmCodeHook)(ArmRuntime *runtime, GuestAddr address, uint32_t size, void *user_data);

#ifdef __cplusplus
extern "C" {
#endif

ArmRuntime *arm_runtime_create_unicorn(GuestMemory *memory);
ArmRuntime *arm_runtime_create_dynarmic(GuestMemory *memory);
ArmRuntime *arm_runtime_create_native(GuestMemory *memory);
void arm_runtime_destroy(ArmRuntime *runtime);
ArmBackendKind arm_runtime_backend(const ArmRuntime *runtime);
GuestMemory *arm_runtime_memory(ArmRuntime *runtime);
void *arm_runtime_native_handle(ArmRuntime *runtime);
bool arm_runtime_reg_read(ArmRuntime *runtime, ArmRegister reg, uint32_t *value);
bool arm_runtime_reg_write(ArmRuntime *runtime, ArmRegister reg, uint32_t value);
bool arm_runtime_mem_read(ArmRuntime *runtime, GuestAddr address, void *data, size_t size);
bool arm_runtime_mem_write(ArmRuntime *runtime, GuestAddr address, const void *data, size_t size);
ArmRunResult arm_runtime_run(ArmRuntime *runtime, const ArmRunOptions *options);
bool arm_runtime_add_code_hook(ArmRuntime *runtime, GuestAddr begin, GuestAddr end,
                               ArmCodeHook callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
