#pragma once

/* Object file writers. They only know the abstract relocation kinds of the
 * MC layer, never an instruction set (design doc, section 14). */

#include "../mc/mc.h"

typedef enum obj_machine : uint8_t {
    OBJ_MACHINE_X86_64,
} obj_machine_t;

typedef struct obj_writer {
    const char *name;
    bool (*write)(const mc_module_t *mc, obj_machine_t machine, cg_buffer_t *out,
                  cg_diag_t *diag);
} obj_writer_t;

extern const obj_writer_t obj_writer_coff;
extern const obj_writer_t obj_writer_elf64;
