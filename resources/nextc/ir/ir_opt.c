#include "ir.h"

/* Small, always-safe MIR cleanups: integer constant folding and branch
 * simplification. Instructions are rewritten in place (value ids are stable),
 * unreachable blocks are left for the back end to drop. */

static uint64_t cg_opt_mask(cg_type_t type, uint64_t value) {
    switch (type) {
    case CG_T_I1:
        return value & 1;
    case CG_T_I8:
        return value & 0xFF;
    case CG_T_I16:
        return value & 0xFFFF;
    case CG_T_I32:
        return value & 0xFFFFFFFFull;
    default:
        return value;
    }
}

static int64_t cg_opt_sext(cg_type_t type, uint64_t value) {
    switch (type) {
    case CG_T_I1:
        return (value & 1) ? -1 : 0;
    case CG_T_I8:
        return (int8_t)value;
    case CG_T_I16:
        return (int16_t)value;
    case CG_T_I32:
        return (int32_t)value;
    default:
        return (int64_t)value;
    }
}

static const cg_inst_t *cg_opt_const(const cg_func_t *func, cg_value_t value) {
    const cg_inst_t *inst;
    if (value >= func->values.count) {
        return nullptr;
    }
    inst = &func->insts.data[func->values.data[value].def];
    return inst->op == CG_OP_ICONST ? inst : nullptr;
}

static bool cg_opt_fold_inst(cg_func_t *func, cg_inst_t *inst) {
    const cg_inst_t *ca = nullptr;
    const cg_inst_t *cb = nullptr;
    cg_type_t type = (cg_type_t)inst->type;
    cg_type_t operand_type = CG_T_VOID;
    uint64_t a;
    uint64_t b;
    uint64_t out;
    switch (inst->op) {
    case CG_OP_ADD: case CG_OP_SUB: case CG_OP_MUL: case CG_OP_AND:
    case CG_OP_OR: case CG_OP_XOR: case CG_OP_SHL: case CG_OP_LSHR:
    case CG_OP_ASHR: case CG_OP_ICMP:
        ca = cg_opt_const(func, inst->a);
        cb = cg_opt_const(func, inst->b);
        operand_type = cg_value_type(func, inst->a);
        break;
    case CG_OP_ZEXT: case CG_OP_SEXT: case CG_OP_TRUNC:
        ca = cg_opt_const(func, inst->a);
        operand_type = cg_value_type(func, inst->a);
        break;
    case CG_OP_SELECT: {
        const cg_inst_t *cond = cg_opt_const(func, inst->a);
        const cg_inst_t *chosen;
        if (cond == nullptr) {
            return false;
        }
        chosen = cg_opt_const(func, (cond->imm & 1) ? inst->b : inst->c);
        if (chosen == nullptr) {
            return false;
        }
        inst->op = CG_OP_ICONST;
        inst->imm = chosen->imm;
        return true;
    }
    default:
        return false;
    }
    if (ca == nullptr || (inst->op >= CG_OP_ADD && cb == nullptr &&
                          inst->op != CG_OP_ZEXT && inst->op != CG_OP_SEXT &&
                          inst->op != CG_OP_TRUNC)) {
        return false;
    }
    if (operand_type == CG_T_PTR) {
        return false;
    }
    a = cg_opt_mask(operand_type, (uint64_t)ca->imm);
    b = cb != nullptr ? cg_opt_mask(operand_type, (uint64_t)cb->imm) : 0;
    switch (inst->op) {
    case CG_OP_ADD: out = a + b; break;
    case CG_OP_SUB: out = a - b; break;
    case CG_OP_MUL: out = a * b; break;
    case CG_OP_AND: out = a & b; break;
    case CG_OP_OR: out = a | b; break;
    case CG_OP_XOR: out = a ^ b; break;
    case CG_OP_SHL:
    case CG_OP_LSHR:
    case CG_OP_ASHR: {
        uint64_t width = (uint64_t)cg_type_size(operand_type) * 8;
        if (operand_type == CG_T_I1 || b >= width) {
            return false; /* leave out-of-range shifts to the target */
        }
        out = inst->op == CG_OP_SHL    ? a << b
              : inst->op == CG_OP_LSHR ? a >> b
                                       : (uint64_t)(cg_opt_sext(operand_type, a) >> b);
        break;
    }
    case CG_OP_ZEXT: out = a; break;
    case CG_OP_SEXT: out = (uint64_t)cg_opt_sext(operand_type, a); break;
    case CG_OP_TRUNC: out = a; break;
    case CG_OP_ICMP: {
        int64_t sa = cg_opt_sext(operand_type, a);
        int64_t sb = cg_opt_sext(operand_type, b);
        switch (inst->cc) {
        case CG_ICMP_EQ: out = a == b; break;
        case CG_ICMP_NE: out = a != b; break;
        case CG_ICMP_SLT: out = sa < sb; break;
        case CG_ICMP_SLE: out = sa <= sb; break;
        case CG_ICMP_SGT: out = sa > sb; break;
        case CG_ICMP_SGE: out = sa >= sb; break;
        case CG_ICMP_ULT: out = a < b; break;
        case CG_ICMP_ULE: out = a <= b; break;
        case CG_ICMP_UGT: out = a > b; break;
        default: out = a >= b; break;
        }
        break;
    }
    default:
        return false;
    }
    inst->op = CG_OP_ICONST;
    inst->imm = (int64_t)cg_opt_mask(type, out);
    return true;
}

/* Removes the phi pairs of `target` that come from `from`. */
static void cg_opt_drop_phi_edge(cg_func_t *func, cg_block_t target,
                                 cg_block_t from) {
    const cg_ir_block_t *block = &func->blocks.data[target];
    for (uint32_t index = 0; index < block->insts.count; ++index) {
        cg_inst_t *inst = &func->insts.data[block->insts.data[index]];
        uint32_t kept = 0;
        if (inst->op != CG_OP_PHI) {
            break;
        }
        for (uint32_t pair = 0; pair < inst->c; ++pair) {
            uint32_t *entry = &func->operands.data[inst->b + pair * 2];
            if (entry[1] == from) {
                continue;
            }
            func->operands.data[inst->b + kept * 2] = entry[0];
            func->operands.data[inst->b + kept * 2 + 1] = entry[1];
            ++kept;
        }
        inst->c = kept;
    }
}

bool cg_optimize_function(cg_module_t *module, cg_func_t *func) {
    (void)module;
    if (!cg_mem2reg(func)) {
        return false;
    }
    /* Constants must be seen before their users: iterate to a fixed point
     * (cheap, the pass only ever turns instructions into constants). */
    for (bool changed = true; changed;) {
        changed = false;
        for (cg_block_t block = 0; block < func->blocks.count; ++block) {
            const cg_ir_block_t *ir_block = &func->blocks.data[block];
            for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
                cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
                if (cg_opt_fold_inst(func, inst)) {
                    changed = true;
                } else if (inst->op == CG_OP_CONDBR) {
                    const cg_inst_t *cond = cg_opt_const(func, inst->a);
                    if (cond != nullptr || inst->b == inst->c) {
                        cg_block_t taken = (cond == nullptr || (cond->imm & 1))
                                               ? inst->b
                                               : inst->c;
                        cg_block_t dropped = taken == inst->b ? inst->c : inst->b;
                        if (dropped != taken) {
                            cg_opt_drop_phi_edge(func, dropped, block);
                        }
                        inst->op = CG_OP_BR;
                        inst->a = taken;
                        inst->b = inst->c = 0;
                        changed = true;
                    }
                }
            }
        }
    }
    return !func->insts.oom;
}
