#pragma once

/* The only platform specific code of the code generator: executable memory
 * for the JIT. Without it the JIT is simply unavailable. */

#include <stdbool.h>
#include <stddef.h>

void *cg_platform_alloc_executable(size_t size);
void cg_platform_free_executable(void *memory, size_t size);
bool cg_platform_jit_available(void);
