#include "ir.h"

#include <stdlib.h>
#include <string.h>

/* Promotes entry-block allocas that are only loaded from and stored to (whole
 * object, offset 0, one scalar type) to SSA values. Phi nodes are placed with
 * the lazy scheme of Braun et al. ("Simple and Efficient Construction of SSA
 * Form"): the CFG is complete, so every block is already sealed.
 *
 * Removed instructions stay in the instruction pool but are dropped from the
 * block lists; users of removed loads are rewritten through `repl`. */

typedef void (*slot_fn)(uint32_t *slot, uint32_t index, void *user);

enum { SLOT_PHI = 1000 };

static void visit_slots(cg_func_t *func, cg_inst_t *inst, slot_fn fn, void *user) {
    switch (inst->op) {
    case CG_OP_ADD: case CG_OP_SUB: case CG_OP_MUL: case CG_OP_SDIV:
    case CG_OP_UDIV: case CG_OP_SREM: case CG_OP_UREM: case CG_OP_AND:
    case CG_OP_OR: case CG_OP_XOR: case CG_OP_SHL: case CG_OP_LSHR:
    case CG_OP_ASHR: case CG_OP_FADD: case CG_OP_FSUB: case CG_OP_FMUL:
    case CG_OP_FDIV: case CG_OP_ICMP: case CG_OP_FCMP: case CG_OP_STORE:
    case CG_OP_MEMCPY: case CG_OP_MEMSET:
        fn(&inst->a, 0, user);
        fn(&inst->b, 1, user);
        break;
    case CG_OP_STACKRESTORE: case CG_OP_DYNALLOCA:
    case CG_OP_FNEG: case CG_OP_ZEXT: case CG_OP_SEXT: case CG_OP_TRUNC:
    case CG_OP_SITOFP: case CG_OP_UITOFP: case CG_OP_FPTOSI: case CG_OP_FPTOUI:
    case CG_OP_FPEXT: case CG_OP_FPTRUNC: case CG_OP_BITCAST: case CG_OP_LOAD:
    case CG_OP_CONDBR:
        fn(&inst->a, 0, user);
        break;
    case CG_OP_XCHG: case CG_OP_XADD:
        fn(&inst->a, 0, user);
        fn(&inst->b, 1, user);
        break;
    case CG_OP_CAS: case CG_OP_SELECT:
        fn(&inst->a, 0, user);
        fn(&inst->b, 1, user);
        fn(&inst->c, 2, user);
        break;
    case CG_OP_RET:
        if (inst->a != CG_INVALID) {
            fn(&inst->a, 0, user);
        }
        break;
    case CG_OP_CALL:
        if (inst->imm == -1) {
            fn(&inst->a, 0, user);
        }
        for (uint32_t i = 0; i < inst->c; ++i) {
            fn(&func->operands.data[inst->b + i], 2 + i, user);
        }
        break;
    case CG_OP_PHI:
        for (uint32_t i = 0; i < inst->c; ++i) {
            fn(&func->operands.data[inst->b + i * 2], SLOT_PHI, user);
        }
        break;
    default:
        break;
    }
}

typedef struct m2r {
    cg_func_t *func;
    uint32_t vars, blocks, values0, insts0;
    uint32_t *var_of;       /* value -> variable or CG_INVALID */
    cg_type_t *var_type;
    uint8_t *var_ok;
    uint32_t *var_undef;    /* per variable: zero constant */
    uint32_t *pred_start, *pred_list;
    uint32_t *last_store;   /* [var * blocks + block] raw stored value */
    uint32_t *in_def;
    uint32_t *repl;
    uint8_t *dead;
    uint32_t *phi_ids;      /* created phi instructions */
    uint32_t *phi_block;
    uint32_t *phi_var;
    uint32_t phi_count;
    bool failed;
} m2r_t;

static uint32_t m2r_find(const m2r_t *m, uint32_t value) {
    while (value < m->values0 + m->vars * m->blocks + m->vars &&
           m->repl[value] != CG_INVALID) {
        value = m->repl[value];
    }
    return value;
}

typedef struct scan_state {
    m2r_t *m;
    cg_inst_t *inst;
} scan_state_t;

static void scan_slot(uint32_t *slot, uint32_t index, void *user) {
    scan_state_t *state = user;
    m2r_t *m = state->m;
    cg_inst_t *inst = state->inst;
    uint32_t var;
    if (*slot >= m->values0 || (var = m->var_of[*slot]) == CG_INVALID) {
        return;
    }
    if (inst->op == CG_OP_LOAD && index == 0 && inst->imm == 0) {
        if (m->var_type[var] == CG_T_VOID) {
            m->var_type[var] = (cg_type_t)inst->type;
        } else if (m->var_type[var] != (cg_type_t)inst->type) {
            m->var_ok[var] = 0;
        }
        return;
    }
    if (inst->op == CG_OP_STORE && index == 0 && inst->imm == 0 &&
        cg_value_type(m->func, inst->b) == (cg_type_t)inst->type) {
        if (m->var_type[var] == CG_T_VOID) {
            m->var_type[var] = (cg_type_t)inst->type;
        } else if (m->var_type[var] != (cg_type_t)inst->type) {
            m->var_ok[var] = 0;
        }
        return;
    }
    m->var_ok[var] = 0;
}

static uint32_t read_in(m2r_t *m, uint32_t var, uint32_t block);

static uint32_t read_out(m2r_t *m, uint32_t var, uint32_t block) {
    uint32_t stored = m->last_store[var * m->blocks + block];
    return stored != CG_INVALID ? stored : read_in(m, var, block);
}

static uint32_t read_in(m2r_t *m, uint32_t var, uint32_t block) {
    uint32_t index = var * m->blocks + block;
    uint32_t count = m->pred_start[block + 1] - m->pred_start[block];
    cg_func_t *func = m->func;
    if (m->in_def[index] != CG_INVALID) {
        return m->in_def[index];
    }
    if (count == 0) {
        m->in_def[index] = m->var_undef[var];
        return m->in_def[index];
    }
    if (count == 1) {
        uint32_t value;
        value = read_out(m, var, m->pred_list[m->pred_start[block]]);
        m->in_def[index] = value;
        return value;
    }
    {
        cg_inst_id_t id = func->insts.count;
        uint32_t value = func->values.count;
        cg_value_info_t info = {.type = m->var_type[var], .def = id};
        cg_inst_t phi = {.op = CG_OP_PHI, .type = m->var_type[var], .dst = value};
        uint32_t *incoming = malloc(count * sizeof(uint32_t));
        uint32_t start;
        if (incoming == nullptr) {
            m->failed = true;
            return m->var_undef[var];
        }
        cg_vec_push(&func->values, info);
        cg_vec_push(&func->insts, phi);
        m->phi_ids[m->phi_count] = id;
        m->phi_block[m->phi_count] = block;
        m->phi_var[m->phi_count] = var;
        ++m->phi_count;
        m->in_def[index] = value;
        for (uint32_t i = 0; i < count; ++i) {
            incoming[i] = read_out(m, var, m->pred_list[m->pred_start[block] + i]);
        }
        start = func->operands.count;
        for (uint32_t i = 0; i < count; ++i) {
            cg_vec_push(&func->operands, incoming[i]);
            cg_vec_push(&func->operands, m->pred_list[m->pred_start[block] + i]);
        }
        func->insts.data[id].b = start;
        func->insts.data[id].c = count;
        free(incoming);
        return value;
    }
}

static void rewrite_slot(uint32_t *slot, uint32_t index, void *user) {
    (void)index;
    *slot = m2r_find(user, *slot);
}

static bool mem2reg_run(cg_func_t *func) {
    m2r_t m = {.func = func};
    uint32_t nblocks = func->blocks.count;
    uint32_t *stack = nullptr;
    uint8_t *reachable = nullptr;
    uint32_t *run = nullptr;
    uint32_t promoted = 0;
    bool ok = false;
    if (nblocks == 0) {
        return true;
    }
    m.blocks = nblocks;
    m.values0 = func->values.count;
    m.insts0 = func->insts.count;
    m.var_of = malloc((m.values0 + 1) * sizeof(uint32_t));
    if (m.var_of == nullptr) {
        return false;
    }
    memset(m.var_of, 0xFF, (m.values0 + 1) * sizeof(uint32_t));
    /* candidates: allocas of the entry block */
    {
        const cg_ir_block_t *entry = &func->blocks.data[0];
        for (uint32_t i = 0; i < entry->insts.count; ++i) {
            const cg_inst_t *inst = &func->insts.data[entry->insts.data[i]];
            if (inst->op == CG_OP_ALLOCA && inst->dst != CG_INVALID) {
                m.var_of[inst->dst] = m.vars++;
            }
        }
    }
    if (m.vars == 0 || (uint64_t)m.vars * nblocks > (1u << 23)) {
        free(m.var_of);
        return true;
    }
    {
        size_t total = (size_t)m.vars * nblocks;
        m.var_type = calloc(m.vars, sizeof(cg_type_t));
        m.var_ok = malloc(m.vars);
        m.var_undef = malloc(m.vars * sizeof(uint32_t));
        m.last_store = malloc(total * sizeof(uint32_t));
        m.in_def = malloc(total * sizeof(uint32_t));
        m.repl = malloc((m.values0 + total + m.vars) * sizeof(uint32_t));
        m.dead = calloc(m.insts0 + total + 2 * m.vars + 1, 1);
        m.phi_ids = malloc((total + 1) * sizeof(uint32_t));
        m.phi_block = malloc((total + 1) * sizeof(uint32_t));
        m.phi_var = malloc((total + 1) * sizeof(uint32_t));
        m.pred_start = calloc(nblocks + 2, sizeof(uint32_t));
        reachable = calloc(nblocks, 1);
        stack = malloc(nblocks * sizeof(uint32_t));
        run = malloc(m.vars * sizeof(uint32_t));
        if (!m.var_type || !m.var_ok || !m.var_undef || !m.last_store || !m.in_def ||
            !m.repl || !m.dead || !m.phi_ids || !m.phi_block || !m.phi_var ||
            !m.pred_start || !reachable || !stack || !run) {
            goto done;
        }
        memset(m.var_ok, 1, m.vars);
        memset(m.last_store, 0xFF, total * sizeof(uint32_t));
        memset(m.in_def, 0xFF, total * sizeof(uint32_t));
        memset(m.repl, 0xFF, (m.values0 + total + m.vars) * sizeof(uint32_t));
    }
    /* uses: anything but a plain whole-object load/store disqualifies */
    for (uint32_t b = 0; b < nblocks; ++b) {
        const cg_ir_block_t *block = &func->blocks.data[b];
        for (uint32_t i = 0; i < block->insts.count; ++i) {
            scan_state_t state = {&m, &func->insts.data[block->insts.data[i]]};
            visit_slots(func, state.inst, scan_slot, &state);
        }
    }
    for (uint32_t v = 0; v < m.vars; ++v) {
        promoted += m.var_ok[v];
    }
    if (promoted == 0) {
        ok = true;
        goto done;
    }
    /* the entry block must not be a branch target */
    {
        uint32_t top = 0;
        stack[top++] = 0;
        reachable[0] = 1;
        while (top != 0) {
            uint32_t b = stack[--top];
            const cg_ir_block_t *block = &func->blocks.data[b];
            const cg_inst_t *term;
            uint32_t succ[2];
            uint32_t n = 0;
            if (block->insts.count == 0) {
                continue;
            }
            term = &func->insts.data[block->insts.data[block->insts.count - 1]];
            if (term->op == CG_OP_BR) {
                succ[n++] = term->a;
            } else if (term->op == CG_OP_CONDBR) {
                succ[n++] = term->b;
                if (term->c != term->b) {
                    succ[n++] = term->c;
                }
            }
            for (uint32_t k = 0; k < n; ++k) {
                if (succ[k] == 0) {
                    ok = true; /* nothing to do */
                    goto done;
                }
                if (succ[k] < nblocks) {
                    ++m.pred_start[succ[k] + 1];
                    if (!reachable[succ[k]]) {
                        reachable[succ[k]] = 1;
                        stack[top++] = succ[k];
                    }
                }
            }
        }
    }
    for (uint32_t b = 0; b < nblocks; ++b) {
        m.pred_start[b + 1] += m.pred_start[b];
    }
    m.pred_list = malloc((m.pred_start[nblocks] + 1) * sizeof(uint32_t));
    {
        uint32_t *fill = malloc((nblocks + 1) * sizeof(uint32_t));
        if (m.pred_list == nullptr || fill == nullptr) {
            free(fill);
            goto done;
        }
        memcpy(fill, m.pred_start, nblocks * sizeof(uint32_t));
        for (uint32_t b = 0; b < nblocks; ++b) {
            const cg_ir_block_t *block = &func->blocks.data[b];
            const cg_inst_t *term;
            if (!reachable[b] || block->insts.count == 0) {
                continue;
            }
            term = &func->insts.data[block->insts.data[block->insts.count - 1]];
            if (term->op == CG_OP_BR && term->a < nblocks) {
                m.pred_list[fill[term->a]++] = b;
            } else if (term->op == CG_OP_CONDBR) {
                if (term->b < nblocks) {
                    m.pred_list[fill[term->b]++] = b;
                }
                if (term->c != term->b && term->c < nblocks) {
                    m.pred_list[fill[term->c]++] = b;
                }
            }
        }
        free(fill);
    }
    /* zero constants for variables that are read */
    {
        uint32_t constants = 0;
        for (uint32_t v = 0; v < m.vars; ++v) {
            cg_type_t type = m.var_type[v];
            cg_inst_t inst;
            cg_value_info_t info;
            if (!m.var_ok[v] || type == CG_T_VOID) {
                m.var_undef[v] = CG_INVALID;
                continue;
            }
            inst = (cg_inst_t){.op = cg_type_is_float(type) ? CG_OP_FCONST : CG_OP_ICONST,
                               .type = type};
            inst.dst = func->values.count;
            info = (cg_value_info_t){.type = type, .def = func->insts.count};
            m.var_undef[v] = inst.dst;
            cg_vec_push(&func->values, info);
            cg_vec_push(&func->insts, inst);
            m.dead[func->insts.count - 1] = 0;
            ++constants;
        }
        (void)constants;
    }
    /* last store of each variable per block */
    for (uint32_t b = 0; b < nblocks; ++b) {
        const cg_ir_block_t *block = &func->blocks.data[b];
        for (uint32_t i = 0; i < block->insts.count; ++i) {
            const cg_inst_t *inst = &func->insts.data[block->insts.data[i]];
            if (inst->op == CG_OP_STORE && inst->a < m.values0 &&
                m.var_of[inst->a] != CG_INVALID && m.var_ok[m.var_of[inst->a]]) {
                m.last_store[m.var_of[inst->a] * nblocks + b] = inst->b;
            }
        }
    }
    /* walk the blocks: loads become the reaching definition */
    for (uint32_t b = 0; b < nblocks; ++b) {
        const cg_ir_block_t *block = &func->blocks.data[b];
        uint32_t count = block->insts.count;
        for (uint32_t v = 0; v < m.vars; ++v) {
            run[v] = CG_INVALID;
        }
        for (uint32_t i = 0; i < count; ++i) {
            cg_inst_id_t id = func->blocks.data[b].insts.data[i];
            cg_inst_t inst = func->insts.data[id];
            uint32_t var = CG_INVALID;
            if (inst.op == CG_OP_ALLOCA && inst.dst < m.values0 &&
                m.var_of[inst.dst] != CG_INVALID && m.var_ok[m.var_of[inst.dst]]) {
                m.dead[id] = 1;
            } else if ((inst.op == CG_OP_LOAD || inst.op == CG_OP_STORE) &&
                       inst.a < m.values0 && (var = m.var_of[inst.a]) != CG_INVALID &&
                       m.var_ok[var]) {
                m.dead[id] = 1;
                if (inst.op == CG_OP_STORE) {
                    run[var] = inst.b;
                } else {
                    uint32_t value = run[var] != CG_INVALID ? run[var] : read_in(&m, var, b);
                    m.repl[inst.dst] = value;
                }
            }
        }
    }
    if (m.failed || func->insts.oom || func->values.oom || func->operands.oom) {
        goto done;
    }
    /* remove trivial phis */
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t p = 0; p < m.phi_count; ++p) {
            const cg_inst_t *phi = &func->insts.data[m.phi_ids[p]];
            uint32_t same = CG_INVALID;
            bool trivial = true;
            if (m.dead[m.phi_ids[p]]) {
                continue;
            }
            for (uint32_t i = 0; i < phi->c && trivial; ++i) {
                uint32_t value = m2r_find(&m, func->operands.data[phi->b + i * 2]);
                if (value == phi->dst) {
                    continue;
                }
                if (same == CG_INVALID) {
                    same = value;
                } else if (same != value) {
                    trivial = false;
                }
            }
            if (trivial) {
                m.repl[phi->dst] = same != CG_INVALID ? same : m.var_undef[m.phi_var[p]];
                m.dead[m.phi_ids[p]] = 1;
                changed = true;
            }
        }
    }
    /* rebuild block lists */
    for (uint32_t b = 0; b < nblocks; ++b) {
        cg_ir_block_t *block = &func->blocks.data[b];
        cg_u32_vec_t rebuilt = {0};
        bool undefs_done = b != 0;
        for (uint32_t p = 0; p < m.phi_count; ++p) {
            if (m.phi_block[p] == b && !m.dead[m.phi_ids[p]]) {
                cg_vec_push(&rebuilt, m.phi_ids[p]);
            }
        }
        for (uint32_t i = 0; i < block->insts.count; ++i) {
            cg_inst_id_t id = block->insts.data[i];
            if (!undefs_done && func->insts.data[id].op != CG_OP_PARAM) {
                for (uint32_t v = 0; v < m.vars; ++v) {
                    if (m.var_undef[v] != CG_INVALID) {
                        cg_vec_push(&rebuilt, func->values.data[m.var_undef[v]].def);
                    }
                }
                undefs_done = true;
            }
            if (id < m.insts0 && m.dead[id]) {
                continue;
            }
            cg_vec_push(&rebuilt, id);
        }
        if (!undefs_done) {
            for (uint32_t v = 0; v < m.vars; ++v) {
                if (m.var_undef[v] != CG_INVALID) {
                    cg_vec_push(&rebuilt, func->values.data[m.var_undef[v]].def);
                }
            }
        }
        free(block->insts.data);
        block->insts = rebuilt;
    }
    /* users of removed values */
    for (uint32_t b = 0; b < nblocks; ++b) {
        const cg_ir_block_t *block = &func->blocks.data[b];
        for (uint32_t i = 0; i < block->insts.count; ++i) {
            visit_slots(func, &func->insts.data[block->insts.data[i]], rewrite_slot, &m);
        }
    }
    ok = !func->insts.oom && !func->values.oom && !func->operands.oom;
done:
    free(m.var_of);
    free(m.var_type);
    free(m.var_ok);
    free(m.var_undef);
    free(m.pred_start);
    free(m.pred_list);
    free(m.last_store);
    free(m.in_def);
    free(m.repl);
    free(m.dead);
    free(m.phi_ids);
    free(m.phi_block);
    free(m.phi_var);
    free(reachable);
    free(stack);
    free(run);
    return ok;
}

bool cg_mem2reg(cg_func_t *func) {
    return mem2reg_run(func);
}
