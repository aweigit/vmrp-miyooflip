#include "./header/guest_memory.h"

#include <stdlib.h>
#include <string.h>
#ifdef VMRP_ENABLE_NATIVE_ARM
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#endif

static bool region_contains(const GuestMemoryRegion *region, GuestAddr address, size_t size) {
    uint64_t start = address;
    uint64_t end = start + size;
    uint64_t region_end = (uint64_t)region->base + region->size;
    return (region->host != NULL || region->fixed_mapping) &&
           start >= region->base && end >= start && end <= region_end;
}

static const GuestMemoryRegion *find_region(const GuestMemory *memory, GuestAddr address, size_t size) {
    if (region_contains(&memory->low, address, size)) return &memory->low;
    if (region_contains(&memory->main, address, size)) return &memory->main;
    return NULL;
}

bool guest_memory_init(GuestMemory *memory, GuestAddr main_base, uint32_t main_size) {
    if (!memory || main_base == 0 || main_size == 0) return false;
    memset(memory, 0, sizeof(*memory));

    memory->low.base = 0;
    memory->low.size = main_base;
    memory->low.permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE | GUEST_MEMORY_EXEC;
#ifdef VMRP_ENABLE_NATIVE_ARM
    int low_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    low_flags |= MAP_FIXED_NOREPLACE;
#else
    low_flags |= MAP_FIXED;
#endif
    void *low_mapping = mmap((void *)0, memory->low.size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             low_flags, -1, 0);
#ifdef MAP_FIXED_NOREPLACE
    if (low_mapping == MAP_FAILED && errno == EINVAL) {
        low_mapping = mmap((void *)0, memory->low.size,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
#endif
    if (low_mapping == MAP_FAILED) {
        perror("native low guest mmap (set vm.mmap_min_addr=0)");
        memory->low.host = NULL;
        memory->low.fixed_mapping = false;
    } else {
        memory->low.host = low_mapping;
        memory->low.fixed_mapping = true;
    }
#else
    memory->low.host = calloc(1, memory->low.size);
#endif

    memory->main.base = main_base;
    memory->main.size = main_size;
    memory->main.permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE | GUEST_MEMORY_EXEC;
#ifdef VMRP_ENABLE_NATIVE_ARM
    int map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    map_flags |= MAP_FIXED_NOREPLACE;
#else
    map_flags |= MAP_FIXED;
#endif
    memory->main.host = mmap((void *)(uintptr_t)main_base, memory->main.size,
                             PROT_READ | PROT_WRITE | PROT_EXEC, map_flags, -1, 0);
#ifdef MAP_FIXED_NOREPLACE
    /* Old ARM kernels reject MAP_FIXED_NOREPLACE with EINVAL. The ARM32
     * executable is deliberately non-PIE and ends below main_base, so only
     * those kernels need the legacy MAP_FIXED fallback. */
    if (memory->main.host == MAP_FAILED && errno == EINVAL) {
        memory->main.host = mmap((void *)(uintptr_t)main_base, memory->main.size,
                                 PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
#endif
    if (memory->main.host == MAP_FAILED) {
        perror("native guest mmap");
        memory->main.host = NULL;
    }
#else
    memory->main.host = malloc(memory->main.size);
#endif

    if ((!memory->low.host && !memory->low.fixed_mapping) || !memory->main.host) {
        guest_memory_destroy(memory);
        return false;
    }
    return true;
}

void guest_memory_destroy(GuestMemory *memory) {
    if (!memory) return;
#ifdef VMRP_ENABLE_NATIVE_ARM
    if (memory->low.fixed_mapping) munmap((void *)0, memory->low.size);
    else free(memory->low.host);
    if (memory->main.host) munmap(memory->main.host, memory->main.size);
#else
    free(memory->low.host);
    free(memory->main.host);
#endif
    memset(memory, 0, sizeof(*memory));
}

const void *guest_memory_translate_const(const GuestMemory *memory, GuestAddr address, size_t size) {
    const GuestMemoryRegion *region = memory ? find_region(memory, address, size) : NULL;
    return region ? region->host + (address - region->base) : NULL;
}

void *guest_memory_translate(GuestMemory *memory, GuestAddr address, size_t size) {
    return (void *)guest_memory_translate_const(memory, address, size);
}

bool guest_memory_read(const GuestMemory *memory, GuestAddr address, void *data, size_t size) {
    const void *source = guest_memory_translate_const(memory, address, size);
    if (!source || (!data && size)) return false;
    memcpy(data, source, size);
    return true;
}

bool guest_memory_write(GuestMemory *memory, GuestAddr address, const void *data, size_t size) {
    void *destination = guest_memory_translate(memory, address, size);
    if (!destination || (!data && size)) return false;
    memcpy(destination, data, size);
    return true;
}

bool guest_memory_address(const GuestMemory *memory, const void *host, GuestAddr *address) {
    const uint8_t *pointer = host;
    if (!memory || !host || !address) return false;
    const GuestMemoryRegion *regions[] = {&memory->low, &memory->main};
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        const GuestMemoryRegion *region = regions[i];
        if (pointer >= region->host && pointer < region->host + region->size) {
            *address = region->base + (GuestAddr)(pointer - region->host);
            return true;
        }
    }
    return false;
}
