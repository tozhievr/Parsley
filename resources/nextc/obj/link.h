#pragma once

/* Static linker for MC modules and the image packers (PE32+ executable, flat
 * binary). Several modules are merged section by section, global symbols are
 * resolved by name, relocations are applied for a given image base. */

#include "obj.h"

typedef enum link_format : uint8_t {
    LINK_FORMAT_PE,   /* Windows console/GUI executable */
    LINK_FORMAT_FLAT, /* raw image, entry reachable at offset 0 */
} link_format_t;

typedef struct link_options {
    link_format_t format;
    const char *entry;          /* entry symbol; PE default "main" */
    uint64_t image_base;        /* PE default 0x140000000, flat default 0 */
    bool gui_subsystem;         /* PE: windows instead of console */
    bool flat_include_bss;      /* flat: append zero filled .bss to the file */
    uint32_t stack_reserve;     /* PE, default 1 MiB */
    /* PE: DLLs searched for imports of undefined symbols (file names such as
     * "kernel32.dll"); each is looked up in `dll_directory` ("" = Windows
     * system directory). */
    const char *const *dlls;
    uint32_t dll_count;
    const char *dll_directory;
    /* PE: symbols known to live in a given DLL (from import libraries); they
     * are not searched in the export tables. */
    const char *const *hint_names;
    const uint32_t *hint_dlls; /* index into `dlls` */
    uint32_t hint_count;
    /* Flat: base of the RAM image when it differs from image_base. */
} link_options_t;

/* Links `count` modules into one image. For PE an entry symbol named `main`
 * is wrapped in a start-up stub that calls ExitProcess(main()). */
bool link_modules(const mc_module_t *const *modules, uint32_t count,
                  const link_options_t *options, cg_buffer_t *out,
                  cg_diag_t *diag);
