#include "obj_read.h"

#include <stdlib.h>
#include <string.h>

/* COFF (AMD64) object reader and archive loader.
 *
 * Library sections are folded into the four MC sections plus MC_TLS:
 * code -> text, uninitialized -> bss, writable -> data, the rest -> rodata.
 * Debug, unwind and linker-directive sections are dropped. Symbols of COMDAT
 * sections become weak so duplicates across members are harmless. */

constexpr uint32_t SCN_CNT_CODE = 0x00000020;
constexpr uint32_t SCN_CNT_UNINIT = 0x00000080;
constexpr uint32_t SCN_LNK_INFO = 0x00000200;
constexpr uint32_t SCN_LNK_REMOVE = 0x00000800;
constexpr uint32_t SCN_LNK_COMDAT = 0x00001000;
constexpr uint32_t SCN_MEM_DISCARDABLE = 0x02000000;
constexpr uint32_t SCN_MEM_WRITE = 0x80000000;
constexpr uint8_t SKIPPED = 0xFF;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static bool starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool dropped_section(const char *name, uint32_t flags) {
    static const char *const prefixes[] = {".debug", ".pdata", ".xdata", ".drectve", ".llvm",
                                           ".gfids", ".giats", ".gljmp", ".sxdata", ".rsrc", ".idata"};
    if (flags & (SCN_LNK_REMOVE | SCN_LNK_INFO | SCN_MEM_DISCARDABLE)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (starts_with(name, prefixes[i])) {
            return true;
        }
    }
    return false;
}

typedef struct weak_alias {
    uint32_t symbol; /* coff index of the weak external */
    uint32_t tag;    /* coff index of its default */
} weak_alias_t;

typedef struct coff_state {
    const uint8_t *data;
    size_t size;
    uint32_t string_table;
} coff_state_t;

static const char *coff_symbol_name(const coff_state_t *c, const uint8_t *entry, char *small) {
    if (get32(entry) == 0) {
        uint32_t offset = get32(entry + 4);
        if ((size_t)c->string_table + offset >= c->size) {
            return nullptr;
        }
        return (const char *)c->data + c->string_table + offset;
    }
    memcpy(small, entry, 8);
    small[8] = '\0';
    return small;
}

mc_module_t *obj_read_coff(const uint8_t *d, size_t n, const char *what, cg_diag_t *diag) {
    mc_module_t *mc = nullptr;
    coff_state_t c = {.data = d, .size = n};
    uint32_t section_count;
    uint32_t symbol_count;
    uint32_t symtab;
    uint32_t optional;
    uint8_t *target = nullptr;
    uint32_t *base = nullptr;
    uint8_t *comdat = nullptr;
    uint32_t *symmap = nullptr;
    weak_alias_t *weak = nullptr;
    uint32_t weak_count = 0;
    bool ok = false;
    if (n < 20 || get16(d) != 0x8664) {
        cg_diag_error(diag, "%s: not an AMD64 COFF object", what);
        return nullptr;
    }
    section_count = get16(d + 2);
    symtab = get32(d + 8);
    symbol_count = get32(d + 12);
    optional = get16(d + 16);
    if (symtab == 0 || (uint64_t)symtab + (uint64_t)symbol_count * 18 + 4 > n ||
        20 + optional + (uint64_t)section_count * 40 > n) {
        cg_diag_error(diag, "%s: truncated COFF object", what);
        return nullptr;
    }
    c.string_table = symtab + symbol_count * 18;
    mc = mc_create();
    target = calloc(section_count + 1, 1);
    comdat = calloc(section_count + 1, 1);
    base = calloc(section_count + 1, sizeof(uint32_t));
    symmap = malloc(((size_t)symbol_count + 1) * sizeof(uint32_t));
    weak = calloc((size_t)symbol_count + 1, sizeof(weak_alias_t));
    if (mc == nullptr || target == nullptr || comdat == nullptr || base == nullptr ||
        symmap == nullptr || weak == nullptr) {
        cg_diag_error(diag, "%s: out of memory", what);
        goto done;
    }
    memset(symmap, 0xFF, ((size_t)symbol_count + 1) * sizeof(uint32_t));
    /* sections */
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *h = d + 20 + optional + (size_t)i * 40;
        char small[9];
        const char *name = small;
        uint32_t flags = get32(h + 36);
        uint32_t raw = get32(h + 16);
        uint32_t pointer = get32(h + 20);
        uint32_t shift = (flags >> 20) & 0xF;
        uint32_t alignment = shift != 0 ? 1u << (shift - 1) : 16u;
        mc_section_id_t id;
        memcpy(small, h, 8);
        small[8] = '\0';
        if (small[0] == '/') {
            uint32_t offset = (uint32_t)strtoul(small + 1, nullptr, 10);
            if ((size_t)c.string_table + offset < n) {
                name = (const char *)d + c.string_table + offset;
            }
        }
        if (dropped_section(name, flags)) {
            target[i] = SKIPPED;
            continue;
        }
        if (starts_with(name, ".tls")) {
            id = MC_TLS;
        } else if (flags & SCN_CNT_UNINIT) {
            id = MC_BSS;
        } else if (flags & SCN_CNT_CODE) {
            id = MC_TEXT;
        } else if (flags & SCN_MEM_WRITE) {
            id = MC_DATA;
        } else {
            id = MC_RODATA;
        }
        target[i] = (uint8_t)id;
        comdat[i] = (flags & SCN_LNK_COMDAT) != 0;
        if (alignment > mc->alignment[id]) {
            mc->alignment[id] = alignment;
        }
        if (id == MC_BSS) {
            mc->bss_size = cg_align_up_u32(mc->bss_size, alignment);
            base[i] = mc->bss_size;
            mc->bss_size += raw;
        } else {
            base[i] = mc_align_section(mc, id, alignment);
            if (raw != 0) {
                if (pointer == 0 || (uint64_t)pointer + raw > n) {
                    cg_diag_error(diag, "%s: section '%s' is outside the file", what, name);
                    goto done;
                }
                cg_buffer_bytes(&mc->sections[id], d + pointer, raw);
            }
        }
    }
    /* symbols */
    for (uint32_t i = 0; i < symbol_count;) {
        const uint8_t *s = d + symtab + (size_t)i * 18;
        uint32_t aux = s[17];
        uint8_t storage = s[16];
        int16_t section = (int16_t)get16(s + 12);
        uint32_t value = get32(s + 8);
        bool is_function = (get16(s + 14) >> 4) == 2;
        char small[9];
        const char *name = coff_symbol_name(&c, s, small);
        uint32_t id = CG_INVALID;
        if (name == nullptr) {
            cg_diag_error(diag, "%s: bad symbol name", what);
            goto done;
        }
        if (storage == 105) {
            weak[weak_count].symbol = i;
            weak[weak_count].tag = aux > 0 ? get32(s + 18) : CG_INVALID;
            ++weak_count;
        } else if ((storage == 2 || storage == 3 || storage == 6) && section > 0) {
            if (section <= (int32_t)section_count && target[section - 1] != SKIPPED) {
                cg_binding_t binding = storage != 2 ? CG_BIND_LOCAL
                                       : comdat[section - 1] ? CG_BIND_WEAK : CG_BIND_GLOBAL;
                id = mc_add_symbol(mc, name, binding);
                if (id != CG_INVALID) {
                    mc_define_symbol(mc, id, (mc_section_id_t)target[section - 1],
                                     base[section - 1] + value, 0, is_function);
                }
            }
        } else if (storage == 2 && section == 0) {
            if (value == 0) {
                id = mc_add_symbol(mc, name, CG_BIND_GLOBAL);
            } else {
                uint32_t alignment = value >= 16 ? 16 : value >= 8 ? 8 : value >= 4 ? 4 : 1;
                id = mc_add_symbol(mc, name, CG_BIND_WEAK);
                mc->bss_size = cg_align_up_u32(mc->bss_size, alignment);
                if (id != CG_INVALID) {
                    mc_define_symbol(mc, id, MC_BSS, mc->bss_size, value, false);
                }
                mc->bss_size += value;
                if (alignment > mc->alignment[MC_BSS]) {
                    mc->alignment[MC_BSS] = alignment;
                }
            }
        }
        symmap[i] = id;
        i += 1 + aux;
    }
    for (uint32_t k = 0; k < weak_count; ++k) {
        const uint8_t *s = d + symtab + (size_t)weak[k].symbol * 18;
        char small[9];
        const char *name = coff_symbol_name(&c, s, small);
        uint32_t id = name != nullptr ? mc_add_symbol(mc, name, CG_BIND_WEAK) : CG_INVALID;
        uint32_t tag = weak[k].tag < symbol_count ? symmap[weak[k].tag] : CG_INVALID;
        if (id != CG_INVALID && tag != CG_INVALID && mc->symbols.data[tag].section != MC_UNDEFINED) {
            const mc_symbol_t *to = &mc->symbols.data[tag];
            mc_define_symbol(mc, id, (mc_section_id_t)to->section, to->offset, to->size,
                             to->is_function);
        }
        symmap[weak[k].symbol] = id;
    }
    /* relocations */
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *h = d + 20 + optional + (size_t)i * 40;
        uint32_t count = get16(h + 32);
        uint32_t pointer = get32(h + 24);
        if (target[i] == SKIPPED || count == 0) {
            continue;
        }
        if ((uint64_t)pointer + (uint64_t)count * 10 > n) {
            cg_diag_error(diag, "%s: relocations are outside the file", what);
            goto done;
        }
        for (uint32_t k = 0; k < count; ++k) {
            const uint8_t *r = d + pointer + (size_t)k * 10;
            uint32_t where = get32(r);
            uint32_t symbol_index = get32(r + 4);
            uint16_t type = get16(r + 8);
            uint32_t symbol = symbol_index < symbol_count ? symmap[symbol_index] : CG_INVALID;
            uint32_t offset = base[i] + where;
            int64_t addend;
            const uint8_t *field;
            mc_reloc_kind_t kind;
            if (type == 0) {
                continue;
            }
            if (target[i] == MC_BSS || offset + 4 > cg_buffer_size(&mc->sections[target[i]])) {
                cg_diag_error(diag, "%s: relocation outside its section", what);
                goto done;
            }
            field = mc->sections[target[i]].bytes.data + offset;
            if (symbol == CG_INVALID) {
                cg_diag_error(diag, "%s: relocation against a dropped symbol", what);
                goto done;
            }
            if (type == 1) { /* ADDR64 */
                kind = MC_RELOC_ABS64;
                addend = (int64_t)get64(field);
            } else if (type >= 4 && type <= 9) { /* REL32, REL32_1 .. REL32_5 */
                kind = MC_RELOC_PC32;
                addend = (int64_t)(int32_t)get32(field) - 4 - (int64_t)(type - 4);
            } else if (type == 0xB) { /* SECREL */
                kind = MC_RELOC_SECREL;
                addend = (int64_t)(int32_t)get32(field);
            } else {
                cg_diag_error(diag, "%s: unsupported relocation type %u", what, (unsigned)type);
                goto done;
            }
            if (!mc_add_reloc(mc, (mc_section_id_t)target[i], offset, symbol, kind, addend)) {
                cg_diag_error(diag, "%s: out of memory", what);
                goto done;
            }
        }
    }
    ok = mc_ok(mc);
    if (!ok) {
        cg_diag_error(diag, "%s: out of memory", what);
    }
done:
    free(target);
    free(comdat);
    free(base);
    free(symmap);
    free(weak);
    if (!ok && mc != nullptr) {
        mc_destroy(mc);
        mc = nullptr;
    }
    return mc;
}

/* ---- archives -------------------------------------------------------------- */

/* Returns the index of the DLL in the archive list, or CG_INVALID. */
static uint32_t archive_add_dll(obj_archive_t *a, const char *name) {
    char **grown;
    for (uint32_t i = 0; i < a->dll_count; ++i) {
        if (strcmp(a->dlls[i], name) == 0) {
            return i;
        }
    }
    grown = realloc(a->dlls, ((size_t)a->dll_count + 1) * sizeof(char *));
    if (grown == nullptr) {
        return CG_INVALID;
    }
    a->dlls = grown;
    a->dlls[a->dll_count] = malloc(strlen(name) + 1);
    if (a->dlls[a->dll_count] == nullptr) {
        return CG_INVALID;
    }
    strcpy(a->dlls[a->dll_count], name);
    return a->dll_count++;
}

static bool archive_add_import(obj_archive_t *a, const char *symbol, uint32_t dll) {
    char **names = realloc(a->import_names, ((size_t)a->import_count + 1) * sizeof(char *));
    uint32_t *dlls;
    if (names == nullptr) {
        return false;
    }
    a->import_names = names;
    dlls = realloc(a->import_dll, ((size_t)a->import_count + 1) * sizeof(uint32_t));
    if (dlls == nullptr) {
        return false;
    }
    a->import_dll = dlls;
    names[a->import_count] = malloc(strlen(symbol) + 1);
    if (names[a->import_count] == nullptr) {
        return false;
    }
    strcpy(names[a->import_count], symbol);
    dlls[a->import_count] = dll;
    ++a->import_count;
    return true;
}

static bool archive_add_member(obj_archive_t *a, mc_module_t *mc) {
    mc_module_t **grown = realloc(a->members, ((size_t)a->member_count + 1) * sizeof(*grown));
    if (grown == nullptr) {
        mc_destroy(mc);
        return false;
    }
    a->members = grown;
    a->members[a->member_count++] = mc;
    return true;
}

void obj_archive_free(obj_archive_t *archive) {
    for (uint32_t i = 0; i < archive->member_count; ++i) {
        mc_destroy(archive->members[i]);
    }
    for (uint32_t i = 0; i < archive->dll_count; ++i) {
        free(archive->dlls[i]);
    }
    for (uint32_t i = 0; i < archive->import_count; ++i) {
        free(archive->import_names[i]);
    }
    free(archive->import_names);
    free(archive->import_dll);
    free(archive->members);
    free(archive->dlls);
    memset(archive, 0, sizeof(*archive));
}

bool obj_archive_load(const uint8_t *data, size_t size, const char *what,
                      obj_archive_t *out, cg_diag_t *diag) {
    size_t pos = 8;
    memset(out, 0, sizeof(*out));
    if (size < 8 || memcmp(data, "!<arch>\n", 8) != 0) {
        mc_module_t *mc = obj_read_coff(data, size, what, diag);
        return mc != nullptr && archive_add_member(out, mc);
    }
    while (pos + 60 <= size) {
        const uint8_t *h = data + pos;
        char digits[11];
        size_t length;
        const uint8_t *body;
        bool special;
        memcpy(digits, h + 48, 10);
        digits[10] = '\0';
        length = (size_t)strtoull(digits, nullptr, 10);
        body = h + 60;
        if (pos + 60 + length > size) {
            cg_diag_error(diag, "%s: truncated archive member", what);
            obj_archive_free(out);
            return false;
        }
        special = h[0] == '/' && (h[1] == ' ' || h[1] == '/');
        if (!special && length >= 20 && get16(body) == 0 && get16(body + 2) == 0xFFFF) {
            /* short import: symbol\0 dll\0 after the 20 byte header */
            const char *text = (const char *)body + 20;
            size_t limit = length - 20;
            size_t symbol_length = 0;
            while (symbol_length < limit && text[symbol_length] != 0) {
                ++symbol_length;
            }
            if (symbol_length + 1 < limit) {
                uint32_t dll = archive_add_dll(out, text + symbol_length + 1);
                char *symbol = malloc(symbol_length + 1);
                bool added = false;
                if (symbol != nullptr) {
                    memcpy(symbol, text, symbol_length);
                    symbol[symbol_length] = 0;
                    added = dll != CG_INVALID && archive_add_import(out, symbol, dll);
                    free(symbol);
                }
                if (!added) {
                    obj_archive_free(out);
                    return false;
                }
            }
        } else if (!special && length >= 20 && get16(body) == 0x8664) {
            mc_module_t *mc = obj_read_coff(body, length, what, diag);
            if (mc == nullptr || !archive_add_member(out, mc)) {
                obj_archive_free(out);
                return false;
            }
        }
        pos += 60 + length + (length & 1);
    }
    return true;
}

/* ---- member selection ------------------------------------------------------ */

static bool exported(const mc_symbol_t *s) {
    return s->section != MC_UNDEFINED && s->binding != CG_BIND_LOCAL;
}

bool obj_archives_select(const mc_module_t *const *base, uint32_t base_count,
                         const obj_archive_t *archives, uint32_t archive_count,
                         const char *entry, obj_module_vec_t *selected) {
    cg_strings_t names = {0};
    const mc_module_t **all = nullptr;
    const mc_module_t **queue = nullptr;
    uint32_t *provider = nullptr;
    uint8_t *defined = nullptr;
    uint8_t *taken = nullptr;
    uint32_t total = 0;
    uint32_t queued = 0;
    uint32_t head = 0;
    uint32_t entry_id;
    bool ok = false;
    for (uint32_t a = 0; a < archive_count; ++a) {
        total += archives[a].member_count;
    }
    all = calloc((size_t)total + 1, sizeof(*all));
    queue = calloc((size_t)total + base_count + 1, sizeof(*queue));
    if (all == nullptr || queue == nullptr) {
        goto done;
    }
    total = 0;
    for (uint32_t a = 0; a < archive_count; ++a) {
        for (uint32_t m = 0; m < archives[a].member_count; ++m) {
            all[total++] = archives[a].members[m];
        }
    }
    for (uint32_t pass = 0; pass < 2; ++pass) {
        uint32_t count = pass == 0 ? base_count : total;
        for (uint32_t m = 0; m < count; ++m) {
            const mc_module_t *mc = pass == 0 ? base[m] : all[m];
            for (uint32_t s = 0; s < mc->symbols.count; ++s) {
                const char *text = mc_symbol_name(mc, s);
                if (mc->symbols.data[s].section == MC_UNDEFINED || exported(&mc->symbols.data[s])) {
                    if (cg_strings_intern(&names, text, strlen(text)) == CG_INVALID) {
                        goto done;
                    }
                }
            }
        }
    }
    entry_id = entry != nullptr ? cg_strings_intern(&names, entry, strlen(entry)) : CG_INVALID;
    provider = malloc(((size_t)names.items.count + 1) * sizeof(uint32_t));
    defined = calloc((size_t)names.items.count + 1, 1);
    taken = calloc((size_t)total + 1, 1);
    if (provider == nullptr || defined == nullptr || taken == nullptr) {
        goto done;
    }
    memset(provider, 0xFF, ((size_t)names.items.count + 1) * sizeof(uint32_t));
    for (uint32_t m = 0; m < total; ++m) {
        for (uint32_t s = 0; s < all[m]->symbols.count; ++s) {
            if (exported(&all[m]->symbols.data[s])) {
                const char *text = mc_symbol_name(all[m], s);
                uint32_t id = cg_strings_intern(&names, text, strlen(text));
                if (provider[id] == CG_INVALID) {
                    provider[id] = m;
                }
            }
        }
    }
    for (uint32_t m = 0; m < base_count; ++m) {
        queue[queued++] = base[m];
        for (uint32_t s = 0; s < base[m]->symbols.count; ++s) {
            if (exported(&base[m]->symbols.data[s])) {
                const char *text = mc_symbol_name(base[m], s);
                defined[cg_strings_intern(&names, text, strlen(text))] = 1;
            }
        }
    }
#define TAKE(member_index)                                                                     \
    do {                                                                                       \
        const mc_module_t *chosen = all[member_index];                                         \
        taken[member_index] = 1;                                                               \
        queue[queued++] = chosen;                                                              \
        if (!cg_vec_push(selected, chosen)) {                                                  \
            goto done;                                                                         \
        }                                                                                      \
        for (uint32_t s = 0; s < chosen->symbols.count; ++s) {                                 \
            if (exported(&chosen->symbols.data[s])) {                                          \
                const char *text = mc_symbol_name(chosen, s);                                  \
                defined[cg_strings_intern(&names, text, strlen(text))] = 1;                    \
            }                                                                                  \
        }                                                                                      \
    } while (0)
    if (entry_id != CG_INVALID && !defined[entry_id] && provider[entry_id] != CG_INVALID) {
        TAKE(provider[entry_id]);
    }
    while (head < queued) {
        const mc_module_t *mc = queue[head++];
        for (uint32_t s = 0; s < mc->symbols.count; ++s) {
            const char *text;
            uint32_t id;
            if (mc->symbols.data[s].section != MC_UNDEFINED) {
                continue;
            }
            text = mc_symbol_name(mc, s);
            id = cg_strings_intern(&names, text, strlen(text));
            if (id < names.items.count && !defined[id] && provider[id] != CG_INVALID &&
                !taken[provider[id]]) {
                TAKE(provider[id]);
            }
        }
    }
#undef TAKE
    ok = true;
done:
    cg_strings_free(&names);
    free(all);
    free(queue);
    free(provider);
    free(defined);
    free(taken);
    return ok;
}
