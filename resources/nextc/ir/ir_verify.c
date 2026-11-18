#include "ir.h"

/* Structural and type verifier for MIR (ADR-002: every level has its own). */

typedef struct cg_verify {
    const cg_module_t *module;
    const cg_func_t *func;
    cg_diag_t *diag;
    cg_cfg_t cfg;
    uint32_t *def_block; /* value -> block of its definition */
    uint32_t *def_index; /* value -> position inside that block */
    bool ok;
} cg_verify_t;

static void cg_verify_fail(cg_verify_t *verify, cg_block_t block,
                           uint32_t position, const char *message) {
    verify->ok = false;
    cg_diag_error(verify->diag, "%s: block %u inst %u: %s",
                  cg_symbol_name(verify->module, verify->func->symbol), block,
                  position, message);
}

static bool cg_verify_value(cg_verify_t *verify, cg_block_t block,
                            uint32_t position, cg_value_t value,
                            const char *role) {
    if (value >= verify->func->values.count) {
        cg_verify_fail(verify, block, position, role);
        return false;
    }
    return true;
}

static bool cg_verify_dominates_use(const cg_verify_t *verify,
                                    cg_value_t value, cg_block_t use_block,
                                    uint32_t use_position) {
    cg_block_t def_block = verify->def_block[value];
    if (def_block == CG_INVALID) {
        return false;
    }
    if (def_block == use_block) {
        return verify->def_index[value] < use_position;
    }
    return cg_cfg_dominates(&verify->cfg, def_block, use_block);
}

static void cg_verify_inst(cg_verify_t *verify, cg_block_t block,
                           uint32_t position, const cg_inst_t *inst) {
    const cg_func_t *func = verify->func;
    const cg_module_t *module = verify->module;
    cg_type_t type_a = CG_T_VOID;
    cg_type_t type_b = CG_T_VOID;
    bool uses_a = false;
    bool uses_b = false;
    bool uses_c = false;
    switch (inst->op) {
    case CG_OP_ADD:
    case CG_OP_SUB:
    case CG_OP_MUL:
    case CG_OP_SDIV:
    case CG_OP_UDIV:
    case CG_OP_SREM:
    case CG_OP_UREM:
    case CG_OP_AND:
    case CG_OP_OR:
    case CG_OP_XOR:
    case CG_OP_SHL:
    case CG_OP_LSHR:
    case CG_OP_ASHR:
    case CG_OP_FADD:
    case CG_OP_FSUB:
    case CG_OP_FMUL:
    case CG_OP_FDIV:
    case CG_OP_ICMP:
    case CG_OP_FCMP:
        uses_a = uses_b = true;
        break;
    case CG_OP_FNEG:
    case CG_OP_ZEXT:
    case CG_OP_SEXT:
    case CG_OP_TRUNC:
    case CG_OP_SITOFP:
    case CG_OP_UITOFP:
    case CG_OP_FPTOSI:
    case CG_OP_FPTOUI:
    case CG_OP_FPEXT:
    case CG_OP_FPTRUNC:
    case CG_OP_BITCAST:
    case CG_OP_LOAD:
    case CG_OP_CONDBR:
        uses_a = true;
        break;
    case CG_OP_STORE:
    case CG_OP_MEMCPY:
    case CG_OP_MEMSET:
    case CG_OP_XCHG:
    case CG_OP_XADD:
        uses_a = uses_b = true;
        break;
    case CG_OP_CAS:
        uses_a = uses_b = uses_c = true;
        break;
    case CG_OP_SELECT:
        uses_a = uses_b = uses_c = true;
        break;
    case CG_OP_RET:
        uses_a = inst->a != CG_INVALID;
        break;
    default:
        break;
    }
    if (uses_a && !cg_verify_value(verify, block, position, inst->a,
                                   "operand a is not a value")) {
        return;
    }
    if (uses_b && !cg_verify_value(verify, block, position, inst->b,
                                   "operand b is not a value")) {
        return;
    }
    if (uses_c && !cg_verify_value(verify, block, position, inst->c,
                                   "operand c is not a value")) {
        return;
    }
    if (uses_a) {
        type_a = cg_value_type(func, inst->a);
        if (!cg_verify_dominates_use(verify, inst->a, block, position)) {
            cg_verify_fail(verify, block, position,
                           "operand a does not dominate its use");
        }
    }
    if (uses_b) {
        type_b = cg_value_type(func, inst->b);
        if (!cg_verify_dominates_use(verify, inst->b, block, position)) {
            cg_verify_fail(verify, block, position,
                           "operand b does not dominate its use");
        }
    }
    if (uses_c && !cg_verify_dominates_use(verify, inst->c, block, position)) {
        cg_verify_fail(verify, block, position,
                       "operand c does not dominate its use");
    }
    switch (inst->op) {
    case CG_OP_ICONST:
        if (!cg_type_is_int(inst->type) && inst->type != CG_T_PTR) {
            cg_verify_fail(verify, block, position, "iconst needs integer type");
        }
        break;
    case CG_OP_FCONST:
        if (!cg_type_is_float(inst->type)) {
            cg_verify_fail(verify, block, position, "fconst needs float type");
        }
        break;
    case CG_OP_PARAM:
        if (block != 0) {
            cg_verify_fail(verify, block, position, "param outside entry");
        }
        break;
    case CG_OP_SYMADDR:
        if (inst->imm < 0 || (uint64_t)inst->imm >= module->symbols.count) {
            cg_verify_fail(verify, block, position, "bad symbol");
        }
        break;
    case CG_OP_STACKSAVE:
        break;
    case CG_OP_STACKRESTORE:
        uses_a = true;
        break;
    case CG_OP_DYNALLOCA:
        uses_a = true;
        break;
    case CG_OP_VAAREA:
        if (!func->variadic) {
            cg_verify_fail(verify, block, position, "vaarea in a function that is not variadic");
        }
        break;
    case CG_OP_ALLOCA:
        if (block != 0) {
            cg_verify_fail(verify, block, position, "alloca outside entry");
        }
        if (inst->a > 12 || inst->imm < 0 || inst->imm > INT32_MAX / 4) {
            cg_verify_fail(verify, block, position,
                           "alloca alignment or size unsupported");
        }
        break;
    case CG_OP_ADD:
    case CG_OP_SUB:
    case CG_OP_MUL:
    case CG_OP_SDIV:
    case CG_OP_UDIV:
    case CG_OP_SREM:
    case CG_OP_UREM:
    case CG_OP_AND:
    case CG_OP_OR:
    case CG_OP_XOR:
        if (!(cg_type_is_int(type_a) || type_a == CG_T_PTR) || type_a != type_b ||
            inst->type != type_a) {
            cg_verify_fail(verify, block, position,
                           "integer binary op needs matching integer operands");
        }
        break;
    case CG_OP_SHL:
    case CG_OP_LSHR:
    case CG_OP_ASHR:
        if (!cg_type_is_int(type_a) || type_a != type_b ||
            inst->type != type_a) {
            cg_verify_fail(verify, block, position,
                           "shift needs matching integer operands");
        }
        break;
    case CG_OP_FADD:
    case CG_OP_FSUB:
    case CG_OP_FMUL:
    case CG_OP_FDIV:
    case CG_OP_FCMP:
        if (!cg_type_is_float(type_a) || type_a != type_b ||
            (inst->op != CG_OP_FCMP && inst->type != type_a) ||
            (inst->op == CG_OP_FCMP &&
             (inst->type != CG_T_I1 || inst->cc >= CG_FCMP_COUNT))) {
            cg_verify_fail(verify, block, position,
                           "float op needs matching float operands");
        }
        break;
    case CG_OP_FNEG:
        if (!cg_type_is_float(type_a) || inst->type != type_a) {
            cg_verify_fail(verify, block, position, "fneg needs a float");
        }
        break;
    case CG_OP_ICMP:
        if (!(cg_type_is_int(type_a) || type_a == CG_T_PTR) || type_a != type_b ||
            inst->type != CG_T_I1 || inst->cc >= CG_ICMP_COUNT) {
            cg_verify_fail(verify, block, position,
                           "icmp needs matching integer operands");
        }
        break;
    case CG_OP_SELECT:
        if (type_a != CG_T_I1 || type_b != cg_value_type(func, inst->c) ||
            inst->type != type_b || type_b == CG_T_VOID) {
            cg_verify_fail(verify, block, position,
                           "select needs an i1 condition and matching arms");
        }
        break;
    case CG_OP_ZEXT:
    case CG_OP_SEXT:
        if (!cg_type_is_int(type_a) || !cg_type_is_int(inst->type) ||
            cg_type_size(inst->type) <= cg_type_size(type_a)) {
            if (!(type_a == CG_T_I1 && inst->type == CG_T_I8)) {
                cg_verify_fail(verify, block, position,
                               "extension needs a wider integer result");
            }
        }
        break;
    case CG_OP_TRUNC:
        if (!cg_type_is_int(type_a) || !cg_type_is_int(inst->type) ||
            cg_type_size(inst->type) > cg_type_size(type_a) ||
            (inst->type == type_a)) {
            cg_verify_fail(verify, block, position,
                           "trunc needs a narrower integer result");
        }
        break;
    case CG_OP_SITOFP:
    case CG_OP_UITOFP:
        if (!cg_type_is_int(type_a) || !cg_type_is_float(inst->type) ||
            (inst->op == CG_OP_UITOFP && type_a == CG_T_I64)) {
            cg_verify_fail(verify, block, position,
                           "int to float conversion unsupported for operand");
        }
        break;
    case CG_OP_FPTOSI:
    case CG_OP_FPTOUI:
        if (!cg_type_is_float(type_a) || !cg_type_is_int(inst->type) ||
            (inst->op == CG_OP_FPTOUI && inst->type == CG_T_I64)) {
            cg_verify_fail(verify, block, position,
                           "float to int conversion unsupported for result");
        }
        break;
    case CG_OP_FPEXT:
        if (type_a != CG_T_F32 || inst->type != CG_T_F64) {
            cg_verify_fail(verify, block, position, "fpext is f32 to f64");
        }
        break;
    case CG_OP_FPTRUNC:
        if (type_a != CG_T_F64 || inst->type != CG_T_F32) {
            cg_verify_fail(verify, block, position, "fptrunc is f64 to f32");
        }
        break;
    case CG_OP_BITCAST:
        if (type_a == CG_T_VOID || inst->type == CG_T_VOID ||
            cg_type_size(type_a) != cg_type_size(inst->type) ||
            type_a == CG_T_I1 || inst->type == CG_T_I1) {
            cg_verify_fail(verify, block, position,
                           "bitcast needs equally sized non-i1 types");
        }
        break;
    case CG_OP_LOAD:
        if (type_a != CG_T_PTR || inst->type == CG_T_VOID) {
            cg_verify_fail(verify, block, position, "load needs a pointer");
        }
        break;
    case CG_OP_STORE:
        if (type_a != CG_T_PTR || type_b == CG_T_VOID || inst->type != type_b) {
            cg_verify_fail(verify, block, position,
                           "store needs a pointer and a value");
        }
        break;
    case CG_OP_MEMCPY:
        if (type_a != CG_T_PTR || type_b != CG_T_PTR || inst->imm < 0) {
            cg_verify_fail(verify, block, position, "memcpy needs pointers");
        }
        break;
    case CG_OP_MEMSET:
        if (type_a != CG_T_PTR || !cg_type_is_int(type_b) || inst->imm < 0) {
            cg_verify_fail(verify, block, position, "memset needs ptr, int");
        }
        break;
    case CG_OP_XCHG:
    case CG_OP_XADD:
    case CG_OP_CAS:
        if (type_a != CG_T_PTR || !(cg_type_is_int(type_b) || type_b == CG_T_PTR) ||
            type_b == CG_T_I1 || inst->type != type_b ||
            (inst->op == CG_OP_CAS && cg_value_type(func, inst->c) != type_b) ||
            (inst->op == CG_OP_XADD && type_b == CG_T_PTR)) {
            cg_verify_fail(verify, block, position, "atomic operation needs a pointer and matching integers");
        }
        break;
    case CG_OP_CALL:
        if (inst->imm >= 0) {
            if ((uint64_t)inst->imm >= module->symbols.count) {
                cg_verify_fail(verify, block, position, "bad callee symbol");
            }
        } else if (!cg_verify_value(verify, block, position, inst->a,
                                    "bad indirect callee") ||
                   cg_value_type(func, inst->a) != CG_T_PTR) {
            cg_verify_fail(verify, block, position, "callee must be a ptr");
        }
        if (inst->b + inst->c > func->operands.count) {
            cg_verify_fail(verify, block, position, "bad call operands");
            break;
        }
        for (uint32_t index = 0; index < inst->c; ++index) {
            cg_value_t argument = func->operands.data[inst->b + index];
            if (argument >= func->values.count) {
                cg_verify_fail(verify, block, position, "bad call argument");
            } else if (!cg_verify_dominates_use(verify, argument, block,
                                                position)) {
                cg_verify_fail(verify, block, position,
                               "call argument does not dominate its use");
            }
        }
        break;
    case CG_OP_PHI:
        if (inst->type == CG_T_VOID) {
            cg_verify_fail(verify, block, position, "phi needs a type");
        }
        break;
    case CG_OP_BR:
        if (inst->a >= func->blocks.count || inst->a == 0) {
            cg_verify_fail(verify, block, position, "bad branch target");
        }
        break;
    case CG_OP_CONDBR:
        if (type_a != CG_T_I1 || inst->b >= func->blocks.count ||
            inst->c >= func->blocks.count || inst->b == 0 || inst->c == 0) {
            cg_verify_fail(verify, block, position,
                           "condbr needs an i1 and valid targets");
        }
        break;
    case CG_OP_RET:
        if (func->ret_type == CG_T_VOID) {
            if (inst->a != CG_INVALID) {
                cg_verify_fail(verify, block, position,
                               "void function returns a value");
            }
        } else if (inst->a == CG_INVALID || type_a != func->ret_type) {
            cg_verify_fail(verify, block, position, "return type mismatch");
        }
        break;
    default:
        break;
    }
}

static void cg_verify_phi(cg_verify_t *verify, cg_block_t block,
                          uint32_t position, const cg_inst_t *inst) {
    const cg_func_t *func = verify->func;
    uint32_t pred_count = verify->cfg.pred_start[block + 1] -
                          verify->cfg.pred_start[block];
    if (inst->b + inst->c * 2 > func->operands.count) {
        cg_verify_fail(verify, block, position, "bad phi operands");
        return;
    }
    if (block == 0) {
        cg_verify_fail(verify, block, position, "phi in the entry block");
        return;
    }
    /* Edges from unreachable blocks are ignored; every reachable predecessor
     * must appear exactly once. */
    uint32_t reachable_incoming = 0;
    for (uint32_t index = 0; index < inst->c; ++index) {
        cg_value_t value = func->operands.data[inst->b + index * 2];
        cg_block_t from = func->operands.data[inst->b + index * 2 + 1];
        if (value >= func->values.count || from >= func->blocks.count) {
            cg_verify_fail(verify, block, position, "bad phi incoming");
            return;
        }
        if (cg_value_type(func, value) != inst->type) {
            cg_verify_fail(verify, block, position, "phi type mismatch");
        }
        if (verify->cfg.rpo_index[from] == CG_INVALID) {
            continue;
        }
        ++reachable_incoming;
        bool is_pred = false;
        for (uint32_t p = verify->cfg.pred_start[block];
             p < verify->cfg.pred_start[block + 1]; ++p) {
            is_pred |= verify->cfg.pred_list[p] == from;
        }
        if (!is_pred) {
            cg_verify_fail(verify, block, position,
                           "phi incoming block is not a predecessor");
        }
        /* The incoming value must be available at the end of `from`. */
        if (verify->def_block[value] == CG_INVALID ||
            (verify->def_block[value] != from &&
             !cg_cfg_dominates(&verify->cfg, verify->def_block[value], from))) {
            cg_verify_fail(verify, block, position,
                           "phi incoming value does not dominate its edge");
        }
    }
    if (reachable_incoming != pred_count) {
        cg_verify_fail(verify, block, position,
                       "phi does not cover every predecessor exactly once");
    }
}

bool cg_verify_function(const cg_module_t *module, const cg_func_t *func,
                        cg_diag_t *diag) {
    cg_verify_t verify = {.module = module, .func = func, .diag = diag,
                          .ok = true};
    uint32_t value_count = func->values.count;
    if (func->blocks.count == 0) {
        cg_diag_error(diag, "%s: no blocks",
                      cg_symbol_name(module, func->symbol));
        return false;
    }
    verify.def_block = malloc((value_count + 1) * sizeof(uint32_t));
    verify.def_index = malloc((value_count + 1) * sizeof(uint32_t));
    if (verify.def_block == nullptr || verify.def_index == nullptr ||
        !cg_cfg_build(&verify.cfg, func)) {
        free(verify.def_block);
        free(verify.def_index);
        cg_diag_error(diag, "out of memory in verifier");
        return false;
    }
    memset(verify.def_block, 0xFF, (value_count + 1) * sizeof(uint32_t));
    for (cg_block_t block = 0; block < func->blocks.count; ++block) {
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            if (inst->dst != CG_INVALID) {
                if (inst->dst >= value_count ||
                    verify.def_block[inst->dst] != CG_INVALID) {
                    cg_verify_fail(&verify, block, index,
                                   "value defined twice or out of range");
                    continue;
                }
                verify.def_block[inst->dst] = block;
                verify.def_index[inst->dst] = index;
            }
        }
    }
    for (cg_block_t block = 0; block < func->blocks.count; ++block) {
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        bool reachable = verify.cfg.rpo_index[block] != CG_INVALID;
        bool in_phis = true;
        if (ir_block->insts.count == 0) {
            cg_verify_fail(&verify, block, 0, "empty block");
            continue;
        }
        for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            bool is_last = index + 1 == ir_block->insts.count;
            bool is_term = (cg_op_flags(inst->op) & CG_OPF_TERM) != 0;
            if (is_term != is_last) {
                cg_verify_fail(&verify, block, index,
                               is_term ? "terminator in the middle of a block"
                                       : "block does not end in a terminator");
            }
            if (inst->op == CG_OP_PHI) {
                if (!in_phis) {
                    cg_verify_fail(&verify, block, index,
                                   "phi after a non-phi instruction");
                }
            } else if (inst->op != CG_OP_PARAM) {
                in_phis = false;
            }
            if (inst->op == CG_OP_PARAM && block == 0 &&
                (inst->imm < 0 || (uint64_t)inst->imm >= func->param_count)) {
                cg_verify_fail(&verify, block, index, "bad param index");
            }
            if (!reachable) {
                continue; /* unreachable code is dropped by the back end */
            }
            cg_verify_inst(&verify, block, index, inst);
            if (inst->op == CG_OP_PHI) {
                cg_verify_phi(&verify, block, index, inst);
            }
        }
    }
    if (verify.cfg.pred_start[1] != verify.cfg.pred_start[0]) {
        cg_verify_fail(&verify, 0, 0, "the entry block has predecessors");
    }
    cg_cfg_free(&verify.cfg);
    free(verify.def_block);
    free(verify.def_index);
    return verify.ok;
}

bool cg_verify_module(const cg_module_t *module, cg_diag_t *diag) {
    bool ok = true;
    for (uint32_t index = 0; index < module->funcs.count; ++index) {
        ok &= cg_verify_function(module, module->funcs.data[index], diag);
    }
    return ok;
}
