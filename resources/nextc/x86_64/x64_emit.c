#include "x64.h"

#include <stdarg.h>
#include <stdio.h>

/* MC emission: encodes the final machine function into the text section,
 * relaxing branches (short to long forms) until sizes converge, and records
 * relocations. Also hosts the module driver. */

typedef struct emit_item {
    x64_encoded_t encoded;
    uint8_t size;
    bool is_jump;
    bool is_long;
    uint32_t target_block;
    uint32_t offset;
} emit_item_t;

bool x64_emit(x64_context_t *context, x64_func_t *func) {
    mc_module_t *mc = context->mc;
    uint32_t total = 0;
    emit_item_t *items;
    uint32_t *block_first;
    uint32_t *block_offset;
    uint32_t function_start;
    uint32_t cursor = 0;
    bool ok = false;
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        total += func->blocks.data[func->layout.data[position]].insts.count;
    }
    items = calloc(total + 1, sizeof(*items));
    block_first = calloc(func->blocks.count + 1, sizeof(uint32_t));
    block_offset = calloc(func->blocks.count + 1, sizeof(uint32_t));
    if (items == nullptr || block_first == nullptr || block_offset == nullptr) {
        goto done;
    }
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        block_first[block] = cursor;
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            const x64_inst_t *inst = &func->blocks.data[block].insts.data[index];
            emit_item_t *item = &items[cursor++];
            if (inst->op == X64_JMP || inst->op == X64_JCC) {
                item->is_jump = true;
                item->target_block = inst->ops[0].aux;
                item->size = 2;
            } else if (!x64_encode(inst, &item->encoded)) {
                cg_diag_error(context->diag, "%s: cannot encode %s",
                              cg_symbol_name(context->module, func->symbol),
                              x64_opinfo((x64_op_t)inst->op)->name);
                goto done;
            } else {
                item->size = item->encoded.length;
            }
        }
    }
    /* Branch relaxation: sizes only ever grow, so this terminates. */
    for (bool changed = true; changed;) {
        uint32_t offset = 0;
        changed = false;
        cursor = 0;
        for (uint32_t position = 0; position < func->layout.count; ++position) {
            uint32_t block = func->layout.data[position];
            block_offset[block] = offset;
            for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
                items[cursor].offset = offset;
                offset += items[cursor].size;
                ++cursor;
            }
        }
        cursor = 0;
        for (uint32_t position = 0; position < func->layout.count; ++position) {
            uint32_t block = func->layout.data[position];
            for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
                emit_item_t *item = &items[cursor++];
                if (item->is_jump && !item->is_long) {
                    int64_t distance = (int64_t)block_offset[item->target_block] -
                                       (int64_t)(item->offset + item->size);
                    if (distance < -128 || distance > 127) {
                        const x64_inst_t *inst = &func->blocks.data[block].insts.data[index];
                        item->is_long = true;
                        item->size = inst->op == X64_JMP ? 5 : 6;
                        changed = true;
                    }
                }
            }
        }
    }
    function_start = mc_align_section(mc, MC_TEXT, 16);
    cursor = 0;
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            const x64_inst_t *inst = &func->blocks.data[block].insts.data[index];
            emit_item_t *item = &items[cursor++];
            cg_buffer_t *text = &mc->sections[MC_TEXT];
            if (item->is_jump) {
                int64_t distance = (int64_t)block_offset[item->target_block] -
                                   (int64_t)(item->offset + item->size);
                if (item->is_long) {
                    if (inst->op == X64_JMP) {
                        cg_buffer_u8(text, 0xE9);
                    } else {
                        cg_buffer_u8(text, 0x0F);
                        cg_buffer_u8(text, (uint8_t)(0x80 + inst->cc));
                    }
                    cg_buffer_u32(text, (uint32_t)(int32_t)distance);
                } else {
                    cg_buffer_u8(text, inst->op == X64_JMP ? 0xEB
                                                           : (uint8_t)(0x70 + inst->cc));
                    cg_buffer_u8(text, (uint8_t)(int8_t)distance);
                }
            } else {
                uint32_t start = (uint32_t)cg_buffer_size(text);
                cg_buffer_bytes(text, item->encoded.bytes, item->encoded.length);
                if (item->encoded.has_reloc) {
                    mc_add_reloc(mc, MC_TEXT, start + item->encoded.reloc.offset,
                                 item->encoded.reloc.symbol,
                                 (mc_reloc_kind_t)item->encoded.reloc.kind,
                                 item->encoded.reloc.addend);
                }
            }
        }
    }
    mc_define_symbol(mc, func->symbol, MC_TEXT, function_start,
                     (uint32_t)cg_buffer_size(&mc->sections[MC_TEXT]) - function_start,
                     true);
    ok = mc_ok(mc);
done:
    free(items);
    free(block_first);
    free(block_offset);
    return ok;
}

/* ---- text printer --------------------------------------------------------- */

typedef struct printer {
    cg_buffer_t *out;
    const cg_module_t *module;
} printer_t;

static void put_text(printer_t *printer, const char *format, ...) {
    char text[512];
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (written > 0) {
        cg_buffer_bytes(printer->out, text,
                        (size_t)written < sizeof(text) ? (size_t)written
                                                       : sizeof(text) - 1);
    }
}

static const char *const cc_names[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                         "s", "ns", "p", "np", "l", "ge", "le", "g"};

static void print_reg(printer_t *printer, uint32_t reg, uint32_t width) {
    if (x64_is_virtual(reg)) {
        put_text(printer, "%%v%u", reg - X64_VREG_BASE);
    } else {
        put_text(printer, "%s", x64_reg_name(reg, width));
    }
}

static const char *size_keyword(uint32_t width) {
    switch (width) {
    case 1: return "byte ptr ";
    case 2: return "word ptr ";
    case 4: return "dword ptr ";
    case 16: return "xmmword ptr ";
    default: return "qword ptr ";
    }
}

static void print_operand(printer_t *printer, const x64_operand_t *operand,
                          uint32_t width, uint32_t mem_width, bool show_size) {
    switch ((x64_okind_t)operand->kind) {
    case X64_O_REG:
        print_reg(printer, operand->reg, width);
        break;
    case X64_O_IMM:
        put_text(printer, "%lld", (long long)operand->imm);
        break;
    case X64_O_MEM:
        if (show_size) {
            put_text(printer, "%s", size_keyword(mem_width));
        }
        put_text(printer, "[");
        switch ((x64_mbase_t)operand->base_kind) {
        case X64_MB_REG:
            print_reg(printer, operand->reg, 8);
            break;
        case X64_MB_FRAME:
            put_text(printer, "slot%u", operand->aux);
            break;
        case X64_MB_INARG:
            put_text(printer, "inarg");
            break;
        case X64_MB_OUTARG:
            put_text(printer, "outarg");
            break;
        case X64_MB_RIP:
            put_text(printer, "rip + %s",
                     operand->aux < printer->module->symbols.count
                         ? cg_symbol_name(printer->module, operand->aux)
                         : ".LC");
            break;
        default:
            break;
        }
        if (operand->index != X64_NOREG) {
            put_text(printer, " + ");
            print_reg(printer, operand->index, 8);
            put_text(printer, "*%u", operand->scale);
        }
        if (operand->imm != 0) {
            put_text(printer, " %c %lld", operand->imm < 0 ? '-' : '+',
                     (long long)(operand->imm < 0 ? -operand->imm : operand->imm));
        }
        put_text(printer, "]");
        break;
    case X64_O_BLOCK:
        put_text(printer, ".LBB%u", operand->aux);
        break;
    case X64_O_SYM:
        put_text(printer, "%s",
                 operand->aux < printer->module->symbols.count
                     ? cg_symbol_name(printer->module, operand->aux)
                     : "?");
        break;
    default:
        break;
    }
}

static void print_inst(printer_t *printer, const x64_inst_t *inst) {
    const char *name = x64_opinfo((x64_op_t)inst->op)->name;
    uint32_t width = inst->width;
    uint32_t operand_count = 0;
    bool is_sse_scalar = false;
    char suffix[16] = "";
    uint32_t dst_width = width;
    uint32_t src_width = width;
    uint32_t mem_width = width;
    for (uint32_t index = 0; index < 3; ++index) {
        if (inst->ops[index].kind != X64_O_NONE) {
            operand_count = index + 1;
        }
    }
    switch ((x64_op_t)inst->op) {
    case X64_MOVS: case X64_ADDS: case X64_SUBS: case X64_MULS: case X64_DIVS:
    case X64_UCOMIS:
        is_sse_scalar = true;
        snprintf(suffix, sizeof(suffix), "%s", width == 4 ? "s" : "d");
        dst_width = src_width = 16;
        mem_width = width;
        break;
    case X64_MOVAPS: case X64_MOVUPS: case X64_XORPS: case X64_ANDPS:
    case X64_ANDNPS: case X64_ORPS:
        dst_width = src_width = 16;
        mem_width = 16;
        break;
    case X64_CVTSI2S:
        snprintf(suffix, sizeof(suffix), "%s", width == 4 ? "s" : "d");
        dst_width = 16;
        src_width = inst->width2;
        mem_width = inst->width2;
        break;
    case X64_CVTTS2SI:
        snprintf(suffix, sizeof(suffix), "%s", width == 4 ? "s" : "d");
        dst_width = inst->width2;
        src_width = 16;
        mem_width = width;
        break;
    case X64_CVTS2S:
        dst_width = src_width = 16;
        mem_width = width;
        break;
    case X64_MOVZX: case X64_MOVSX:
        dst_width = inst->op == X64_MOVSX && inst->width2 != 0 ? inst->width2 : 4;
        src_width = width;
        mem_width = width;
        break;
    case X64_MOVQ_XG:
        dst_width = 16;
        src_width = width;
        break;
    case X64_MOVQ_GX:
        dst_width = width;
        src_width = 16;
        break;
    case X64_LEA:
        mem_width = 8;
        break;
    default:
        break;
    }
    put_text(printer, "    ");
    switch ((x64_op_t)inst->op) {
    case X64_MOVS:
        put_text(printer, "mov%s", suffix);
        break;
    case X64_ADDS: case X64_SUBS: case X64_MULS: case X64_DIVS: case X64_UCOMIS:
        put_text(printer, "%s%s", name, is_sse_scalar && suffix[0] == 's' ? "s" : "d");
        break;
    case X64_CVTSI2S:
        put_text(printer, "cvtsi2s%s", suffix);
        break;
    case X64_CVTTS2SI:
        put_text(printer, "cvtts%s2si", suffix);
        break;
    case X64_CVTS2S:
        put_text(printer, width == 4 ? "cvtss2sd" : "cvtsd2ss");
        break;
    case X64_SETCC:
        put_text(printer, "set%s", cc_names[inst->cc]);
        break;
    case X64_JCC:
        put_text(printer, "j%s", cc_names[inst->cc]);
        break;
    case X64_CMOVCC:
        put_text(printer, "cmov%s", cc_names[inst->cc]);
        break;
    case X64_CDQ:
        put_text(printer, width == 8 ? "cqo" : "cdq");
        break;
    case X64_MOVQ_XG: case X64_MOVQ_GX:
        put_text(printer, width == 8 ? "movq" : "movd");
        break;
    default:
        put_text(printer, "%s", name);
        break;
    }
    for (uint32_t index = 0; index < operand_count; ++index) {
        const x64_operand_t *operand = &inst->ops[index];
        uint32_t reg_width = index == 0 ? dst_width : src_width;
        if (inst->op == X64_SHL || inst->op == X64_SHR || inst->op == X64_SAR) {
            if (index == 1) {
                reg_width = 1;
            }
        }
        if ((inst->op == X64_SETCC)) {
            reg_width = 1;
        }
        put_text(printer, index == 0 ? " " : ", ");
        print_operand(printer, operand, reg_width, mem_width, true);
    }
    (void)is_sse_scalar;
    put_text(printer, "\n");
}

bool x64_print(const x64_func_t *func, const cg_module_t *module, cg_buffer_t *out) {
    printer_t printer = {.out = out, .module = module};
    put_text(&printer, "%s:\n", cg_symbol_name(module, func->symbol));
    for (uint32_t position = 0; position < func->layout.count; ++position) {
        uint32_t block = func->layout.data[position];
        put_text(&printer, ".LBB%u:\n", block);
        for (uint32_t index = 0; index < func->blocks.data[block].insts.count; ++index) {
            print_inst(&printer, &func->blocks.data[block].insts.data[index]);
        }
    }
    return !out->bytes.oom;
}

/* ---- module driver --------------------------------------------------------- */

bool x64_compile_module(x64_context_t *context) {
    const cg_module_t *module = context->module;
    for (uint32_t index = 0; index < module->funcs.count; ++index) {
        cg_func_t *ir = module->funcs.data[index];
        x64_func_t machine;
        bool ok;
        if (!x64_select(context, ir, &machine)) {
            return false;
        }
        ok = x64_allocate(context, &machine) && x64_lower_frame(context, &machine) &&
             x64_emit(context, &machine);
        x64_func_free(&machine);
        if (!ok) {
            return false;
        }
    }
    return true;
}
