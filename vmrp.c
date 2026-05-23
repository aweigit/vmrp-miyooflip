#include "./header/vmrp.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "./header/bridge.h"
#include "./header/fileLib.h"
#include "./header/memory.h"
#include "./header/utils.h"
#include "./header/debug.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

uint8_t *mrpMemPtr;  // 模拟器的全部内存
static void *lowMem = NULL;  // 0-CODE_ADDRESS 的低地址内存映射
static uc_engine *uc = NULL;

#ifdef DEBUG
static void hook_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    printf(">>> Tracing basic block at 0x%" PRIx64 ", block size = 0x%x\n", address, size);
}
static void hook_mem_valid(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
    printf(">>> Tracing mem_valid mem_type:%s at 0x%" PRIx64 ", size:0x%x, value:0x%" PRIx64 "\n",
           memTypeStr(type), address, size, value);
    if (type == UC_MEM_READ && size <= 4) {
        uint32_t v, pc;
        uc_mem_read(uc, address, &v, size);
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        printf("PC:0x%X,read:0x%X\n", pc, v);
    }
}

#endif

static bool hook_mem_invalid(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
    printf(">>> Tracing mem_invalid mem_type:%s at 0x%" PRIx64 ", size:0x%x, value:0x%" PRIx64 "\n",
           memTypeStr(type), address, size, value);
    dumpREG(uc);
    return false;
}

static bool hook_insn_invalid(uc_engine *uc, void *user_data) {
    uint32_t pc, lr, sp;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    printf(">>> Tracing insn_invalid at PC=0x%X, LR=0x%X, SP=0x%X\n", pc, lr, sp);
    dumpREG(uc);
    // Dump 16 bytes at PC
    if (pc >= START_ADDRESS && pc < END_ADDRESS) {
        uint32_t data[4];
        uc_mem_read(uc, pc, &data, sizeof(data));
        printf("    mem@PC:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
    }
    // Dump 32 bytes at LR (8 instructions) aligned down
    if (lr >= START_ADDRESS && lr < END_ADDRESS) {
        uint32_t data[8];
        uint32_t lr_aligned = lr & ~3;
        uc_mem_read(uc, lr_aligned, &data, sizeof(data));
        printf("    mem@LR-:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
        printf("    mem@LR+:  %08X %08X %08X %08X\n", data[4], data[5], data[6], data[7]);
    }
    // Dump top of stack (16 words)
    if (sp >= START_ADDRESS && sp < END_ADDRESS) {
        uint32_t data[16];
        uc_mem_read(uc, sp, &data, sizeof(data));
        printf("    mem@SP:  %08X %08X %08X %08X\n", data[0], data[1], data[2], data[3]);
        printf("            %08X %08X %08X %08X\n", data[4], data[5], data[6], data[7]);
        printf("            %08X %08X %08X %08X\n", data[8], data[9], data[10], data[11]);
        printf("            %08X %08X %08X %08X\n", data[12], data[13], data[14], data[15]);
    }
    return false;
}

int freeVmrp(uc_engine *uc) {
    if (lowMem) { free(lowMem); lowMem = NULL; }
    free(mrpMemPtr);
    uc_close(uc);
    return 0;
}

uc_engine *initVmrp() {
    uc_engine *uc;
    uc_err err;
    uc_hook trace;

    err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err) {
        printf("Failed on uc_open() with error returned: %u (%s)\n", err, uc_strerror(err));
        return NULL;
    }

    mrpMemPtr = malloc(TOTAL_MEMORY);
    // unicorn存在BUG，UC_HOOK_MEM_INVALID只能拦截第一次UC_MEM_FETCH_PROT，所以干脆设置成可执行，统一在UC_HOOK_CODE事件中处理
    err = uc_mem_map_ptr(uc, START_ADDRESS, TOTAL_MEMORY, UC_PROT_ALL, mrpMemPtr);
    if (err) {
        printf("Failed mem map: %u (%s)\n", err, uc_strerror(err));
        goto end;
    }
    initMemoryManager(MEMORY_MANAGER_ADDRESS, MEMORY_MANAGER_SIZE);

    err = bridge_init(uc);
    if (err) {
        printf("Failed bridge_init(): %u (%s)\n", err, uc_strerror(err));
        goto end;
    }

    // 一些游戏检测内存时会去读写0-CODE_ADDRESS地址的值，映射一块内存避免无效内存访问
    lowMem = malloc(CODE_ADDRESS);
    memset(lowMem, 0, CODE_ADDRESS);
    uc_mem_map_ptr(uc, 0, CODE_ADDRESS, UC_PROT_ALL, lowMem);

#ifdef DEBUG
    uc_hook_add(uc, &trace, UC_HOOK_BLOCK, hook_block, NULL, 1, 0);
    uc_hook_add(uc, &trace, UC_HOOK_MEM_VALID, hook_mem_valid, NULL, 1, 0);
    uc_hook_add(uc, &trace, UC_HOOK_CODE, hook_code_trace, NULL, 1, 0);
#endif
    uc_hook_add(uc, &trace, UC_HOOK_MEM_INVALID, hook_mem_invalid, NULL, 1, 0);
    uc_hook_add(uc, &trace, UC_HOOK_INSN_INVALID, hook_insn_invalid, NULL, 1, 0);

    // 设置栈
    uint32_t value = STACK_ADDRESS + STACK_SIZE;  // 满递减
    uc_reg_write(uc, UC_ARM_REG_SP, &value);

    return uc;
end:
    uc_close(uc);
    return NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
int32_t c_event(int32_t code, int32_t p1, int32_t p2) {
    if (uc) {
        return bridge_dsm_mr_event(uc, code, p1, p2);
    }
    return MR_FAILED;
}
#endif

int32_t event(int32_t code, int32_t p1, int32_t p2) {
    if (uc) {
        return bridge_dsm_mr_event(uc, code, p1, p2);
    }
    return MR_FAILED;
}

int32_t timer() {
    if (uc) {
        return bridge_dsm_mr_timer(uc);
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
    uc_mem_write(uc, CODE_ADDRESS, buf, len);
    free(buf);
    return MR_SUCCESS;
}

int startVmrp(const char *mrpFile, const char *extName, const char *entry) {
    uc = initVmrp();
    if (uc == NULL) {
        printf("initVmrp() fail.\n");
        return MR_FAILED;
    }

    if (loadCode() == MR_FAILED) {
        printf("loadCode fail.\n");
        return MR_FAILED;
    }
    bridge_ext_init(uc);

    if (bridge_dsm_init(uc) == MR_SUCCESS) {
        dumpREG(uc);

        uint32_t ret = bridge_dsm_mr_start_dsm(uc, (char *)mrpFile, (char *)extName, (char *)entry);
        printf("bridge_dsm_mr_start_dsm('%s','%s','%s'): 0x%X\n",
               mrpFile, extName, entry ? entry : "NULL", ret);
    }
    return MR_SUCCESS;
}