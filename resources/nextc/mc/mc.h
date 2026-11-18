#pragma once

/* MC layer: sections, symbols and relocations of one object. Written by the
 * back end, read by the object writers and by the JIT linker. Knows nothing
 * about the ISA except the abstract relocation kinds. */

#include "../ir/ir.h"

typedef enum mc_section_id : uint8_t {
    MC_TEXT,
    MC_DATA,
    MC_RODATA,
    MC_BSS,
    MC_TLS, /* thread-local template; only produced by the object reader */
    MC_SECTION_COUNT,
} mc_section_id_t;

constexpr uint8_t MC_UNDEFINED = 0xFF;

typedef enum mc_reloc_kind : uint8_t {
    MC_RELOC_ABS64, /* S + A */
    MC_RELOC_PC32,  /* S + A - P, 32 bit field */
    MC_RELOC_PLT32, /* like PC32, for calls */
    MC_RELOC_SECREL, /* offset of S inside the thread-local block, + A */
} mc_reloc_kind_t;

typedef struct mc_symbol {
    uint32_t name; /* in mc_module_t::names */
    uint8_t section; /* mc_section_id_t or MC_UNDEFINED */
    cg_binding_t binding;
    bool is_function;
    uint32_t offset;
    uint32_t size;
} mc_symbol_t;

typedef struct mc_reloc {
    uint8_t section;
    uint8_t kind;
    uint32_t offset;
    uint32_t symbol;
    int64_t addend;
} mc_reloc_t;

typedef struct mc_module {
    cg_strings_t names;
    CG_VEC(mc_symbol_t) symbols;
    cg_buffer_t sections[MC_SECTION_COUNT]; /* BSS only uses bss_size */
    uint32_t alignment[MC_SECTION_COUNT];
    uint32_t bss_size;
    CG_VEC(mc_reloc_t) relocs;
    uint32_t constant_counter;
} mc_module_t;

mc_module_t *mc_create(void);
void mc_destroy(mc_module_t *mc);
bool mc_ok(const mc_module_t *mc);
static inline const char *mc_symbol_name(const mc_module_t *mc,
                                         uint32_t symbol) {
    return cg_strings_get(&mc->names, mc->symbols.data[symbol].name);
}

uint32_t mc_add_symbol(mc_module_t *mc, const char *name, cg_binding_t binding);
void mc_define_symbol(mc_module_t *mc, uint32_t symbol, mc_section_id_t section,
                      uint32_t offset, uint32_t size, bool is_function);
bool mc_add_reloc(mc_module_t *mc, mc_section_id_t section, uint32_t offset,
                  uint32_t symbol, mc_reloc_kind_t kind, int64_t addend);
/* Appends zero padding up to `alignment` and returns the new offset. */
uint32_t mc_align_section(mc_module_t *mc, mc_section_id_t section,
                          uint32_t alignment);
/* Interns a read-only constant and returns a local symbol for it. */
uint32_t mc_add_constant(mc_module_t *mc, const void *bytes, uint32_t size,
                         uint32_t alignment);

/* Creates one MC symbol per module symbol (same ids) and lays out the data
 * objects with their relocations. */
bool mc_import_module(mc_module_t *mc, const cg_module_t *module);
