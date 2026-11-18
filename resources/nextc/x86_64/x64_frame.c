#include "x64.h"

/* Frame lowering: stack layout, prologue/epilogue, frame operand resolution
 * and branch cleanup. Layout, from high to low addresses:
 *
 *   [rbp+16+shadow ...]  stack arguments        (caller)
 *   [rbp+16 ...]         shadow space (Win64)   (caller)
 *   [rbp+8]              return address
 *   [rbp]                saved rbp
 *   [rbp-8 ...]          saved callee-saved registers (push)
 *   ...                  saved xmm registers, allocas, spill slots
 *   [rsp ...]            outgoing argument area (shadow space + stack args)
 */

static x64_inst_t frame_inst(x64_op_t op, uint32_t width, x64_operand_t a,
                             x64_operand_t b) {
    x64_inst_t inst = {.op = op, .width = (uint8_t)width};
    inst.ops[0] = a;
    inst.ops[1] = b;
    inst.ops[2] = (x64_operand_t){.index = X64_NOREG};
    return inst;
}

typedef struct frame_plan {
    uint32_t push_regs[16];
    uint32_t push_count;
    uint32_t xmm_regs[16];
    int32_t xmm_offsets[16];
    uint32_t xmm_count;
} frame_plan_t;

static bool build_prologue(const x64_func_t *func, const frame_plan_t *plan,
                           x64_inst_vec_t *out) {
    x64_operand_t rsp = x64_reg(X64_RSP);
    x64_operand_t rbp = x64_reg(X64_RBP);
    cg_vec_push(out, frame_inst(X64_PUSH, 8, rbp, (x64_operand_t){.index = X64_NOREG}));
    cg_vec_push(out, frame_inst(X64_MOV, 8, rbp, rsp));
    for (uint32_t index = 0; index < plan->push_count; ++index) {
        cg_vec_push(out, frame_inst(X64_PUSH, 8, x64_reg(plan->push_regs[index]),
                                    (x64_operand_t){.index = X64_NOREG}));
    }
    if (func->frame_size > 4096) {
        uint32_t pages = func->frame_size / 4096;
        for (uint32_t page = 0; page < pages; ++page) {
            cg_vec_push(out, frame_inst(X64_SUB, 8, rsp, x64_imm(4096)));
            cg_vec_push(out, frame_inst(X64_OR, 8, x64_mem_reg(X64_RSP, 0), x64_imm(0)));
        }
        if (func->frame_size % 4096 != 0) {
            cg_vec_push(out, frame_inst(X64_SUB, 8, rsp,
                                        x64_imm(func->frame_size % 4096)));
        }
    } else if (func->frame_size != 0) {
        cg_vec_push(out, frame_inst(X64_SUB, 8, rsp, x64_imm(func->frame_size)));
    }
    if (func->frame_align > 16) {
        cg_vec_push(out, frame_inst(X64_AND, 8, rsp, x64_imm(-(int64_t)func->frame_align)));
    }
    for (uint32_t index = 0; index < plan->xmm_count; ++index) {
        cg_vec_push(out, frame_inst(X64_MOVUPS, 16,
                                    x64_mem_reg(X64_RBP, plan->xmm_offsets[index]),
                                    x64_reg(plan->xmm_regs[index])));
    }
    return !out->oom;
}

static bool build_epilogue(const x64_func_t *func, const frame_plan_t *plan,
                           x64_inst_vec_t *out) {
    for (uint32_t index = 0; index < plan->xmm_count; ++index) {
        cg_vec_push(out, frame_inst(X64_MOVUPS, 16, x64_reg(plan->xmm_regs[index]),
                                    x64_mem_reg(X64_RBP, plan->xmm_offsets[index])));
    }
    if (plan->push_count != 0) {
        cg_vec_push(out, frame_inst(X64_LEA, 8, x64_reg(X64_RSP),
                                    x64_mem_reg(X64_RBP,
                                                -8 * (int64_t)plan->push_count)));
        for (uint32_t index = plan->push_count; index-- > 0;) {
            cg_vec_push(out, frame_inst(X64_POP, 8, x64_reg(plan->push_regs[index]),
                                        (x64_operand_t){.index = X64_NOREG}));
        }
    } else {
        cg_vec_push(out, frame_inst(X64_MOV, 8, x64_reg(X64_RSP), x64_reg(X64_RBP)));
    }
    cg_vec_push(out, frame_inst(X64_POP, 8, x64_reg(X64_RBP),
                                (x64_operand_t){.index = X64_NOREG}));
    (void)func;
    return !out->oom;
}

static void resolve_operand(const x64_func_t *func, x64_operand_t *operand) {
    if (operand->kind != X64_O_MEM) {
        return;
    }
    switch ((x64_mbase_t)operand->base_kind) {
    case X64_MB_FRAME:
        operand->imm += func->slots.data[operand->aux].offset;
        operand->reg = func->frame_align > 16 ? X64_RSP : X64_RBP;
        operand->base_kind = X64_MB_REG;
        break;
    case X64_MB_INARG:
        operand->imm += 16;
        operand->reg = X64_RBP;
        operand->base_kind = X64_MB_REG;
        break;
    case X64_MB_OUTARG:
        if (operand->aux == 1) {
            operand->imm += func->max_outgoing; /* dynamic stack block */
        }
        operand->reg = X64_RSP;
        operand->base_kind = X64_MB_REG;
        break;
    default:
        break;
    }
}

static x64_cc_t invert_cc(x64_cc_t cc) { return (x64_cc_t)(cc ^ 1); }

bool x64_lower_frame(x64_context_t *context, x64_func_t *func) {
    frame_plan_t plan = {0};
    uint32_t cursor;
    x64_inst_vec_t prologue = {};
    x64_inst_vec_t epilogue = {};
    uint32_t locals;
    const x64_abi_t *abi = func->abi;
    if (func->has_dynamic_alloca) {
        func->max_outgoing = cg_align_up_u32(func->max_outgoing, 16);
    }
    for (uint32_t reg = 0; reg < 16; ++reg) {
        if ((func->used_callee_saved & x64_mask(reg)) != 0) {
            plan.push_regs[plan.push_count++] = reg;
        }
    }
    cursor = 8 * plan.push_count;
    for (uint32_t index = 0; index < 16; ++index) {
        if ((func->used_callee_saved & x64_mask(X64_XMM0 + index)) != 0) {
            cursor += 16;
            plan.xmm_regs[plan.xmm_count] = X64_XMM0 + index;
            plan.xmm_offsets[plan.xmm_count] = -(int32_t)cursor;
            ++plan.xmm_count;
        }
    }
    func->frame_align = 0;
    for (uint32_t index = 0; index < func->slots.count; ++index) {
        if (func->slots.data[index].align > func->frame_align) {
            func->frame_align = func->slots.data[index].align;
        }
    }
    if (func->frame_align > 16) {
        /* Over-aligned slots: rsp is realigned in the prologue and the slots sit
         * above the outgoing area, addressed from rsp. */
        uint32_t offset = func->max_outgoing;
        if (func->frame_align > 4096 || func->has_dynamic_alloca) {
            cg_diag_error(context->diag, "unsupported stack alignment");
            return false;
        }
        for (uint32_t index = 0; index < func->slots.count; ++index) {
            x64_slot_t *slot = &func->slots.data[index];
            uint32_t align = slot->align == 0 ? 1 : slot->align;
            offset = cg_align_up_u32(offset, align);
            slot->offset = (int32_t)offset;
            offset += slot->size;
        }
        locals = cursor - 8 * plan.push_count + offset + func->frame_align;
        func->pushed_bytes = 8 * plan.push_count;
        func->frame_size = cg_align_up_u32(locals + func->pushed_bytes, 16) - func->pushed_bytes;
        goto frame_done;
    }
    for (uint32_t index = 0; index < func->slots.count; ++index) {
        x64_slot_t *slot = &func->slots.data[index];
        uint32_t align = slot->align == 0 ? 1 : slot->align;
        if (cursor > (uint32_t)INT32_MAX - slot->size - 16) {
            cg_diag_error(context->diag, "frame too large");
            return false;
        }
        cursor = cg_align_up_u32(cursor + slot->size, align);
        slot->offset = -(int32_t)cursor;
    }
    locals = cursor - 8 * plan.push_count + func->max_outgoing;
    func->pushed_bytes = 8 * plan.push_count;
    func->frame_size = cg_align_up_u32(locals + func->pushed_bytes, 16) - func->pushed_bytes;
frame_done:
    if (func->frame_size > 256 * 4096) {
        cg_diag_error(context->diag, "frame too large");
        return false;
    }
    (void)abi;
    if (!build_prologue(func, &plan, &prologue) ||
        !build_epilogue(func, &plan, &epilogue)) {
        cg_vec_free(&prologue);
        cg_vec_free(&epilogue);
        return false;
    }
    /* resolve frame operands, expand returns */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        x64_block_t *block = &func->blocks.data[func->layout.data[position]];
        x64_inst_vec_t rewritten = {};
        if (position == 0) {
            for (uint32_t index = 0; index < prologue.count; ++index) {
                cg_vec_push(&rewritten, prologue.data[index]);
            }
        }
        for (uint32_t index = 0; index < block->insts.count; ++index) {
            x64_inst_t inst = block->insts.data[index];
            for (uint32_t op = 0; op < 3; ++op) {
                resolve_operand(func, &inst.ops[op]);
            }
            if (inst.op == X64_RET) {
                for (uint32_t e = 0; e < epilogue.count; ++e) {
                    cg_vec_push(&rewritten, epilogue.data[e]);
                }
            }
            cg_vec_push(&rewritten, inst);
        }
        cg_vec_free(&block->insts);
        block->insts = rewritten;
        if (rewritten.oom) {
            cg_vec_free(&prologue);
            cg_vec_free(&epilogue);
            return false;
        }
    }
    cg_vec_free(&prologue);
    cg_vec_free(&epilogue);
    /* branch cleanup using the final layout */
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        x64_block_t *block = &func->blocks.data[func->layout.data[position]];
        uint32_t next = position + 1 < func->layout.count
                            ? func->layout.data[position + 1]
                            : CG_INVALID;
        for (bool changed = true; changed && block->insts.count != 0;) {
            x64_inst_t *last = &block->insts.data[block->insts.count - 1];
            changed = false;
            if (last->op == X64_JMP && last->ops[0].aux == next) {
                --block->insts.count;
                changed = true;
            } else if (last->op == X64_JMP && block->insts.count >= 2) {
                x64_inst_t *previous = &block->insts.data[block->insts.count - 2];
                if (previous->op == X64_JCC && previous->ops[0].aux == next) {
                    previous->cc = invert_cc((x64_cc_t)previous->cc);
                    previous->ops[0] = last->ops[0];
                    --block->insts.count;
                    changed = true;
                } else if (previous->op == X64_JCC &&
                           previous->ops[0].aux == last->ops[0].aux) {
                    /* both edges go to the same place */
                    *previous = *last;
                    --block->insts.count;
                    changed = true;
                }
            } else if (last->op == X64_JCC && last->ops[0].aux == next) {
                --block->insts.count;
                changed = true;
            }
        }
    }
    return true;
}
