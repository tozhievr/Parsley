#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static linker + PE/flat packers. See link.h. */

typedef struct ldef {
    uint32_t module;
    uint32_t symbol;
    bool weak;
    bool valid;
} ldef_t;

typedef struct import_fn {
    uint32_t name; /* in the link name table */
    uint32_t dll;  /* index into options->dlls */
    uint32_t stub; /* offset of the jmp stub inside the stub area */
    uint32_t iat;  /* offset of the IAT slot inside the import blob */
} import_fn_t;

typedef struct linker {
    const mc_module_t *const *modules;
    uint32_t count;
    const link_options_t *options;
    cg_diag_t *diag;
    cg_strings_t names;
    CG_VEC(ldef_t) defs;                 /* by interned name id */
    uint32_t (*module_offset)[MC_SECTION_COUNT];
    uint32_t merged_size[MC_SECTION_COUNT];
    uint32_t merged_align[MC_SECTION_COUNT];
    cg_buffer_t merged[MC_SECTION_COUNT];
    uint64_t rva[MC_SECTION_COUNT];      /* start of each merged section */
    CG_VEC(import_fn_t) imports;
    uint32_t *import_of;                 /* name id -> import index or CG_INVALID */
    uint32_t import_cap;
    uint32_t stub_area;                  /* offset of the stubs inside .text */
    uint32_t start_stub;                 /* offset of the start-up code in .text */
    uint32_t start_stub_size;
    uint32_t flat_header;                /* bytes of the flat entry jump */
    uint32_t import_blob_rva;            /* rva of the import blob (IAT slots are inside) */
} linker_t;

static void linker_free(linker_t *l) {
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        cg_buffer_free(&l->merged[id]);
    }
    cg_vec_free(&l->defs);
    cg_vec_free(&l->imports);
    cg_strings_free(&l->names);
    free(l->module_offset);
    free(l->import_of);
}

static uint32_t name_id(linker_t *l, const mc_module_t *mc, uint32_t symbol) {
    const char *text = mc_symbol_name(mc, symbol);
    return cg_strings_intern(&l->names, text, strlen(text));
}

/* ---- PE export tables (to find the DLL behind an import) ---------------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;
    if (file == nullptr) {
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return nullptr;
    }
    data = malloc((size_t)length);
    if (data != nullptr && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = nullptr;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static uint32_t pe_rva_to_offset(const uint8_t *image, size_t size, uint32_t rva) {
    uint32_t pe = rd32(image + 0x3C);
    uint32_t sections = rd16(image + pe + 6);
    uint32_t optional = rd16(image + pe + 20);
    const uint8_t *table = image + pe + 24 + optional;
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t *s = table + i * 40;
        uint32_t virtual_address = rd32(s + 12);
        uint32_t span = rd32(s + 8) > rd32(s + 16) ? rd32(s + 8) : rd32(s + 16);
        if ((size_t)(s - image) + 40 > size) {
            break;
        }
        if (rva >= virtual_address && rva < virtual_address + span) {
            return rd32(s + 20) + (rva - virtual_address);
        }
    }
    return CG_INVALID;
}

/* Loads the export names of a DLL; returns false when it cannot be read. */
typedef struct export_list {
    uint8_t *image;
    size_t size;
    uint32_t count;
    uint32_t names_offset;
} export_list_t;

static bool exports_open(export_list_t *list, const char *directory, const char *dll) {
    char path[1024];
    uint32_t pe, rva, offset;
    const uint8_t *dir;
    memset(list, 0, sizeof(*list));
    if (dll[0] == '/' || dll[0] == '\\' || (dll[0] != '\0' && dll[1] == ':')) {
        snprintf(path, sizeof(path), "%s", dll);
    } else {
        snprintf(path, sizeof(path), "%s\\%s", directory, dll);
    }
    list->image = read_file(path, &list->size);
    if (list->image == nullptr || list->size < 0x200 || list->image[0] != 'M') {
        free(list->image);
        list->image = nullptr;
        return false;
    }
    pe = rd32(list->image + 0x3C);
    if (pe + 24 + 112 + 8 > list->size || rd16(list->image + pe + 24) != 0x20B) {
        free(list->image);
        list->image = nullptr;
        return false;
    }
    rva = rd32(list->image + pe + 24 + 112);
    offset = rva != 0 ? pe_rva_to_offset(list->image, list->size, rva) : CG_INVALID;
    if (offset == CG_INVALID || offset + 40 > list->size) {
        free(list->image);
        list->image = nullptr;
        return false;
    }
    dir = list->image + offset;
    list->count = rd32(dir + 24);
    list->names_offset = pe_rva_to_offset(list->image, list->size, rd32(dir + 32));
    if (list->names_offset == CG_INVALID) {
        free(list->image);
        list->image = nullptr;
        return false;
    }
    return true;
}

static bool exports_has(const export_list_t *list, const char *name) {
    for (uint32_t i = 0; i < list->count; ++i) {
        uint32_t name_rva, name_offset;
        if (list->names_offset + (size_t)i * 4 + 4 > list->size) {
            return false;
        }
        name_rva = rd32(list->image + list->names_offset + i * 4);
        name_offset = pe_rva_to_offset(list->image, list->size, name_rva);
        if (name_offset != CG_INVALID && name_offset < list->size &&
            strcmp((const char *)list->image + name_offset, name) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- merging --------------------------------------------------------------- */

static bool merge_sections(linker_t *l) {
    l->module_offset = calloc(l->count + 1, sizeof(*l->module_offset));
    if (l->module_offset == nullptr) {
        return false;
    }
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        l->merged_align[id] = 1;
    }
    for (uint32_t m = 0; m < l->count; ++m) {
        const mc_module_t *mc = l->modules[m];
        for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
            uint32_t alignment = mc->alignment[id] != 0 ? mc->alignment[id] : 1;
            uint32_t size = id == MC_BSS ? mc->bss_size
                                         : (uint32_t)cg_buffer_size(&mc->sections[id]);
            uint32_t offset = cg_align_up_u32(l->merged_size[id], alignment);
            if (alignment > l->merged_align[id]) {
                l->merged_align[id] = alignment;
            }
            l->module_offset[m][id] = offset;
            if (id != MC_BSS) {
                cg_buffer_zeros(&l->merged[id], offset - l->merged[id].bytes.count);
                cg_buffer_bytes(&l->merged[id], mc->sections[id].bytes.data, size);
            }
            l->merged_size[id] = offset + size;
        }
    }
    return true;
}

static bool collect_definitions(linker_t *l) {
    for (uint32_t m = 0; m < l->count; ++m) {
        const mc_module_t *mc = l->modules[m];
        for (uint32_t s = 0; s < mc->symbols.count; ++s) {
            const mc_symbol_t *symbol = &mc->symbols.data[s];
            uint32_t id;
            ldef_t *def;
            if (symbol->section == MC_UNDEFINED || symbol->binding == CG_BIND_LOCAL) {
                continue;
            }
            id = name_id(l, mc, s);
            if (id == CG_INVALID) {
                return false;
            }
            while (l->defs.count <= id) {
                ldef_t empty = {0};
                if (!cg_vec_push(&l->defs, empty)) {
                    return false;
                }
            }
            def = &l->defs.data[id];
            if (def->valid && !def->weak && symbol->binding != CG_BIND_WEAK) {
                cg_diag_error(l->diag, "link: duplicate definition of '%s'",
                              mc_symbol_name(mc, s));
                return false;
            }
            if (!def->valid || (def->weak && symbol->binding != CG_BIND_WEAK)) {
                *def = (ldef_t){m, s, symbol->binding == CG_BIND_WEAK, true};
            }
        }
    }
    return true;
}

/* Resolves a symbol of module `m` to a defining (module, symbol) pair. */
static bool resolve(linker_t *l, uint32_t m, uint32_t s, uint32_t *dm, uint32_t *ds) {
    const mc_module_t *mc = l->modules[m];
    const mc_symbol_t *symbol = &mc->symbols.data[s];
    uint32_t id;
    if (symbol->section != MC_UNDEFINED) {
        *dm = m;
        *ds = s;
        return true;
    }
    id = name_id(l, mc, s);
    if (id != CG_INVALID && id < l->defs.count && l->defs.data[id].valid) {
        *dm = l->defs.data[id].module;
        *ds = l->defs.data[id].symbol;
        return true;
    }
    return false;
}

static const mc_symbol_t *def_symbol(const linker_t *l, uint32_t m, uint32_t s) {
    return &l->modules[m]->symbols.data[s];
}

/* Address (merged section rva + offset) of a defined symbol. */
static uint64_t symbol_rva(const linker_t *l, uint32_t m, uint32_t s) {
    const mc_symbol_t *symbol = def_symbol(l, m, s);
    if (symbol->section == MC_TLS) {
        /* the markers delimit the whole merged thread-local template */
        const char *name = mc_symbol_name(l->modules[m], s);
        if (strcmp(name, "_tls_start") == 0) {
            return l->rva[MC_TLS];
        }
        if (strcmp(name, "_tls_end") == 0) {
            return l->rva[MC_TLS] + l->merged_size[MC_TLS];
        }
    }
    return l->rva[symbol->section] + l->module_offset[m][symbol->section] + symbol->offset;
}

/* ---- PE -------------------------------------------------------------------- */

constexpr uint32_t PE_SECTION_ALIGN = 0x1000;
constexpr uint32_t PE_FILE_ALIGN = 0x200;
constexpr uint32_t PE_HEADER_SIZE = 0x200;
constexpr uint32_t STUB_SIZE = 8; /* jmp [rip+rel32] + padding */

static void out_pad_to(cg_buffer_t *out, size_t size) {
    if (cg_buffer_size(out) < size) {
        cg_buffer_zeros(out, size - cg_buffer_size(out));
    }
}

/* Finds the imports: unresolved symbols that some DLL exports. */
static bool collect_imports(linker_t *l, bool want_exit_process) {
    export_list_t *lists = nullptr;
    const link_options_t *o = l->options;
    char directory[512];
    const char *root = getenv("SystemRoot");
    bool ok = false;
    snprintf(directory, sizeof(directory), "%s", o->dll_directory != nullptr && o->dll_directory[0] != '\0'
                                                     ? o->dll_directory : "");
    if (directory[0] == '\0') {
        snprintf(directory, sizeof(directory), "%s\\System32", root != nullptr ? root : "C:\\Windows");
    }
    lists = calloc(o->dll_count + 1, sizeof(export_list_t));
    l->import_cap = l->names.items.count + 16;
    for (uint32_t m = 0; m < l->count; ++m) {
        l->import_cap += l->modules[m]->symbols.count;
    }
    l->import_of = malloc(l->import_cap * sizeof(uint32_t));
    if (lists == nullptr || l->import_of == nullptr) {
        goto done;
    }
    memset(l->import_of, 0xFF, l->import_cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < o->dll_count; ++i) {
        (void)exports_open(&lists[i], directory, o->dlls[i]);
    }
    {
        uint32_t extra = want_exit_process ? 1 : 0;
        for (uint32_t pass = 0; pass < l->count + extra; ++pass) {
            const mc_module_t *mc = pass < l->count ? l->modules[pass] : nullptr;
            uint32_t symbols = mc != nullptr ? mc->symbols.count : 1;
            for (uint32_t s = 0; s < symbols; ++s) {
                const char *text = mc != nullptr ? mc_symbol_name(mc, s) : "ExitProcess";
                uint32_t id, dm, ds;
                if (strncmp(text, "__imp_", 6) == 0) {
                    text += 6; /* a use of the IAT slot itself */
                }
                import_fn_t fn = {0};
                if (mc != nullptr) {
                    if (mc->symbols.data[s].section != MC_UNDEFINED || resolve(l, pass, s, &dm, &ds)) {
                        continue;
                    }
                }
                id = cg_strings_intern(&l->names, text, strlen(text));
                if (id == CG_INVALID) {
                    goto done;
                }
                if (id < l->defs.count && l->defs.data[id].valid) {
                    continue;
                }
                if (id >= l->import_cap) {
                    goto done;
                }
                if (l->import_of[id] != CG_INVALID) {
                    continue;
                }
                fn.dll = CG_INVALID;
                for (uint32_t i = 0; i < o->hint_count; ++i) {
                    if (strcmp(o->hint_names[i], text) == 0) {
                        fn.dll = o->hint_dlls[i];
                        break;
                    }
                }
                for (uint32_t i = 0; fn.dll == CG_INVALID && i < o->dll_count; ++i) {
                    if (lists[i].image != nullptr && exports_has(&lists[i], text)) {
                        fn.dll = i;
                        break;
                    }
                }
                if (fn.dll == CG_INVALID) {
                    continue; /* reported by the relocation pass */
                }
                fn.name = id;
                l->import_of[id] = l->imports.count;
                cg_vec_push(&l->imports, fn);
            }
        }
    }
    ok = true;
done:
    if (lists != nullptr) {
        for (uint32_t i = 0; i < o->dll_count; ++i) {
            free(lists[i].image);
        }
    }
    free(lists);
    return ok && !l->imports.oom;
}

static uint32_t import_for(const linker_t *l, uint32_t id) {
    return l->import_of != nullptr && id < l->import_cap ? l->import_of[id] : CG_INVALID;
}

static int import_order(const void *a, const void *b) {
    const import_fn_t *x = a, *y = b;
    return x->dll != y->dll ? (x->dll < y->dll ? -1 : 1) : (x->name < y->name ? -1 : x->name > y->name);
}

/* Builds the import blob (descriptors, ILT/IAT, names) for a given rva. */
static bool build_imports(linker_t *l, uint32_t blob_rva, cg_buffer_t *blob,
                          uint32_t *iat_start, uint32_t *iat_size, uint32_t *dir_size) {
    const link_options_t *o = l->options;
    uint32_t dll_used[64] = {0};
    uint32_t dll_order[64];
    uint32_t used = 0;
    uint32_t cursor;
    uint32_t desc_count;
    uint32_t hint_offset;
    uint32_t *ilt_start;
    uint32_t *name_start;
    if (l->imports.count == 0) {
        *iat_start = *iat_size = *dir_size = 0;
        return true;
    }
    qsort(l->imports.data, l->imports.count, sizeof(import_fn_t), import_order);
    for (uint32_t i = 0; i < l->imports.count; ++i) {
        uint32_t d = l->imports.data[i].dll;
        l->import_of[l->imports.data[i].name] = i;
        if (d < 64 && !dll_used[d]) {
            dll_used[d] = 1;
            dll_order[used++] = d;
        }
    }
    desc_count = used + 1;
    /* layout: descriptors | (ILT | IAT per dll) | hint/name | dll names */
    ilt_start = calloc(used + 1, sizeof(uint32_t));
    name_start = calloc(l->imports.count + 1, sizeof(uint32_t));
    if (ilt_start == nullptr || name_start == nullptr) {
        free(ilt_start);
        free(name_start);
        return false;
    }
    cursor = desc_count * 20;
    for (uint32_t k = 0; k < used; ++k) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < l->imports.count; ++i) {
            n += l->imports.data[i].dll == dll_order[k];
        }
        ilt_start[k] = cursor;
        cursor += (n + 1) * 8 * 2;
    }
    hint_offset = cursor;
    for (uint32_t i = 0; i < l->imports.count; ++i) {
        const char *text = cg_strings_get(&l->names, l->imports.data[i].name);
        name_start[i] = cursor;
        cursor += 2 + (uint32_t)strlen(text) + 1;
        cursor = cg_align_up_u32(cursor, 2);
    }
    (void)hint_offset;
    {
        uint32_t *dll_name = calloc(used + 1, sizeof(uint32_t));
        uint32_t total;
        if (dll_name == nullptr) {
            free(ilt_start);
            free(name_start);
            return false;
        }
        for (uint32_t k = 0; k < used; ++k) {
            dll_name[k] = cursor;
            cursor += (uint32_t)strlen(o->dlls[dll_order[k]]) + 1;
        }
        total = cursor;
        cg_buffer_zeros(blob, total);
        {
            uint8_t *b = blob->bytes.data;
            uint32_t first_iat = CG_INVALID, last_end = 0;
            for (uint32_t k = 0; k < used; ++k) {
                uint32_t n = 0;
                uint32_t ilt = ilt_start[k];
                uint32_t slot = 0;
                for (uint32_t i = 0; i < l->imports.count; ++i) {
                    n += l->imports.data[i].dll == dll_order[k];
                }
                {
                    uint32_t iat = ilt + (n + 1) * 8;
                    uint32_t v[5] = {blob_rva + ilt, 0, 0, blob_rva + dll_name[k], blob_rva + iat};
                    memcpy(b + k * 20, v, 20);
                    if (first_iat == CG_INVALID) {
                        first_iat = iat;
                    }
                    last_end = iat + (n + 1) * 8;
                    for (uint32_t i = 0; i < l->imports.count; ++i) {
                        if (l->imports.data[i].dll != dll_order[k]) {
                            continue;
                        }
                        {
                            uint64_t entry = blob_rva + name_start[i];
                            memcpy(b + ilt + slot * 8, &entry, 8);
                            memcpy(b + iat + slot * 8, &entry, 8);
                        }
                        l->imports.data[i].iat = iat + slot * 8;
                        ++slot;
                    }
                }
            }
            for (uint32_t i = 0; i < l->imports.count; ++i) {
                const char *text = cg_strings_get(&l->names, l->imports.data[i].name);
                memcpy(b + name_start[i] + 2, text, strlen(text));
            }
            for (uint32_t k = 0; k < used; ++k) {
                memcpy(b + dll_name[k], o->dlls[dll_order[k]], strlen(o->dlls[dll_order[k]]));
            }
            *iat_start = blob_rva + first_iat;
            *iat_size = last_end - first_iat;
            *dir_size = desc_count * 20;
        }
        free(dll_name);
    }
    free(ilt_start);
    free(name_start);
    return !blob->bytes.oom;
}

static void put32(uint8_t *where, uint32_t v) { memcpy(where, &v, 4); }

/* Applies all relocations; `import_address` maps an import to its stub rva. */
static bool apply_relocs(linker_t *l, uint64_t base) {
    for (uint32_t m = 0; m < l->count; ++m) {
        const mc_module_t *mc = l->modules[m];
        for (uint32_t r = 0; r < mc->relocs.count; ++r) {
            const mc_reloc_t *reloc = &mc->relocs.data[r];
            uint32_t dm, ds;
            uint64_t target;
            uint8_t *place = l->merged[reloc->section].bytes.data +
                             l->module_offset[m][reloc->section] + reloc->offset;
            uint64_t place_rva = l->rva[reloc->section] + l->module_offset[m][reloc->section] + reloc->offset;
            if (resolve(l, m, reloc->symbol, &dm, &ds)) {
                target = symbol_rva(l, dm, ds);
                if (reloc->kind == MC_RELOC_SECREL) {
                    target -= l->rva[def_symbol(l, dm, ds)->section];
                }
            } else {
                const char *text = mc_symbol_name(mc, reloc->symbol);
                bool via_slot = strncmp(text, "__imp_", 6) == 0;
                uint32_t id = via_slot ? (uint32_t)cg_strings_intern(&l->names, text + 6, strlen(text + 6))
                                       : name_id(l, mc, reloc->symbol);
                uint32_t imp = id != CG_INVALID ? import_for(l, id) : CG_INVALID;
                if (imp == CG_INVALID) {
                    cg_diag_error(l->diag, "link: undefined symbol '%s'", text);
                    return false;
                }
                target = via_slot ? l->import_blob_rva + l->imports.data[imp].iat
                                  : l->rva[MC_TEXT] + l->stub_area + imp * STUB_SIZE;
            }
            if (reloc->kind == MC_RELOC_SECREL) {
                put32(place, (uint32_t)(int32_t)((int64_t)target + reloc->addend));
            } else if (reloc->kind == MC_RELOC_ABS64) {
                uint64_t value = base + target + (uint64_t)reloc->addend;
                memcpy(place, &value, 8);
            } else {
                int64_t delta = (int64_t)(target + (uint64_t)reloc->addend) - (int64_t)place_rva;
                if (delta != (int32_t)delta) {
                    cg_diag_error(l->diag, "link: relocation to '%s' out of range",
                                  mc_symbol_name(mc, reloc->symbol));
                    return false;
                }
                put32(place, (uint32_t)(int32_t)delta);
            }
        }
    }
    return true;
}

static bool find_entry(linker_t *l, const char *name, uint32_t *dm, uint32_t *ds) {
    uint32_t id = cg_strings_intern(&l->names, name, strlen(name));
    if (id == CG_INVALID || id >= l->defs.count || !l->defs.data[id].valid) {
        return false;
    }
    *dm = l->defs.data[id].module;
    *ds = l->defs.data[id].symbol;
    return true;
}

static bool link_pe(linker_t *l, cg_buffer_t *out) {
    const link_options_t *o = l->options;
    const char *entry = o->entry != nullptr ? o->entry : "main";
    uint64_t base = o->image_base != 0 ? o->image_base : 0x140000000ull;
    bool wrap = strcmp(entry, "main") == 0;
    uint32_t em, es;
    uint32_t text_size, rdata_size, data_size, bss_at, imports_at;
    uint32_t text_va = PE_SECTION_ALIGN, rdata_va, data_va, image_size;
    uint32_t iat_rva = 0, iat_size = 0, dir_size = 0;
    uint32_t text_raw, rdata_raw, data_raw;
    uint32_t tls_at, data_file;
    uint32_t tls_dir_rva = 0;
    cg_buffer_t datablob = {0};
    cg_buffer_t blob = {0};
    cg_buffer_t text = {0};
    bool ok = false;
    if (!find_entry(l, entry, &em, &es)) {
        cg_diag_error(l->diag, "link: entry symbol '%s' is not defined", entry);
        return false;
    }
    if (!collect_imports(l, wrap)) {
        cg_diag_error(l->diag, "link: out of memory");
        return false;
    }
    /* stubs live after the merged text */
    l->stub_area = cg_align_up_u32(l->merged_size[MC_TEXT], 16);
    {
        uint32_t stubs = l->imports.count * STUB_SIZE;
        l->start_stub = cg_align_up_u32(l->stub_area + stubs, 16);
        l->start_stub_size = wrap ? 32 : 0;
        text_size = l->start_stub + l->start_stub_size;
    }
    rdata_va = text_va + cg_align_up_u32(text_size, PE_SECTION_ALIGN);
    imports_at = cg_align_up_u32(l->merged_size[MC_RODATA], 16);
    /* the import blob size does not depend on the rva: build once to size it */
    if (!build_imports(l, rdata_va + imports_at, &blob, &iat_rva, &iat_size, &dir_size)) {
        goto done;
    }
    rdata_size = imports_at + (uint32_t)cg_buffer_size(&blob);
    data_va = rdata_va + cg_align_up_u32(rdata_size ? rdata_size : 1, PE_SECTION_ALIGN);
    tls_at = cg_align_up_u32(l->merged_size[MC_DATA], l->merged_align[MC_TLS]);
    data_file = l->merged_size[MC_TLS] != 0 ? tls_at + l->merged_size[MC_TLS]
                                            : l->merged_size[MC_DATA];
    bss_at = cg_align_up_u32(data_file, l->merged_align[MC_BSS]);
    data_size = bss_at + l->merged_size[MC_BSS];
    l->rva[MC_TEXT] = text_va;
    l->rva[MC_RODATA] = rdata_va;
    l->rva[MC_DATA] = data_va;
    l->rva[MC_TLS] = data_va + tls_at;
    l->rva[MC_BSS] = data_va + bss_at;
    l->import_blob_rva = rdata_va + imports_at;
    image_size = data_va + cg_align_up_u32(data_size ? data_size : 1, PE_SECTION_ALIGN);
    /* stubs and start-up code */
    cg_buffer_bytes(&text, l->merged[MC_TEXT].bytes.data, l->merged_size[MC_TEXT]);
    cg_buffer_zeros(&text, text_size - cg_buffer_size(&text));
    for (uint32_t i = 0; i < l->imports.count; ++i) {
        uint8_t *stub = text.bytes.data + l->stub_area + i * STUB_SIZE;
        uint32_t slot_rva = rdata_va + imports_at + l->imports.data[i].iat;
        uint32_t stub_rva = text_va + l->stub_area + i * STUB_SIZE;
        stub[0] = 0xFF;
        stub[1] = 0x25;
        put32(stub + 2, slot_rva - (stub_rva + 6));
        stub[6] = stub[7] = 0xCC;
    }
    if (wrap) {
        uint8_t *code = text.bytes.data + l->start_stub;
        uint32_t code_rva = text_va + l->start_stub;
        uint32_t main_rva = (uint32_t)symbol_rva(l, em, es);
        uint32_t exit_id = cg_strings_intern(&l->names, "ExitProcess", 11);
        uint32_t exit_import = import_for(l, exit_id);
        if (exit_import == CG_INVALID) {
            cg_diag_error(l->diag, "link: ExitProcess not found in the import libraries");
            goto done;
        }
        code[0] = 0x48; code[1] = 0x83; code[2] = 0xEC; code[3] = 0x28; /* sub rsp, 40 */
        code[4] = 0xE8;                                                   /* call main */
        put32(code + 5, main_rva - (code_rva + 9));
        code[9] = 0x89; code[10] = 0xC1;                                  /* mov ecx, eax */
        code[11] = 0xFF; code[12] = 0x15;                                 /* call [ExitProcess] */
        put32(code + 13, rdata_va + imports_at + l->imports.data[exit_import].iat - (code_rva + 17));
        code[17] = 0xCC;
    }
    /* merged sections need room before applying relocations */
    if (!apply_relocs(l, base)) {
        goto done;
    }
    /* relocations patched l->merged; refresh the text copy */
    memcpy(text.bytes.data, l->merged[MC_TEXT].bytes.data, l->merged_size[MC_TEXT]);
    text_raw = cg_align_up_u32(text_size, PE_FILE_ALIGN);
    rdata_raw = cg_align_up_u32(rdata_size, PE_FILE_ALIGN);
    data_raw = cg_align_up_u32(data_file, PE_FILE_ALIGN);
    {
        uint32_t tm, ts;
        cg_buffer_bytes(&datablob, l->merged[MC_DATA].bytes.data, l->merged_size[MC_DATA]);
        if (l->merged_size[MC_TLS] != 0) {
            cg_buffer_zeros(&datablob, tls_at - l->merged_size[MC_DATA]);
            cg_buffer_bytes(&datablob, l->merged[MC_TLS].bytes.data, l->merged_size[MC_TLS]);
            if (find_entry(l, "_tls_used", &tm, &ts)) {
                tls_dir_rva = (uint32_t)symbol_rva(l, tm, ts);
            }
        }
    }
    {
        uint32_t entry_rva = wrap ? text_va + l->start_stub : (uint32_t)symbol_rva(l, em, es);
        uint32_t sections = 1 + (rdata_size != 0) + (data_size != 0);
        uint32_t raw_text = PE_HEADER_SIZE;
        uint32_t raw_rdata = raw_text + text_raw;
        uint32_t raw_data = raw_rdata + rdata_raw;
        uint8_t dos[64] = {'M', 'Z'};
        put32(dos + 0x3C, 0x40);
        cg_buffer_bytes(out, dos, 64);
        cg_buffer_bytes(out, "PE\0\0", 4);
        cg_buffer_u16(out, 0x8664);
        cg_buffer_u16(out, (uint16_t)sections);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, 0);
        cg_buffer_u16(out, 240);
        cg_buffer_u16(out, 0x0022);
        cg_buffer_u16(out, 0x20B);
        cg_buffer_u8(out, 14);
        cg_buffer_u8(out, 0);
        cg_buffer_u32(out, text_raw);
        cg_buffer_u32(out, rdata_raw + data_raw);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, entry_rva);
        cg_buffer_u32(out, text_va);
        cg_buffer_u64(out, base);
        cg_buffer_u32(out, PE_SECTION_ALIGN);
        cg_buffer_u32(out, PE_FILE_ALIGN);
        cg_buffer_u16(out, 6); cg_buffer_u16(out, 0);
        cg_buffer_u16(out, 0); cg_buffer_u16(out, 0);
        cg_buffer_u16(out, 6); cg_buffer_u16(out, 0);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, image_size);
        cg_buffer_u32(out, PE_HEADER_SIZE);
        cg_buffer_u32(out, 0);
        cg_buffer_u16(out, o->gui_subsystem ? 2 : 3);
        cg_buffer_u16(out, 0x8100);
        cg_buffer_u64(out, o->stack_reserve != 0 ? o->stack_reserve : 0x100000);
        cg_buffer_u64(out, 0x1000);
        cg_buffer_u64(out, 0x100000);
        cg_buffer_u64(out, 0x1000);
        cg_buffer_u32(out, 0);
        cg_buffer_u32(out, 16);
        for (uint32_t d = 0; d < 16; ++d) {
            uint32_t rva = 0, size = 0;
            if (d == 1 && dir_size != 0) {
                rva = rdata_va + imports_at;
                size = dir_size;
            } else if (d == 9 && tls_dir_rva != 0) {
                rva = tls_dir_rva;
                size = 0x28;
            } else if (d == 12 && iat_size != 0) {
                rva = iat_rva;
                size = iat_size;
            }
            cg_buffer_u32(out, rva);
            cg_buffer_u32(out, size);
        }
        {
            struct { const char *name; uint32_t vsize, va, raw_size, raw, flags; } hdr[3] = {
                {".text", text_size, text_va, text_raw, raw_text, 0x60000020},
                {".rdata", rdata_size, rdata_va, rdata_raw, raw_rdata, 0x40000040},
                {".data", data_size, data_va, data_raw, raw_data, 0xC0000040},
            };
            for (uint32_t i = 0; i < 3; ++i) {
                char name[8] = {0};
                if ((i == 1 && rdata_size == 0) || (i == 2 && data_size == 0)) {
                    continue;
                }
                memcpy(name, hdr[i].name, strlen(hdr[i].name));
                cg_buffer_bytes(out, name, 8);
                cg_buffer_u32(out, hdr[i].vsize);
                cg_buffer_u32(out, hdr[i].va);
                cg_buffer_u32(out, hdr[i].raw_size);
                cg_buffer_u32(out, hdr[i].raw_size != 0 ? hdr[i].raw : 0);
                cg_buffer_u32(out, 0);
                cg_buffer_u32(out, 0);
                cg_buffer_u32(out, 0);
                cg_buffer_u32(out, hdr[i].flags);
            }
        }
        out_pad_to(out, raw_text);
        cg_buffer_bytes(out, text.bytes.data, text_size);
        out_pad_to(out, raw_text + text_raw);
        if (rdata_size != 0) {
            cg_buffer_bytes(out, l->merged[MC_RODATA].bytes.data, l->merged_size[MC_RODATA]);
            out_pad_to(out, raw_rdata + imports_at);
            cg_buffer_bytes(out, blob.bytes.data, cg_buffer_size(&blob));
            out_pad_to(out, raw_rdata + rdata_raw);
        }
        if (data_raw != 0) {
            cg_buffer_bytes(out, datablob.bytes.data, cg_buffer_size(&datablob));
            out_pad_to(out, raw_data + data_raw);
        }
    }
    ok = !out->bytes.oom;
done:
    cg_buffer_free(&blob);
    cg_buffer_free(&text);
    cg_buffer_free(&datablob);
    return ok;
}

/* ---- flat ------------------------------------------------------------------ */

static bool link_flat(linker_t *l, cg_buffer_t *out) {
    const link_options_t *o = l->options;
    uint64_t base = o->image_base;
    uint32_t cursor;
    uint32_t em = 0, es = 0;
    static const mc_section_id_t order[MC_SECTION_COUNT] = {MC_TEXT, MC_RODATA, MC_DATA, MC_TLS,
                                                            MC_BSS};
    if (o->entry != nullptr) {
        if (!find_entry(l, o->entry, &em, &es)) {
            cg_diag_error(l->diag, "link: entry symbol '%s' is not defined", o->entry);
            return false;
        }
        l->flat_header = 16;
    }
    cursor = l->flat_header;
    for (uint32_t i = 0; i < MC_SECTION_COUNT; ++i) {
        mc_section_id_t id = order[i];
        cursor = cg_align_up_u32(cursor, l->merged_align[id]);
        l->rva[id] = cursor;
        cursor += l->merged_size[id];
    }
    if (!apply_relocs(l, base)) {
        return false;
    }
    if (l->flat_header != 0) {
        uint8_t jump[16];
        uint32_t target = (uint32_t)symbol_rva(l, em, es);
        memset(jump, 0xCC, sizeof(jump));
        jump[0] = 0xE9;
        put32(jump + 1, target - 5);
        cg_buffer_bytes(out, jump, 16);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        mc_section_id_t id = order[i];
        out_pad_to(out, l->rva[id]);
        cg_buffer_bytes(out, l->merged[id].bytes.data, l->merged_size[id]);
    }
    if (o->flat_include_bss) {
        out_pad_to(out, l->rva[MC_BSS] + l->merged_size[MC_BSS]);
    }
    return !out->bytes.oom;
}

bool link_modules(const mc_module_t *const *modules, uint32_t count,
                  const link_options_t *options, cg_buffer_t *out, cg_diag_t *diag) {
    linker_t l = {.modules = modules, .count = count, .options = options, .diag = diag};
    bool ok = false;
    if (count == 0) {
        cg_diag_error(diag, "link: no input");
        return false;
    }
    if (!merge_sections(&l) || !collect_definitions(&l)) {
        if (diag->error_count == 0) {
            cg_diag_error(diag, "link: out of memory");
        }
        goto done;
    }
    ok = options->format == LINK_FORMAT_PE ? link_pe(&l, out) : link_flat(&l, out);
done:
    linker_free(&l);
    return ok;
}
