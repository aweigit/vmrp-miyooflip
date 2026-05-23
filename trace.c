#include "./header/debug.h"
#include "./header/utils.h"
#include "./header/vmrp.h"
#include <capstone/capstone.h>

#define TRACE_BUF_SIZE 256

static struct {
    uint32_t pc;
    uint32_t address;
    uint32_t size;
    uint8_t binary[4];
    int mode;  // 0=ARM, 1=Thumb
} trace_buf[TRACE_BUF_SIZE];

static int trace_idx = 0;

void hook_code_trace(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    uint32_t pc;
    uint32_t cpsr;

    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);

    int i = trace_idx % TRACE_BUF_SIZE;
    trace_buf[i].pc = pc;
    trace_buf[i].address = (uint32_t)address;
    trace_buf[i].size = size;
    trace_buf[i].mode = (cpsr & (1 << 5)) ? 1 : 0;

    if (size <= 4) {
        uc_mem_read(uc, address, trace_buf[i].binary, size);
    } else {
        memset(trace_buf[i].binary, 0, 4);
    }
    trace_idx++;
}

void dump_trace(void) {
    csh handle;
    int start, count;

    if (trace_idx <= TRACE_BUF_SIZE) {
        start = 0;
        count = trace_idx;
    } else {
        start = trace_idx % TRACE_BUF_SIZE;
        count = TRACE_BUF_SIZE;
    }

    printf(">>> Last %d instructions before crash:\n", count);
    for (int n = 0; n < count; n++) {
        int i = (start + n) % TRACE_BUF_SIZE;
        cs_mode mode = trace_buf[i].mode ? CS_MODE_THUMB : CS_MODE_ARM;

        if (cs_open(CS_ARCH_ARM, mode, &handle) == CS_ERR_OK) {
            cs_insn *insn;
            size_t cnt = cs_disasm(handle, trace_buf[i].binary, trace_buf[i].size,
                                    trace_buf[i].address, 1, &insn);
            if (cnt > 0) {
                printf("    0x%X:  %-7s %s\n", trace_buf[i].pc,
                       insn[0].mnemonic, insn[0].op_str);
                cs_free(insn, cnt);
            } else {
                printf("    0x%X:  ??? (invalid)\n", trace_buf[i].pc);
            }
            cs_close(&handle);
        }
    }
}
