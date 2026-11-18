#pragma once

/* Public API of the code generator (design doc, section 20).
 *
 * The front end builds a MIR module (ir/ir.h), then asks for an object file,
 * an assembly listing or an in-memory JIT image. Target: x86-64 only. */

#include "ir/ir.h"

typedef enum cg_os : uint8_t {
    CG_OS_WINDOWS, /* Win64 calling convention */
    CG_OS_LINUX,   /* System V AMD64 calling convention */
    CG_OS_BAREMETAL, /* no OS: Win64 calling convention, flat or ELF objects */
} cg_os_t;

typedef enum cg_format : uint8_t {
    CG_FORMAT_DEFAULT, /* COFF for Windows, ELF64 for Linux */
    CG_FORMAT_COFF,
    CG_FORMAT_ELF64,
} cg_format_t;

typedef struct cg_options {
    cg_os_t os;
    cg_format_t format;
    uint32_t opt_level;   /* 0: plain code, 1 and above: folding and fusion */
    bool verify;          /* run the MIR verifier first */
    bool optimize_ir;     /* run the MIR cleanups (constant folding) */
} cg_options_t;

/* Options for the host: Windows, default format, optimization level 1. */
cg_options_t cg_default_options(void);

/* Compiles the module to a relocatable object file. */
bool cg_compile_to_object(cg_module_t *module, const cg_options_t *options,
                          cg_buffer_t *out, cg_diag_t *diag);

/* Compiles the module and prints the machine code as Intel syntax text. */
bool cg_compile_to_asm(cg_module_t *module, const cg_options_t *options,
                       cg_buffer_t *out, cg_diag_t *diag);

/* ---- Linked images ----------------------------------------------------------- */

typedef enum cg_image_kind : uint8_t {
    CG_IMAGE_PE,   /* Windows executable */
    CG_IMAGE_FLAT, /* raw binary; an entry jump is placed at offset 0 */
} cg_image_kind_t;

typedef struct cg_image_options {
    cg_image_kind_t kind;
    const char *entry;           /* PE default "main" (wrapped to ExitProcess) */
    uint64_t image_base;         /* 0 = format default */
    bool gui_subsystem;          /* PE only */
    bool include_bss;            /* flat only: append zero filled .bss */
    const char *const *dlls;     /* PE imports are searched in these DLLs */
    uint32_t dll_count;
    const char *dll_directory;   /* null/"" = Windows system directory */
    /* Static libraries (.lib/.a archives or COFF objects) linked after the
     * modules; only the members that are needed are used. With a PE image and
     * no explicit entry, the library's start-up code is the entry. */
    const char *const *libraries;
    uint32_t library_count;
} cg_image_options_t;

/* Compiles and links the modules into one executable image. */
bool cg_link_image(cg_module_t *const *modules, uint32_t count,
                   const cg_options_t *options, const cg_image_options_t *image,
                   cg_buffer_t *out, cg_diag_t *diag);

/* ---- JIT ------------------------------------------------------------------ */

typedef struct cg_jit cg_jit_t;

typedef struct cg_jit_host {
    void *user;
    /* Address of an external symbol, or null when unknown. */
    void *(*resolve)(void *user, const char *name);
} cg_jit_host_t;

/* Compiles the module into executable memory (Windows hosts only). */
cg_jit_t *cg_jit_create(cg_module_t *module, const cg_options_t *options,
                        const cg_jit_host_t *host, cg_diag_t *diag);
void *cg_jit_symbol(const cg_jit_t *jit, const char *name);
size_t cg_jit_code_size(const cg_jit_t *jit);
void cg_jit_destroy(cg_jit_t *jit);
