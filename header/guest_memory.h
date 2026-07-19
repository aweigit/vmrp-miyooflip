#ifndef VMRP_GUEST_MEMORY_H
#define VMRP_GUEST_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t GuestAddr;

enum GuestMemoryPermission {
    GUEST_MEMORY_READ = 1u << 0,
    GUEST_MEMORY_WRITE = 1u << 1,
    GUEST_MEMORY_EXEC = 1u << 2,
};

typedef struct GuestMemoryRegion {
    GuestAddr base;
    uint32_t size;
    uint8_t *host;
    uint32_t permissions;
    bool fixed_mapping;
} GuestMemoryRegion;

typedef struct GuestMemory {
    GuestMemoryRegion low;
    GuestMemoryRegion main;
} GuestMemory;

#ifdef __cplusplus
extern "C" {
#endif
bool guest_memory_init(GuestMemory *memory, GuestAddr main_base, uint32_t main_size);
void guest_memory_destroy(GuestMemory *memory);
void *guest_memory_translate(GuestMemory *memory, GuestAddr address, size_t size);
const void *guest_memory_translate_const(const GuestMemory *memory, GuestAddr address, size_t size);
bool guest_memory_read(const GuestMemory *memory, GuestAddr address, void *data, size_t size);
bool guest_memory_write(GuestMemory *memory, GuestAddr address, const void *data, size_t size);
bool guest_memory_address(const GuestMemory *memory, const void *host, GuestAddr *address);
#ifdef __cplusplus
}
#endif

#endif
