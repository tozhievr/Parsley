#pragma once

/* COFF object and archive readers. A library member becomes an MC module so
 * the static linker can merge it with freshly compiled code. */

#include "obj.h"

typedef CG_VEC(const mc_module_t *) obj_module_vec_t;

typedef struct obj_archive {
    mc_module_t **members;
    uint32_t member_count;
    char **dlls; /* DLL names found in import members */
    uint32_t dll_count;
    char **import_names;      /* symbols of short import members */
    uint32_t *import_dll;     /* index into dlls for each import name */
    uint32_t import_count;
} obj_archive_t;

/* Parses a COFF object (AMD64). `what` names the input in diagnostics. */
mc_module_t *obj_read_coff(const uint8_t *data, size_t size, const char *what,
                           cg_diag_t *diag);

/* Loads a `.lib`/`.a` archive, or a single COFF object, from memory. */
bool obj_archive_load(const uint8_t *data, size_t size, const char *what,
                      obj_archive_t *out, cg_diag_t *diag);
void obj_archive_free(obj_archive_t *archive);

/* Appends to `selected` (as non-owning pointers) the archive members that are
 * needed to satisfy the undefined symbols of `base` and of `entry`. */
bool obj_archives_select(const mc_module_t *const *base, uint32_t base_count,
                         const obj_archive_t *archives, uint32_t archive_count,
                         const char *entry, obj_module_vec_t *selected);
