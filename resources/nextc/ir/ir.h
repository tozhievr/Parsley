#pragma once

/* MIR: the target independent, SSA form input of the code generator.
 *
 * Scalars only (i1..i64, f32, f64, ptr); aggregates are handled by the front
 * end through pointers, alloca and memcpy. A function is a list of blocks,
 * every block a list of instructions; values are numbered per function and
 * every value has exactly one defining instruction. */

#include "../base/cg_base.h"

typedef enum cg_type : uint8_t {
    CG_T_VOID,
    CG_T_I1,
    CG_T_I8,
    CG_T_I16,
    CG_T_I32,
    CG_T_I64,
    CG_T_F32,
    CG_T_F64,
    CG_T_PTR,
    CG_T_COUNT
} cg_type_t;

uint32_t cg_type_size(cg_type_t type);
const char *cg_type_name(cg_type_t type);
static inline bool cg_type_is_float(cg_type_t type) {
    return type == CG_T_F32 || type == CG_T_F64;
}
static inline bool cg_type_is_int(cg_type_t type) {
    return type >= CG_T_I1 && type <= CG_T_I64;
}

typedef enum cg_op : uint8_t {
#define X(name, text, flags) CG_OP_##name,
#include "ir_ops.def"
#undef X
    CG_OP_COUNT
} cg_op_t;

enum {
    CG_OPF_DEF = 1,
    CG_OPF_TERM = 2,
    CG_OPF_SIDE = 4,
    CG_OPF_PURE = 8,
};
uint32_t cg_op_flags(cg_op_t op);
const char *cg_op_name(cg_op_t op);

typedef enum cg_icmp : uint8_t {
    CG_ICMP_EQ,
    CG_ICMP_NE,
    CG_ICMP_SLT,
    CG_ICMP_SLE,
    CG_ICMP_SGT,
    CG_ICMP_SGE,
    CG_ICMP_ULT,
    CG_ICMP_ULE,
    CG_ICMP_UGT,
    CG_ICMP_UGE,
    CG_ICMP_COUNT
} cg_icmp_t;

/* Ordered predicates are false for NaN, UNE is true for NaN. */
typedef enum cg_fcmp : uint8_t {
    CG_FCMP_OEQ,
    CG_FCMP_ONE,
    CG_FCMP_UNE,
    CG_FCMP_OLT,
    CG_FCMP_OLE,
    CG_FCMP_OGT,
    CG_FCMP_OGE,
    CG_FCMP_ORD,
    CG_FCMP_UNO,
    CG_FCMP_COUNT
} cg_fcmp_t;

/* One instruction. Field use by opcode:
 *   ICONST   imm = value                       FCONST  imm = raw bits
 *   PARAM    imm = parameter index             SYMADDR imm = symbol
 *   ALLOCA   imm = size, a = log2(alignment)
 *   binary   a, b                              FNEG/casts  a
 *   ICMP/FCMP cc, a, b                         SELECT  a = cond, b, c
 *   LOAD     a = pointer, imm = offset         STORE   a = pointer, b = value,
 *                                                      imm = offset
 *   MEMCPY   a = dst, b = src, imm = length    MEMSET  a = dst, b = byte value,
 *                                                      imm = length
 *   CALL     imm = callee symbol or -1 with a = callee pointer value,
 *            b = first argument index in the operand pool, c = argument count,
 *            flags bit 0 = variadic call
 *   PHI      b = start in the operand pool of (value, block) pairs, c = pairs
 *   BR       a = target                        CONDBR  a = cond, b = then,
 *                                                      c = else
 *   RET      a = value or CG_INVALID
 * `type` is the result type (stored type for STORE, return type for CALL). */
typedef struct cg_inst {
    cg_op_t op;
    uint8_t type;
    uint8_t cc;
    uint8_t flags;
    cg_value_t dst;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    int64_t imm;
} cg_inst_t;

constexpr uint8_t CG_CALL_VARIADIC = 1;

typedef struct cg_value_info {
    uint8_t type;
    cg_inst_id_t def;
} cg_value_info_t;

typedef struct cg_ir_block {
    cg_u32_vec_t insts;
} cg_ir_block_t;

typedef struct cg_func {
    cg_symbol_t symbol;
    cg_type_t ret_type;
    bool variadic;
    uint32_t param_count;
    CG_VEC(cg_value_info_t) values;
    CG_VEC(cg_inst_t) insts;
    CG_VEC(cg_ir_block_t) blocks;
    cg_u32_vec_t operands;
} cg_func_t;

typedef enum cg_binding : uint8_t {
    CG_BIND_LOCAL,
    CG_BIND_GLOBAL,
    CG_BIND_WEAK,
} cg_binding_t;

typedef enum cg_symkind : uint8_t {
    CG_SYM_UNKNOWN, /* referenced but neither declared as code nor data */
    CG_SYM_FUNC,
    CG_SYM_DATA,
} cg_symkind_t;

typedef enum cg_datasec : uint8_t {
    CG_DATA_RW,
    CG_DATA_RO,
    CG_DATA_BSS,
} cg_datasec_t;

typedef struct cg_symbol_info {
    uint32_t name;
    cg_symkind_t kind;
    cg_binding_t binding;
    cg_datasec_t section;
    bool defined;
    uint32_t size;
    uint32_t align;
    uint32_t data_offset; /* in module data pool, DATA symbols */
    uint32_t func_index;  /* in module functions, FUNC symbols */
} cg_symbol_info_t;

/* 64-bit absolute address stored inside a data symbol. */
typedef struct cg_data_reloc {
    cg_symbol_t owner;
    uint32_t offset;
    cg_symbol_t target;
    int64_t addend;
} cg_data_reloc_t;

typedef struct cg_module {
    cg_strings_t names;
    CG_VEC(cg_symbol_info_t) symbols;
    CG_VEC(cg_func_t *) funcs;
    cg_buffer_t data;
    CG_VEC(cg_data_reloc_t) relocs;
} cg_module_t;

cg_module_t *cg_module_create(void);
void cg_module_destroy(cg_module_t *module);
const char *cg_symbol_name(const cg_module_t *module, cg_symbol_t symbol);

/* Finds or creates a symbol by name. */
cg_symbol_t cg_module_symbol(cg_module_t *module, const char *name);
/* Declares an external function (no body). */
cg_symbol_t cg_module_declare_function(cg_module_t *module, const char *name);
/* Creates a function with a body. Block 0 (entry) and one PARAM instruction
 * per parameter are created; parameter i is value i. */
cg_func_t *cg_module_add_function(cg_module_t *module, const char *name,
                                  cg_type_t ret_type,
                                  const cg_type_t *param_types,
                                  uint32_t param_count, bool variadic,
                                  cg_binding_t binding);
/* Defines a data object; `init` may be null for zero fill. */
cg_symbol_t cg_module_add_data(cg_module_t *module, const char *name,
                               uint32_t size, uint32_t align, const void *init,
                               cg_datasec_t section, cg_binding_t binding);
bool cg_module_add_data_reloc(cg_module_t *module, cg_symbol_t owner,
                              uint32_t offset, cg_symbol_t target,
                              int64_t addend);
bool cg_module_ok(const cg_module_t *module);
cg_func_t *cg_module_function_of(const cg_module_t *module,
                                 cg_symbol_t symbol);

/* ---- builder -------------------------------------------------------- */

typedef struct cg_builder {
    cg_module_t *module;
    cg_func_t *func;
    cg_block_t block;
} cg_builder_t;

void cg_builder_init(cg_builder_t *builder, cg_module_t *module,
                     cg_func_t *func);
cg_block_t cg_builder_new_block(cg_builder_t *builder);
void cg_builder_set_block(cg_builder_t *builder, cg_block_t block);
cg_value_t cg_func_param(const cg_func_t *func, uint32_t index);
cg_type_t cg_value_type(const cg_func_t *func, cg_value_t value);

cg_value_t cg_build_iconst(cg_builder_t *builder, cg_type_t type, int64_t value);
cg_value_t cg_build_fconst(cg_builder_t *builder, cg_type_t type, double value);
/* Address of the first anonymous argument of a variadic function (the
 * register arguments are homed first). Windows x64 only. */
cg_value_t cg_build_vaarea(cg_builder_t *builder);
/* Atomic operations (sequentially consistent), integer or pointer typed:
 * XCHG/XADD store b (or the sum) to *a and return the old value; CAS stores
 * c when *a equals b and returns the old value. */
cg_value_t cg_build_atomic_xchg(cg_builder_t *builder, cg_value_t pointer, cg_value_t value);
cg_value_t cg_build_atomic_xadd(cg_builder_t *builder, cg_value_t pointer, cg_value_t value);
cg_value_t cg_build_atomic_cas(cg_builder_t *builder, cg_value_t pointer, cg_value_t expected,
                               cg_value_t desired);
void cg_build_fence(cg_builder_t *builder);
/* Dynamic stack: stacksave returns the current stack pointer, stackrestore
 * releases everything allocated since, dynalloca allocates `size` bytes (an i64
 * value) aligned to 16 and returns the address. */
cg_value_t cg_build_stacksave(cg_builder_t *builder);
void cg_build_stackrestore(cg_builder_t *builder, cg_value_t saved);
cg_value_t cg_build_dynalloca(cg_builder_t *builder, cg_value_t size);
cg_value_t cg_build_symaddr(cg_builder_t *builder, cg_symbol_t symbol);
cg_value_t cg_build_alloca(cg_builder_t *builder, uint32_t size,
                           uint32_t align);
/* Like cg_build_alloca but always placed in the entry block. */
cg_value_t cg_build_alloca_in_entry(cg_builder_t *builder, uint32_t size,
                                    uint32_t align);
cg_value_t cg_build_binary(cg_builder_t *builder, cg_op_t op, cg_value_t a,
                           cg_value_t b);
cg_value_t cg_build_fneg(cg_builder_t *builder, cg_value_t a);
cg_value_t cg_build_icmp(cg_builder_t *builder, cg_icmp_t cc, cg_value_t a,
                         cg_value_t b);
cg_value_t cg_build_fcmp(cg_builder_t *builder, cg_fcmp_t cc, cg_value_t a,
                         cg_value_t b);
cg_value_t cg_build_select(cg_builder_t *builder, cg_value_t cond,
                           cg_value_t if_true, cg_value_t if_false);
cg_value_t cg_build_cast(cg_builder_t *builder, cg_op_t op, cg_type_t type,
                         cg_value_t a);
cg_value_t cg_build_load(cg_builder_t *builder, cg_type_t type,
                         cg_value_t pointer, int64_t offset);
void cg_build_store(cg_builder_t *builder, cg_value_t pointer, cg_value_t value,
                    int64_t offset);
void cg_build_memcpy(cg_builder_t *builder, cg_value_t dst, cg_value_t src,
                     uint32_t length);
void cg_build_memset(cg_builder_t *builder, cg_value_t dst, cg_value_t byte,
                     uint32_t length);
/* Direct call; returns CG_INVALID for void. */
cg_value_t cg_build_call(cg_builder_t *builder, cg_symbol_t callee,
                         cg_type_t ret_type, const cg_value_t *args,
                         uint32_t arg_count, bool variadic);
cg_value_t cg_build_call_indirect(cg_builder_t *builder, cg_value_t callee,
                                  cg_type_t ret_type, const cg_value_t *args,
                                  uint32_t arg_count, bool variadic);
cg_value_t cg_build_phi(cg_builder_t *builder, cg_type_t type);
bool cg_phi_add_incoming(cg_func_t *func, cg_value_t phi, cg_value_t value,
                         cg_block_t block);
void cg_build_br(cg_builder_t *builder, cg_block_t target);
void cg_build_condbr(cg_builder_t *builder, cg_value_t cond, cg_block_t if_true,
                     cg_block_t if_false);
void cg_build_ret(cg_builder_t *builder, cg_value_t value);
void cg_build_unreachable(cg_builder_t *builder);

/* ---- analysis ---------------------------------------------------------- */

/* Successors of a block (0..2, from its terminator). */
uint32_t cg_block_successors(const cg_func_t *func, cg_block_t block,
                             cg_block_t out[2]);

typedef struct cg_cfg {
    uint32_t block_count;
    cg_u32_vec_t rpo;      /* reachable blocks in reverse post order */
    uint32_t *rpo_index;   /* block -> position in rpo or CG_INVALID */
    uint32_t *idom;        /* immediate dominator, entry maps to itself */
    uint32_t *pred_start;  /* CSR predecessor lists (reachable preds only) */
    uint32_t *pred_list;
    uint32_t *loop_depth;  /* natural loop nesting depth per block */
} cg_cfg_t;

bool cg_cfg_build(cg_cfg_t *cfg, const cg_func_t *func);
void cg_cfg_free(cg_cfg_t *cfg);
bool cg_cfg_dominates(const cg_cfg_t *cfg, cg_block_t a, cg_block_t b);

/* ---- verifier, printer, parser, interpreter, optimizer --------------- */

bool cg_verify_function(const cg_module_t *module, const cg_func_t *func,
                        cg_diag_t *diag);
bool cg_verify_module(const cg_module_t *module, cg_diag_t *diag);

bool cg_print_module(const cg_module_t *module, cg_buffer_t *out);
bool cg_print_function(const cg_module_t *module, const cg_func_t *func,
                       cg_buffer_t *out);
/* Parses the text form produced by cg_print_module into a fresh module. */
cg_module_t *cg_parse_module(const char *text, size_t length, cg_diag_t *diag);

/* Folds constants, simplifies branches, removes unreachable blocks and dead
 * values. Returns false only on allocation failure. */
bool cg_optimize_function(cg_module_t *module, cg_func_t *func);
/* Promotes scalar allocas to SSA values (used by cg_optimize_function). */
bool cg_mem2reg(cg_func_t *func);

typedef struct cg_interp_host {
    void *user;
    /* Calls a function that has no body. Returns false when unknown. */
    bool (*call)(void *user, const char *name, const uint64_t *args,
                 uint32_t count, cg_type_t ret_type, uint64_t *result);
} cg_interp_host_t;

/* Reference interpreter: the oracle for differential tests. Values are raw
 * 64-bit patterns (f32 in the low 32 bits). */
bool cg_interp_run(const cg_module_t *module, cg_symbol_t function,
                   const uint64_t *args, uint32_t arg_count, uint64_t *result,
                   const cg_interp_host_t *host, cg_diag_t *diag);
