#include "mc.h"

#include <stdio.h>

mc_module_t *mc_create(void) {
    mc_module_t *mc = calloc(1, sizeof(*mc));
    if (mc != nullptr) {
        for (uint32_t index = 0; index < MC_SECTION_COUNT; ++index) {
            mc->alignment[index] = 1;
        }
        mc->alignment[MC_TEXT] = 16;
    }
    return mc;
}

void mc_destroy(mc_module_t *mc) {
    if (mc == nullptr) {
        return;
    }
    for (uint32_t index = 0; index < MC_SECTION_COUNT; ++index) {
        cg_buffer_free(&mc->sections[index]);
    }
    cg_vec_free(&mc->symbols);
    cg_vec_free(&mc->relocs);
    cg_strings_free(&mc->names);
    free(mc);
}

bool mc_ok(const mc_module_t *mc) {
    if (mc->symbols.oom || mc->relocs.oom || mc->names.items.oom) {
        return false;
    }
    for (uint32_t index = 0; index < MC_SECTION_COUNT; ++index) {
        if (mc->sections[index].bytes.oom) {
            return false;
        }
    }
    return true;
}

uint32_t mc_add_symbol(mc_module_t *mc, const char *name, cg_binding_t binding) {
    mc_symbol_t symbol = {.section = MC_UNDEFINED, .binding = binding};
    uint32_t id = mc->symbols.count;
    symbol.name = cg_strings_intern(&mc->names, name, strlen(name));
    if (symbol.name == CG_INVALID) {
        mc->names.items.oom = true;
        return CG_INVALID;
    }
    if (!cg_vec_push(&mc->symbols, symbol)) {
        return CG_INVALID;
    }
    return id;
}

void mc_define_symbol(mc_module_t *mc, uint32_t symbol, mc_section_id_t section,
                      uint32_t offset, uint32_t size, bool is_function) {
    mc_symbol_t *entry = &mc->symbols.data[symbol];
    entry->section = section;
    entry->offset = offset;
    entry->size = size;
    entry->is_function = is_function;
}

bool mc_add_reloc(mc_module_t *mc, mc_section_id_t section, uint32_t offset,
                  uint32_t symbol, mc_reloc_kind_t kind, int64_t addend) {
    mc_reloc_t reloc = {.section = section, .kind = kind, .offset = offset,
                        .symbol = symbol, .addend = addend};
    return cg_vec_push(&mc->relocs, reloc);
}

uint32_t mc_align_section(mc_module_t *mc, mc_section_id_t section,
                          uint32_t alignment) {
    if (alignment > mc->alignment[section]) {
        mc->alignment[section] = alignment;
    }
    if (section == MC_BSS) {
        mc->bss_size = cg_align_up_u32(mc->bss_size, alignment);
        return mc->bss_size;
    }
    cg_buffer_align(&mc->sections[section], alignment);
    return (uint32_t)cg_buffer_size(&mc->sections[section]);
}

uint32_t mc_add_constant(mc_module_t *mc, const void *bytes, uint32_t size,
                         uint32_t alignment) {
    char name[32];
    uint32_t symbol;
    uint32_t offset;
    snprintf(name, sizeof(name), ".LC%u", mc->constant_counter++);
    symbol = mc_add_symbol(mc, name, CG_BIND_LOCAL);
    if (symbol == CG_INVALID) {
        return CG_INVALID;
    }
    offset = mc_align_section(mc, MC_RODATA, alignment);
    cg_buffer_bytes(&mc->sections[MC_RODATA], bytes, size);
    mc_define_symbol(mc, symbol, MC_RODATA, offset, size, false);
    return symbol;
}

bool mc_import_module(mc_module_t *mc, const cg_module_t *module) {
    for (cg_symbol_t symbol = 0; symbol < module->symbols.count; ++symbol) {
        const cg_symbol_info_t *info = &module->symbols.data[symbol];
        uint32_t id = mc_add_symbol(mc, cg_symbol_name(module, symbol),
                                    info->binding);
        if (id != symbol) {
            return false;
        }
        mc->symbols.data[id].is_function = info->kind == CG_SYM_FUNC;
    }
    for (cg_symbol_t symbol = 0; symbol < module->symbols.count; ++symbol) {
        const cg_symbol_info_t *info = &module->symbols.data[symbol];
        mc_section_id_t section;
        uint32_t offset;
        if (info->kind != CG_SYM_DATA || !info->defined) {
            continue;
        }
        section = info->section == CG_DATA_RW   ? MC_DATA
                  : info->section == CG_DATA_RO ? MC_RODATA
                                                : MC_BSS;
        offset = mc_align_section(mc, section, info->align);
        if (section == MC_BSS) {
            mc->bss_size += info->size;
        } else {
            cg_buffer_bytes(&mc->sections[section],
                            module->data.bytes.data + info->data_offset,
                            info->size);
        }
        mc_define_symbol(mc, symbol, section, offset, info->size, false);
    }
    for (uint32_t index = 0; index < module->relocs.count; ++index) {
        const cg_data_reloc_t *reloc = &module->relocs.data[index];
        const mc_symbol_t *owner = &mc->symbols.data[reloc->owner];
        if (owner->section == MC_UNDEFINED || owner->section == MC_BSS) {
            return false;
        }
        mc_add_reloc(mc, (mc_section_id_t)owner->section,
                     owner->offset + reloc->offset, reloc->target,
                     MC_RELOC_ABS64, reloc->addend);
    }
    return mc_ok(mc);
}
