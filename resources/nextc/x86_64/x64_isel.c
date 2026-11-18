#include "x64.h"

/* Instruction selection: MIR (SSA) to x86-64 MInst over virtual registers.
 *
 * Constants, symbol addresses and alloca addresses are never given a virtual
 * register of their own: they are rematerialized at every use (or folded into
 * an immediate / addressing mode). A first pass decides which pure values are
 * folded into their users; the second pass emits code and skips them. */

typedef struct x64_addr {
    uint8_t base_kind; /* X64_MB_* */
    cg_value_t base;   /* value for MB_REG, CG_INVALID otherwise */
    cg_value_t index;
    uint8_t scale;
    int64_t disp;
    uint32_t aux; /* frame slot or symbol */
} x64_addr_t;

typedef struct x64_const_cache {
    uint64_t bits[2];
    uint32_t size;
    uint32_t symbol;
} x64_const_entry_t;

typedef struct x64_sel {
    x64_context_t *context;
    const cg_func_t *func;
    x64_func_t *out;
    cg_cfg_t cfg;
    const x64_abi_t *abi;
    uint32_t *vreg_of;
    uint32_t *use_count;
    uint32_t *fold_count;
    uint32_t *def_block;
    uint32_t *block_mb;
    uint32_t *alloca_slot;
    cg_u32_vec_t edge_blocks;
    CG_VEC(x64_const_entry_t) constants;
    uint32_t current;      /* machine block being filled */
    cg_block_t ir_block;   /* IR block being translated */
    bool failed;
} x64_sel_t;

static void sel_fail(x64_sel_t *sel, const char *message) {
    if (!sel->failed) {
        cg_diag_error(sel->context->diag, "%s: %s",
                      cg_symbol_name(sel->context->module, sel->func->symbol),
                      message);
    }
    sel->failed = true;
}

static const cg_inst_t *def_of(const x64_sel_t *sel, cg_value_t value) {
    return &sel->func->insts.data[sel->func->values.data[value].def];
}

static uint32_t type_width(cg_type_t type) {
    return type == CG_T_I1 ? 1 : cg_type_size(type);
}

static bool type_is_xmm(cg_type_t type) { return cg_type_is_float(type); }

/* ---- emission helpers ------------------------------------------------ */

static x64_inst_t make_inst(x64_op_t op, uint32_t width, x64_operand_t a,
                            x64_operand_t b, x64_operand_t c) {
    x64_inst_t inst = {.op = op, .width = (uint8_t)width};
    inst.ops[0] = a;
    inst.ops[1] = b;
    inst.ops[2] = c;
    return inst;
}

static constexpr x64_operand_t none_operand = {.index = X64_NOREG};

static void emit_inst(x64_sel_t *sel, x64_inst_t inst) {
    x64_block_t *block = &sel->out->blocks.data[sel->current];
    if (!cg_vec_push(&block->insts, inst)) {
        sel->out->oom = true;
    }
}

static void emit2(x64_sel_t *sel, x64_op_t op, uint32_t width, x64_operand_t a,
                  x64_operand_t b) {
    emit_inst(sel, make_inst(op, width, a, b, none_operand));
}

static void emit1(x64_sel_t *sel, x64_op_t op, uint32_t width, x64_operand_t a) {
    emit_inst(sel, make_inst(op, width, a, none_operand, none_operand));
}

static uint32_t new_vreg(x64_sel_t *sel, bool is_xmm) {
    uint32_t reg = x64_new_vreg(sel->out, is_xmm);
    if (reg == CG_INVALID) {
        sel->out->oom = true;
        return X64_VREG_BASE;
    }
    return reg;
}

/* Copies between registers of the same class. */
static void emit_copy(x64_sel_t *sel, uint32_t dst, uint32_t src, bool is_xmm) {
    if (dst == src) {
        return;
    }
    if (is_xmm) {
        emit2(sel, X64_MOVAPS, 16, x64_reg(dst), x64_reg(src));
    } else {
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(src));
    }
}

static void emit_load_imm(x64_sel_t *sel, uint32_t dst, int64_t value) {
    emit2(sel, X64_MOV, 8, x64_reg(dst), x64_imm(value));
}

static uint32_t const_symbol(x64_sel_t *sel, uint64_t low, uint64_t high,
                             uint32_t size) {
    uint8_t bytes[16] = {0};
    uint32_t symbol;
    for (uint32_t index = 0; index < sel->constants.count; ++index) {
        const x64_const_entry_t *entry = &sel->constants.data[index];
        if (entry->size == size && entry->bits[0] == low && entry->bits[1] == high) {
            return entry->symbol;
        }
    }
    memcpy(bytes, &low, 8);
    memcpy(bytes + 8, &high, 8);
    symbol = mc_add_constant(sel->context->mc, bytes, size, size == 16 ? 16 : size);
    if (symbol == CG_INVALID) {
        sel->out->oom = true;
        return 0;
    }
    {
        x64_const_entry_t entry = {{low, high}, size, symbol};
        cg_vec_push(&sel->constants, entry);
    }
    return symbol;
}

static x64_operand_t rip_mem(uint32_t symbol, int64_t disp) {
    return (x64_operand_t){.kind = X64_O_MEM, .base_kind = X64_MB_RIP,
                           .index = X64_NOREG, .aux = symbol, .scale = 1,
                           .imm = disp};
}

static x64_operand_t frame_mem(uint32_t slot, int64_t disp) {
    return (x64_operand_t){.kind = X64_O_MEM, .base_kind = X64_MB_FRAME,
                           .index = X64_NOREG, .aux = slot, .scale = 1,
                           .imm = disp};
}

/* A value in a virtual register, materializing rematerializable values. */
static uint32_t reg_of(x64_sel_t *sel, cg_value_t value) {
    const cg_inst_t *def;
    uint32_t reg;
    if (value >= sel->func->values.count) {
        sel_fail(sel, "invalid value");
        return X64_VREG_BASE;
    }
    def = def_of(sel, value);
    switch (def->op) {
    case CG_OP_ICONST:
        reg = new_vreg(sel, false);
        emit_load_imm(sel, reg, def->imm);
        return reg;
    case CG_OP_FCONST:
        reg = new_vreg(sel, true);
        if (def->imm == 0) {
            emit2(sel, X64_XORPS, 16, x64_reg(reg), x64_reg(reg));
        } else {
            uint32_t width = def->type == CG_T_F32 ? 4 : 8;
            uint32_t symbol = const_symbol(sel, (uint64_t)def->imm, 0, width);
            emit2(sel, X64_MOVS, width, x64_reg(reg), rip_mem(symbol, 0));
        }
        return reg;
    case CG_OP_SYMADDR:
        reg = new_vreg(sel, false);
        emit2(sel, X64_LEA, 8, x64_reg(reg), rip_mem((uint32_t)def->imm, 0));
        return reg;
    case CG_OP_ALLOCA:
        reg = new_vreg(sel, false);
        emit2(sel, X64_LEA, 8, x64_reg(reg), frame_mem(sel->alloca_slot[value], 0));
        return reg;
    default:
        if (sel->vreg_of[value] == X64_NOREG) {
            sel_fail(sel, "value used before its definition was selected");
            return X64_VREG_BASE;
        }
        return sel->vreg_of[value];
    }
}

static bool imm_of(const x64_sel_t *sel, cg_value_t value, int64_t *imm) {
    const cg_inst_t *def;
    if (value >= sel->func->values.count) {
        return false;
    }
    def = def_of(sel, value);
    if (def->op != CG_OP_ICONST || def->imm < INT32_MIN || def->imm > INT32_MAX) {
        return false;
    }
    *imm = def->imm;
    return true;
}

static bool is_const_value(const x64_sel_t *sel, cg_value_t value) {
    return def_of(sel, value)->op == CG_OP_ICONST;
}

/* ---- addressing modes ------------------------------------------------- */

static bool add_fits(int64_t a, int64_t b, int64_t *out) {
    int64_t sum;
    if (__builtin_add_overflow(a, b, &sum) || sum < INT32_MIN || sum > INT32_MAX) {
        return false;
    }
    *out = sum;
    return true;
}

/* Detects `i << k` (k <= 3) and `i * {1,2,4,8}`. */
static bool match_scaled(const x64_sel_t *sel, cg_value_t value, cg_value_t *index,
                         uint8_t *scale) {
    const cg_inst_t *def = def_of(sel, value);
    int64_t imm;
    if (sel->use_count[value] != 1 ||
        (def->op != CG_OP_SHL && def->op != CG_OP_MUL) || !imm_of(sel, def->b, &imm)) {
        return false;
    }
    if (def->op == CG_OP_SHL && imm >= 1 && imm <= 3) {
        *scale = (uint8_t)(1u << imm);
    } else if (def->op == CG_OP_MUL && (imm == 2 || imm == 4 || imm == 8)) {
        *scale = (uint8_t)imm;
    } else {
        return false;
    }
    *index = def->a;
    return true;
}

static void mark_folded(x64_sel_t *sel, bool mark, cg_value_t value) {
    if (mark) {
        ++sel->fold_count[value];
    }
}

static x64_addr_t match_addr(x64_sel_t *sel, cg_value_t pointer, int64_t offset,
                             bool mark) {
    x64_addr_t addr = {.base_kind = X64_MB_REG, .base = CG_INVALID,
                       .index = CG_INVALID, .scale = 1, .disp = offset};
    cg_value_t cur = pointer;
    if (sel->context->options.opt_level == 0) {
        addr.base = cur;
        if (def_of(sel, cur)->op == CG_OP_ALLOCA) {
            addr.base_kind = X64_MB_FRAME;
            addr.aux = sel->alloca_slot[cur];
            addr.base = CG_INVALID;
        }
        return addr;
    }
    for (uint32_t depth = 0; depth < 4; ++depth) {
        const cg_inst_t *def = def_of(sel, cur);
        int64_t constant;
        int64_t sum;
        if (def->op == CG_OP_ALLOCA) {
            addr.base_kind = X64_MB_FRAME;
            addr.aux = sel->alloca_slot[cur];
            addr.base = CG_INVALID;
            return addr;
        }
        if (def->op == CG_OP_SYMADDR && addr.index == CG_INVALID) {
            addr.base_kind = X64_MB_RIP;
            addr.aux = (uint32_t)def->imm;
            addr.base = CG_INVALID;
            return addr;
        }
        if (def->op == CG_OP_ADD) {
            if (imm_of(sel, def->b, &constant) && add_fits(addr.disp, constant, &sum)) {
                addr.disp = sum;
                mark_folded(sel, mark, cur);
                cur = def->a;
                continue;
            }
            if (imm_of(sel, def->a, &constant) && add_fits(addr.disp, constant, &sum)) {
                addr.disp = sum;
                mark_folded(sel, mark, cur);
                cur = def->b;
                continue;
            }
            if (sel->use_count[cur] == 1 && addr.index == CG_INVALID) {
                cg_value_t index;
                uint8_t scale;
                mark_folded(sel, mark, cur);
                if (match_scaled(sel, def->b, &index, &scale)) {
                    mark_folded(sel, mark, def->b);
                    addr.index = index;
                    addr.scale = scale;
                    cur = def->a;
                } else if (match_scaled(sel, def->a, &index, &scale)) {
                    mark_folded(sel, mark, def->a);
                    addr.index = index;
                    addr.scale = scale;
                    cur = def->b;
                } else {
                    addr.index = def->b;
                    cur = def->a;
                }
                /* The remaining operand becomes the base (frame slots keep
                 * their special base kind). */
                if (def_of(sel, cur)->op == CG_OP_ALLOCA) {
                    addr.base_kind = X64_MB_FRAME;
                    addr.aux = sel->alloca_slot[cur];
                    addr.base = CG_INVALID;
                    return addr;
                }
                addr.base = cur;
                return addr;
            }
        }
        break;
    }
    addr.base = cur;
    return addr;
}

static x64_operand_t addr_operand(x64_sel_t *sel, const x64_addr_t *addr) {
    x64_operand_t mem = {.kind = X64_O_MEM, .base_kind = addr->base_kind,
                         .index = X64_NOREG, .scale = addr->scale,
                         .imm = addr->disp, .aux = addr->aux};
    if (addr->base_kind == X64_MB_REG) {
        mem.reg = reg_of(sel, addr->base);
    }
    if (addr->index != CG_INVALID) {
        mem.index = reg_of(sel, addr->index);
    }
    return mem;
}

/* ---- condition codes -------------------------------------------------- */

static x64_cc_t icmp_cc(cg_icmp_t cc) {
    static const x64_cc_t table[CG_ICMP_COUNT] = {
        X64_CC_E, X64_CC_NE, X64_CC_L, X64_CC_LE, X64_CC_G,
        X64_CC_GE, X64_CC_B, X64_CC_BE, X64_CC_A, X64_CC_AE};
    return table[cc];
}

static cg_icmp_t icmp_swap(cg_icmp_t cc) {
    switch (cc) {
    case CG_ICMP_SLT: return CG_ICMP_SGT;
    case CG_ICMP_SLE: return CG_ICMP_SGE;
    case CG_ICMP_SGT: return CG_ICMP_SLT;
    case CG_ICMP_SGE: return CG_ICMP_SLE;
    case CG_ICMP_ULT: return CG_ICMP_UGT;
    case CG_ICMP_ULE: return CG_ICMP_UGE;
    case CG_ICMP_UGT: return CG_ICMP_ULT;
    case CG_ICMP_UGE: return CG_ICMP_ULE;
    default: return cc;
    }
}

/* Float predicates that map to a single condition code after an optional
 * operand swap. */
static bool fcmp_single(cg_fcmp_t cc, x64_cc_t *out, bool *swap) {
    *swap = false;
    switch (cc) {
    case CG_FCMP_ONE: *out = X64_CC_NE; return true;
    case CG_FCMP_OGT: *out = X64_CC_A; return true;
    case CG_FCMP_OGE: *out = X64_CC_AE; return true;
    case CG_FCMP_OLT: *out = X64_CC_A; *swap = true; return true;
    case CG_FCMP_OLE: *out = X64_CC_AE; *swap = true; return true;
    case CG_FCMP_ORD: *out = X64_CC_NP; return true;
    case CG_FCMP_UNO: *out = X64_CC_P; return true;
    default: return false;
    }
}

static bool can_fuse_compare(const x64_sel_t *sel, cg_value_t cond,
                             cg_block_t block) {
    const cg_inst_t *def;
    x64_cc_t cc;
    bool swap;
    if (sel->context->options.opt_level == 0 || cond >= sel->func->values.count ||
        sel->use_count[cond] != 1 || sel->def_block[cond] != block) {
        return false;
    }
    def = def_of(sel, cond);
    if (def->op == CG_OP_ICMP) {
        return true;
    }
    return def->op == CG_OP_FCMP && fcmp_single((cg_fcmp_t)def->cc, &cc, &swap);
}

/* Emits the flag-setting instruction of a compare and returns the condition
 * code that is true when the compare holds. */
static x64_cc_t emit_compare(x64_sel_t *sel, const cg_inst_t *cmp) {
    cg_type_t operand_type = cg_value_type(sel->func, cmp->a);
    if (cmp->op == CG_OP_FCMP) {
        x64_cc_t cc;
        bool swap;
        uint32_t width = operand_type == CG_T_F32 ? 4 : 8;
        cg_value_t left = cmp->a;
        cg_value_t right = cmp->b;
        uint32_t left_reg;
        uint32_t right_reg;
        if (!fcmp_single((cg_fcmp_t)cmp->cc, &cc, &swap)) {
            sel_fail(sel, "internal: compare is not fusable");
            return X64_CC_E;
        }
        if (swap) {
            left = cmp->b;
            right = cmp->a;
        }
        left_reg = reg_of(sel, left);
        right_reg = reg_of(sel, right);
        emit2(sel, X64_UCOMIS, width, x64_reg(left_reg), x64_reg(right_reg));
        return cc;
    }
    {
        cg_icmp_t cc = (cg_icmp_t)cmp->cc;
        uint32_t width = type_width(operand_type);
        cg_value_t left = cmp->a;
        cg_value_t right = cmp->b;
        int64_t imm;
        if (is_const_value(sel, left) && !is_const_value(sel, right)) {
            left = cmp->b;
            right = cmp->a;
            cc = icmp_swap(cc);
        }
        if (imm_of(sel, right, &imm)) {
            if (width < 8) {
                /* compare at the native width: reduce the immediate */
                imm = width == 1   ? (int64_t)(int8_t)imm
                      : width == 2 ? (int64_t)(int16_t)imm
                                   : (int64_t)(int32_t)imm;
            }
            if (imm == 0 && (cc == CG_ICMP_EQ || cc == CG_ICMP_NE)) {
                uint32_t reg = reg_of(sel, left);
                emit2(sel, X64_TEST, width, x64_reg(reg), x64_reg(reg));
            } else {
                emit2(sel, X64_CMP, width, x64_reg(reg_of(sel, left)), x64_imm(imm));
            }
        } else {
            uint32_t left_reg = reg_of(sel, left);
            uint32_t right_reg = reg_of(sel, right);
            emit2(sel, X64_CMP, width, x64_reg(left_reg), x64_reg(right_reg));
        }
        return icmp_cc(cc);
    }
}

/* Produces flags for a boolean value and returns the true-condition. */
static x64_cc_t emit_condition(x64_sel_t *sel, cg_value_t cond) {
    if (can_fuse_compare(sel, cond, sel->ir_block)) {
        return emit_compare(sel, def_of(sel, cond));
    }
    {
        uint32_t reg = reg_of(sel, cond);
        emit2(sel, X64_TEST, 1, x64_reg(reg), x64_reg(reg));
        return X64_CC_NE;
    }
}

/* ---- per instruction lowering ---------------------------------------- */

static uint32_t dst_reg(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t reg = new_vreg(sel, type_is_xmm((cg_type_t)inst->type));
    sel->vreg_of[inst->dst] = reg;
    return reg;
}

static void lower_int_binary(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    uint32_t width = type_width(type);
    cg_value_t left = inst->a;
    cg_value_t right = inst->b;
    bool commutative = inst->op == CG_OP_ADD || inst->op == CG_OP_MUL ||
                       inst->op == CG_OP_AND || inst->op == CG_OP_OR ||
                       inst->op == CG_OP_XOR;
    int64_t imm;
    uint32_t dst;
    x64_op_t op;
    if (commutative && is_const_value(sel, left) && !is_const_value(sel, right)) {
        left = inst->b;
        right = inst->a;
    }
    dst = dst_reg(sel, inst);
    if (inst->op == CG_OP_MUL) {
        uint32_t mul_width = width < 4 ? 4 : width;
        if (imm_of(sel, right, &imm)) {
            if (imm > 0 && (imm & (imm - 1)) == 0) {
                emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, left)));
                if (imm > 1) {
                    emit2(sel, X64_SHL, mul_width, x64_reg(dst),
                          x64_imm(__builtin_ctzll((uint64_t)imm)));
                }
                return;
            }
            emit_inst(sel, make_inst(X64_IMUL3, mul_width, x64_reg(dst),
                                     x64_reg(reg_of(sel, left)), x64_imm(imm)));
            return;
        }
        {
            uint32_t right_reg = reg_of(sel, right);
            emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, left)));
            emit2(sel, X64_IMUL2, mul_width, x64_reg(dst), x64_reg(right_reg));
        }
        return;
    }
    if ((inst->op == CG_OP_ADD || inst->op == CG_OP_SUB) && width >= 4 &&
        sel->context->options.opt_level > 0) {
        /* three operand form through lea */
        if (imm_of(sel, right, &imm) && imm != INT32_MIN &&
            !is_const_value(sel, left)) {
            int64_t disp = inst->op == CG_OP_ADD ? imm : -imm;
            x64_operand_t mem = x64_mem_reg(reg_of(sel, left), disp);
            emit2(sel, X64_LEA, width, x64_reg(dst), mem);
            return;
        }
        if (inst->op == CG_OP_ADD && !imm_of(sel, right, &imm) &&
            !is_const_value(sel, left)) {
            x64_operand_t mem = x64_mem_reg(reg_of(sel, left), 0);
            mem.index = reg_of(sel, right);
            emit2(sel, X64_LEA, width, x64_reg(dst), mem);
            return;
        }
    }
    op = inst->op == CG_OP_ADD   ? X64_ADD
         : inst->op == CG_OP_SUB ? X64_SUB
         : inst->op == CG_OP_AND ? X64_AND
         : inst->op == CG_OP_OR  ? X64_OR
                                 : X64_XOR;
    if (imm_of(sel, right, &imm)) {
        if (width < 8) {
            imm = width == 1 ? (int64_t)(int8_t)imm
                  : width == 2 ? (int64_t)(int16_t)imm
                               : (int64_t)(int32_t)imm;
        }
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, left)));
        emit2(sel, op, width, x64_reg(dst), x64_imm(imm));
    } else {
        uint32_t right_reg = reg_of(sel, right);
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, left)));
        emit2(sel, op, width, x64_reg(dst), x64_reg(right_reg));
    }
}

static void lower_shift(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    uint32_t width = type_width(type);
    x64_op_t op = inst->op == CG_OP_SHL ? X64_SHL
                  : inst->op == CG_OP_LSHR ? X64_SHR : X64_SAR;
    uint32_t dst = dst_reg(sel, inst);
    int64_t imm;
    if (imm_of(sel, inst->b, &imm)) {
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, inst->a)));
        emit2(sel, op, width, x64_reg(dst),
              x64_imm(imm & (width == 8 ? 63 : 31)));
        return;
    }
    {
        uint32_t value = reg_of(sel, inst->a);
        uint32_t count = reg_of(sel, inst->b);
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(value));
        emit2(sel, X64_MOV, 4, x64_reg(X64_RCX), x64_reg(count));
        emit2(sel, op, width, x64_reg(dst), x64_reg(X64_RCX));
    }
}

static void lower_divide(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    uint32_t width = type_width(type);
    bool is_signed = inst->op == CG_OP_SDIV || inst->op == CG_OP_SREM;
    bool want_remainder = inst->op == CG_OP_SREM || inst->op == CG_OP_UREM;
    uint32_t dst = dst_reg(sel, inst);
    uint32_t left = reg_of(sel, inst->a);
    uint32_t right = reg_of(sel, inst->b);
    uint32_t div_width = width < 4 ? 4 : width;
    x64_inst_t divide;
    if (width < 4) {
        uint32_t extended_left = new_vreg(sel, false);
        uint32_t extended_right = new_vreg(sel, false);
        x64_inst_t ext = make_inst(is_signed ? X64_MOVSX : X64_MOVZX, width,
                                   x64_reg(extended_left), x64_reg(left),
                                   none_operand);
        ext.width2 = 4;
        emit_inst(sel, ext);
        ext.ops[0] = x64_reg(extended_right);
        ext.ops[1] = x64_reg(right);
        emit_inst(sel, ext);
        left = extended_left;
        right = extended_right;
    }
    emit2(sel, X64_MOV, 8, x64_reg(X64_RAX), x64_reg(left));
    if (is_signed) {
        x64_inst_t sign = make_inst(X64_CDQ, div_width, none_operand,
                                    none_operand, none_operand);
        sign.imp_uses = x64_mask(X64_RAX);
        sign.imp_defs = x64_mask(X64_RDX);
        emit_inst(sel, sign);
    } else {
        emit2(sel, X64_MOV, 4, x64_reg(X64_RDX), x64_imm(0));
    }
    divide = make_inst(is_signed ? X64_IDIV : X64_DIV, div_width, x64_reg(right),
                       none_operand, none_operand);
    divide.imp_uses = x64_mask(X64_RAX) | x64_mask(X64_RDX);
    divide.imp_defs = x64_mask(X64_RAX) | x64_mask(X64_RDX);
    emit_inst(sel, divide);
    emit2(sel, X64_MOV, 8, x64_reg(dst),
          x64_reg(want_remainder ? X64_RDX : X64_RAX));
}

static void lower_float_binary(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t width = inst->type == CG_T_F32 ? 4 : 8;
    x64_op_t op = inst->op == CG_OP_FADD   ? X64_ADDS
                  : inst->op == CG_OP_FSUB ? X64_SUBS
                  : inst->op == CG_OP_FMUL ? X64_MULS
                                           : X64_DIVS;
    uint32_t dst = dst_reg(sel, inst);
    uint32_t right = reg_of(sel, inst->b);
    emit2(sel, X64_MOVAPS, 16, x64_reg(dst), x64_reg(reg_of(sel, inst->a)));
    emit2(sel, op, width, x64_reg(dst), x64_reg(right));
}

static void lower_fneg(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t dst = dst_reg(sel, inst);
    uint32_t symbol = inst->type == CG_T_F32
                          ? const_symbol(sel, 0x80000000ull, 0, 16)
                          : const_symbol(sel, 0x8000000000000000ull, 0, 16);
    emit2(sel, X64_MOVAPS, 16, x64_reg(dst), x64_reg(reg_of(sel, inst->a)));
    emit2(sel, X64_XORPS, 16, x64_reg(dst), rip_mem(symbol, 0));
}

static void lower_compare_value(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t dst = dst_reg(sel, inst);
    if (inst->op == CG_OP_FCMP) {
        x64_cc_t cc;
        bool swap;
        if (!fcmp_single((cg_fcmp_t)inst->cc, &cc, &swap)) {
            /* OEQ / UNE need both ZF and PF. */
            uint32_t width = cg_value_type(sel->func, inst->a) == CG_T_F32 ? 4 : 8;
            uint32_t parity = new_vreg(sel, false);
            bool equal = inst->cc == CG_FCMP_OEQ;
            uint32_t left = reg_of(sel, inst->a);
            uint32_t right = reg_of(sel, inst->b);
            x64_inst_t set;
            emit2(sel, X64_UCOMIS, width, x64_reg(left), x64_reg(right));
            set = make_inst(X64_SETCC, 1, x64_reg(dst), none_operand, none_operand);
            set.cc = equal ? X64_CC_E : X64_CC_NE;
            emit_inst(sel, set);
            set.ops[0] = x64_reg(parity);
            set.cc = equal ? X64_CC_NP : X64_CC_P;
            emit_inst(sel, set);
            emit2(sel, equal ? X64_AND : X64_OR, 1, x64_reg(dst), x64_reg(parity));
            emit2(sel, X64_MOVZX, 1, x64_reg(dst), x64_reg(dst));
            return;
        }
        cc = emit_compare(sel, inst);
        {
            x64_inst_t set = make_inst(X64_SETCC, 1, x64_reg(dst), none_operand,
                                       none_operand);
            set.cc = cc;
            emit_inst(sel, set);
        }
    } else {
        x64_inst_t set = make_inst(X64_SETCC, 1, x64_reg(dst), none_operand,
                                   none_operand);
        set.cc = emit_compare(sel, inst);
        emit_inst(sel, set);
    }
    emit2(sel, X64_MOVZX, 1, x64_reg(dst), x64_reg(dst));
}

static void lower_select(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    uint32_t dst = dst_reg(sel, inst);
    if (type_is_xmm(type)) {
        uint32_t cond = reg_of(sel, inst->a);
        uint32_t if_true = reg_of(sel, inst->b);
        uint32_t if_false = reg_of(sel, inst->c);
        uint32_t mask_gpr = new_vreg(sel, false);
        uint32_t mask = new_vreg(sel, true);
        uint32_t other = new_vreg(sel, true);
        emit2(sel, X64_MOV, 4, x64_reg(mask_gpr), x64_reg(cond));
        emit1(sel, X64_NEG, 8, x64_reg(mask_gpr));
        emit2(sel, X64_MOVQ_XG, 8, x64_reg(mask), x64_reg(mask_gpr));
        emit2(sel, X64_MOVAPS, 16, x64_reg(other), x64_reg(mask));
        emit2(sel, X64_ANDPS, 16, x64_reg(mask), x64_reg(if_true));
        emit2(sel, X64_ANDNPS, 16, x64_reg(other), x64_reg(if_false));
        emit2(sel, X64_ORPS, 16, x64_reg(mask), x64_reg(other));
        emit2(sel, X64_MOVAPS, 16, x64_reg(dst), x64_reg(mask));
        return;
    }
    {
        uint32_t if_true = reg_of(sel, inst->b);
        uint32_t if_false = reg_of(sel, inst->c);
        x64_inst_t move;
        uint32_t width = type_width(type) < 4 ? 4 : type_width(type);
        x64_cc_t cc;
        emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(if_false));
        cc = emit_condition(sel, inst->a);
        move = make_inst(X64_CMOVCC, width, x64_reg(dst), x64_reg(if_true),
                         none_operand);
        move.cc = cc;
        emit_inst(sel, move);
    }
}

static void lower_cast(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    cg_type_t from = cg_value_type(sel->func, inst->a);
    uint32_t dst = dst_reg(sel, inst);
    uint32_t src;
    switch (inst->op) {
    case CG_OP_ZEXT:
        src = reg_of(sel, inst->a);
        if (from == CG_T_I32 || type_width(from) == type_width(type)) {
            emit2(sel, X64_MOV, 4, x64_reg(dst), x64_reg(src));
        } else {
            x64_inst_t ext = make_inst(X64_MOVZX, type_width(from), x64_reg(dst),
                                       x64_reg(src), none_operand);
            emit_inst(sel, ext);
        }
        break;
    case CG_OP_SEXT: {
        src = reg_of(sel, inst->a);
        if (from == CG_T_I1) {
            emit2(sel, X64_MOVZX, 1, x64_reg(dst), x64_reg(src));
            emit1(sel, X64_NEG, type_width(type) == 8 ? 8 : 4, x64_reg(dst));
        } else {
            x64_inst_t ext = make_inst(X64_MOVSX, type_width(from), x64_reg(dst),
                                       x64_reg(src), none_operand);
            ext.width2 = type_width(type) == 8 ? 8 : 4;
            emit_inst(sel, ext);
        }
        break;
    }
    case CG_OP_TRUNC:
        src = reg_of(sel, inst->a);
        emit2(sel, X64_MOV, from == CG_T_I64 || from == CG_T_PTR ? 8 : 4,
              x64_reg(dst), x64_reg(src));
        if (type == CG_T_I1) {
            emit2(sel, X64_AND, 4, x64_reg(dst), x64_imm(1));
        }
        break;
    case CG_OP_SITOFP:
    case CG_OP_UITOFP: {
        uint32_t source = reg_of(sel, inst->a);
        uint32_t int_width = type_width(from);
        uint32_t src_width = 4;
        x64_inst_t convert;
        if (int_width < 4) {
            uint32_t extended = new_vreg(sel, false);
            x64_inst_t ext = make_inst(inst->op == CG_OP_SITOFP ? X64_MOVSX : X64_MOVZX,
                                       int_width, x64_reg(extended), x64_reg(source),
                                       none_operand);
            ext.width2 = 4;
            emit_inst(sel, ext);
            source = extended;
            if (inst->op == CG_OP_UITOFP) {
                src_width = 4;
            }
        } else if (int_width == 8) {
            src_width = 8;
        } else if (inst->op == CG_OP_UITOFP) {
            uint32_t extended = new_vreg(sel, false);
            emit2(sel, X64_MOV, 4, x64_reg(extended), x64_reg(source));
            source = extended;
            src_width = 8;
        }
        convert = make_inst(X64_CVTSI2S, type == CG_T_F32 ? 4 : 8, x64_reg(dst),
                            x64_reg(source), none_operand);
        convert.width2 = (uint8_t)src_width;
        emit_inst(sel, convert);
        break;
    }
    case CG_OP_FPTOSI:
    case CG_OP_FPTOUI: {
        x64_inst_t convert = make_inst(X64_CVTTS2SI, from == CG_T_F32 ? 4 : 8,
                                       x64_reg(dst), x64_reg(reg_of(sel, inst->a)),
                                       none_operand);
        convert.width2 = (inst->op == CG_OP_FPTOUI || type_width(type) == 8) ? 8 : 4;
        emit_inst(sel, convert);
        break;
    }
    case CG_OP_FPEXT:
    case CG_OP_FPTRUNC: {
        x64_inst_t convert = make_inst(X64_CVTS2S, from == CG_T_F32 ? 4 : 8,
                                       x64_reg(dst), x64_reg(reg_of(sel, inst->a)),
                                       none_operand);
        emit_inst(sel, convert);
        break;
    }
    case CG_OP_BITCAST:
        src = reg_of(sel, inst->a);
        if (type_is_xmm(type) && !type_is_xmm(from)) {
            emit2(sel, X64_MOVQ_XG, type_width(type), x64_reg(dst), x64_reg(src));
        } else if (!type_is_xmm(type) && type_is_xmm(from)) {
            emit2(sel, X64_MOVQ_GX, type_width(type), x64_reg(dst), x64_reg(src));
        } else {
            emit_copy(sel, dst, src, type_is_xmm(type));
        }
        break;
    default:
        sel_fail(sel, "unsupported cast");
        break;
    }
}

/* Atomic read-modify-write operations; the address is always a plain register. */
static void lower_atomic(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    uint32_t width = type_width(type);
    x64_operand_t mem = x64_mem_reg(reg_of(sel, inst->a), 0);
    uint32_t dst = dst_reg(sel, inst);
    if (inst->op == CG_OP_CAS) {
        x64_inst_t exchange;
        emit2(sel, X64_MOV, 8, x64_reg(X64_RAX), x64_reg(reg_of(sel, inst->b)));
        exchange = make_inst(X64_CMPXCHG, width, mem, x64_reg(reg_of(sel, inst->c)),
                             none_operand);
        exchange.imp_uses = x64_mask(X64_RAX);
        exchange.imp_defs = x64_mask(X64_RAX);
        emit_inst(sel, exchange);
        if (width < 4) {
            emit_inst(sel, make_inst(X64_MOVZX, width, x64_reg(dst), x64_reg(X64_RAX),
                                     none_operand));
        } else {
            emit2(sel, X64_MOV, width, x64_reg(dst), x64_reg(X64_RAX));
        }
        return;
    }
    emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(reg_of(sel, inst->b)));
    emit2(sel, inst->op == CG_OP_XCHG ? X64_XCHG : X64_XADD, width, mem, x64_reg(dst));
    if (width < 4) {
        emit_inst(sel, make_inst(X64_MOVZX, width, x64_reg(dst), x64_reg(dst), none_operand));
    }
}

static void lower_load(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    x64_addr_t addr = match_addr(sel, inst->a, inst->imm, false);
    x64_operand_t mem = addr_operand(sel, &addr);
    uint32_t dst = dst_reg(sel, inst);
    uint32_t width = type_width(type);
    if (type_is_xmm(type)) {
        emit2(sel, X64_MOVS, width, x64_reg(dst), mem);
    } else if (width < 4) {
        x64_inst_t ext = make_inst(X64_MOVZX, width, x64_reg(dst), mem, none_operand);
        emit_inst(sel, ext);
    } else {
        emit2(sel, X64_MOV, width, x64_reg(dst), mem);
    }
}

static void lower_store(x64_sel_t *sel, const cg_inst_t *inst) {
    cg_type_t type = (cg_type_t)inst->type;
    x64_addr_t addr = match_addr(sel, inst->a, inst->imm, false);
    uint32_t width = type_width(type);
    x64_operand_t mem;
    int64_t imm;
    if (type_is_xmm(type)) {
        uint32_t value = reg_of(sel, inst->b);
        mem = addr_operand(sel, &addr);
        emit2(sel, X64_MOVS, width, mem, x64_reg(value));
        return;
    }
    if (imm_of(sel, inst->b, &imm) && addr.base_kind != X64_MB_RIP) {
        mem = addr_operand(sel, &addr);
        emit2(sel, X64_MOV, width, mem, x64_imm(imm));
        return;
    }
    {
        uint32_t value = reg_of(sel, inst->b);
        mem = addr_operand(sel, &addr);
        emit2(sel, X64_MOV, width, mem, x64_reg(value));
    }
}

static x64_operand_t offset_mem(x64_operand_t mem, int64_t extra) {
    mem.imm += extra;
    return mem;
}

static void lower_memcpy(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t length = (uint32_t)inst->imm;
    if (length == 0) {
        return;
    }
    if (length <= 64) {
        x64_addr_t src_addr = match_addr(sel, inst->b, 0, false);
        x64_addr_t dst_addr = match_addr(sel, inst->a, 0, false);
        x64_operand_t src = addr_operand(sel, &src_addr);
        x64_operand_t dst = addr_operand(sel, &dst_addr);
        uint32_t offset = 0;
        while (offset < length) {
            uint32_t left = length - offset;
            uint32_t chunk = left >= 16 ? 16 : left >= 8 ? 8 : left >= 4 ? 4
                             : left >= 2 ? 2 : 1;
            if (chunk == 16) {
                uint32_t temp = new_vreg(sel, true);
                emit2(sel, X64_MOVUPS, 16, x64_reg(temp), offset_mem(src, offset));
                emit2(sel, X64_MOVUPS, 16, offset_mem(dst, offset), x64_reg(temp));
            } else {
                uint32_t temp = new_vreg(sel, false);
                emit2(sel, X64_MOV, chunk, x64_reg(temp), offset_mem(src, offset));
                emit2(sel, X64_MOV, chunk, offset_mem(dst, offset), x64_reg(temp));
            }
            offset += chunk;
        }
        return;
    }
    {
        uint32_t dst = reg_of(sel, inst->a);
        uint32_t src = reg_of(sel, inst->b);
        x64_inst_t copy = make_inst(X64_REP_MOVSB, 1, none_operand, none_operand,
                                    none_operand);
        emit2(sel, X64_MOV, 8, x64_reg(X64_RDI), x64_reg(dst));
        emit2(sel, X64_MOV, 8, x64_reg(X64_RSI), x64_reg(src));
        emit2(sel, X64_MOV, 8, x64_reg(X64_RCX), x64_imm(length));
        copy.imp_uses = x64_mask(X64_RDI) | x64_mask(X64_RSI) | x64_mask(X64_RCX);
        copy.imp_defs = copy.imp_uses;
        emit_inst(sel, copy);
    }
}

static void lower_memset(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t length = (uint32_t)inst->imm;
    int64_t byte;
    if (length == 0) {
        return;
    }
    if (length <= 64 && imm_of(sel, inst->b, &byte)) {
        x64_addr_t dst_addr = match_addr(sel, inst->a, 0, false);
        x64_operand_t dst = addr_operand(sel, &dst_addr);
        uint64_t pattern = (uint64_t)(byte & 0xFF) * 0x0101010101010101ull;
        uint32_t offset = 0;
        uint32_t pattern_reg = new_vreg(sel, false);
        emit2(sel, X64_MOV, 8, x64_reg(pattern_reg), x64_imm((int64_t)pattern));
        while (offset < length) {
            uint32_t left = length - offset;
            uint32_t chunk = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
            emit2(sel, X64_MOV, chunk, offset_mem(dst, offset), x64_reg(pattern_reg));
            offset += chunk;
        }
        return;
    }
    {
        uint32_t dst = reg_of(sel, inst->a);
        uint32_t value = reg_of(sel, inst->b);
        x64_inst_t fill = make_inst(X64_REP_STOSB, 1, none_operand, none_operand,
                                    none_operand);
        emit2(sel, X64_MOV, 8, x64_reg(X64_RDI), x64_reg(dst));
        emit2(sel, X64_MOV, 4, x64_reg(X64_RAX), x64_reg(value));
        emit2(sel, X64_MOV, 8, x64_reg(X64_RCX), x64_imm(length));
        fill.imp_uses = x64_mask(X64_RDI) | x64_mask(X64_RAX) | x64_mask(X64_RCX);
        fill.imp_defs = x64_mask(X64_RDI) | x64_mask(X64_RCX);
        emit_inst(sel, fill);
    }
}

static uint32_t caller_saved_mask(const x64_abi_t *abi) {
    uint32_t all = 0;
    for (uint32_t reg = 0; reg < 16; ++reg) {
        if (reg != X64_RSP && reg != X64_RBP) {
            all |= x64_mask(reg);
        }
    }
    all |= 0xFFFF0000u; /* every xmm register */
    return all & ~abi->callee_saved;
}

static void lower_call(x64_sel_t *sel, const cg_inst_t *inst) {
    const cg_func_t *func = sel->func;
    const x64_abi_t *abi = sel->abi;
    cg_type_t types[64];
    x64_arg_loc_t locations[64];
    uint32_t stack_bytes;
    uint32_t outgoing;
    uint32_t used_regs = 0;
    uint32_t float_count = 0;
    uint32_t target_reg = X64_NOREG;
    x64_inst_t call;
    if (inst->c > 64) {
        sel_fail(sel, "too many call arguments");
        return;
    }
    for (uint32_t index = 0; index < inst->c; ++index) {
        types[index] = cg_value_type(func, func->operands.data[inst->b + index]);
    }
    stack_bytes = x64_abi_assign(abi, types, inst->c,
                                 (inst->flags & CG_CALL_VARIADIC) != 0, locations);
    outgoing = cg_align_up_u32(abi->shadow_space + stack_bytes, 16);
    if (outgoing > sel->out->max_outgoing) {
        sel->out->max_outgoing = outgoing;
    }
    sel->out->has_calls = true;
    if (inst->imm < 0) {
        target_reg = reg_of(sel, inst->a);
    }
    for (uint32_t index = 0; index < inst->c; ++index) {
        cg_value_t value = func->operands.data[inst->b + index];
        if (locations[index].reg >= 0) {
            continue;
        }
        {
            int64_t imm;
            x64_operand_t slot = {.kind = X64_O_MEM, .base_kind = X64_MB_OUTARG,
                                  .index = X64_NOREG, .scale = 1,
                                  .imm = abi->shadow_space + locations[index].stack_offset};
            if (type_is_xmm(types[index])) {
                emit2(sel, X64_MOVS, type_width(types[index]), slot,
                      x64_reg(reg_of(sel, value)));
            } else if (imm_of(sel, value, &imm)) {
                emit2(sel, X64_MOV, 8, slot, x64_imm(imm));
            } else {
                emit2(sel, X64_MOV, 8, slot, x64_reg(reg_of(sel, value)));
            }
        }
    }
    for (uint32_t index = 0; index < inst->c; ++index) {
        cg_value_t value = func->operands.data[inst->b + index];
        uint32_t reg;
        if (locations[index].reg < 0) {
            continue;
        }
        reg = (uint32_t)locations[index].reg;
        used_regs |= x64_mask(reg);
        if (type_is_xmm(types[index])) {
            emit2(sel, X64_MOVAPS, 16, x64_reg(reg), x64_reg(reg_of(sel, value)));
            ++float_count;
            if (locations[index].mirror_reg >= 0) {
                uint32_t mirror = (uint32_t)locations[index].mirror_reg;
                emit2(sel, X64_MOVQ_GX, 8, x64_reg(mirror), x64_reg(reg));
                used_regs |= x64_mask(mirror);
            }
        } else {
            uint32_t width = type_width(types[index]);
            int64_t imm;
            if (imm_of(sel, value, &imm) || is_const_value(sel, value)) {
                emit2(sel, X64_MOV, 8, x64_reg(reg),
                      x64_imm(is_const_value(sel, value) ? def_of(sel, value)->imm
                                                         : imm));
            } else if (width < 4) {
                x64_inst_t ext = make_inst(X64_MOVZX, width, x64_reg(reg),
                                           x64_reg(reg_of(sel, value)),
                                           none_operand);
                emit_inst(sel, ext);
            } else {
                emit2(sel, X64_MOV, 8, x64_reg(reg), x64_reg(reg_of(sel, value)));
            }
        }
    }
    if ((inst->flags & CG_CALL_VARIADIC) != 0 && abi->variadic_sets_al) {
        emit2(sel, X64_MOV, 4, x64_reg(X64_RAX), x64_imm(float_count));
        used_regs |= x64_mask(X64_RAX);
    }
    call = make_inst(X64_CALL, 8,
                     inst->imm >= 0 ? x64_sym((uint32_t)inst->imm)
                                    : x64_reg(target_reg),
                     none_operand, none_operand);
    call.imp_uses = used_regs;
    call.imp_defs = caller_saved_mask(abi);
    emit_inst(sel, call);
    if (inst->dst != CG_INVALID) {
        uint32_t dst = dst_reg(sel, inst);
        if (type_is_xmm((cg_type_t)inst->type)) {
            emit2(sel, X64_MOVAPS, 16, x64_reg(dst), x64_reg(X64_XMM0));
        } else {
            emit2(sel, X64_MOV, 8, x64_reg(dst), x64_reg(X64_RAX));
        }
    }
}

/* ---- phi copies and terminators ----------------------------------------- */

typedef struct x64_copy {
    uint32_t dst;
    uint32_t src;        /* vreg, or X64_NOREG for a rematerialized value */
    cg_value_t value;
    bool is_xmm;
} x64_copy_t;

static bool block_has_phi(const x64_sel_t *sel, cg_block_t block) {
    const cg_ir_block_t *ir_block = &sel->func->blocks.data[block];
    for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
        const cg_inst_t *inst = &sel->func->insts.data[ir_block->insts.data[index]];
        if (inst->op == CG_OP_PHI) {
            return true;
        }
        if (inst->op != CG_OP_PARAM) {
            return false;
        }
    }
    return false;
}

static void emit_phi_copies(x64_sel_t *sel, cg_block_t from, cg_block_t to) {
    const cg_func_t *func = sel->func;
    const cg_ir_block_t *ir_block = &func->blocks.data[to];
    CG_VEC(x64_copy_t) copies = {};
    for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
        const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
        if (inst->op != CG_OP_PHI) {
            if (inst->op == CG_OP_PARAM) {
                continue;
            }
            break;
        }
        for (uint32_t pair = 0; pair < inst->c; ++pair) {
            if (func->operands.data[inst->b + pair * 2 + 1] != from) {
                continue;
            }
            {
                cg_value_t value = func->operands.data[inst->b + pair * 2];
                const cg_inst_t *def = def_of(sel, value);
                bool lazy = def->op == CG_OP_ICONST || def->op == CG_OP_FCONST ||
                            def->op == CG_OP_SYMADDR || def->op == CG_OP_ALLOCA;
                x64_copy_t copy = {.dst = sel->vreg_of[inst->dst],
                                   .src = lazy ? X64_NOREG : sel->vreg_of[value],
                                   .value = value,
                                   .is_xmm = type_is_xmm((cg_type_t)inst->type)};
                if (copy.dst != copy.src) {
                    cg_vec_push(&copies, copy);
                }
            }
            break;
        }
    }
    /* Parallel copy sequentialization. */
    for (bool progress = true; progress && copies.count != 0;) {
        progress = false;
        for (uint32_t index = 0; index < copies.count; ++index) {
            bool is_source = false;
            for (uint32_t other = 0; other < copies.count; ++other) {
                if (other != index && copies.data[other].src == copies.data[index].dst) {
                    is_source = true;
                    break;
                }
            }
            if (is_source || copies.data[index].src == X64_NOREG) {
                continue;
            }
            emit_copy(sel, copies.data[index].dst, copies.data[index].src,
                      copies.data[index].is_xmm);
            copies.data[index] = copies.data[--copies.count];
            progress = true;
            break;
        }
        if (!progress) {
            /* Only cycles and rematerialized values are left. */
            bool cyclic = false;
            for (uint32_t index = 0; index < copies.count && !cyclic; ++index) {
                if (copies.data[index].src == X64_NOREG) {
                    continue;
                }
                {
                    uint32_t temp = new_vreg(sel, copies.data[index].is_xmm);
                    uint32_t saved = copies.data[index].dst;
                    emit_copy(sel, temp, saved, copies.data[index].is_xmm);
                    for (uint32_t other = 0; other < copies.count; ++other) {
                        if (copies.data[other].src == saved) {
                            copies.data[other].src = temp;
                        }
                    }
                    cyclic = true;
                    progress = true;
                }
            }
        }
    }
    for (uint32_t index = 0; index < copies.count; ++index) {
        /* Rematerialized values: emitted last, they read no virtual register. */
        uint32_t source = reg_of(sel, copies.data[index].value);
        emit_copy(sel, copies.data[index].dst, source, copies.data[index].is_xmm);
    }
    cg_vec_free(&copies);
}

static void set_successors(x64_sel_t *sel, uint32_t first, uint32_t second,
                           uint32_t count) {
    x64_block_t *block = &sel->out->blocks.data[sel->current];
    block->succs[0] = first;
    block->succs[1] = second;
    block->succ_count = count;
}

static uint32_t edge_target(x64_sel_t *sel, cg_block_t from, cg_block_t to) {
    uint32_t edge;
    uint32_t saved;
    if (!block_has_phi(sel, to)) {
        return sel->block_mb[to];
    }
    edge = x64_new_block(sel->out);
    if (edge == CG_INVALID) {
        sel->out->oom = true;
        return sel->block_mb[to];
    }
    cg_vec_push(&sel->edge_blocks, edge);
    saved = sel->current;
    sel->current = edge;
    emit_phi_copies(sel, from, to);
    emit1(sel, X64_JMP, 8, x64_block(sel->block_mb[to]));
    set_successors(sel, sel->block_mb[to], 0, 1);
    sel->current = saved;
    return edge;
}

/* Dynamic stack allocation. The block sits just above the outgoing argument
 * area, so rsp keeps pointing at the bottom of that area. Sizes of a page or
 * more are probed page by page (Windows commits the stack through its guard
 * page). */
static void lower_dynalloca(x64_sel_t *sel, const cg_inst_t *inst) {
    uint32_t rem = new_vreg(sel, false);
    uint32_t loop = x64_new_block(sel->out);
    uint32_t body = x64_new_block(sel->out);
    uint32_t done = x64_new_block(sel->out);
    uint32_t dst = new_vreg(sel, false);
    x64_inst_t jump;
    x64_operand_t top = {.kind = X64_O_MEM, .base_kind = X64_MB_OUTARG, .index = X64_NOREG,
                         .scale = 1, .aux = 1};
    if (loop == CG_INVALID || body == CG_INVALID || done == CG_INVALID) {
        sel->out->oom = true;
        return;
    }
    sel->out->has_dynamic_alloca = true;
    cg_vec_push(&sel->edge_blocks, loop);
    cg_vec_push(&sel->edge_blocks, body);
    cg_vec_push(&sel->edge_blocks, done);
    emit_copy(sel, rem, reg_of(sel, inst->a), false);
    emit2(sel, X64_ADD, 8, x64_reg(rem), x64_imm(15));
    emit2(sel, X64_AND, 8, x64_reg(rem), x64_imm(-16));
    emit1(sel, X64_JMP, 8, x64_block(loop));
    set_successors(sel, loop, 0, 1);
    sel->current = loop;
    emit2(sel, X64_CMP, 8, x64_reg(rem), x64_imm(4096));
    jump = make_inst(X64_JCC, 8, x64_block(done), none_operand, none_operand);
    jump.cc = X64_CC_BE;
    emit_inst(sel, jump);
    emit1(sel, X64_JMP, 8, x64_block(body));
    set_successors(sel, done, body, 2);
    sel->current = body;
    emit2(sel, X64_SUB, 8, x64_reg(X64_RSP), x64_imm(4096));
    emit2(sel, X64_OR, 8, x64_mem_reg(X64_RSP, 0), x64_imm(0));
    emit2(sel, X64_SUB, 8, x64_reg(rem), x64_imm(4096));
    emit1(sel, X64_JMP, 8, x64_block(loop));
    set_successors(sel, loop, 0, 1);
    sel->current = done;
    emit2(sel, X64_SUB, 8, x64_reg(X64_RSP), x64_reg(rem));
    emit2(sel, X64_LEA, 8, x64_reg(dst), top);
    sel->vreg_of[inst->dst] = dst;
}

static void lower_terminator(x64_sel_t *sel, const cg_inst_t *inst) {
    switch (inst->op) {
    case CG_OP_BR:
        emit_phi_copies(sel, sel->ir_block, inst->a);
        emit1(sel, X64_JMP, 8, x64_block(sel->block_mb[inst->a]));
        set_successors(sel, sel->block_mb[inst->a], 0, 1);
        break;
    case CG_OP_CONDBR: {
        x64_cc_t cc = emit_condition(sel, inst->a);
        uint32_t then_block = edge_target(sel, sel->ir_block, inst->b);
        uint32_t else_block = edge_target(sel, sel->ir_block, inst->c);
        x64_inst_t jump = make_inst(X64_JCC, 8, x64_block(then_block),
                                    none_operand, none_operand);
        jump.cc = cc;
        emit_inst(sel, jump);
        emit1(sel, X64_JMP, 8, x64_block(else_block));
        set_successors(sel, then_block, else_block, 2);
        break;
    }
    case CG_OP_RET: {
        x64_inst_t ret = make_inst(X64_RET, 8, none_operand, none_operand,
                                   none_operand);
        if (inst->a != CG_INVALID) {
            uint32_t value = reg_of(sel, inst->a);
            if (type_is_xmm(sel->func->ret_type)) {
                emit2(sel, X64_MOVAPS, 16, x64_reg(X64_XMM0), x64_reg(value));
                ret.imp_uses = x64_mask(X64_XMM0);
            } else {
                emit2(sel, X64_MOV, 8, x64_reg(X64_RAX), x64_reg(value));
                ret.imp_uses = x64_mask(X64_RAX);
            }
        }
        emit_inst(sel, ret);
        break;
    }
    case CG_OP_UNREACHABLE:
        emit_inst(sel, make_inst(X64_UD2, 1, none_operand, none_operand,
                                 none_operand));
        break;
    default:
        break;
    }
}

/* ---- driver ------------------------------------------------------------ */

static void count_uses(x64_sel_t *sel) {
    const cg_func_t *func = sel->func;
    for (uint32_t order = 0; order < sel->cfg.rpo.count; ++order) {
        cg_block_t block = sel->cfg.rpo.data[order];
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            switch (inst->op) {
            case CG_OP_ADD: case CG_OP_SUB: case CG_OP_MUL: case CG_OP_SDIV:
            case CG_OP_UDIV: case CG_OP_SREM: case CG_OP_UREM: case CG_OP_AND:
            case CG_OP_OR: case CG_OP_XOR: case CG_OP_SHL: case CG_OP_LSHR:
            case CG_OP_ASHR: case CG_OP_FADD: case CG_OP_FSUB: case CG_OP_FMUL:
            case CG_OP_FDIV: case CG_OP_ICMP: case CG_OP_FCMP: case CG_OP_STORE:
            case CG_OP_MEMCPY: case CG_OP_MEMSET: case CG_OP_XCHG: case CG_OP_XADD:
                ++sel->use_count[inst->a];
                ++sel->use_count[inst->b];
                break;
            case CG_OP_CAS:
            case CG_OP_SELECT:
                ++sel->use_count[inst->a];
                ++sel->use_count[inst->b];
                ++sel->use_count[inst->c];
                break;
            case CG_OP_FNEG: case CG_OP_ZEXT: case CG_OP_SEXT: case CG_OP_TRUNC:
            case CG_OP_SITOFP: case CG_OP_UITOFP: case CG_OP_FPTOSI:
            case CG_OP_FPTOUI: case CG_OP_FPEXT: case CG_OP_FPTRUNC:
            case CG_OP_STACKRESTORE: case CG_OP_DYNALLOCA:
            case CG_OP_BITCAST: case CG_OP_LOAD: case CG_OP_CONDBR:
                ++sel->use_count[inst->a];
                break;
            case CG_OP_RET:
                if (inst->a != CG_INVALID) {
                    ++sel->use_count[inst->a];
                }
                break;
            case CG_OP_CALL:
                if (inst->imm < 0) {
                    ++sel->use_count[inst->a];
                }
                for (uint32_t arg = 0; arg < inst->c; ++arg) {
                    ++sel->use_count[func->operands.data[inst->b + arg]];
                }
                break;
            case CG_OP_PHI:
                for (uint32_t pair = 0; pair < inst->c; ++pair) {
                    /* Phi sources are copied on the edge: count the use only
                     * when the edge is reachable. */
                    cg_block_t from = func->operands.data[inst->b + pair * 2 + 1];
                    if (from < func->blocks.count &&
                        sel->cfg.rpo_index[from] != CG_INVALID) {
                        ++sel->use_count[func->operands.data[inst->b + pair * 2]];
                    }
                }
                break;
            default:
                break;
            }
            if (inst->dst != CG_INVALID) {
                sel->def_block[inst->dst] = block;
            }
        }
    }
}

static bool is_lazy_op(cg_op_t op) {
    return op == CG_OP_ICONST || op == CG_OP_FCONST || op == CG_OP_SYMADDR ||
           op == CG_OP_ALLOCA;
}

/* Marks the values that the second pass folds into their users. */
static void mark_folds(x64_sel_t *sel) {
    const cg_func_t *func = sel->func;
    for (uint32_t order = 0; order < sel->cfg.rpo.count; ++order) {
        cg_block_t block = sel->cfg.rpo.data[order];
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            if (inst->op == CG_OP_LOAD || inst->op == CG_OP_STORE) {
                (void)match_addr(sel, inst->a, inst->imm, true);
            } else if (inst->op == CG_OP_MEMCPY) {
                (void)match_addr(sel, inst->a, 0, true);
                (void)match_addr(sel, inst->b, 0, true);
            } else if (inst->op == CG_OP_MEMSET) {
                if (inst->imm <= 64 && is_const_value(sel, inst->b)) {
                    (void)match_addr(sel, inst->a, 0, true);
                }
            } else if (inst->op == CG_OP_CONDBR || inst->op == CG_OP_SELECT) {
                /* float selects blend with a mask and need the boolean value */
                if (!(inst->op == CG_OP_SELECT && type_is_xmm((cg_type_t)inst->type)) &&
                    can_fuse_compare(sel, inst->a, block)) {
                    ++sel->fold_count[inst->a];
                }
            }
        }
    }
}

static bool is_dead(const x64_sel_t *sel, const cg_inst_t *inst) {
    if (inst->dst == CG_INVALID) {
        return false;
    }
    if (sel->use_count[inst->dst] == sel->fold_count[inst->dst] &&
        (cg_op_flags(inst->op) & CG_OPF_PURE) != 0) {
        return true;
    }
    return sel->use_count[inst->dst] == 0 && inst->op == CG_OP_LOAD;
}

static void emit_params(x64_sel_t *sel) {
    const cg_func_t *func = sel->func;
    cg_type_t types[64];
    x64_arg_loc_t locations[64];
    if (func->param_count > 64) {
        sel_fail(sel, "too many parameters");
        return;
    }
    for (uint32_t index = 0; index < func->param_count; ++index) {
        types[index] = cg_value_type(func, index);
    }
    (void)x64_abi_assign(sel->abi, types, func->param_count, false, locations);
    if (func->variadic && sel->abi->shadow_space != 0) {
        /* Win64: home the integer argument registers into the shadow space so
         * the anonymous arguments are contiguous with the stack arguments. */
        static const uint32_t home[4] = {X64_RCX, X64_RDX, X64_R8, X64_R9};
        for (uint32_t slot = func->param_count; slot < 4; ++slot) {
            x64_operand_t mem = {.kind = X64_O_MEM, .base_kind = X64_MB_INARG,
                                 .index = X64_NOREG, .scale = 1, .imm = 8 * (int64_t)slot};
            emit2(sel, X64_MOV, 8, mem, x64_reg(home[slot]));
        }
    }
    for (uint32_t index = 0; index < func->param_count; ++index) {
        uint32_t reg;
        if (sel->use_count[index] == 0) {
            continue;
        }
        reg = new_vreg(sel, type_is_xmm(types[index]));
        sel->vreg_of[index] = reg;
        if (locations[index].reg >= 0) {
            emit_copy(sel, reg, (uint32_t)locations[index].reg,
                      type_is_xmm(types[index]));
        } else {
            x64_operand_t slot = {.kind = X64_O_MEM, .base_kind = X64_MB_INARG,
                                  .index = X64_NOREG, .scale = 1,
                                  .imm = sel->abi->shadow_space +
                                         locations[index].stack_offset};
            if (type_is_xmm(types[index])) {
                emit2(sel, X64_MOVS, type_width(types[index]), x64_reg(reg), slot);
            } else {
                emit2(sel, X64_MOV, 8, x64_reg(reg), slot);
            }
        }
    }
}

static void select_block(x64_sel_t *sel, cg_block_t block) {
    const cg_func_t *func = sel->func;
    const cg_ir_block_t *ir_block = &func->blocks.data[block];
    sel->current = sel->block_mb[block];
    sel->ir_block = block;
    if (block == 0) {
        emit_params(sel);
    }
    for (uint32_t index = 0; index < ir_block->insts.count && !sel->failed; ++index) {
        const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
        cg_op_t op = inst->op;
        if (op == CG_OP_PHI || op == CG_OP_PARAM || is_lazy_op(op)) {
            continue;
        }
        if ((cg_op_flags(op) & CG_OPF_TERM) != 0) {
            lower_terminator(sel, inst);
            continue;
        }
        if (is_dead(sel, inst)) {
            continue;
        }
        switch (op) {
        case CG_OP_ADD: case CG_OP_SUB: case CG_OP_MUL: case CG_OP_AND:
        case CG_OP_OR: case CG_OP_XOR:
            lower_int_binary(sel, inst);
            break;
        case CG_OP_SDIV: case CG_OP_UDIV: case CG_OP_SREM: case CG_OP_UREM:
            lower_divide(sel, inst);
            break;
        case CG_OP_SHL: case CG_OP_LSHR: case CG_OP_ASHR:
            lower_shift(sel, inst);
            break;
        case CG_OP_FADD: case CG_OP_FSUB: case CG_OP_FMUL: case CG_OP_FDIV:
            lower_float_binary(sel, inst);
            break;
        case CG_OP_FNEG:
            lower_fneg(sel, inst);
            break;
        case CG_OP_ICMP: case CG_OP_FCMP:
            lower_compare_value(sel, inst);
            break;
        case CG_OP_SELECT:
            lower_select(sel, inst);
            break;
        case CG_OP_ZEXT: case CG_OP_SEXT: case CG_OP_TRUNC: case CG_OP_SITOFP:
        case CG_OP_UITOFP: case CG_OP_FPTOSI: case CG_OP_FPTOUI:
        case CG_OP_FPEXT: case CG_OP_FPTRUNC: case CG_OP_BITCAST:
            lower_cast(sel, inst);
            break;
        case CG_OP_LOAD:
            lower_load(sel, inst);
            break;
        case CG_OP_STORE:
            lower_store(sel, inst);
            break;
        case CG_OP_MEMCPY:
            lower_memcpy(sel, inst);
            break;
        case CG_OP_MEMSET:
            lower_memset(sel, inst);
            break;
        case CG_OP_CALL:
            lower_call(sel, inst);
            break;
        case CG_OP_XCHG: case CG_OP_XADD: case CG_OP_CAS:
            lower_atomic(sel, inst);
            break;
        case CG_OP_FENCE:
            emit_inst(sel, make_inst(X64_MFENCE, 1, none_operand, none_operand, none_operand));
            break;
        case CG_OP_STACKSAVE:
            sel->out->has_dynamic_alloca = true;
            sel->vreg_of[inst->dst] = new_vreg(sel, false);
            emit2(sel, X64_MOV, 8, x64_reg(sel->vreg_of[inst->dst]), x64_reg(X64_RSP));
            break;
        case CG_OP_STACKRESTORE:
            sel->out->has_dynamic_alloca = true;
            emit2(sel, X64_MOV, 8, x64_reg(X64_RSP), x64_reg(reg_of(sel, inst->a)));
            break;
        case CG_OP_DYNALLOCA:
            lower_dynalloca(sel, inst);
            break;
        case CG_OP_VAAREA:
            if (sel->abi->shadow_space == 0) {
                sel_fail(sel, "variadic functions are only supported for the Windows ABI");
                break;
            }
            sel->vreg_of[inst->dst] = new_vreg(sel, false);
            emit2(sel, X64_LEA, 8, x64_reg(sel->vreg_of[inst->dst]),
                  (x64_operand_t){.kind = X64_O_MEM, .base_kind = X64_MB_INARG,
                                  .index = X64_NOREG, .scale = 1,
                                  .imm = 8 * (int64_t)func->param_count});
            break;
        default:
            sel_fail(sel, "unsupported instruction");
            break;
        }
    }
}

bool x64_select(x64_context_t *context, const cg_func_t *func, x64_func_t *out) {
    x64_sel_t sel = {.context = context, .func = func, .out = out,
                     .abi = context->options.abi};
    uint32_t value_count = func->values.count;
    bool ok = false;
    x64_func_init(out, func->symbol, sel.abi);
    if (!cg_cfg_build(&sel.cfg, func)) {
        cg_diag_error(context->diag, "out of memory building the CFG");
        return false;
    }
    sel.vreg_of = malloc((value_count + 1) * sizeof(uint32_t));
    sel.use_count = calloc(value_count + 1, sizeof(uint32_t));
    sel.fold_count = calloc(value_count + 1, sizeof(uint32_t));
    sel.def_block = malloc((value_count + 1) * sizeof(uint32_t));
    sel.alloca_slot = malloc((value_count + 1) * sizeof(uint32_t));
    sel.block_mb = malloc((func->blocks.count + 1) * sizeof(uint32_t));
    if (!sel.vreg_of || !sel.use_count || !sel.fold_count || !sel.def_block ||
        !sel.alloca_slot || !sel.block_mb) {
        cg_diag_error(context->diag, "out of memory in instruction selection");
        goto done;
    }
    memset(sel.vreg_of, 0xFF, (value_count + 1) * sizeof(uint32_t));
    memset(sel.def_block, 0xFF, (value_count + 1) * sizeof(uint32_t));
    memset(sel.alloca_slot, 0xFF, (value_count + 1) * sizeof(uint32_t));
    memset(sel.block_mb, 0xFF, (func->blocks.count + 1) * sizeof(uint32_t));
    count_uses(&sel);
    /* Machine blocks in reverse post order; unreachable blocks are dropped. */
    for (uint32_t order = 0; order < sel.cfg.rpo.count; ++order) {
        cg_block_t block = sel.cfg.rpo.data[order];
        sel.block_mb[block] = x64_new_block(out);
        if (sel.block_mb[block] == CG_INVALID) {
            goto done;
        }
        cg_vec_push(&out->layout, sel.block_mb[block]);
    }
    /* Stack slots for allocas and virtual registers for phis. */
    {
        const cg_ir_block_t *entry = &func->blocks.data[0];
        for (uint32_t index = 0; index < entry->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[entry->insts.data[index]];
            if (inst->op == CG_OP_ALLOCA) {
                sel.alloca_slot[inst->dst] = x64_new_slot(
                    out, inst->imm == 0 ? 1 : (uint32_t)inst->imm, 1u << inst->a);
            }
        }
        for (uint32_t order = 0; order < sel.cfg.rpo.count; ++order) {
            const cg_ir_block_t *ir_block = &func->blocks.data[sel.cfg.rpo.data[order]];
            for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
                const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
                if (inst->op == CG_OP_PHI) {
                    sel.vreg_of[inst->dst] =
                        new_vreg(&sel, type_is_xmm((cg_type_t)inst->type));
                }
            }
        }
    }
    if (context->options.opt_level > 0) {
        mark_folds(&sel);
    }
    for (uint32_t order = 0; order < sel.cfg.rpo.count && !sel.failed; ++order) {
        select_block(&sel, sel.cfg.rpo.data[order]);
    }
    for (uint32_t index = 0; index < sel.edge_blocks.count; ++index) {
        cg_vec_push(&out->layout, sel.edge_blocks.data[index]);
    }
    ok = !sel.failed && x64_func_ok(out) && !sel.constants.oom &&
         !sel.edge_blocks.oom;
    if (!ok && !sel.failed) {
        cg_diag_error(context->diag, "out of memory in instruction selection");
    }
done:
    free(sel.vreg_of);
    free(sel.use_count);
    free(sel.fold_count);
    free(sel.def_block);
    free(sel.alloca_slot);
    free(sel.block_mb);
    cg_vec_free(&sel.edge_blocks);
    cg_vec_free(&sel.constants);
    cg_cfg_free(&sel.cfg);
    if (!ok) {
        x64_func_free(out);
    }
    return ok;
}
