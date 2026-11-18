#include "obj.h"

/* ELF64 relocatable object writer for x86-64. */

constexpr uint16_t ELF_EM_X86_64 = 62;
constexpr uint32_t ELF_R_X86_64_64 = 1;
constexpr uint32_t ELF_R_X86_64_PC32 = 2;
constexpr uint32_t ELF_R_X86_64_PLT32 = 4;

typedef struct elf_section {
    const char *name;
    uint32_t type;
    uint64_t flags;
    cg_buffer_t data;
    uint32_t link;
    uint32_t info;
    uint64_t align;
    uint64_t entsize;
    uint64_t size_override; /* NOBITS */
    uint32_t name_offset;
    uint64_t offset;
} elf_section_t;

static void shstr_add(cg_buffer_t *table, elf_section_t *section) {
    section->name_offset = (uint32_t)cg_buffer_size(table);
    cg_buffer_bytes(table, section->name, strlen(section->name) + 1);
}

static bool elf_write(const mc_module_t *mc, obj_machine_t machine,
                      cg_buffer_t *out, cg_diag_t *diag) {
    enum { S_NULL, S_TEXT, S_DATA, S_RODATA, S_BSS, S_NOTE, S_RELA_TEXT,
           S_RELA_DATA, S_RELA_RODATA, S_SYMTAB, S_STRTAB, S_SHSTRTAB, S_COUNT };
    elf_section_t sections[S_COUNT] = {};
    cg_buffer_t shstrtab = {};
    cg_buffer_t strtab = {};
    uint32_t *elf_index = nullptr;
    uint32_t symbol_count = 1;
    uint32_t first_global = 0;
    bool ok = false;
    static const mc_section_id_t mc_for[3] = {MC_TEXT, MC_DATA, MC_RODATA};
    if (machine != OBJ_MACHINE_X86_64) {
        cg_diag_error(diag, "elf: unsupported machine");
        return false;
    }
    sections[S_TEXT] = (elf_section_t){.name = ".text", .type = 1, .flags = 6,
                                       .align = mc->alignment[MC_TEXT]};
    sections[S_DATA] = (elf_section_t){.name = ".data", .type = 1, .flags = 3,
                                       .align = mc->alignment[MC_DATA]};
    sections[S_RODATA] = (elf_section_t){.name = ".rodata", .type = 1, .flags = 2,
                                         .align = mc->alignment[MC_RODATA]};
    sections[S_BSS] = (elf_section_t){.name = ".bss", .type = 8, .flags = 3,
                                      .align = mc->alignment[MC_BSS],
                                      .size_override = mc->bss_size};
    sections[S_NOTE] = (elf_section_t){.name = ".note.GNU-stack", .type = 1,
                                       .align = 1};
    sections[S_RELA_TEXT] = (elf_section_t){.name = ".rela.text", .type = 4,
                                            .flags = 0x40, .align = 8, .entsize = 24};
    sections[S_RELA_DATA] = (elf_section_t){.name = ".rela.data", .type = 4,
                                            .flags = 0x40, .align = 8, .entsize = 24};
    sections[S_RELA_RODATA] = (elf_section_t){.name = ".rela.rodata", .type = 4,
                                              .flags = 0x40, .align = 8,
                                              .entsize = 24};
    sections[S_SYMTAB] = (elf_section_t){.name = ".symtab", .type = 2, .align = 8,
                                         .entsize = 24};
    sections[S_STRTAB] = (elf_section_t){.name = ".strtab", .type = 3, .align = 1};
    sections[S_SHSTRTAB] = (elf_section_t){.name = ".shstrtab", .type = 3,
                                           .align = 1};
    for (uint32_t id = 0; id < 3; ++id) {
        cg_buffer_bytes(&sections[S_TEXT + id].data, mc->sections[mc_for[id]].bytes.data,
                        cg_buffer_size(&mc->sections[mc_for[id]]));
    }
    /* symbol order: null, locals, then globals and undefined */
    elf_index = calloc(mc->symbols.count + 1, sizeof(uint32_t));
    if (elf_index == nullptr) {
        goto done;
    }
    cg_buffer_u8(&strtab, 0);
    cg_buffer_zeros(&sections[S_SYMTAB].data, 24);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            first_global = symbol_count;
        }
        for (uint32_t index = 0; index < mc->symbols.count; ++index) {
            const mc_symbol_t *symbol = &mc->symbols.data[index];
            bool is_local = symbol->binding == CG_BIND_LOCAL &&
                            symbol->section != MC_UNDEFINED;
            const char *name;
            uint32_t name_offset;
            uint16_t section_index;
            uint8_t type;
            uint8_t bind;
            if (is_local != (pass == 0)) {
                continue;
            }
            name = mc_symbol_name(mc, index);
            name_offset = (uint32_t)cg_buffer_size(&strtab);
            cg_buffer_bytes(&strtab, name, strlen(name) + 1);
            section_index = symbol->section == MC_UNDEFINED ? 0
                            : symbol->section == MC_TEXT   ? S_TEXT
                            : symbol->section == MC_DATA   ? S_DATA
                            : symbol->section == MC_RODATA ? S_RODATA
                                                           : S_BSS;
            type = symbol->section == MC_UNDEFINED ? 0 : symbol->is_function ? 2 : 1;
            bind = symbol->binding == CG_BIND_LOCAL
                       ? (symbol->section == MC_UNDEFINED ? 1 : 0)
                   : symbol->binding == CG_BIND_WEAK ? 2
                                                     : 1;
            cg_buffer_u32(&sections[S_SYMTAB].data, name_offset);
            cg_buffer_u8(&sections[S_SYMTAB].data, (uint8_t)((bind << 4) | type));
            cg_buffer_u8(&sections[S_SYMTAB].data, 0);
            cg_buffer_u16(&sections[S_SYMTAB].data, section_index);
            cg_buffer_u64(&sections[S_SYMTAB].data,
                          symbol->section == MC_UNDEFINED ? 0 : symbol->offset);
            cg_buffer_u64(&sections[S_SYMTAB].data, symbol->size);
            elf_index[index] = symbol_count++;
        }
        if (pass == 0) {
            first_global = symbol_count;
        }
    }
    sections[S_SYMTAB].link = S_STRTAB;
    sections[S_SYMTAB].info = first_global;
    cg_buffer_bytes(&sections[S_STRTAB].data, strtab.bytes.data, cg_buffer_size(&strtab));
    for (uint32_t index = 0; index < mc->relocs.count; ++index) {
        const mc_reloc_t *reloc = &mc->relocs.data[index];
        uint32_t target = reloc->section == MC_TEXT   ? S_RELA_TEXT
                          : reloc->section == MC_DATA ? S_RELA_DATA
                                                      : S_RELA_RODATA;
        uint32_t type = reloc->kind == MC_RELOC_ABS64   ? ELF_R_X86_64_64
                        : reloc->kind == MC_RELOC_PLT32 ? ELF_R_X86_64_PLT32
                                                        : ELF_R_X86_64_PC32;
        cg_buffer_u64(&sections[target].data, reloc->offset);
        cg_buffer_u64(&sections[target].data,
                      ((uint64_t)elf_index[reloc->symbol] << 32) | type);
        cg_buffer_u64(&sections[target].data, (uint64_t)reloc->addend);
    }
    sections[S_RELA_TEXT].link = sections[S_RELA_DATA].link =
        sections[S_RELA_RODATA].link = S_SYMTAB;
    sections[S_RELA_TEXT].info = S_TEXT;
    sections[S_RELA_DATA].info = S_DATA;
    sections[S_RELA_RODATA].info = S_RODATA;
    cg_buffer_u8(&shstrtab, 0);
    for (uint32_t index = 1; index < S_COUNT; ++index) {
        shstr_add(&shstrtab, &sections[index]);
    }
    cg_buffer_bytes(&sections[S_SHSTRTAB].data, shstrtab.bytes.data,
                    cg_buffer_size(&shstrtab));
    /* layout */
    {
        uint64_t cursor = 64;
        for (uint32_t index = 1; index < S_COUNT; ++index) {
            elf_section_t *section = &sections[index];
            uint64_t align = section->align == 0 ? 1 : section->align;
            cursor = (cursor + align - 1) & ~(align - 1);
            section->offset = cursor;
            if (section->type != 8) {
                cursor += cg_buffer_size(&section->data);
            }
        }
        cursor = (cursor + 7) & ~(uint64_t)7;
        /* header */
        {
            static const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
            cg_buffer_bytes(out, ident, 16);
            cg_buffer_u16(out, 1);
            cg_buffer_u16(out, ELF_EM_X86_64);
            cg_buffer_u32(out, 1);
            cg_buffer_u64(out, 0);
            cg_buffer_u64(out, 0);
            cg_buffer_u64(out, cursor);
            cg_buffer_u32(out, 0);
            cg_buffer_u16(out, 64);
            cg_buffer_u16(out, 0);
            cg_buffer_u16(out, 0);
            cg_buffer_u16(out, 64);
            cg_buffer_u16(out, S_COUNT);
            cg_buffer_u16(out, S_SHSTRTAB);
        }
        for (uint32_t index = 1; index < S_COUNT; ++index) {
            elf_section_t *section = &sections[index];
            if (section->type == 8) {
                continue;
            }
            cg_buffer_zeros(out, (size_t)(section->offset - cg_buffer_size(out)));
            cg_buffer_bytes(out, section->data.bytes.data, cg_buffer_size(&section->data));
        }
        cg_buffer_zeros(out, (size_t)(cursor - cg_buffer_size(out)));
        cg_buffer_zeros(out, 64); /* null section header */
        for (uint32_t index = 1; index < S_COUNT; ++index) {
            const elf_section_t *section = &sections[index];
            cg_buffer_u32(out, section->name_offset);
            cg_buffer_u32(out, section->type);
            cg_buffer_u64(out, section->flags);
            cg_buffer_u64(out, 0);
            cg_buffer_u64(out, section->offset);
            cg_buffer_u64(out, section->type == 8 ? section->size_override
                                                  : cg_buffer_size(&section->data));
            cg_buffer_u32(out, section->link);
            cg_buffer_u32(out, section->info);
            cg_buffer_u64(out, section->align == 0 ? 1 : section->align);
            cg_buffer_u64(out, section->entsize);
        }
    }
    ok = !out->bytes.oom;
done:
    for (uint32_t index = 0; index < S_COUNT; ++index) {
        cg_buffer_free(&sections[index].data);
    }
    cg_buffer_free(&shstrtab);
    cg_buffer_free(&strtab);
    free(elf_index);
    return ok;
}

const obj_writer_t obj_writer_elf64 = {.name = "elf64", .write = elf_write};
