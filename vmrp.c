#include "./header/vmrp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./header/bridge.h"
#include "./header/fileLib.h"
#include "./header/memory.h"
#include "./header/utils.h"
#include "./header/arm_runtime.h"
#include "./header/guest_memory.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

uint8_t *mrpMemPtr;  // 模拟器的全部内存
static GuestMemory guestMemory;
static ArmRuntime *runtime = NULL;

int freeVmrp(void) {
    arm_runtime_destroy(runtime);
    runtime = NULL;
    guest_memory_destroy(&guestMemory);
    mrpMemPtr = NULL;
    return 0;
}

ArmRuntime *initVmrp(void) {
    if (!guest_memory_init(&guestMemory, START_ADDRESS, TOTAL_MEMORY)) {
        fprintf(stderr, "Failed to allocate guest memory\n");
        return NULL;
    }
    mrpMemPtr = guestMemory.main.host;
    const char *requested_backend = getenv("VMRP_CPU_BACKEND");
    ArmRuntime *new_runtime = NULL;
#ifdef VMRP_ENABLE_NATIVE_ARM
    if (!requested_backend || strcmp(requested_backend, "native") == 0) {
        new_runtime = arm_runtime_create_native(&guestMemory);
    } else {
        fprintf(stderr, "Unknown CPU backend '%s' (expected native)\n", requested_backend);
    }
#elif defined(VMRP_ENABLE_DYNARMIC)
    if (!requested_backend || strcmp(requested_backend, "dynarmic") == 0) {
        new_runtime = arm_runtime_create_dynarmic(&guestMemory);
    } else if (strcmp(requested_backend, "unicorn") != 0) {
        fprintf(stderr, "Unknown CPU backend '%s' (expected dynarmic or unicorn)\n",
                requested_backend);
    }
#else
    if (requested_backend && strcmp(requested_backend, "unicorn") != 0) {
        fprintf(stderr, "CPU backend '%s' is not available in this build\n", requested_backend);
    }
#endif
    if (!new_runtime && (!requested_backend || strcmp(requested_backend, "unicorn") == 0)) {
#ifndef VMRP_ENABLE_NATIVE_ARM
        new_runtime = arm_runtime_create_unicorn(&guestMemory);
#endif
    }
    if (!new_runtime) {
        fprintf(stderr, "Failed to create requested ARM runtime\n");
        guest_memory_destroy(&guestMemory);
        mrpMemPtr = NULL;
        return NULL;
    }
    const ArmBackendKind backend_kind = arm_runtime_backend(new_runtime);
    printf("CPU backend: %s\n", backend_kind == ARM_BACKEND_NATIVE ? "native" :
           backend_kind == ARM_BACKEND_DYNARMIC ? "dynarmic" : "unicorn");
    initMemoryManager(MEMORY_MANAGER_ADDRESS, MEMORY_MANAGER_SIZE);

    int err = bridge_init(new_runtime);
    if (err) {
        fprintf(stderr, "Failed bridge_init(): %d\n", err);
        goto end;
    }
    // 设置栈
    uint32_t value = STACK_ADDRESS + STACK_SIZE;  // 满递减
    arm_runtime_reg_write(new_runtime, ARM_RUNTIME_SP, value);

    return new_runtime;
end:
    arm_runtime_destroy(new_runtime);
    guest_memory_destroy(&guestMemory);
    mrpMemPtr = NULL;
    return NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
int32_t c_event(int32_t code, int32_t p1, int32_t p2) {
    if (runtime) {
        return bridge_dsm_mr_event(runtime, code, p1, p2);
    }
    return MR_FAILED;
}
#endif

int32_t event(int32_t code, int32_t p1, int32_t p2) {
    if (runtime) {
        return bridge_dsm_mr_event(runtime, code, p1, p2);
    }
    return MR_FAILED;
}

int32_t timer() {
    if (runtime) {
        return bridge_dsm_mr_timer(runtime);
    }
    return MR_FAILED;
}

int32_t loadCode() {
    char *filename = "cfunction.ext";
    int32_t len = my_getLen(filename);
    char *buf = readFile(filename);
    if (buf == NULL) {
        return MR_FAILED;
    }
    if (!arm_runtime_mem_write(runtime, CODE_ADDRESS, buf, len)) {
        free(buf);
        return MR_FAILED;
    }
    free(buf);
    return MR_SUCCESS;
}

int startVmrp(const char *mrpFile, const char *extName, const char *entry) {
    runtime = initVmrp();
    if (runtime == NULL) {
        printf("initVmrp() fail.\n");
        return MR_FAILED;
    }

    if (loadCode() == MR_FAILED) {
        printf("loadCode fail.\n");
        return MR_FAILED;
    }
    bridge_ext_init(runtime);

    if (bridge_dsm_init(runtime) == MR_SUCCESS) {
        uint32_t pc = 0;
        arm_runtime_reg_read(runtime, ARM_RUNTIME_PC, &pc);
        printf("ARM runtime initialized at PC=0x%X\n", pc);

        uint32_t ret = bridge_dsm_mr_start_dsm(runtime, (char *)mrpFile, (char *)extName, (char *)entry);
        printf("bridge_dsm_mr_start_dsm('%s','%s','%s'): 0x%X\n",
               mrpFile, extName, entry ? entry : "NULL", ret);
    }

    return MR_SUCCESS;
}
