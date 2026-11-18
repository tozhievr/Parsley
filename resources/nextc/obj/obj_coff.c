#include "obj.h"

/* COFF (PE/COFF object) writer for AMD64. */

constexpr uint16_t COFF_MACHINE_AMD64 = 0x8664;
constexpr uint32_t COFF_SCN_CODE = 0x00000020;
constexpr uint32_t COFF_SCN_INITIALIZED = 0x00000040;
constexpr uint32_t COFF_SCN_UNINITIALIZED = 0x00000080;
constexpr uint32_t COFF_SCN_EXEC = 0x20000000;
constexpr uint32_t COFF_SCN_READ = 0x40000000;
constexpr uint32_t COFF_SCN_WRITE = 0x80000000;
constexpr uint16_t COFF_REL_AMD64_ADDR64 = 1;
constexpr uint16_t COFF_REL_AMD64_REL32 = 4;

static uint32_t coff_align_flag(uint32_t alignment) {
    uint32_t log2 = 0;
    while (((uint32_t)1 << log2) < alignment && log2 < 13) {
        ++log2;
    }
    return (log2 + 1) << 20;
}

typedef struct coff_section_info {
    const char *name;
    uint32_t flags;
    mc_section_id_t id;
} coff_section_info_t;

static bool coff_write(const mc_module_t *mc, obj_machine_t machine,
                       cg_buffer_t *out, cg_diag_t *diag) {
    static const coff_section_info_t layout[MC_SECTION_COUNT] = {
        {".text", COFF_SCN_CODE | COFF_SCN_EXEC | COFF_SCN_READ, MC_TEXT},
        {".data", COFF_SCN_INITIALIZED | COFF_SCN_READ | COFF_SCN_WRITE, MC_DATA},
        {".rdata", COFF_SCN_INITIALIZED | COFF_SCN_READ, MC_RODATA},
        {".bss", COFF_SCN_UNINITIALIZED | COFF_SCN_READ | COFF_SCN_WRITE, MC_BSS},
    };
    uint32_t section_number[MC_SECTION_COUNT] = {0};
    uint32_t present[MC_SECTION_COUNT];
    uint32_t present_count = 0;
    uint32_t reloc_count[MC_SECTION_COUNT] = {0};
    uint32_t raw_size[MC_SECTION_COUNT];
    uint32_t raw_offset[MC_SECTION_COUNT] = {0};
    uint32_t reloc_offset[MC_SECTION_COUNT] = {0};
    cg_buffer_t patched[MC_SECTION_COUNT] = {};
    cg_buffer_t string_table = {};
    uint32_t *name_offsets = nullptr;
    uint32_t cursor;
    bool ok = false;
    if (machine != OBJ_MACHINE_X86_64) {
        cg_diag_error(diag, "coff: unsupported machine");
        return false;
    }
    for (uint32_t index = 0; index < mc->relocs.count; ++index) {
        ++reloc_count[mc->relocs.data[index].section];
    }
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        raw_size[id] = id == MC_BSS ? mc->bss_size
                                    : (uint32_t)cg_buffer_size(&mc->sections[id]);
        if (raw_size[id] != 0 || id == MC_TEXT) {
            present[present_count++] = id;
            section_number[id] = present_count;
        }
        if (reloc_count[id] > 0xFFFF) {
            cg_diag_error(diag, "coff: too many relocations in one section");
            return false;
        }
    }
    /* patched copies of the section data (inline addends) */
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        if (id != MC_BSS) {
            cg_buffer_bytes(&patched[id], mc->sections[id].bytes.data, raw_size[id]);
        }
    }
    for (uint32_t index = 0; index < mc->relocs.count; ++index) {
        const mc_reloc_t *reloc = &mc->relocs.data[index];
        uint8_t *field = patched[reloc->section].bytes.data + reloc->offset;
        if (reloc->kind == MC_RELOC_ABS64) {
            uint64_t value = (uint64_t)reloc->addend;
            memcpy(field, &value, 8);
        } else {
            int64_t inline_value = reloc->addend + 4;
            int32_t narrow = (int32_t)inline_value;
            if (inline_value != narrow) {
                cg_diag_error(diag, "coff: relocation addend out of range");
                goto done;
            }
            memcpy(field, &narrow, 4);
        }
    }
    /* string table: long symbol names */
    name_offsets = calloc(mc->symbols.count + 1, sizeof(uint32_t));
    if (name_offsets == nullptr) {
        goto done;
    }
    cg_buffer_u32(&string_table, 0); /* size, patched below */
    for (uint32_t index = 0; index < mc->symbols.count; ++index) {
        const char *name = mc_symbol_name(mc, index);
        size_t length = strlen(name);
        if (length > 8) {
            name_offsets[index] = (uint32_t)cg_buffer_size(&string_table);
            cg_buffer_bytes(&string_table, name, length + 1);
        }
    }
    cg_buffer_patch32(&string_table, 0, (uint32_t)cg_buffer_size(&string_table));
    /* file layout */
    cursor = 20 + 40 * present_count;
    for (uint32_t slot = 0; slot < present_count; ++slot) {
        uint32_t id = present[slot];
        if (id != MC_BSS && raw_size[id] != 0) {
            cursor = cg_align_up_u32(cursor, 4);
            raw_offset[id] = cursor;
            cursor += raw_size[id];
        }
    }
    for (uint32_t slot = 0; slot < present_count; ++slot) {
        uint32_t id = present[slot];
        if (reloc_count[id] != 0) {
            cursor = cg_align_up_u32(cursor, 4);
            reloc_offset[id] = cursor;
            cursor += reloc_count[id] * 10;
        }
    }
    cursor = cg_align_up_u32(cursor, 4);
    /* header */
    cg_buffer_u16(out, COFF_MACHINE_AMD64);
    cg_buffer_u16(out, (uint16_t)present_count);
    cg_buffer_u32(out, 0);
    cg_buffer_u32(out, cursor);
    cg_buffer_u32(out, mc->symbols.count);
    cg_buffer_u16(out, 0);
    cg_buffer_u16(out, 0);
    for (uint32_t slot = 0; slot < present_count; ++slot) {
        uint32_t id = present[slot];
        const coff_section_info_t *info = &layout[id];
        char name[8] = {0};
        memcpy(name, info->name, strlen(info->name));
        cg_buffer_bytes(out, name, 8);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, raw_size[id]);
        cg_buffer_u32(out, raw_offset[id]);
        cg_buffer_u32(out, reloc_offset[id]);
        cg_buffer_u32(out, 0);
        cg_buffer_u16(out, (uint16_t)reloc_count[id]);
        cg_buffer_u16(out, 0);
        cg_buffer_u32(out, info->flags | coff_align_flag(mc->alignment[id]));
    }
    for (uint32_t slot = 0; slot < present_count; ++slot) {
        uint32_t id = present[slot];
        if (id != MC_BSS && raw_size[id] != 0) {
            cg_buffer_align(out, 4);
            cg_buffer_bytes(out, patched[id].bytes.data, raw_size[id]);
        }
    }
    for (uint32_t slot = 0; slot < present_count; ++slot) {
        uint32_t id = present[slot];
        if (reloc_count[id] == 0) {
            continue;
        }
        cg_buffer_align(out, 4);
        for (uint32_t index = 0; index < mc->relocs.count; ++index) {
            const mc_reloc_t *reloc = &mc->relocs.data[index];
            if (reloc->section != id) {
                continue;
            }
            cg_buffer_u32(out, reloc->offset);
            cg_buffer_u32(out, reloc->symbol);
            cg_buffer_u16(out, reloc->kind == MC_RELOC_ABS64 ? COFF_REL_AMD64_ADDR64
                                                             : COFF_REL_AMD64_REL32);
        }
    }
    cg_buffer_align(out, 4);
    for (uint32_t index = 0; index < mc->symbols.count; ++index) {
        const mc_symbol_t *symbol = &mc->symbols.data[index];
        const char *name = mc_symbol_name(mc, index);
        size_t length = strlen(name);
        bool defined = symbol->section != MC_UNDEFINED;
        char short_name[8] = {0};
        if (length <= 8) {
            memcpy(short_name, name, length);
            cg_buffer_bytes(out, short_name, 8);
        } else {
            cg_buffer_u32(out, 0);
            cg_buffer_u32(out, name_offsets[index]);
        }
        cg_buffer_u32(out, defined ? symbol->offset : 0);
        cg_buffer_u16(out, (uint16_t)(defined ? section_number[symbol->section] : 0));
        cg_buffer_u16(out, symbol->is_function ? 0x20 : 0);
        cg_buffer_u8(out, symbol->binding == CG_BIND_LOCAL ? 3 : 2);
        cg_buffer_u8(out, 0);
    }
    cg_buffer_bytes(out, string_table.bytes.data, cg_buffer_size(&string_table));
    ok = !out->bytes.oom;
done:
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        cg_buffer_free(&patched[id]);
    }
    cg_buffer_free(&string_table);
    free(name_offsets);
    return ok;
}

const obj_writer_t obj_writer_coff = {.name = "coff", .write = coff_write};
