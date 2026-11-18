#include "x64.h"

/* Register allocation: SSA-free linear scan over live intervals with fixed
 * (physical) register constraints.
 *
 *  1. number the instructions in layout order (use at 2i, def at 2i+1);
 *  2. block level liveness of virtual registers (bit sets);
 *  3. one interval [start, end] per virtual register;
 *  4. busy ranges of physical registers from explicit uses, defs and
 *     implicit clobbers (calls, division, string moves);
 *  5. linear scan: a register is free when no active interval holds it and
 *     no busy range overlaps; hints remove most copies; the interval ending
 *     last is spilled when nothing is free (whole interval spills);
 *  6. rewrite, folding spilled operands into memory operands where the
 *     instruction allows it, otherwise reloading through scratch registers.
 */

typedef struct ra_range {
    uint32_t begin;
    uint32_t end;
} ra_range_t;

typedef struct ra_state {
    x64_func_t *func;
    const x64_abi_t *abi;
    uint32_t vreg_count;
    uint32_t *block_first; /* first instruction number of each block */
    uint32_t *block_last;
    CG_VEC(ra_range_t) busy[X64_PHYS_COUNT];
    uint32_t *order;   /* vregs sorted by interval start */
    uint32_t *active;  /* assigned vregs whose interval is still open */
    uint32_t active_count;
} ra_state_t;

static bool inst_reg_role(const x64_inst_t *inst, uint32_t operand,
                          x64_role_t *role) {
    const x64_opinfo_t *info = x64_opinfo((x64_op_t)inst->op);
    if (inst->ops[operand].kind != X64_O_REG) {
        return false;
    }
    *role = (x64_role_t)info->roles[operand];
    return true;
}

/* Calls `visit(context, reg, is_def, is_use)` for every register mentioned. */
typedef void (*ra_visit_t)(void *context, uint32_t reg, bool is_def, bool is_use);

static void for_each_reg(const x64_inst_t *inst, ra_visit_t visit, void *context) {
    for (uint32_t index = 0; index < 3; ++index) {
        const x64_operand_t *operand = &inst->ops[index];
        x64_role_t role;
        if (operand->kind == X64_O_MEM) {
            if (operand->base_kind == X64_MB_REG && operand->reg != X64_NOREG) {
                visit(context, operand->reg, false, true);
            }
            if (operand->index != X64_NOREG) {
                visit(context, operand->index, false, true);
            }
        } else if (inst_reg_role(inst, index, &role)) {
            visit(context, operand->reg, role == X64_ROLE_D || role == X64_ROLE_M,
                  role == X64_ROLE_U || role == X64_ROLE_M);
        }
    }
}

/* ---- liveness ------------------------------------------------------------ */

typedef struct ra_scan {
    cg_bitset_t *use;
    cg_bitset_t *def;
    cg_bitset_t *killed;
} ra_scan_t;

typedef struct ra_block_sets {
    cg_bitset_t use;
    cg_bitset_t def;
    cg_bitset_t live_in;
    cg_bitset_t live_out;
} ra_block_sets_t;

typedef struct ra_collect {
    ra_block_sets_t *sets;
} ra_collect_t;

static void collect_visit(void *context, uint32_t reg, bool is_def, bool is_use) {
    ra_block_sets_t *sets = context;
    if (!x64_is_virtual(reg)) {
        return;
    }
    {
        uint32_t index = reg - X64_VREG_BASE;
        if (is_use && !cg_bitset_test(&sets->def, index)) {
            cg_bitset_set(&sets->use, index);
        }
        if (is_def) {
            cg_bitset_set(&sets->def, index);
        }
    }
}

typedef struct ra_interval_ctx {
    x64_func_t *func;
    uint32_t position;
} ra_interval_ctx_t;

static void interval_visit(void *context, uint32_t reg, bool is_def, bool is_use) {
    ra_interval_ctx_t *ctx = context;
    x64_vreg_t *vreg;
    if (!x64_is_virtual(reg)) {
        return;
    }
    vreg = &ctx->func->vregs.data[reg - X64_VREG_BASE];
    if (is_use) {
        uint32_t position = ctx->position * 2;
        vreg->start = vreg->start < position ? vreg->start : position;
        vreg->end = vreg->end > position ? vreg->end : position;
    }
    if (is_def) {
        uint32_t position = ctx->position * 2 + 1;
        vreg->start = vreg->start < position ? vreg->start : position;
        vreg->end = vreg->end > position ? vreg->end : position;
    }
}

/* ---- physical register busy ranges ---------------------------------------- */

typedef struct ra_phys_ctx {
    uint32_t live_mask;
    uint32_t live_end[X64_PHYS_COUNT];
    uint32_t position;
    ra_state_t *state;
    bool def_phase;
} ra_phys_ctx_t;

static void phys_add_range(ra_state_t *state, uint32_t reg, uint32_t begin,
                           uint32_t end) {
    ra_range_t range = {begin, end};
    cg_vec_push(&state->busy[reg], range);
}

static void phys_def(ra_phys_ctx_t *ctx, uint32_t reg) {
    uint32_t position = ctx->position * 2 + 1;
    if (ctx->live_mask & x64_mask(reg)) {
        phys_add_range(ctx->state, reg, position, ctx->live_end[reg]);
        ctx->live_mask &= ~x64_mask(reg);
    } else {
        phys_add_range(ctx->state, reg, position, position);
    }
}

static void phys_use(ra_phys_ctx_t *ctx, uint32_t reg) {
    if (!(ctx->live_mask & x64_mask(reg))) {
        ctx->live_mask |= x64_mask(reg);
        ctx->live_end[reg] = ctx->position * 2;
    }
}

static void phys_visit(void *context, uint32_t reg, bool is_def, bool is_use) {
    ra_phys_ctx_t *ctx = context;
    if (reg >= X64_PHYS_COUNT) {
        return;
    }
    if (ctx->def_phase) {
        if (is_def) {
            phys_def(ctx, reg);
        }
    } else if (is_use) {
        phys_use(ctx, reg);
    }
}

static int range_compare(const void *left, const void *right) {
    const ra_range_t *a = left;
    const ra_range_t *b = right;
    if (a->begin != b->begin) {
        return a->begin < b->begin ? -1 : 1;
    }
    return a->end < b->end ? -1 : a->end > b->end ? 1 : 0;
}

static void normalize_ranges(ra_state_t *state) {
    for (uint32_t reg = 0; reg < X64_PHYS_COUNT; ++reg) {
        typeof(state->busy[reg]) *list = &state->busy[reg];
        uint32_t out = 0;
        if (list->count == 0) {
            continue;
        }
        qsort(list->data, list->count, sizeof(ra_range_t), range_compare);
        for (uint32_t index = 1; index < list->count; ++index) {
            if (list->data[index].begin <= list->data[out].end) {
                if (list->data[index].end > list->data[out].end) {
                    list->data[out].end = list->data[index].end;
                }
            } else {
                list->data[++out] = list->data[index];
            }
        }
        list->count = out + 1;
    }
}

static bool phys_is_busy(const ra_state_t *state, uint32_t reg, uint32_t begin,
                         uint32_t end) {
    const ra_range_t *data = state->busy[reg].data;
    uint32_t low = 0;
    uint32_t high = state->busy[reg].count;
    /* first range whose end >= begin */
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        if (data[middle].end < begin) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < state->busy[reg].count && data[low].begin <= end;
}

/* ---- allocation -------------------------------------------------------- */

/* Stable merge sort of the vregs by interval start (no global state). */
static bool sort_by_start(const x64_func_t *func, uint32_t *items, uint32_t count) {
    uint32_t *temp = malloc((count + 1) * sizeof(uint32_t));
    if (temp == nullptr) {
        return false;
    }
    for (uint32_t width = 1; width < count; width *= 2) {
        for (uint32_t low = 0; low < count; low += 2 * width) {
            uint32_t middle = low + width < count ? low + width : count;
            uint32_t high = low + 2 * width < count ? low + 2 * width : count;
            uint32_t left = low;
            uint32_t right = middle;
            uint32_t out = low;
            while (left < middle && right < high) {
                uint32_t a = items[left];
                uint32_t b = items[right];
                bool take_left = func->vregs.data[a].start <= func->vregs.data[b].start;
                temp[out++] = take_left ? items[left++] : items[right++];
            }
            while (left < middle) {
                temp[out++] = items[left++];
            }
            while (right < high) {
                temp[out++] = items[right++];
            }
        }
        memcpy(items, temp, count * sizeof(uint32_t));
    }
    free(temp);
    return true;
}

static uint32_t candidate_registers(const ra_state_t *state, bool is_xmm,
                                    uint32_t *out) {
    uint32_t count = 0;
    uint32_t callee_saved = state->abi->callee_saved;
    /* caller-saved registers first, they cost nothing in the prologue */
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t index = 0; index < 16; ++index) {
            uint32_t reg = is_xmm ? X64_XMM0 + index : index;
            bool allowed = is_xmm ? (state->abi->allocatable_xmm >> index) & 1
                                  : (state->abi->allocatable_gpr >> reg) & 1;
            bool saved = (callee_saved >> reg) & 1;
            if (allowed && saved == (pass == 1)) {
                out[count++] = reg;
            }
        }
    }
    return count;
}

static bool allocate_intervals(ra_state_t *state) {
    x64_func_t *func = state->func;
    uint32_t order_count = 0;
    uint32_t gpr_candidates[16];
    uint32_t xmm_candidates[16];
    uint32_t gpr_count = candidate_registers(state, false, gpr_candidates);
    uint32_t xmm_count = candidate_registers(state, true, xmm_candidates);
    for (uint32_t index = 0; index < state->vreg_count; ++index) {
        if (func->vregs.data[index].start != UINT32_MAX) {
            state->order[order_count++] = index;
        }
    }
    if (!sort_by_start(func, state->order, order_count)) {
        return false;
    }
    for (uint32_t cursor = 0; cursor < order_count; ++cursor) {
        uint32_t id = state->order[cursor];
        x64_vreg_t *current = &func->vregs.data[id];
        const uint32_t *candidates = current->is_xmm ? xmm_candidates
                                                     : gpr_candidates;
        uint32_t candidate_count = current->is_xmm ? xmm_count : gpr_count;
        uint32_t chosen = X64_NOREG;
        uint32_t used_mask = 0;
        /* expire intervals that ended before this one starts */
        for (uint32_t index = 0; index < state->active_count;) {
            if (func->vregs.data[state->active[index]].end < current->start) {
                state->active[index] = state->active[--state->active_count];
            } else {
                used_mask |= x64_mask((uint32_t)func->vregs.data[state->active[index]].assigned);
                ++index;
            }
        }
        /* a hint is tried first */
        if (current->hint != X64_NOREG) {
            uint32_t hint = current->hint;
            if (x64_is_virtual(hint)) {
                const x64_vreg_t *other = &func->vregs.data[hint - X64_VREG_BASE];
                hint = other->assigned >= 0 ? (uint32_t)other->assigned : X64_NOREG;
            }
            if (hint != X64_NOREG && hint < X64_PHYS_COUNT &&
                x64_is_xmm(hint) == (bool)current->is_xmm &&
                !(used_mask & x64_mask(hint)) &&
                !phys_is_busy(state, hint, current->start, current->end)) {
                for (uint32_t index = 0; index < candidate_count; ++index) {
                    if (candidates[index] == hint) {
                        chosen = hint;
                    }
                }
            }
        }
        for (uint32_t index = 0; chosen == X64_NOREG && index < candidate_count;
             ++index) {
            uint32_t reg = candidates[index];
            if (!(used_mask & x64_mask(reg)) &&
                !phys_is_busy(state, reg, current->start, current->end)) {
                chosen = reg;
            }
        }
        if (chosen == X64_NOREG) {
            /* Spill the interval that ends last among the compatible ones. */
            uint32_t victim = CG_INVALID;
            uint32_t victim_end = current->end;
            for (uint32_t index = 0; index < state->active_count; ++index) {
                const x64_vreg_t *other = &func->vregs.data[state->active[index]];
                bool compatible = false;
                for (uint32_t c = 0; c < candidate_count; ++c) {
                    compatible |= candidates[c] == (uint32_t)other->assigned;
                }
                if (compatible && other->end > victim_end &&
                    !phys_is_busy(state, (uint32_t)other->assigned, current->start,
                                  current->end)) {
                    victim = index;
                    victim_end = other->end;
                }
            }
            if (victim != CG_INVALID) {
                x64_vreg_t *spilled = &func->vregs.data[state->active[victim]];
                chosen = (uint32_t)spilled->assigned;
                spilled->assigned = -1;
                spilled->spill_slot = (int32_t)x64_new_slot(func, 8, 8);
                state->active[victim] = state->active[--state->active_count];
            }
        }
        if (chosen == X64_NOREG) {
            current->assigned = -1;
            current->spill_slot = (int32_t)x64_new_slot(func, 8, 8);
            continue;
        }
        current->assigned = (int32_t)chosen;
        state->active[state->active_count++] = id;
    }
    return true;
}

/* ---- rewrite ----------------------------------------------------------- */

typedef struct ra_rewrite {
    x64_func_t *func;
    x64_inst_vec_t *out;
} ra_rewrite_t;

static x64_operand_t slot_operand(int32_t slot) {
    return (x64_operand_t){.kind = X64_O_MEM, .base_kind = X64_MB_FRAME,
                           .index = X64_NOREG, .scale = 1, .aux = (uint32_t)slot};
}

static bool is_spilled(const x64_func_t *func, uint32_t reg) {
    return x64_is_virtual(reg) &&
           func->vregs.data[reg - X64_VREG_BASE].assigned < 0;
}

static int32_t slot_of(const x64_func_t *func, uint32_t reg) {
    return func->vregs.data[reg - X64_VREG_BASE].spill_slot;
}

static bool vreg_is_xmm(const x64_func_t *func, uint32_t reg) {
    return func->vregs.data[reg - X64_VREG_BASE].is_xmm;
}

static void push_inst(ra_rewrite_t *rewrite, x64_inst_t inst) {
    if (((inst.op == X64_MOV && inst.width == 8) || inst.op == X64_MOVAPS) &&
        inst.ops[0].kind == X64_O_REG && inst.ops[1].kind == X64_O_REG &&
        inst.ops[0].reg == inst.ops[1].reg) {
        return; /* self copy */
    }
    cg_vec_push(rewrite->out, inst);
}

static x64_inst_t simple_inst(x64_op_t op, uint32_t width, x64_operand_t a,
                              x64_operand_t b) {
    x64_inst_t inst = {.op = op, .width = (uint8_t)width};
    inst.ops[0] = a;
    inst.ops[1] = b;
    inst.ops[2] = (x64_operand_t){.index = X64_NOREG};
    return inst;
}

static x64_inst_t load_spill(const x64_func_t *func, uint32_t scratch,
                             uint32_t vreg) {
    return vreg_is_xmm(func, vreg)
               ? simple_inst(X64_MOVS, 8, x64_reg(scratch),
                             slot_operand(slot_of(func, vreg)))
               : simple_inst(X64_MOV, 8, x64_reg(scratch),
                             slot_operand(slot_of(func, vreg)));
}

static x64_inst_t store_spill(const x64_func_t *func, uint32_t scratch,
                              uint32_t vreg) {
    return vreg_is_xmm(func, vreg)
               ? simple_inst(X64_MOVS, 8, slot_operand(slot_of(func, vreg)),
                             x64_reg(scratch))
               : simple_inst(X64_MOV, 8, slot_operand(slot_of(func, vreg)),
                             x64_reg(scratch));
}

static uint32_t resolve(const x64_func_t *func, uint32_t reg) {
    if (x64_is_virtual(reg)) {
        const x64_vreg_t *vreg = &func->vregs.data[reg - X64_VREG_BASE];
        if (vreg->assigned >= 0) {
            return (uint32_t)vreg->assigned;
        }
    }
    return reg;
}

static bool op_allows_memory_source(x64_op_t op) {
    switch (op) {
    case X64_ADD: case X64_SUB: case X64_AND: case X64_OR: case X64_XOR:
    case X64_CMP: case X64_IMUL2: case X64_CMOVCC: case X64_MOVZX:
    case X64_MOVSX: case X64_ADDS: case X64_SUBS: case X64_MULS: case X64_DIVS:
    case X64_UCOMIS: case X64_CVTSI2S: case X64_CVTTS2SI: case X64_CVTS2S:
        return true;
    default:
        return false;
    }
}

static bool op_allows_memory_dest(x64_op_t op) {
    switch (op) {
    case X64_ADD: case X64_SUB: case X64_AND: case X64_OR: case X64_XOR:
    case X64_CMP: case X64_TEST: case X64_NEG: case X64_NOT: case X64_SHL:
    case X64_SHR: case X64_SAR: case X64_XCHG: case X64_XADD: case X64_CMPXCHG:
        return true;
    default:
        return false;
    }
}

static void rewrite_inst(ra_rewrite_t *rewrite, const x64_inst_t *original) {
    const x64_func_t *func = rewrite->func;
    x64_inst_t inst = *original;
    const x64_opinfo_t *info = x64_opinfo((x64_op_t)inst.op);
    uint32_t gpr_next = 0;
    uint32_t xmm_next = 0;
    struct {
        uint32_t vreg;
        uint32_t scratch;
        bool load;
        bool store;
    } map[4];
    uint32_t map_count = 0;
    /* Fast paths for plain copies. */
    if ((inst.op == X64_MOV && inst.width == 8) || inst.op == X64_MOVAPS) {
        x64_operand_t *dst = &inst.ops[0];
        x64_operand_t *src = &inst.ops[1];
        bool dst_spilled = dst->kind == X64_O_REG && is_spilled(func, dst->reg);
        bool src_spilled = src->kind == X64_O_REG && is_spilled(func, src->reg);
        bool xmm = inst.op == X64_MOVAPS;
        if (dst->kind == X64_O_REG && src->kind == X64_O_REG &&
            (dst_spilled || src_spilled)) {
            uint32_t scratch = xmm ? X64_XSCRATCH0 : X64_SCRATCH0;
            x64_op_t move = xmm ? X64_MOVS : X64_MOV;
            if (dst_spilled && src_spilled) {
                push_inst(rewrite, simple_inst(move, 8, x64_reg(scratch),
                                               slot_operand(slot_of(func, src->reg))));
                push_inst(rewrite, simple_inst(move, 8,
                                               slot_operand(slot_of(func, dst->reg)),
                                               x64_reg(scratch)));
            } else if (dst_spilled) {
                push_inst(rewrite, simple_inst(move, 8,
                                               slot_operand(slot_of(func, dst->reg)),
                                               x64_reg(resolve(func, src->reg))));
            } else {
                push_inst(rewrite, simple_inst(move, 8, x64_reg(resolve(func, dst->reg)),
                                               slot_operand(slot_of(func, src->reg))));
            }
            return;
        }
        if (dst->kind == X64_O_REG && dst_spilled && src->kind == X64_O_IMM &&
            src->imm >= INT32_MIN && src->imm <= INT32_MAX && !xmm) {
            push_inst(rewrite, simple_inst(X64_MOV, 8,
                                           slot_operand(slot_of(func, dst->reg)), *src));
            return;
        }
    }
    /* Operand folding of spilled values into memory operands. */
    {
        x64_operand_t *dst = &inst.ops[0];
        x64_operand_t *src = &inst.ops[1];
        bool dst_spilled = dst->kind == X64_O_REG && is_spilled(func, dst->reg);
        bool src_spilled = src->kind == X64_O_REG && is_spilled(func, src->reg);
        if (src_spilled && !dst_spilled && dst->kind == X64_O_REG &&
            op_allows_memory_source((x64_op_t)inst.op)) {
            x64_operand_t mem = slot_operand(slot_of(func, src->reg));
            *src = mem;
        } else if (dst_spilled && op_allows_memory_dest((x64_op_t)inst.op) &&
                   (src->kind == X64_O_IMM || src->kind == X64_O_NONE ||
                    (src->kind == X64_O_REG && !is_spilled(func, src->reg)))) {
            *dst = slot_operand(slot_of(func, dst->reg));
        }
    }
    /* Memory operands: base and index. */
    for (uint32_t index = 0; index < 3; ++index) {
        x64_operand_t *operand = &inst.ops[index];
        bool base_spilled;
        bool index_spilled;
        uint32_t other_spills = 0;
        if (operand->kind != X64_O_MEM) {
            continue;
        }
        base_spilled = operand->base_kind == X64_MB_REG &&
                       is_spilled(func, operand->reg);
        index_spilled = operand->index != X64_NOREG &&
                        is_spilled(func, operand->index);
        for (uint32_t other = 0; other < 3; ++other) {
            if (other != index && inst.ops[other].kind == X64_O_REG &&
                is_spilled(func, inst.ops[other].reg)) {
                ++other_spills;
            }
        }
        if (base_spilled && index_spilled && other_spills != 0) {
            x64_operand_t folded = {.kind = X64_O_MEM, .base_kind = X64_MB_REG,
                                    .reg = X64_SCRATCH0, .index = operand->index,
                                    .scale = operand->scale, .imm = operand->imm};
            push_inst(rewrite, load_spill(func, X64_SCRATCH0, operand->reg));
            push_inst(rewrite, load_spill(func, X64_SCRATCH1, operand->index));
            folded.index = X64_SCRATCH1;
            push_inst(rewrite, simple_inst(X64_LEA, 8, x64_reg(X64_SCRATCH0), folded));
            operand->reg = X64_SCRATCH0;
            operand->index = X64_NOREG;
            operand->scale = 1;
            operand->imm = 0;
            gpr_next = 1;
            base_spilled = index_spilled = false;
            /* r11 is free again for the register operands */
        }
        if (base_spilled) {
            uint32_t scratch = gpr_next == 0 ? X64_SCRATCH0 : X64_SCRATCH1;
            ++gpr_next;
            push_inst(rewrite, load_spill(func, scratch, operand->reg));
            operand->reg = scratch;
        }
        if (index_spilled) {
            uint32_t scratch = gpr_next == 0 ? X64_SCRATCH0 : X64_SCRATCH1;
            ++gpr_next;
            push_inst(rewrite, load_spill(func, scratch, operand->index));
            operand->index = scratch;
        }
    }
    /* Remaining register operands. */
    for (uint32_t index = 0; index < 3; ++index) {
        x64_operand_t *operand = &inst.ops[index];
        x64_role_t role = (x64_role_t)info->roles[index];
        uint32_t reg;
        if (operand->kind != X64_O_REG) {
            continue;
        }
        reg = operand->reg;
        if (!is_spilled(func, reg)) {
            operand->reg = resolve(func, reg);
            continue;
        }
        {
            uint32_t scratch = X64_NOREG;
            for (uint32_t m = 0; m < map_count; ++m) {
                if (map[m].vreg == reg) {
                    scratch = map[m].scratch;
                    map[m].load |= role == X64_ROLE_U || role == X64_ROLE_M;
                    map[m].store |= role == X64_ROLE_D || role == X64_ROLE_M;
                }
            }
            if (scratch == X64_NOREG) {
                if (vreg_is_xmm(func, reg)) {
                    scratch = xmm_next++ == 0 ? X64_XSCRATCH0 : X64_XSCRATCH1;
                } else {
                    scratch = gpr_next++ == 0 ? X64_SCRATCH0 : X64_SCRATCH1;
                }
                map[map_count].vreg = reg;
                map[map_count].scratch = scratch;
                map[map_count].load = role == X64_ROLE_U || role == X64_ROLE_M;
                map[map_count].store = role == X64_ROLE_D || role == X64_ROLE_M;
                ++map_count;
            }
            operand->reg = scratch;
        }
    }
    for (uint32_t m = 0; m < map_count; ++m) {
        if (map[m].load) {
            push_inst(rewrite, load_spill(func, map[m].scratch, map[m].vreg));
        }
    }
    /* Memory operand registers that are plainly assigned. */
    for (uint32_t index = 0; index < 3; ++index) {
        x64_operand_t *operand = &inst.ops[index];
        if (operand->kind == X64_O_MEM) {
            if (operand->base_kind == X64_MB_REG) {
                operand->reg = resolve(func, operand->reg);
            }
            if (operand->index != X64_NOREG) {
                operand->index = resolve(func, operand->index);
            }
        }
    }
    push_inst(rewrite, inst);
    for (uint32_t m = 0; m < map_count; ++m) {
        if (map[m].store) {
            push_inst(rewrite, store_spill(func, map[m].scratch, map[m].vreg));
        }
    }
}

typedef struct ra_used {
    uint32_t mask;
} ra_used_t;

static void used_visit(void *context, uint32_t reg, bool is_def, bool is_use) {
    ra_used_t *used = context;
    (void)is_use;
    if (is_def && reg < X64_PHYS_COUNT) {
        used->mask |= x64_mask(reg);
    }
}

bool x64_allocate(x64_context_t *context, x64_func_t *func) {
    ra_state_t state = {.func = func, .abi = func->abi};
    uint32_t block_count = func->blocks.count;
    uint32_t total = 0;
    ra_block_sets_t *sets = nullptr;
    bool ok = false;
    uint32_t vreg_count = func->vregs.count;
    state.vreg_count = vreg_count;
    state.block_first = calloc(block_count + 1, sizeof(uint32_t));
    state.block_last = calloc(block_count + 1, sizeof(uint32_t));
    state.order = malloc((vreg_count + 1) * sizeof(uint32_t));
    state.active = malloc((vreg_count + 1) * sizeof(uint32_t));
    sets = calloc(block_count + 1, sizeof(ra_block_sets_t));
    if (!state.block_first || !state.block_last || !state.order || !state.active ||
        !sets) {
        goto done;
    }
    for (uint32_t index = 0; index < vreg_count; ++index) {
        func->vregs.data[index].start = UINT32_MAX;
        func->vregs.data[index].end = 0;
        func->vregs.data[index].assigned = -1;
        func->vregs.data[index].spill_slot = -1;
    }
    /* numbering */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        state.block_first[block] = total;
        total += func->blocks.data[block].insts.count;
        state.block_last[block] = total - 1;
    }
    /* liveness */
    for (uint32_t block = 0; block < block_count; ++block) {
        if (!cg_bitset_init(&sets[block].use, vreg_count) ||
            !cg_bitset_init(&sets[block].def, vreg_count) ||
            !cg_bitset_init(&sets[block].live_in, vreg_count) ||
            !cg_bitset_init(&sets[block].live_out, vreg_count)) {
            goto done;
        }
    }
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            for_each_reg(&func->blocks.data[block].insts.data[index], collect_visit,
                         &sets[block]);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t position = func->layout.count; position-- > 0;) {
            uint32_t block = func->layout.data[position];
            const x64_block_t *machine = &func->blocks.data[block];
            for (uint32_t s = 0; s < machine->succ_count; ++s) {
                changed |= cg_bitset_union(&sets[block].live_out,
                                           &sets[machine->succs[s]].live_in);
            }
            changed |= cg_bitset_union(&sets[block].live_in, &sets[block].use);
            changed |= cg_bitset_union_minus(&sets[block].live_in,
                                             &sets[block].live_out, &sets[block].def);
        }
    }
    /* intervals */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        uint32_t first = state.block_first[block];
        uint32_t last = state.block_last[block];
        for (uint32_t id = 0; id < vreg_count; ++id) {
            x64_vreg_t *vreg = &func->vregs.data[id];
            if (cg_bitset_test(&sets[block].live_in, id)) {
                vreg->start = vreg->start < first * 2 ? vreg->start : first * 2;
                vreg->end = vreg->end > first * 2 ? vreg->end : first * 2;
            }
            if (cg_bitset_test(&sets[block].live_out, id)) {
                vreg->start = vreg->start < last * 2 + 1 ? vreg->start : last * 2 + 1;
                vreg->end = vreg->end > last * 2 + 1 ? vreg->end : last * 2 + 1;
            }
        }
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            ra_interval_ctx_t ctx = {.func = func, .position = first + index};
            for_each_reg(&func->blocks.data[block].insts.data[index], interval_visit,
                         &ctx);
        }
    }
    /* physical register busy ranges */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        uint32_t first = state.block_first[block];
        ra_phys_ctx_t ctx = {.state = &state};
        for (uint32_t index = func->blocks.data[block].insts.count; index-- > 0;) {
            const x64_inst_t *inst = &func->blocks.data[block].insts.data[index];
            ctx.position = first + index;
            ctx.def_phase = true;
            for_each_reg(inst, phys_visit, &ctx);
            for (uint32_t reg = 0; reg < X64_PHYS_COUNT; ++reg) {
                if (inst->imp_defs & x64_mask(reg)) {
                    phys_def(&ctx, reg);
                }
            }
            ctx.def_phase = false;
            for_each_reg(inst, phys_visit, &ctx);
            for (uint32_t reg = 0; reg < X64_PHYS_COUNT; ++reg) {
                if (inst->imp_uses & x64_mask(reg)) {
                    phys_use(&ctx, reg);
                }
            }
        }
        for (uint32_t reg = 0; reg < X64_PHYS_COUNT; ++reg) {
            if (ctx.live_mask & x64_mask(reg)) {
                phys_add_range(&state, reg, first * 2, ctx.live_end[reg]);
            }
        }
    }
    normalize_ranges(&state);
    /* hints from copies */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            const x64_inst_t *inst = &func->blocks.data[block].insts.data[index];
            if (!((inst->op == X64_MOV && inst->width == 8) || inst->op == X64_MOVAPS) ||
                inst->ops[0].kind != X64_O_REG || inst->ops[1].kind != X64_O_REG) {
                continue;
            }
            for (uint32_t side = 0; side < 2; ++side) {
                uint32_t mine = inst->ops[side].reg;
                uint32_t other = inst->ops[1 - side].reg;
                if (x64_is_virtual(mine) &&
                    func->vregs.data[mine - X64_VREG_BASE].hint == X64_NOREG) {
                    func->vregs.data[mine - X64_VREG_BASE].hint = other;
                }
            }
        }
    }
    if (!allocate_intervals(&state) || func->slots.oom) {
        goto done;
    }
    /* rewrite */
    {
        ra_used_t used = {0};
        for (uint32_t position = 0; position < func->layout.count; ++position) {
            uint32_t block = func->layout.data[position];
            x64_block_t *machine = &func->blocks.data[block];
            x64_inst_vec_t rewritten = {};
            ra_rewrite_t rewrite = {.func = func, .out = &rewritten};
            for (uint32_t index = 0; index < machine->insts.count; ++index) {
                rewrite_inst(&rewrite, &machine->insts.data[index]);
            }
            cg_vec_free(&machine->insts);
            machine->insts = rewritten;
            for (uint32_t index = 0; index < machine->insts.count; ++index) {
                const x64_inst_t *inst = &machine->insts.data[index];
                for_each_reg(inst, used_visit, &used);
                used.mask |= inst->imp_defs;
            }
            if (rewritten.oom) {
                goto done;
            }
        }
        func->used_callee_saved = used.mask & func->abi->callee_saved;
        /* scratch registers are never callee-saved */
    }
    ok = x64_func_ok(func);
done:
    if (!ok) {
        cg_diag_error(context->diag, "register allocation failed");
    }
    if (sets != nullptr) {
        for (uint32_t block = 0; block < block_count; ++block) {
            cg_bitset_free(&sets[block].use);
            cg_bitset_free(&sets[block].def);
            cg_bitset_free(&sets[block].live_in);
            cg_bitset_free(&sets[block].live_out);
        }
    }
    free(sets);
    free(state.block_first);
    free(state.block_last);
    free(state.order);
    free(state.active);
    for (uint32_t reg = 0; reg < X64_PHYS_COUNT; ++reg) {
        cg_vec_free(&state.busy[reg]);
    }
    return ok;
}
