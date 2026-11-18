#pragma once

/* x86-64 back end: machine instructions (MInst), ABI data, instruction
 * selection, register allocation, frame lowering, encoding and printing.
 * Everything target specific lives below this directory. */

#include "../ir/ir.h"
#include "../mc/mc.h"

/* ---- registers ---------------------------------------------------------- */

enum {
    X64_RAX, X64_RCX, X64_RDX, X64_RBX, X64_RSP, X64_RBP, X64_RSI, X64_RDI,
    X64_R8, X64_R9, X64_R10, X64_R11, X64_R12, X64_R13, X64_R14, X64_R15,
    X64_XMM0 = 16, /* xmm0..xmm15 are 16..31 */
    X64_XMM14 = 30,
    X64_XMM15 = 31,
    X64_PHYS_COUNT = 32,
    X64_VREG_BASE = 64, /* register ids >= 64 are virtual */
};

constexpr uint32_t X64_NOREG = CG_INVALID;

static inline bool x64_is_virtual(uint32_t reg) {
    return reg != X64_NOREG && reg >= X64_VREG_BASE;
}
static inline bool x64_is_xmm(uint32_t reg) {
    return reg >= X64_XMM0 && reg < X64_PHYS_COUNT;
}
static inline uint32_t x64_mask(uint32_t reg) { return (uint32_t)1 << reg; }

/* Scratch registers used by spill code; never allocated. */
constexpr uint32_t X64_SCRATCH0 = X64_R10;
constexpr uint32_t X64_SCRATCH1 = X64_R11;
constexpr uint32_t X64_XSCRATCH0 = X64_XMM14;
constexpr uint32_t X64_XSCRATCH1 = X64_XMM15;

typedef enum x64_cc : uint8_t {
    X64_CC_O, X64_CC_NO, X64_CC_B, X64_CC_AE, X64_CC_E, X64_CC_NE, X64_CC_BE,
    X64_CC_A, X64_CC_S, X64_CC_NS, X64_CC_P, X64_CC_NP, X64_CC_L, X64_CC_GE,
    X64_CC_LE, X64_CC_G,
} x64_cc_t;

/* ---- instructions ------------------------------------------------------- */

/* X(name, mnemonic, role0, role1, role2)
 * roles: N none, U use, D def, M use+def (for register operands; the
 * registers inside a memory operand are always uses). */
#define X64_OPS(X)                                                             \
    X(MOV, "mov", D, U, N)                                                     \
    X(MOVZX, "movzx", D, U, N)                                                 \
    X(MOVSX, "movsx", D, U, N)                                                 \
    X(LEA, "lea", D, U, N)                                                     \
    X(ADD, "add", M, U, N)                                                     \
    X(SUB, "sub", M, U, N)                                                     \
    X(AND, "and", M, U, N)                                                     \
    X(OR, "or", M, U, N)                                                       \
    X(XOR, "xor", M, U, N)                                                     \
    X(CMP, "cmp", U, U, N)                                                     \
    X(TEST, "test", U, U, N)                                                   \
    X(IMUL2, "imul", M, U, N)                                                  \
    X(IMUL3, "imul", D, U, U)                                                  \
    X(NEG, "neg", M, N, N)                                                     \
    X(NOT, "not", M, N, N)                                                     \
    X(SHL, "shl", M, U, N)                                                     \
    X(SHR, "shr", M, U, N)                                                     \
    X(SAR, "sar", M, U, N)                                                     \
    X(IDIV, "idiv", U, N, N)                                                   \
    X(DIV, "div", U, N, N)                                                     \
    X(CDQ, "cdq", N, N, N)                                                     \
    X(SETCC, "set", D, N, N)                                                   \
    X(CMOVCC, "cmov", M, U, N)                                                 \
    X(JMP, "jmp", U, N, N)                                                     \
    X(JCC, "j", U, N, N)                                                       \
    X(CALL, "call", U, N, N)                                                   \
    X(RET, "ret", N, N, N)                                                     \
    X(UD2, "ud2", N, N, N)                                                     \
    X(XCHG, "xchg", U, M, N)                                                   \
    X(XADD, "lock xadd", U, M, N)                                              \
    X(CMPXCHG, "lock cmpxchg", U, U, N)                                        \
    X(MFENCE, "mfence", N, N, N)                                               \
    X(PUSH, "push", U, N, N)                                                   \
    X(POP, "pop", D, N, N)                                                     \
    X(REP_MOVSB, "rep movsb", N, N, N)                                         \
    X(REP_STOSB, "rep stosb", N, N, N)                                         \
    X(MOVS, "movs", D, U, N)                                                   \
    X(MOVAPS, "movaps", D, U, N)                                               \
    X(MOVUPS, "movups", D, U, N)                                               \
    X(ADDS, "adds", M, U, N)                                                   \
    X(SUBS, "subs", M, U, N)                                                   \
    X(MULS, "muls", M, U, N)                                                   \
    X(DIVS, "divs", M, U, N)                                                   \
    X(UCOMIS, "ucomis", U, U, N)                                               \
    X(CVTSI2S, "cvtsi2s", D, U, N)                                             \
    X(CVTTS2SI, "cvtts2si", D, U, N)                                           \
    X(CVTS2S, "cvts2s", D, U, N)                                               \
    X(XORPS, "xorps", M, U, N)                                                 \
    X(ANDPS, "andps", M, U, N)                                                 \
    X(ANDNPS, "andnps", M, U, N)                                               \
    X(ORPS, "orps", M, U, N)                                                   \
    X(MOVQ_XG, "movq", D, U, N)                                                \
    X(MOVQ_GX, "movq", D, U, N)

typedef enum x64_op : uint8_t {
#define X(name, text, r0, r1, r2) X64_##name,
    X64_OPS(X)
#undef X
    X64_OP_COUNT
} x64_op_t;

typedef enum x64_role : uint8_t {
    X64_ROLE_N,
    X64_ROLE_U,
    X64_ROLE_D,
    X64_ROLE_M,
} x64_role_t;

typedef struct x64_opinfo {
    const char *name;
    uint8_t roles[3];
} x64_opinfo_t;

const x64_opinfo_t *x64_opinfo(x64_op_t op);

typedef enum x64_okind : uint8_t {
    X64_O_NONE,
    X64_O_REG,
    X64_O_IMM,
    X64_O_MEM,
    X64_O_BLOCK, /* aux = machine block */
    X64_O_SYM,   /* aux = symbol (call target) */
} x64_okind_t;

typedef enum x64_mbase : uint8_t {
    X64_MB_NONE,   /* absolute [disp32] or [index*scale+disp] */
    X64_MB_REG,    /* reg + disp */
    X64_MB_FRAME,  /* local slot aux + disp, resolved by frame lowering */
    X64_MB_INARG,  /* incoming stack argument at disp */
    X64_MB_OUTARG, /* outgoing area at disp */
    X64_MB_RIP,    /* rip + symbol aux + disp */
} x64_mbase_t;

typedef struct x64_operand {
    uint8_t kind;
    uint8_t base_kind;
    uint8_t scale;
    uint32_t reg;   /* REG operand, or MEM base register */
    uint32_t index; /* MEM index register or X64_NOREG */
    int64_t imm;    /* IMM value or MEM displacement */
    uint32_t aux;
} x64_operand_t;

typedef struct x64_inst {
    uint8_t op;
    uint8_t width;  /* operand size in bytes (source size for extensions) */
    uint8_t width2; /* destination size where it differs */
    uint8_t cc;
    uint32_t imp_uses; /* implicit physical registers read */
    uint32_t imp_defs; /* implicit physical registers written or clobbered */
    x64_operand_t ops[3];
} x64_inst_t;

static inline x64_operand_t x64_reg(uint32_t reg) {
    return (x64_operand_t){.kind = X64_O_REG, .reg = reg, .index = X64_NOREG};
}
static inline x64_operand_t x64_imm(int64_t value) {
    return (x64_operand_t){.kind = X64_O_IMM, .imm = value, .index = X64_NOREG};
}
static inline x64_operand_t x64_mem_reg(uint32_t base, int64_t disp) {
    return (x64_operand_t){.kind = X64_O_MEM, .base_kind = X64_MB_REG,
                           .reg = base, .index = X64_NOREG, .scale = 1,
                           .imm = disp};
}
static inline x64_operand_t x64_block(uint32_t block) {
    return (x64_operand_t){.kind = X64_O_BLOCK, .aux = block,
                           .index = X64_NOREG};
}
static inline x64_operand_t x64_sym(uint32_t symbol) {
    return (x64_operand_t){.kind = X64_O_SYM, .aux = symbol,
                           .index = X64_NOREG};
}

/* ---- ABI as data -------------------------------------------------------- */

typedef struct x64_abi {
    const char *name;
    uint8_t int_args[6];
    uint8_t int_arg_count;
    uint8_t float_args[8];
    uint8_t float_arg_count;
    bool shared_positions;      /* Win64: one slot counter for int and float */
    bool variadic_floats_in_int; /* Win64: floats also go in the int slot */
    bool variadic_sets_al;      /* SysV: al = number of vector registers */
    uint32_t shadow_space;
    uint32_t callee_saved;      /* mask over physical register ids */
    uint32_t allocatable_gpr;
    uint32_t allocatable_xmm;
} x64_abi_t;

extern const x64_abi_t x64_abi_win64;
extern const x64_abi_t x64_abi_sysv;

typedef struct x64_arg_loc {
    int32_t reg;          /* physical register or -1 for the stack */
    int32_t mirror_reg;   /* Win64 variadic float: integer register or -1 */
    uint32_t stack_offset; /* bytes from the start of the stack argument area */
} x64_arg_loc_t;

/* Assigns locations to arguments of the given types; returns the size of the
 * stack argument area (without shadow space). */
uint32_t x64_abi_assign(const x64_abi_t *abi, const cg_type_t *types,
                        uint32_t count, bool variadic_call,
                        x64_arg_loc_t *locations);

/* ---- machine function --------------------------------------------------- */

typedef CG_VEC(x64_inst_t) x64_inst_vec_t;

typedef struct x64_block_mc {
    x64_inst_vec_t insts;
    uint32_t succs[2];
    uint32_t succ_count;
} x64_block_t;

typedef struct x64_slot {
    uint32_t size;
    uint32_t align;
    int32_t offset; /* from rbp, negative, assigned by frame lowering */
} x64_slot_t;

typedef struct x64_vreg {
    uint8_t is_xmm;
    int32_t assigned;  /* physical register, -1 if spilled/unassigned */
    int32_t spill_slot; /* slot index or -1 */
    uint32_t start;
    uint32_t end;
    uint32_t hint;
} x64_vreg_t;

typedef struct x64_func {
    cg_symbol_t symbol;
    const x64_abi_t *abi;
    CG_VEC(x64_block_t) blocks;
    cg_u32_vec_t layout; /* machine block ids in emission order */
    CG_VEC(x64_vreg_t) vregs;
    CG_VEC(x64_slot_t) slots;
    uint32_t max_outgoing;   /* bytes needed at the bottom of the frame */
    uint32_t used_callee_saved;
    uint32_t frame_size;     /* bytes subtracted from rsp after the pushes */
    uint32_t pushed_bytes;
    uint32_t frame_align;    /* > 16: slots are rsp-relative, rsp is realigned */
    bool has_dynamic_alloca; /* the function moves rsp at run time */
    bool has_calls;
    bool oom;
} x64_func_t;

void x64_func_init(x64_func_t *func, cg_symbol_t symbol, const x64_abi_t *abi);
void x64_func_free(x64_func_t *func);
bool x64_func_ok(const x64_func_t *func);
uint32_t x64_new_block(x64_func_t *func);
uint32_t x64_new_vreg(x64_func_t *func, bool is_xmm);
uint32_t x64_new_slot(x64_func_t *func, uint32_t size, uint32_t align);

/* ---- passes ------------------------------------------------------------- */

typedef struct x64_options {
    const x64_abi_t *abi;
    uint32_t opt_level; /* 0: no folding of addresses and compares */
} x64_options_t;

typedef struct x64_context {
    cg_module_t *module;
    mc_module_t *mc;
    x64_options_t options;
    cg_diag_t *diag;
} x64_context_t;

/* Instruction selection: MIR to MInst over virtual registers. */
bool x64_select(x64_context_t *context, const cg_func_t *func,
                x64_func_t *out);
/* Register allocation: replaces virtual registers by physical ones and adds
 * spill code. */
bool x64_allocate(x64_context_t *context, x64_func_t *func);
/* Frame layout, prologue/epilogue, branch cleanup. */
bool x64_lower_frame(x64_context_t *context, x64_func_t *func);
/* Encodes the function into the text section of `context->mc`. */
bool x64_emit(x64_context_t *context, x64_func_t *func);
/* Intel syntax listing of a machine function. */
bool x64_print(const x64_func_t *func, const cg_module_t *module,
               cg_buffer_t *out);

/* Compiles a whole MIR module into the MC module. */
bool x64_compile_module(x64_context_t *context);

/* ---- encoder (also used by tests) ----------------------------------------- */

typedef struct x64_reloc_note {
    uint8_t kind; /* mc_reloc_kind_t */
    uint32_t offset; /* offset of the field inside the instruction */
    uint32_t symbol;
    int64_t addend;
} x64_reloc_note_t;

typedef struct x64_encoded {
    uint8_t bytes[16];
    uint8_t length;
    bool has_reloc;
    x64_reloc_note_t reloc;
} x64_encoded_t;

/* Encodes one non-branch instruction with physical registers only. Returns
 * false for an unencodable instruction. */
bool x64_encode(const x64_inst_t *inst, x64_encoded_t *out);
/* Names of the 64/32/16/8 bit registers. */
const char *x64_reg_name(uint32_t reg, uint32_t width);
