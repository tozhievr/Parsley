#include "x64.h"

/* x86-64 machine code encoder. A hand written encoder behind the same
 * interface a table driven one would have (design note, risk table): x86 has
 * an irregular encoding, so the opcode logic lives here. */

typedef struct x64_form {
    uint8_t prefix;   /* mandatory prefix (0x66, 0xF2, 0xF3) or 0 */
    bool opsize16;    /* operand size prefix 0x66 */
    bool rex_w;
    bool escape;      /* 0F opcode map */
    uint8_t opcode;
    uint32_t reg;     /* reg field: register id or opcode extension */
    bool reg_is_byte; /* reg field names an 8-bit register */
    bool rm_is_byte;
    uint8_t trailing; /* immediate bytes after the displacement */
} x64_form_t;

typedef struct x64_writer {
    x64_encoded_t *out;
    bool failed;
} x64_writer_t;

static void put(x64_writer_t *writer, uint8_t byte) {
    if (writer->out->length >= sizeof(writer->out->bytes)) {
        writer->failed = true;
        return;
    }
    writer->out->bytes[writer->out->length++] = byte;
}

static void put_le(x64_writer_t *writer, uint64_t value, uint32_t bytes) {
    for (uint32_t index = 0; index < bytes; ++index) {
        put(writer, (uint8_t)(value >> (8 * index)));
    }
}

static uint32_t hw(uint32_t reg) { return reg & 15; }

static bool needs_byte_rex(uint32_t reg) {
    return reg < 16 && reg >= 4 && reg <= 7;
}

static bool fits_i8(int64_t value) { return value >= -128 && value <= 127; }
static bool fits_i32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

/* Emits prefixes, REX, opcode, ModRM, SIB and displacement. */
static void put_form(x64_writer_t *writer, const x64_form_t *form,
                     const x64_operand_t *rm) {
    uint32_t rex = 0;
    bool force_rex = false;
    uint32_t reg_bits = hw(form->reg);
    if (form->prefix != 0) {
        put(writer, form->prefix);
    } else if (form->opsize16) {
        put(writer, 0x66);
    }
    if (form->rex_w) {
        rex |= 8;
    }
    if (reg_bits & 8) {
        rex |= 4;
    }
    if (form->reg_is_byte && needs_byte_rex(form->reg)) {
        force_rex = true;
    }
    if (rm->kind == X64_O_REG) {
        if (hw(rm->reg) & 8) {
            rex |= 1;
        }
        if (form->rm_is_byte && needs_byte_rex(rm->reg)) {
            force_rex = true;
        }
    } else if (rm->kind == X64_O_MEM) {
        if (rm->base_kind == X64_MB_REG && (hw(rm->reg) & 8)) {
            rex |= 1;
        }
        if (rm->index != X64_NOREG && (hw(rm->index) & 8)) {
            rex |= 2;
        }
    } else {
        writer->failed = true;
        return;
    }
    if (rex != 0 || force_rex) {
        put(writer, (uint8_t)(0x40 | rex));
    }
    if (form->escape) {
        put(writer, 0x0F);
    }
    put(writer, form->opcode);
    if (rm->kind == X64_O_REG) {
        put(writer, (uint8_t)(0xC0 | ((reg_bits & 7) << 3) | (hw(rm->reg) & 7)));
        return;
    }
    {
        int64_t disp = rm->imm;
        uint32_t scale_bits = rm->scale == 8   ? 3
                              : rm->scale == 4 ? 2
                              : rm->scale == 2 ? 1
                                               : 0;
        if (rm->base_kind == X64_MB_RIP) {
            put(writer, (uint8_t)(((reg_bits & 7) << 3) | 5));
            writer->out->has_reloc = true;
            writer->out->reloc = (x64_reloc_note_t){
                .kind = MC_RELOC_PC32,
                .offset = writer->out->length,
                .symbol = rm->aux,
                .addend = disp - 4 - form->trailing};
            put_le(writer, 0, 4);
            return;
        }
        if (!fits_i32(disp)) {
            writer->failed = true;
            return;
        }
        if (rm->base_kind == X64_MB_NONE) {
            /* [index*scale + disp32] or absolute: SIB with no base. */
            put(writer, (uint8_t)(((reg_bits & 7) << 3) | 4));
            put(writer, (uint8_t)((scale_bits << 6) |
                                  ((rm->index == X64_NOREG ? 4 : (hw(rm->index) & 7))
                                   << 3) |
                                  5));
            put_le(writer, (uint64_t)disp, 4);
            return;
        }
        if (rm->base_kind != X64_MB_REG) {
            writer->failed = true; /* frame slots must be resolved by now */
            return;
        }
        {
            uint32_t base = hw(rm->reg) & 7;
            bool need_sib = rm->index != X64_NOREG || base == 4;
            uint32_t mod = (disp == 0 && base != 5)  ? 0
                           : fits_i8(disp)           ? 1
                                                     : 2;
            put(writer, (uint8_t)((mod << 6) | ((reg_bits & 7) << 3) |
                                  (need_sib ? 4 : base)));
            if (need_sib) {
                if (rm->index != X64_NOREG && hw(rm->index) == 4 &&
                    rm->index < 16) {
                    writer->failed = true; /* rsp cannot be an index */
                    return;
                }
                put(writer,
                    (uint8_t)((scale_bits << 6) |
                              ((rm->index == X64_NOREG ? 4 : (hw(rm->index) & 7))
                               << 3) |
                              base));
            }
            if (mod == 1) {
                put_le(writer, (uint64_t)disp, 1);
            } else if (mod == 2) {
                put_le(writer, (uint64_t)disp, 4);
            }
        }
    }
}

static x64_form_t form_basic(uint8_t opcode, uint32_t reg, uint32_t width) {
    return (x64_form_t){.opcode = opcode, .reg = reg,
                        .rex_w = width == 8, .opsize16 = width == 2,
                        .rm_is_byte = width == 1};
}

static bool is_gpr_operand(const x64_operand_t *op) {
    return op->kind == X64_O_REG && op->reg < 16;
}

static bool is_xmm_operand(const x64_operand_t *op) {
    return op->kind == X64_O_REG && x64_is_xmm(op->reg);
}

static bool is_rm(const x64_operand_t *op) {
    return op->kind == X64_O_REG || op->kind == X64_O_MEM;
}

static void put_imm(x64_writer_t *writer, int64_t value, uint32_t bytes) {
    put_le(writer, (uint64_t)value, bytes);
}

static uint8_t alu_slot(x64_op_t op) {
    switch (op) {
    case X64_ADD: return 0;
    case X64_OR: return 1;
    case X64_AND: return 4;
    case X64_SUB: return 5;
    case X64_XOR: return 6;
    default: return 7; /* CMP */
    }
}

static bool encode_alu(x64_writer_t *writer, const x64_inst_t *inst) {
    const x64_operand_t *dst = &inst->ops[0];
    const x64_operand_t *src = &inst->ops[1];
    uint32_t width = inst->width;
    uint8_t slot = inst->op == X64_TEST ? 0 : alu_slot((x64_op_t)inst->op);
    bool is_test = inst->op == X64_TEST;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return false;
    }
    if (src->kind == X64_O_IMM) {
        int64_t imm = src->imm;
        x64_form_t form;
        if (!is_rm(dst) || !fits_i32(imm)) {
            return false;
        }
        if (is_test) {
            form = form_basic(width == 1 ? 0xF6 : 0xF7, 0, width);
            form.trailing = width == 1 ? 1 : width == 2 ? 2 : 4;
            put_form(writer, &form, dst);
            put_imm(writer, imm, form.trailing);
            return !writer->failed;
        }
        if (width == 1) {
            form = form_basic(0x80, slot, 1);
            form.trailing = 1;
            put_form(writer, &form, dst);
            put_imm(writer, imm, 1);
        } else if (fits_i8(imm)) {
            form = form_basic(0x83, slot, width);
            form.trailing = 1;
            put_form(writer, &form, dst);
            put_imm(writer, imm, 1);
        } else {
            form = form_basic(0x81, slot, width);
            form.trailing = width == 2 ? 2 : 4;
            put_form(writer, &form, dst);
            put_imm(writer, imm, form.trailing);
        }
        return !writer->failed;
    }
    if (is_gpr_operand(src) && is_rm(dst)) {
        x64_form_t form = form_basic(
            is_test ? (width == 1 ? 0x84 : 0x85)
                    : (uint8_t)((slot << 3) | (width == 1 ? 0 : 1)),
            src->reg, width);
        form.reg_is_byte = width == 1;
        put_form(writer, &form, dst);
        return !writer->failed;
    }
    if (is_gpr_operand(dst) && src->kind == X64_O_MEM && !is_test) {
        x64_form_t form = form_basic(
            (uint8_t)((slot << 3) | (width == 1 ? 2 : 3)), dst->reg, width);
        form.reg_is_byte = width == 1;
        put_form(writer, &form, src);
        return !writer->failed;
    }
    return false;
}

static bool encode_mov(x64_writer_t *writer, const x64_inst_t *inst) {
    const x64_operand_t *dst = &inst->ops[0];
    const x64_operand_t *src = &inst->ops[1];
    uint32_t width = inst->width;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return false;
    }
    if (src->kind == X64_O_IMM) {
        int64_t imm = src->imm;
        if (is_gpr_operand(dst)) {
            uint32_t reg = hw(dst->reg);
            if (width == 8 && imm >= 0 && imm <= (int64_t)UINT32_MAX) {
                width = 4; /* zero extending 32 bit move */
            }
            if (width == 8 && !fits_i32(imm)) {
                put(writer, (uint8_t)(0x48 | ((reg >> 3) & 1)));
                put(writer, (uint8_t)(0xB8 | (reg & 7)));
                put_imm(writer, imm, 8);
                return !writer->failed;
            }
            if (width == 8) {
                x64_form_t form = form_basic(0xC7, 0, 8);
                form.trailing = 4;
                put_form(writer, &form, dst);
                put_imm(writer, imm, 4);
                return !writer->failed;
            }
            if (width == 2) {
                put(writer, 0x66);
            }
            if ((reg >> 3) & 1 || (width == 1 && needs_byte_rex(dst->reg))) {
                put(writer, (uint8_t)(0x40 | ((reg >> 3) & 1)));
            }
            put(writer, (uint8_t)((width == 1 ? 0xB0 : 0xB8) | (reg & 7)));
            put_imm(writer, imm, width);
            return !writer->failed;
        }
        if (dst->kind == X64_O_MEM && fits_i32(imm)) {
            x64_form_t form = form_basic(width == 1 ? 0xC6 : 0xC7, 0, width);
            form.trailing = width == 1 ? 1 : width == 2 ? 2 : 4;
            put_form(writer, &form, dst);
            put_imm(writer, imm, form.trailing);
            return !writer->failed;
        }
        return false;
    }
    if (is_gpr_operand(src) && is_rm(dst)) {
        x64_form_t form = form_basic(width == 1 ? 0x88 : 0x89, src->reg, width);
        form.reg_is_byte = width == 1;
        put_form(writer, &form, dst);
        return !writer->failed;
    }
    if (is_gpr_operand(dst) && src->kind == X64_O_MEM) {
        x64_form_t form = form_basic(width == 1 ? 0x8A : 0x8B, dst->reg, width);
        form.reg_is_byte = width == 1;
        put_form(writer, &form, src);
        return !writer->failed;
    }
    return false;
}

/* SSE helpers --------------------------------------------------------------- */

static uint8_t sse_prefix(uint32_t width) { return width == 4 ? 0xF3 : 0xF2; }

static bool encode_sse_rm(x64_writer_t *writer, uint8_t prefix, bool rex_w,
                          uint8_t opcode, uint32_t reg, const x64_operand_t *rm) {
    x64_form_t form = {.prefix = prefix, .rex_w = rex_w, .escape = true,
                       .opcode = opcode, .reg = reg};
    put_form(writer, &form, rm);
    return !writer->failed;
}

bool x64_encode(const x64_inst_t *inst, x64_encoded_t *out) {
    x64_writer_t writer = {.out = out};
    const x64_operand_t *a = &inst->ops[0];
    const x64_operand_t *b = &inst->ops[1];
    uint32_t width = inst->width;
    memset(out, 0, sizeof(*out));
    switch ((x64_op_t)inst->op) {
    case X64_MOV:
        return encode_mov(&writer, inst);
    case X64_ADD: case X64_SUB: case X64_AND: case X64_OR: case X64_XOR:
    case X64_CMP: case X64_TEST:
        return encode_alu(&writer, inst);
    case X64_MOVZX: {
        x64_form_t form = {.escape = true, .reg = a->reg,
                           .opcode = (uint8_t)(width == 1 ? 0xB6 : 0xB7),
                           .rm_is_byte = width == 1};
        if (!is_gpr_operand(a) || !is_rm(b) || (width != 1 && width != 2)) {
            return false;
        }
        put_form(&writer, &form, b);
        return !writer.failed;
    }
    case X64_MOVSX: {
        x64_form_t form = {.escape = width != 4, .reg = a->reg,
                           .rex_w = inst->width2 == 8,
                           .rm_is_byte = width == 1,
                           .opcode = (uint8_t)(width == 1   ? 0xBE
                                               : width == 2 ? 0xBF
                                                            : 0x63)};
        if (!is_gpr_operand(a) || !is_rm(b) ||
            (width != 1 && width != 2 && width != 4)) {
            return false;
        }
        put_form(&writer, &form, b);
        return !writer.failed;
    }
    case X64_LEA: {
        x64_form_t form = form_basic(0x8D, a->reg, width);
        if (!is_gpr_operand(a) || b->kind != X64_O_MEM || (width != 4 && width != 8)) {
            return false;
        }
        put_form(&writer, &form, b);
        return !writer.failed;
    }
    case X64_IMUL2: {
        x64_form_t form = {.escape = true, .opcode = 0xAF, .reg = a->reg,
                           .rex_w = width == 8, .opsize16 = width == 2};
        if (!is_gpr_operand(a) || !is_rm(b) || width == 1) {
            return false;
        }
        put_form(&writer, &form, b);
        return !writer.failed;
    }
    case X64_IMUL3: {
        const x64_operand_t *imm = &inst->ops[2];
        x64_form_t form = {.opcode = fits_i8(imm->imm) ? 0x6B : 0x69,
                           .reg = a->reg, .rex_w = width == 8,
                           .opsize16 = width == 2};
        if (!is_gpr_operand(a) || !is_rm(b) || imm->kind != X64_O_IMM ||
            width == 1 || !fits_i32(imm->imm)) {
            return false;
        }
        form.trailing = fits_i8(imm->imm) ? 1 : (width == 2 ? 2 : 4);
        put_form(&writer, &form, b);
        put_imm(&writer, imm->imm, form.trailing);
        return !writer.failed;
    }
    case X64_NEG:
    case X64_NOT: {
        x64_form_t form = form_basic(width == 1 ? 0xF6 : 0xF7,
                                     inst->op == X64_NEG ? 3 : 2, width);
        if (!is_rm(a)) {
            return false;
        }
        put_form(&writer, &form, a);
        return !writer.failed;
    }
    case X64_SHL:
    case X64_SHR:
    case X64_SAR: {
        uint32_t slot = inst->op == X64_SHL ? 4 : inst->op == X64_SHR ? 5 : 7;
        x64_form_t form;
        if (!is_rm(a)) {
            return false;
        }
        if (b->kind == X64_O_IMM) {
            form = form_basic(width == 1 ? 0xC0 : 0xC1, slot, width);
            form.trailing = 1;
            put_form(&writer, &form, a);
            put_imm(&writer, b->imm, 1);
        } else {
            form = form_basic(width == 1 ? 0xD2 : 0xD3, slot, width);
            put_form(&writer, &form, a);
        }
        return !writer.failed;
    }
    case X64_IDIV:
    case X64_DIV: {
        x64_form_t form = form_basic(0xF7, inst->op == X64_IDIV ? 7 : 6, width);
        if (!is_rm(a) || (width != 4 && width != 8)) {
            return false;
        }
        put_form(&writer, &form, a);
        return !writer.failed;
    }
    case X64_CDQ:
        if (width == 8) {
            put(&writer, 0x48);
        }
        put(&writer, 0x99);
        return !writer.failed;
    case X64_SETCC: {
        x64_form_t form = {.escape = true, .opcode = (uint8_t)(0x90 + inst->cc),
                           .reg = 0, .rm_is_byte = true};
        if (!is_rm(a)) {
            return false;
        }
        put_form(&writer, &form, a);
        return !writer.failed;
    }
    case X64_CMOVCC: {
        x64_form_t form = {.escape = true, .opcode = (uint8_t)(0x40 + inst->cc),
                           .reg = a->reg, .rex_w = width == 8,
                           .opsize16 = width == 2};
        if (!is_gpr_operand(a) || !is_rm(b) || width == 1) {
            return false;
        }
        put_form(&writer, &form, b);
        return !writer.failed;
    }
    case X64_CALL:
        if (a->kind == X64_O_SYM) {
            put(&writer, 0xE8);
            out->has_reloc = true;
            out->reloc = (x64_reloc_note_t){.kind = MC_RELOC_PLT32,
                                            .offset = out->length,
                                            .symbol = a->aux, .addend = -4};
            put_le(&writer, 0, 4);
        } else {
            x64_form_t form = {.opcode = 0xFF, .reg = 2};
            if (!is_rm(a)) {
                return false;
            }
            put_form(&writer, &form, a);
        }
        return !writer.failed;
    case X64_RET:
        put(&writer, 0xC3);
        return true;
    case X64_UD2:
        put(&writer, 0x0F);
        put(&writer, 0x0B);
        return true;
    case X64_MFENCE:
        put(&writer, 0x0F);
        put(&writer, 0xAE);
        put(&writer, 0xF0);
        return true;
    case X64_XCHG:
    case X64_XADD:
    case X64_CMPXCHG: {
        x64_form_t form = {.reg = b->reg, .rex_w = width == 8, .opsize16 = width == 2,
                           .rm_is_byte = width == 1, .reg_is_byte = width == 1};
        if (!is_rm(a) || !is_gpr_operand(b) ||
            (width != 1 && width != 2 && width != 4 && width != 8)) {
            return false;
        }
        if (inst->op == X64_XCHG) {
            form.opcode = width == 1 ? 0x86 : 0x87;
        } else {
            put(&writer, 0xF0);
            form.escape = true;
            form.opcode = inst->op == X64_XADD ? (width == 1 ? 0xC0 : 0xC1)
                                               : (width == 1 ? 0xB0 : 0xB1);
        }
        put_form(&writer, &form, a);
        return !writer.failed;
    }
    case X64_PUSH:
    case X64_POP: {
        uint32_t reg = hw(a->reg);
        if (!is_gpr_operand(a)) {
            return false;
        }
        if (reg & 8) {
            put(&writer, 0x41);
        }
        put(&writer, (uint8_t)((inst->op == X64_PUSH ? 0x50 : 0x58) | (reg & 7)));
        return true;
    }
    case X64_REP_MOVSB:
        put(&writer, 0xF3);
        put(&writer, 0xA4);
        return true;
    case X64_REP_STOSB:
        put(&writer, 0xF3);
        put(&writer, 0xAA);
        return true;
    case X64_MOVS:
        if (width != 4 && width != 8) {
            return false;
        }
        if (is_xmm_operand(a) && is_rm(b)) {
            return encode_sse_rm(&writer, sse_prefix(width), false, 0x10, a->reg, b);
        }
        if (a->kind == X64_O_MEM && is_xmm_operand(b)) {
            return encode_sse_rm(&writer, sse_prefix(width), false, 0x11, b->reg, a);
        }
        return false;
    case X64_MOVAPS:
    case X64_MOVUPS: {
        uint8_t load = inst->op == X64_MOVAPS ? 0x28 : 0x10;
        if (is_xmm_operand(a) && is_rm(b)) {
            return encode_sse_rm(&writer, 0, false, load, a->reg, b);
        }
        if (a->kind == X64_O_MEM && is_xmm_operand(b)) {
            return encode_sse_rm(&writer, 0, false, (uint8_t)(load + 1), b->reg, a);
        }
        return false;
    }
    case X64_ADDS: case X64_SUBS: case X64_MULS: case X64_DIVS: {
        uint8_t opcode = inst->op == X64_ADDS   ? 0x58
                         : inst->op == X64_MULS ? 0x59
                         : inst->op == X64_SUBS ? 0x5C
                                                : 0x5E;
        if (!is_xmm_operand(a) || !is_rm(b) || (width != 4 && width != 8)) {
            return false;
        }
        return encode_sse_rm(&writer, sse_prefix(width), false, opcode, a->reg, b);
    }
    case X64_UCOMIS:
        if (!is_xmm_operand(a) || !is_rm(b) || (width != 4 && width != 8)) {
            return false;
        }
        return encode_sse_rm(&writer, width == 8 ? 0x66 : 0, false, 0x2E, a->reg, b);
    case X64_CVTSI2S:
        if (!is_xmm_operand(a) || !is_rm(b) || (width != 4 && width != 8) ||
            (inst->width2 != 4 && inst->width2 != 8)) {
            return false;
        }
        return encode_sse_rm(&writer, sse_prefix(width), inst->width2 == 8, 0x2A,
                             a->reg, b);
    case X64_CVTTS2SI:
        if (!is_gpr_operand(a) || !is_rm(b) || (width != 4 && width != 8) ||
            (inst->width2 != 4 && inst->width2 != 8)) {
            return false;
        }
        return encode_sse_rm(&writer, sse_prefix(width), inst->width2 == 8, 0x2C,
                             a->reg, b);
    case X64_CVTS2S:
        if (!is_xmm_operand(a) || !is_rm(b) || (width != 4 && width != 8)) {
            return false;
        }
        return encode_sse_rm(&writer, sse_prefix(width), false, 0x5A, a->reg, b);
    case X64_XORPS: case X64_ANDPS: case X64_ANDNPS: case X64_ORPS: {
        uint8_t opcode = inst->op == X64_XORPS    ? 0x57
                         : inst->op == X64_ANDPS  ? 0x54
                         : inst->op == X64_ANDNPS ? 0x55
                                                  : 0x56;
        if (!is_xmm_operand(a) || !is_rm(b)) {
            return false;
        }
        return encode_sse_rm(&writer, 0, false, opcode, a->reg, b);
    }
    case X64_MOVQ_XG:
        if (!is_xmm_operand(a) || !is_rm(b)) {
            return false;
        }
        return encode_sse_rm(&writer, 0x66, width == 8, 0x6E, a->reg, b);
    case X64_MOVQ_GX:
        if (!is_gpr_operand(a) || !is_xmm_operand(b)) {
            return false;
        }
        return encode_sse_rm(&writer, 0x66, width == 8, 0x7E, b->reg, a);
    default:
        return false;
    }
}

static const char *const gpr64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                      "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                      "r12", "r13", "r14", "r15"};
static const char *const gpr32[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp",
                                      "esi", "edi", "r8d", "r9d", "r10d", "r11d",
                                      "r12d", "r13d", "r14d", "r15d"};
static const char *const gpr16[16] = {"ax", "cx", "dx", "bx", "sp", "bp",
                                      "si", "di", "r8w", "r9w", "r10w", "r11w",
                                      "r12w", "r13w", "r14w", "r15w"};
static const char *const gpr8[16] = {"al", "cl", "dl", "bl", "spl", "bpl",
                                     "sil", "dil", "r8b", "r9b", "r10b", "r11b",
                                     "r12b", "r13b", "r14b", "r15b"};
static const char *const xmm_names[16] = {
    "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

const char *x64_reg_name(uint32_t reg, uint32_t width) {
    if (reg >= 16 && reg < 32) {
        return xmm_names[reg - 16];
    }
    if (reg >= 16) {
        return "?";
    }
    switch (width) {
    case 1: return gpr8[reg];
    case 2: return gpr16[reg];
    case 4: return gpr32[reg];
    default: return gpr64[reg];
    }
}
