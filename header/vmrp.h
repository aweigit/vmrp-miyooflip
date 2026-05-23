#ifndef __VMRP__H__
#define __VMRP__H__

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "utils.h"

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

#define CODE_ADDRESS 0x80000       // 代码段的起始地址
#define CODE_SIZE 1024 * 1024 * 1  // 代码段长度

#define STACK_ADDRESS (CODE_ADDRESS + CODE_SIZE)  // 栈开始地址
#define STACK_SIZE 1024 * 1024 * 1                // 栈大小

#define MEMORY_MANAGER_ADDRESS (STACK_ADDRESS + STACK_SIZE)  // 由malloc和free管理的模拟器内存
#define MEMORY_MANAGER_SIZE 1024 * 1024 * 6

#define START_ADDRESS CODE_ADDRESS
#define END_ADDRESS (MEMORY_MANAGER_ADDRESS + MEMORY_MANAGER_SIZE)
#define TOTAL_MEMORY (END_ADDRESS - START_ADDRESS)

extern uint8_t *mrpMemPtr;

static inline void *getMrpMemPtr(uint32_t addr) {
    return mrpMemPtr + (addr - START_ADDRESS);
}

static inline uint32_t toMrpMemAddr(void *ptr) {
    return ((uint8_t *)ptr - mrpMemPtr) + START_ADDRESS;
}

int32_t event(int32_t code, int32_t p1, int32_t p2);
int32_t timer();
int startVmrp(const char *mrpFile, const char *extName, const char *entry);

#endif
