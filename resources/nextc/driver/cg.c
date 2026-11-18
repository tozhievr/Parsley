#include "../cg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../obj/link.h"
#include "../obj/obj.h"
#include "../obj/obj_read.h"
#include "../platform/jit_mem.h"
#include "../x86_64/x64.h"

cg_options_t cg_default_options(void) {
    cg_options_t options = {.os = CG_OS_WINDOWS, .format = CG_FORMAT_DEFAULT,
                            .opt_level = 1, .verify = true, .optimize_ir = true};
    return options;
}

static const x64_abi_t *abi_for(const cg_options_t *options) {
    return options->os == CG_OS_LINUX ? &x64_abi_sysv : &x64_abi_win64;
}

/* Compiles into a fresh MC module. */
static mc_module_t *compile_to_mc(cg_module_t *module, const cg_options_t *options,
                                  cg_diag_t *diag) {
    mc_module_t *mc;
    x64_context_t context = {.module = module, .diag = diag};
    if (!cg_module_ok(module)) {
        cg_diag_error(diag, "module construction ran out of memory");
        return nullptr;
    }
    if (options->optimize_ir) {
        for (uint32_t index = 0; index < module->funcs.count; ++index) {
            if (!cg_optimize_function(module, module->funcs.data[index])) {
                cg_diag_error(diag, "out of memory in the IR optimizer");
                return nullptr;
            }
        }
    }
    if (options->verify && !cg_verify_module(module, diag)) {
        return nullptr;
    }
    mc = mc_create();
    if (mc == nullptr) {
        return nullptr;
    }
    context.mc = mc;
    context.options.abi = abi_for(options);
    context.options.opt_level = options->opt_level;
    if (!mc_import_module(mc, module) || !x64_compile_module(&context)) {
        if (diag->error_count == 0) {
            cg_diag_error(diag, "code generation failed");
        }
        mc_destroy(mc);
        return nullptr;
    }
    return mc;
}

bool cg_compile_to_object(cg_module_t *module, const cg_options_t *options,
                          cg_buffer_t *out, cg_diag_t *diag) {
    mc_module_t *mc = compile_to_mc(module, options, diag);
    cg_format_t format = options->format;
    bool ok;
    if (mc == nullptr) {
        return false;
    }
    if (format == CG_FORMAT_DEFAULT) {
        format = options->os == CG_OS_WINDOWS ? CG_FORMAT_COFF : CG_FORMAT_ELF64;
    }
    ok = (format == CG_FORMAT_COFF ? obj_writer_coff : obj_writer_elf64)
             .write(mc, OBJ_MACHINE_X86_64, out, diag);
    mc_destroy(mc);
    return ok;
}

bool cg_compile_to_asm(cg_module_t *module, const cg_options_t *options,
                       cg_buffer_t *out, cg_diag_t *diag) {
    x64_context_t context = {.module = module, .diag = diag};
    mc_module_t *mc;
    bool ok = true;
    if (!cg_module_ok(module)) {
        cg_diag_error(diag, "module construction ran out of memory");
        return false;
    }
    if (options->optimize_ir) {
        for (uint32_t index = 0; index < module->funcs.count; ++index) {
            (void)cg_optimize_function(module, module->funcs.data[index]);
        }
    }
    if (options->verify && !cg_verify_module(module, diag)) {
        return false;
    }
    mc = mc_create();
    if (mc == nullptr || !mc_import_module(mc, module)) {
        mc_destroy(mc);
        return false;
    }
    context.mc = mc;
    context.options.abi = abi_for(options);
    context.options.opt_level = options->opt_level;
    for (uint32_t index = 0; ok && index < module->funcs.count; ++index) {
        x64_func_t machine;
        ok = x64_select(&context, module->funcs.data[index], &machine);
        if (!ok) {
            break;
        }
        ok = x64_allocate(&context, &machine);
        ok = ok && x64_lower_frame(&context, &machine);
        ok = ok && x64_print(&machine, module, out);
        x64_func_free(&machine);
    }
    mc_destroy(mc);
    return ok;
}

static uint8_t *read_whole_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t *data = nullptr;
    long length;
    if (file == nullptr) {
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)length);
        if (data != nullptr && fread(data, 1, (size_t)length, file) != (size_t)length) {
            free(data);
            data = nullptr;
        }
        *size = (size_t)length;
    }
    fclose(file);
    return data;
}

#define LIBRARY_ENTRY "__libc_mainCRTStartup"

bool cg_link_image(cg_module_t *const *modules, uint32_t count,
                   const cg_options_t *options, const cg_image_options_t *image,
                   cg_buffer_t *out, cg_diag_t *diag) {
    mc_module_t **mcs = calloc(count + 1, sizeof(*mcs));
    obj_archive_t *archives = calloc(image->library_count + 1, sizeof(*archives));
    uint32_t loaded = 0;
    obj_module_vec_t selected = {0};
    const mc_module_t **all = nullptr;
    const char **dlls = nullptr;
    uint32_t dll_count = 0;
    uint32_t **remap = nullptr;
    bool remap_failed = false;
    const char **hint_names = nullptr;
    uint32_t *hint_dlls = nullptr;
    uint32_t hint_count = 0;
    link_options_t link = {
        .format = image->kind == CG_IMAGE_PE ? LINK_FORMAT_PE : LINK_FORMAT_FLAT,
        .entry = image->entry, .image_base = image->image_base,
        .gui_subsystem = image->gui_subsystem, .flat_include_bss = image->include_bss,
        .dll_directory = image->dll_directory,
    };
    bool ok = mcs != nullptr && archives != nullptr;
    for (uint32_t index = 0; ok && index < count; ++index) {
        mcs[index] = compile_to_mc(modules[index], options, diag);
        ok = mcs[index] != nullptr;
    }
    for (uint32_t index = 0; ok && index < image->library_count; ++index) {
        size_t size = 0;
        uint8_t *bytes = read_whole_file(image->libraries[index], &size);
        if (bytes == nullptr) {
            cg_diag_error(diag, "cannot read library '%s'", image->libraries[index]);
            ok = false;
            break;
        }
        ok = obj_archive_load(bytes, size, image->libraries[index], &archives[loaded], diag);
        free(bytes);
        if (ok) {
            ++loaded;
        }
    }
    if (ok && loaded > 0) {
        if (link.entry == nullptr && image->kind == CG_IMAGE_PE) {
            for (uint32_t a = 0; a < loaded && link.entry == nullptr; ++a) {
                for (uint32_t m = 0; m < archives[a].member_count && link.entry == nullptr; ++m) {
                    const mc_module_t *mc = archives[a].members[m];
                    for (uint32_t s = 0; s < mc->symbols.count; ++s) {
                        if (mc->symbols.data[s].section != MC_UNDEFINED &&
                            strcmp(mc_symbol_name(mc, s), LIBRARY_ENTRY) == 0) {
                            link.entry = LIBRARY_ENTRY;
                            break;
                        }
                    }
                }
            }
        }
        ok = obj_archives_select((const mc_module_t *const *)mcs, count, archives, loaded,
                                 link.entry != nullptr ? link.entry : "main", &selected);
        if (!ok) {
            cg_diag_error(diag, "out of memory while selecting library members");
        }
    }
    if (ok) {
        uint32_t extra_dlls = 0;
        uint32_t hints = 0;
        for (uint32_t a = 0; a < loaded; ++a) {
            extra_dlls += archives[a].dll_count;
            hints += archives[a].import_count;
        }
        remap = calloc((size_t)loaded + 1, sizeof(*remap));
        for (uint32_t a = 0; remap != nullptr && a < loaded; ++a) {
            remap[a] = calloc((size_t)archives[a].dll_count + 1, sizeof(uint32_t));
            if (remap[a] == nullptr) {
                remap_failed = true;
            }
        }
        hint_names = calloc((size_t)hints + 1, sizeof(*hint_names));
        hint_dlls = calloc((size_t)hints + 1, sizeof(*hint_dlls));
        all = calloc((size_t)count + selected.count + 1, sizeof(*all));
        dlls = calloc((size_t)image->dll_count + extra_dlls + 1, sizeof(*dlls));
        ok = all != nullptr && dlls != nullptr && remap != nullptr && !remap_failed &&
             hint_names != nullptr && hint_dlls != nullptr;
        if (ok) {
            for (uint32_t index = 0; index < count; ++index) {
                all[index] = mcs[index];
            }
            for (uint32_t index = 0; index < selected.count; ++index) {
                all[count + index] = selected.data[index];
            }
            for (uint32_t index = 0; index < image->dll_count; ++index) {
                dlls[dll_count++] = image->dlls[index];
            }
            for (uint32_t a = 0; a < loaded; ++a) {
                for (uint32_t index = 0; index < archives[a].dll_count; ++index) {
                    uint32_t slot = 0;
                    while (slot < dll_count && strcmp(dlls[slot], archives[a].dlls[index]) != 0) {
                        ++slot;
                    }
                    if (slot == dll_count) {
                        dlls[dll_count++] = archives[a].dlls[index];
                    }
                    remap[a][index] = slot;
                }
                for (uint32_t index = 0; index < archives[a].import_count; ++index) {
                    hint_names[hint_count] = archives[a].import_names[index];
                    hint_dlls[hint_count++] = remap[a][archives[a].import_dll[index]];
                }
            }
            link.dlls = dlls;
            link.dll_count = dll_count;
            link.hint_names = hint_names;
            link.hint_dlls = hint_dlls;
            link.hint_count = hint_count;
        }
    }
    ok = ok && link_modules(all, count + selected.count, &link, out, diag);
    for (uint32_t index = 0; mcs != nullptr && index < count; ++index) {
        mc_destroy(mcs[index]);
    }
    for (uint32_t a = 0; a < loaded; ++a) {
        obj_archive_free(&archives[a]);
    }
    cg_vec_free(&selected);
    for (uint32_t a = 0; remap != nullptr && a < loaded; ++a) {
        free(remap[a]);
    }
    free(remap);
    free(hint_names);
    free(hint_dlls);
    free(all);
    free(dlls);
    free(archives);
    free(mcs);
    return ok;
}

/* ---- JIT --------------------------------------------------------------- */

struct cg_jit {
    uint8_t *memory;
    size_t size;
    uint8_t *base[MC_SECTION_COUNT];
    mc_module_t *mc;
    uint64_t *symbol_address; /* per mc symbol */
};

constexpr uint32_t JIT_STUB_SIZE = 16;

static void patch(uint8_t *where, const void *value, size_t size) {
    memcpy(where, value, size);
}

cg_jit_t *cg_jit_create(cg_module_t *module, const cg_options_t *options,
                        const cg_jit_host_t *host, cg_diag_t *diag) {
    cg_jit_t *jit;
    mc_module_t *mc;
    size_t offsets[MC_SECTION_COUNT];
    size_t total;
    uint32_t stub_count = 0;
    uint8_t *stub_base;
    uint32_t *stub_index;
    if (options->os != CG_OS_WINDOWS || !cg_platform_jit_available()) {
        cg_diag_error(diag, "the JIT needs a Windows x64 host");
        return nullptr;
    }
    mc = compile_to_mc(module, options, diag);
    if (mc == nullptr) {
        return nullptr;
    }
    jit = calloc(1, sizeof(*jit));
    stub_index = calloc(mc->symbols.count + 1, sizeof(uint32_t));
    if (jit == nullptr || stub_index == nullptr) {
        free(jit);
        free(stub_index);
        mc_destroy(mc);
        return nullptr;
    }
    jit->mc = mc;
    jit->symbol_address = calloc(mc->symbols.count + 1, sizeof(uint64_t));
    memset(stub_index, 0xFF, (mc->symbols.count + 1) * sizeof(uint32_t));
    /* one stub per undefined symbol that is the target of a PC relative
     * reference: the host may be further than +-2 GiB away */
    for (uint32_t index = 0; index < mc->relocs.count; ++index) {
        const mc_reloc_t *reloc = &mc->relocs.data[index];
        if (reloc->kind != MC_RELOC_ABS64 &&
            mc->symbols.data[reloc->symbol].section == MC_UNDEFINED &&
            (reloc->kind == MC_RELOC_PLT32 ||
             mc->symbols.data[reloc->symbol].is_function) &&
            stub_index[reloc->symbol] == CG_INVALID) {
            stub_index[reloc->symbol] = stub_count++;
        }
    }
    total = 0;
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        size_t size = id == MC_BSS ? mc->bss_size : cg_buffer_size(&mc->sections[id]);
        total = (total + 63) & ~(size_t)63;
        offsets[id] = total;
        total += size;
        if (id == MC_TEXT) {
            total = (total + 15) & ~(size_t)15;
            total += (size_t)stub_count * JIT_STUB_SIZE;
        }
    }
    total = (total + 4095) & ~(size_t)4095;
    jit->size = total == 0 ? 4096 : total;
    jit->memory = cg_platform_alloc_executable(jit->size);
    if (jit->memory == nullptr || jit->symbol_address == nullptr) {
        cg_diag_error(diag, "cannot allocate executable memory");
        cg_jit_destroy(jit);
        free(stub_index);
        return nullptr;
    }
    memset(jit->memory, 0, jit->size);
    for (uint32_t id = 0; id < MC_SECTION_COUNT; ++id) {
        jit->base[id] = jit->memory + offsets[id];
        if (id != MC_BSS) {
            memcpy(jit->base[id], mc->sections[id].bytes.data,
                   cg_buffer_size(&mc->sections[id]));
        }
    }
    stub_base = jit->base[MC_TEXT] +
                ((cg_buffer_size(&mc->sections[MC_TEXT]) + 15) & ~(size_t)15);
    for (uint32_t index = 0; index < mc->symbols.count; ++index) {
        const mc_symbol_t *symbol = &mc->symbols.data[index];
        if (symbol->section != MC_UNDEFINED) {
            jit->symbol_address[index] =
                (uint64_t)(uintptr_t)(jit->base[symbol->section] + symbol->offset);
        } else {
            void *address = host != nullptr && host->resolve != nullptr
                                ? host->resolve(host->user, mc_symbol_name(mc, index))
                                : nullptr;
            jit->symbol_address[index] = (uint64_t)(uintptr_t)address;
        }
    }
    for (uint32_t index = 0; index < mc->relocs.count; ++index) {
        const mc_reloc_t *reloc = &mc->relocs.data[index];
        uint8_t *place = jit->base[reloc->section] + reloc->offset;
        uint64_t target = jit->symbol_address[reloc->symbol];
        if (target == 0) {
            cg_diag_error(diag, "unresolved symbol %s",
                          mc_symbol_name(mc, reloc->symbol));
            cg_jit_destroy(jit);
            free(stub_index);
            return nullptr;
        }
        if (reloc->kind == MC_RELOC_ABS64) {
            uint64_t value = target + (uint64_t)reloc->addend;
            patch(place, &value, 8);
        } else {
            int64_t delta;
            int32_t narrow;
            if (stub_index[reloc->symbol] != CG_INVALID) {
                uint8_t *stub = stub_base + (size_t)stub_index[reloc->symbol] * JIT_STUB_SIZE;
                stub[0] = 0x48;
                stub[1] = 0xB8;
                patch(stub + 2, &target, 8);
                stub[10] = 0xFF;
                stub[11] = 0xE0;
                target = (uint64_t)(uintptr_t)stub;
            }
            delta = (int64_t)(target + (uint64_t)reloc->addend) -
                    (int64_t)(uintptr_t)place;
            narrow = (int32_t)delta;
            if (delta != narrow) {
                cg_diag_error(diag, "relocation to %s is out of range",
                              mc_symbol_name(mc, reloc->symbol));
                cg_jit_destroy(jit);
                free(stub_index);
                return nullptr;
            }
            patch(place, &narrow, 4);
        }
    }
    free(stub_index);
    return jit;
}

void *cg_jit_symbol(const cg_jit_t *jit, const char *name) {
    for (uint32_t index = 0; index < jit->mc->symbols.count; ++index) {
        if (jit->mc->symbols.data[index].section != MC_UNDEFINED &&
            strcmp(mc_symbol_name(jit->mc, index), name) == 0) {
            return (void *)(uintptr_t)jit->symbol_address[index];
        }
    }
    return nullptr;
}

size_t cg_jit_code_size(const cg_jit_t *jit) {
    return cg_buffer_size(&jit->mc->sections[MC_TEXT]);
}

void cg_jit_destroy(cg_jit_t *jit) {
    if (jit == nullptr) {
        return;
    }
    if (jit->memory != nullptr) {
        cg_platform_free_executable(jit->memory, jit->size);
    }
    free(jit->symbol_address);
    mc_destroy(jit->mc);
    free(jit);
}
