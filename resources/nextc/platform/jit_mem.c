#include "jit_mem.h"

#if defined(_WIN32)

extern void *VirtualAlloc(void *address, unsigned long long size,
                          unsigned long type, unsigned long protect);
extern int VirtualFree(void *address, unsigned long long size,
                       unsigned long type);

void *cg_platform_alloc_executable(size_t size) {
    /* MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE */
    return VirtualAlloc(nullptr, size, 0x3000, 0x40);
}

void cg_platform_free_executable(void *memory, size_t size) {
    (void)size;
    /* MEM_RELEASE */
    VirtualFree(memory, 0, 0x8000);
}

bool cg_platform_jit_available(void) { return true; }

#else

void *cg_platform_alloc_executable(size_t size) {
    (void)size;
    return nullptr;
}

void cg_platform_free_executable(void *memory, size_t size) {
    (void)memory;
    (void)size;
}

bool cg_platform_jit_available(void) { return false; }

#endif
