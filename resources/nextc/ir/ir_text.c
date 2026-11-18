#include "ir.h"

#include <stdarg.h>
#include <stdio.h>

/* Text form of MIR: printer and parser. parse(print(m)) prints identically. */

static const char *const cg_icmp_names[CG_ICMP_COUNT] = {
    "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"};
static const char *const cg_fcmp_names[CG_FCMP_COUNT] = {
    "oeq", "one", "une", "olt", "ole", "ogt", "oge", "ord", "uno"};
static const char *const cg_section_names[3] = {"rw", "ro", "bss"};
static const char *const cg_binding_names[3] = {"local", "global", "weak"};

static bool cg_out(cg_buffer_t *out, const char *format, ...) {
    char text[512];
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return false;
    }
    if ((size_t)written >= sizeof(text)) {
        written = sizeof(text) - 1;
    }
    return cg_buffer_bytes(out, text, (size_t)written);
}

static void cg_print_inst(const cg_module_t *module, const cg_func_t *func,
                          const cg_inst_t *inst, cg_buffer_t *out) {
    cg_type_t type_a = inst->a < func->values.count &&
                               (inst->op != CG_OP_ALLOCA)
                           ? cg_value_type(func, inst->a)
                           : CG_T_VOID;
    cg_out(out, "  ");
    if (inst->dst != CG_INVALID) {
        cg_out(out, "%%%u = ", inst->dst);
    }
    switch (inst->op) {
    case CG_OP_ICONST:
        cg_out(out, "iconst %s %lld", cg_type_name((cg_type_t)inst->type),
               (long long)inst->imm);
        break;
    case CG_OP_FCONST:
        cg_out(out, "fconst %s 0x%llx", cg_type_name((cg_type_t)inst->type),
               (unsigned long long)inst->imm);
        break;
    case CG_OP_SYMADDR:
        cg_out(out, "symaddr @%s", cg_symbol_name(module, (cg_symbol_t)inst->imm));
        break;
    case CG_OP_ALLOCA:
        cg_out(out, "alloca %lld align %u", (long long)inst->imm,
               (unsigned)1 << inst->a);
        break;
    case CG_OP_ICMP:
        cg_out(out, "icmp %s %s %%%u, %%%u", cg_icmp_names[inst->cc],
               cg_type_name(type_a), inst->a, inst->b);
        break;
    case CG_OP_FCMP:
        cg_out(out, "fcmp %s %s %%%u, %%%u", cg_fcmp_names[inst->cc],
               cg_type_name(type_a), inst->a, inst->b);
        break;
    case CG_OP_SELECT:
        cg_out(out, "select %s %%%u, %%%u, %%%u",
               cg_type_name((cg_type_t)inst->type), inst->a, inst->b, inst->c);
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
        cg_out(out, "%s %s %%%u", cg_op_name(inst->op),
               cg_type_name((cg_type_t)inst->type), inst->a);
        break;
    case CG_OP_LOAD:
        cg_out(out, "load %s %%%u, %lld", cg_type_name((cg_type_t)inst->type),
               inst->a, (long long)inst->imm);
        break;
    case CG_OP_STORE:
        cg_out(out, "store %s %%%u, %%%u, %lld",
               cg_type_name((cg_type_t)inst->type), inst->b, inst->a,
               (long long)inst->imm);
        break;
    case CG_OP_MEMCPY:
    case CG_OP_MEMSET:
        cg_out(out, "%s %%%u, %%%u, %lld", cg_op_name(inst->op), inst->a,
               inst->b, (long long)inst->imm);
        break;
    case CG_OP_CALL:
        cg_out(out, "call%s %s ", (inst->flags & CG_CALL_VARIADIC) ? " variadic" : "",
               cg_type_name((cg_type_t)inst->type));
        if (inst->imm >= 0) {
            cg_out(out, "@%s", cg_symbol_name(module, (cg_symbol_t)inst->imm));
        } else {
            cg_out(out, "%%%u", inst->a);
        }
        cg_out(out, "(");
        for (uint32_t index = 0; index < inst->c; ++index) {
            cg_out(out, "%s%%%u", index ? ", " : "",
                   func->operands.data[inst->b + index]);
        }
        cg_out(out, ")");
        break;
    case CG_OP_PHI:
        cg_out(out, "phi %s ", cg_type_name((cg_type_t)inst->type));
        for (uint32_t index = 0; index < inst->c; ++index) {
            cg_out(out, "%s[%%%u, b%u]", index ? ", " : "",
                   func->operands.data[inst->b + index * 2],
                   func->operands.data[inst->b + index * 2 + 1]);
        }
        break;
    case CG_OP_BR:
        cg_out(out, "br b%u", inst->a);
        break;
    case CG_OP_CONDBR:
        cg_out(out, "condbr %%%u, b%u, b%u", inst->a, inst->b, inst->c);
        break;
    case CG_OP_RET:
        if (inst->a == CG_INVALID) {
            cg_out(out, "ret void");
        } else {
            cg_out(out, "ret %s %%%u", cg_type_name(type_a), inst->a);
        }
        break;
    case CG_OP_UNREACHABLE:
        cg_out(out, "unreachable");
        break;
    case CG_OP_VAAREA:
        cg_out(out, "vaarea");
        break;
    case CG_OP_XCHG:
    case CG_OP_XADD:
        cg_out(out, "%s %s %%%u, %%%u", cg_op_name(inst->op),
               cg_type_name((cg_type_t)inst->type), inst->a, inst->b);
        break;
    case CG_OP_CAS:
        cg_out(out, "cas %s %%%u, %%%u, %%%u", cg_type_name((cg_type_t)inst->type), inst->a,
               inst->b, inst->c);
        break;
    case CG_OP_FENCE:
        cg_out(out, "fence");
        break;
    case CG_OP_STACKSAVE:
        cg_out(out, "stacksave");
        break;
    case CG_OP_STACKRESTORE:
        cg_out(out, "stackrestore %%%u", inst->a);
        break;
    case CG_OP_DYNALLOCA:
        cg_out(out, "dynalloca %%%u", inst->a);
        break;
    default: /* plain binary operators */
        cg_out(out, "%s %s %%%u, %%%u", cg_op_name(inst->op),
               cg_type_name((cg_type_t)inst->type), inst->a, inst->b);
        break;
    }
    cg_out(out, "\n");
}

bool cg_print_function(const cg_module_t *module, const cg_func_t *func,
                       cg_buffer_t *out) {
    const cg_symbol_info_t *info = &module->symbols.data[func->symbol];
    cg_out(out, "func @%s(", cg_symbol_name(module, func->symbol));
    for (uint32_t index = 0; index < func->param_count; ++index) {
        cg_out(out, "%s%s", index ? ", " : "",
               cg_type_name(cg_value_type(func, index)));
    }
    cg_out(out, "%s) -> %s %s {\n", func->variadic ? ", ..." : "",
           cg_type_name(func->ret_type), cg_binding_names[info->binding]);
    for (cg_block_t block = 0; block < func->blocks.count; ++block) {
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        cg_out(out, "b%u:\n", block);
        for (uint32_t index = 0; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            if (inst->op == CG_OP_PARAM) {
                continue;
            }
            cg_print_inst(module, func, inst, out);
        }
    }
    return cg_out(out, "}\n");
}

bool cg_print_module(const cg_module_t *module, cg_buffer_t *out) {
    for (cg_symbol_t symbol = 0; symbol < module->symbols.count; ++symbol) {
        const cg_symbol_info_t *info = &module->symbols.data[symbol];
        if (info->kind == CG_SYM_DATA && info->defined) {
            cg_out(out, "global @%s %s %s %u %u", cg_symbol_name(module, symbol),
                   cg_binding_names[info->binding],
                   cg_section_names[info->section], info->align, info->size);
            if (info->section != CG_DATA_BSS) {
                cg_out(out, " =");
                for (uint32_t index = 0; index < info->size; ++index) {
                    cg_out(out, "%s%02x", index == 0 ? " " : "",
                           module->data.bytes.data[info->data_offset + index]);
                }
            }
            cg_out(out, "\n");
        } else if (info->kind == CG_SYM_FUNC && !info->defined) {
            cg_out(out, "declare @%s\n", cg_symbol_name(module, symbol));
        } else if (info->kind == CG_SYM_UNKNOWN) {
            cg_out(out, "extern @%s\n", cg_symbol_name(module, symbol));
        }
    }
    for (uint32_t index = 0; index < module->relocs.count; ++index) {
        const cg_data_reloc_t *reloc = &module->relocs.data[index];
        cg_out(out, "reloc @%s %u @%s %lld\n",
               cg_symbol_name(module, reloc->owner), reloc->offset,
               cg_symbol_name(module, reloc->target), (long long)reloc->addend);
    }
    for (uint32_t index = 0; index < module->funcs.count; ++index) {
        cg_print_function(module, module->funcs.data[index], out);
    }
    return !out->bytes.oom;
}

/* ---- parser ------------------------------------------------------------- */

typedef struct cg_parser {
    const char *cursor;
    const char *end;
    cg_module_t *module;
    cg_diag_t *diag;
    uint32_t line;
    bool failed;
} cg_parser_t;

static void cg_parse_fail(cg_parser_t *parser, const char *message) {
    if (!parser->failed) {
        cg_diag_error(parser->diag, "ir parse error at line %u: %s", parser->line,
                      message);
    }
    parser->failed = true;
}

static void cg_skip_blanks(cg_parser_t *parser) {
    while (parser->cursor < parser->end &&
           (*parser->cursor == ' ' || *parser->cursor == '\t' ||
            *parser->cursor == '\r')) {
        ++parser->cursor;
    }
}

static bool cg_at_end_of_line(cg_parser_t *parser) {
    cg_skip_blanks(parser);
    return parser->cursor >= parser->end || *parser->cursor == '\n';
}

static bool cg_accept(cg_parser_t *parser, const char *text) {
    size_t length = strlen(text);
    cg_skip_blanks(parser);
    if ((size_t)(parser->end - parser->cursor) >= length &&
        memcmp(parser->cursor, text, length) == 0) {
        parser->cursor += length;
        return true;
    }
    return false;
}

static void cg_expect(cg_parser_t *parser, const char *text) {
    if (!cg_accept(parser, text)) {
        cg_parse_fail(parser, "unexpected token");
    }
}

static bool cg_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' ||
           c == '?';
}

/* Reads a word into `buffer`; returns its length (0 on failure). */
static size_t cg_word(cg_parser_t *parser, char *buffer, size_t capacity) {
    size_t length = 0;
    cg_skip_blanks(parser);
    while (parser->cursor < parser->end && cg_is_word(*parser->cursor)) {
        if (length + 1 < capacity) {
            buffer[length++] = *parser->cursor;
        }
        ++parser->cursor;
    }
    buffer[length] = '\0';
    return length;
}

static int64_t cg_number(cg_parser_t *parser) {
    bool negative = false;
    uint64_t value = 0;
    bool any = false;
    cg_skip_blanks(parser);
    if (parser->cursor < parser->end && *parser->cursor == '-') {
        negative = true;
        ++parser->cursor;
    }
    if (parser->end - parser->cursor > 2 && parser->cursor[0] == '0' &&
        parser->cursor[1] == 'x') {
        parser->cursor += 2;
        while (parser->cursor < parser->end) {
            char c = *parser->cursor;
            int digit = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
            if (digit < 0) {
                break;
            }
            value = value * 16 + (uint64_t)digit;
            any = true;
            ++parser->cursor;
        }
    } else {
        while (parser->cursor < parser->end && *parser->cursor >= '0' &&
               *parser->cursor <= '9') {
            value = value * 10 + (uint64_t)(*parser->cursor - '0');
            any = true;
            ++parser->cursor;
        }
    }
    if (!any) {
        cg_parse_fail(parser, "number expected");
    }
    return negative ? (int64_t)(0 - value) : (int64_t)value;
}

static cg_type_t cg_parse_type(cg_parser_t *parser) {
    char word[32];
    cg_word(parser, word, sizeof(word));
    for (uint32_t type = 0; type < CG_T_COUNT; ++type) {
        if (strcmp(word, cg_type_name((cg_type_t)type)) == 0) {
            return (cg_type_t)type;
        }
    }
    cg_parse_fail(parser, "type expected");
    return CG_T_VOID;
}

static cg_symbol_t cg_parse_symbol(cg_parser_t *parser) {
    char word[256];
    cg_skip_blanks(parser);
    if (!cg_accept(parser, "@")) {
        cg_parse_fail(parser, "symbol expected");
        return CG_INVALID;
    }
    if (cg_word(parser, word, sizeof(word)) == 0) {
        cg_parse_fail(parser, "symbol name expected");
        return CG_INVALID;
    }
    return cg_module_symbol(parser->module, word);
}

static uint32_t cg_parse_prefixed(cg_parser_t *parser, char prefix) {
    char text[2] = {prefix, 0};
    cg_expect(parser, text);
    return (uint32_t)cg_number(parser);
}

static void cg_skip_line(cg_parser_t *parser) {
    while (parser->cursor < parser->end && *parser->cursor != '\n') {
        ++parser->cursor;
    }
    if (parser->cursor < parser->end) {
        ++parser->cursor;
        ++parser->line;
    }
}

static void cg_define_value(cg_func_t *func, cg_value_t value, cg_type_t type,
                            cg_inst_id_t def) {
    while (func->values.count <= value) {
        cg_value_info_t empty = {.type = CG_T_VOID, .def = CG_INVALID};
        cg_vec_push(&func->values, empty);
    }
    func->values.data[value].type = type;
    func->values.data[value].def = def;
}

static void cg_ensure_block(cg_func_t *func, cg_block_t block) {
    while (func->blocks.count <= block) {
        cg_ir_block_t empty = {};
        cg_vec_push(&func->blocks, empty);
    }
}

static void cg_parse_inst(cg_parser_t *parser, cg_func_t *func,
                          cg_block_t block) {
    cg_inst_t inst = {.dst = CG_INVALID, .a = CG_INVALID};
    char word[64];
    cg_op_t op = CG_OP_COUNT;
    cg_value_t dst = CG_INVALID;
    cg_type_t type = CG_T_VOID;
    cg_skip_blanks(parser);
    if (parser->cursor < parser->end && *parser->cursor == '%') {
        dst = cg_parse_prefixed(parser, '%');
        cg_expect(parser, "=");
    }
    cg_word(parser, word, sizeof(word));
    if (strcmp(word, "call") == 0) {
        op = CG_OP_CALL;
    } else {
        for (uint32_t index = 0; index < CG_OP_COUNT; ++index) {
            if (strcmp(word, cg_op_name((cg_op_t)index)) == 0) {
                op = (cg_op_t)index;
            }
        }
    }
    if (op == CG_OP_COUNT || op == CG_OP_PARAM) {
        cg_parse_fail(parser, "unknown instruction");
        return;
    }
    inst.op = op;
    switch (op) {
    case CG_OP_ICONST:
        type = cg_parse_type(parser);
        inst.imm = cg_number(parser);
        break;
    case CG_OP_FCONST:
        type = cg_parse_type(parser);
        inst.imm = cg_number(parser);
        break;
    case CG_OP_SYMADDR:
        type = CG_T_PTR;
        inst.imm = cg_parse_symbol(parser);
        break;
    case CG_OP_ALLOCA: {
        uint32_t align;
        type = CG_T_PTR;
        inst.imm = cg_number(parser);
        cg_expect(parser, "align");
        align = (uint32_t)cg_number(parser);
        inst.a = 0;
        while (((uint32_t)1 << inst.a) < align && inst.a < 31) {
            ++inst.a;
        }
        break;
    }
    case CG_OP_ICMP:
    case CG_OP_FCMP: {
        bool found = false;
        uint32_t count = op == CG_OP_ICMP ? CG_ICMP_COUNT : CG_FCMP_COUNT;
        cg_word(parser, word, sizeof(word));
        for (uint32_t index = 0; index < count; ++index) {
            const char *name =
                op == CG_OP_ICMP ? cg_icmp_names[index] : cg_fcmp_names[index];
            if (strcmp(word, name) == 0) {
                inst.cc = (uint8_t)index;
                found = true;
            }
        }
        if (!found) {
            cg_parse_fail(parser, "bad predicate");
        }
        cg_parse_type(parser);
        type = CG_T_I1;
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, '%');
        break;
    }
    case CG_OP_SELECT:
        type = cg_parse_type(parser);
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.c = cg_parse_prefixed(parser, '%');
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
        type = cg_parse_type(parser);
        inst.a = cg_parse_prefixed(parser, '%');
        break;
    case CG_OP_LOAD:
        type = cg_parse_type(parser);
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.imm = cg_number(parser);
        break;
    case CG_OP_STORE:
        type = cg_parse_type(parser);
        inst.b = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.imm = cg_number(parser);
        break;
    case CG_OP_MEMCPY:
    case CG_OP_MEMSET:
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.imm = cg_number(parser);
        break;
    case CG_OP_CALL: {
        if (cg_accept(parser, "variadic")) {
            inst.flags |= CG_CALL_VARIADIC;
        }
        type = cg_parse_type(parser);
        inst.b = func->operands.count;
        cg_skip_blanks(parser);
        if (parser->cursor < parser->end && *parser->cursor == '@') {
            inst.imm = cg_parse_symbol(parser);
        } else {
            inst.imm = -1;
            inst.a = cg_parse_prefixed(parser, '%');
        }
        cg_expect(parser, "(");
        if (!cg_accept(parser, ")")) {
            do {
                cg_value_t argument = cg_parse_prefixed(parser, '%');
                cg_vec_push(&func->operands, argument);
                ++inst.c;
            } while (cg_accept(parser, ","));
            cg_expect(parser, ")");
        }
        break;
    }
    case CG_OP_PHI:
        type = cg_parse_type(parser);
        inst.b = func->operands.count;
        do {
            cg_value_t value;
            cg_block_t from;
            cg_expect(parser, "[");
            value = cg_parse_prefixed(parser, '%');
            cg_expect(parser, ",");
            from = cg_parse_prefixed(parser, 'b');
            cg_expect(parser, "]");
            cg_vec_push(&func->operands, value);
            cg_vec_push(&func->operands, from);
            ++inst.c;
        } while (!parser->failed && cg_accept(parser, ","));
        break;
    case CG_OP_BR:
        inst.a = cg_parse_prefixed(parser, 'b');
        cg_ensure_block(func, inst.a);
        break;
    case CG_OP_CONDBR:
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, 'b');
        cg_expect(parser, ",");
        inst.c = cg_parse_prefixed(parser, 'b');
        cg_ensure_block(func, inst.b);
        cg_ensure_block(func, inst.c);
        break;
    case CG_OP_RET:
        if (cg_accept(parser, "void")) {
            inst.a = CG_INVALID;
        } else {
            cg_parse_type(parser);
            inst.a = cg_parse_prefixed(parser, '%');
        }
        break;
    case CG_OP_UNREACHABLE:
        break;
    case CG_OP_VAAREA:
        type = CG_T_PTR;
        break;
    case CG_OP_XCHG:
    case CG_OP_XADD:
    case CG_OP_CAS:
        type = cg_parse_type(parser);
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, '%');
        if (inst.op == CG_OP_CAS) {
            cg_expect(parser, ",");
            inst.c = cg_parse_prefixed(parser, '%');
        }
        break;
    case CG_OP_FENCE:
        break;
    case CG_OP_STACKSAVE:
        type = CG_T_PTR;
        break;
    case CG_OP_STACKRESTORE:
        inst.a = cg_parse_prefixed(parser, '%');
        break;
    case CG_OP_DYNALLOCA:
        type = CG_T_PTR;
        inst.a = cg_parse_prefixed(parser, '%');
        break;
    default: /* binary */
        type = cg_parse_type(parser);
        inst.a = cg_parse_prefixed(parser, '%');
        cg_expect(parser, ",");
        inst.b = cg_parse_prefixed(parser, '%');
        break;
    }
    if (parser->failed) {
        return;
    }
    inst.type = type;
    if (dst != CG_INVALID && type != CG_T_VOID) {
        inst.dst = dst;
        cg_define_value(func, dst, type, func->insts.count);
    }
    cg_vec_push(&func->insts, inst);
    cg_vec_push(&func->blocks.data[block].insts, func->insts.count - 1);
    if (!cg_at_end_of_line(parser)) {
        cg_parse_fail(parser, "trailing text");
    }
}

static void cg_parse_function(cg_parser_t *parser) {
    char name[256];
    char word[32];
    cg_type_t params[64];
    uint32_t param_count = 0;
    bool variadic = false;
    cg_type_t ret_type;
    cg_binding_t binding = CG_BIND_GLOBAL;
    cg_func_t *func;
    cg_block_t block = CG_INVALID;
    cg_expect(parser, "@");
    cg_word(parser, name, sizeof(name));
    cg_expect(parser, "(");
    if (!cg_accept(parser, ")")) {
        do {
            if (cg_accept(parser, "...")) {
                variadic = true;
                break;
            }
            if (param_count == 64) {
                cg_parse_fail(parser, "too many parameters");
                return;
            }
            params[param_count++] = cg_parse_type(parser);
        } while (!parser->failed && cg_accept(parser, ","));
        cg_expect(parser, ")");
    }
    cg_expect(parser, "->");
    ret_type = cg_parse_type(parser);
    cg_word(parser, word, sizeof(word));
    for (uint32_t index = 0; index < 3; ++index) {
        if (strcmp(word, cg_binding_names[index]) == 0) {
            binding = (cg_binding_t)index;
        }
    }
    cg_expect(parser, "{");
    if (parser->failed) {
        return;
    }
    func = cg_module_add_function(parser->module, name, ret_type, params,
                                  param_count, variadic, binding);
    if (func == nullptr) {
        cg_parse_fail(parser, "cannot define function");
        return;
    }
    cg_skip_line(parser);
    while (!parser->failed && parser->cursor < parser->end) {
        cg_skip_blanks(parser);
        if (parser->cursor >= parser->end) {
            break;
        }
        if (*parser->cursor == '\n') {
            cg_skip_line(parser);
            continue;
        }
        if (*parser->cursor == '}') {
            cg_skip_line(parser);
            return;
        }
        if (*parser->cursor == 'b' && parser->cursor + 1 < parser->end &&
            parser->cursor[1] >= '0' && parser->cursor[1] <= '9') {
            block = cg_parse_prefixed(parser, 'b');
            cg_expect(parser, ":");
            cg_ensure_block(func, block);
            cg_skip_line(parser);
            continue;
        }
        if (block == CG_INVALID) {
            cg_parse_fail(parser, "instruction outside a block");
            return;
        }
        cg_parse_inst(parser, func, block);
        cg_skip_line(parser);
    }
    cg_parse_fail(parser, "unterminated function");
}

static void cg_parse_global(cg_parser_t *parser) {
    char word[64];
    cg_symbol_t symbol;
    cg_binding_t binding = CG_BIND_GLOBAL;
    cg_datasec_t section = CG_DATA_RW;
    uint32_t align;
    uint32_t size;
    uint8_t *init = nullptr;
    char name[256];
    cg_expect(parser, "@");
    cg_word(parser, name, sizeof(name));
    cg_word(parser, word, sizeof(word));
    for (uint32_t index = 0; index < 3; ++index) {
        if (strcmp(word, cg_binding_names[index]) == 0) {
            binding = (cg_binding_t)index;
        }
    }
    cg_word(parser, word, sizeof(word));
    for (uint32_t index = 0; index < 3; ++index) {
        if (strcmp(word, cg_section_names[index]) == 0) {
            section = (cg_datasec_t)index;
        }
    }
    align = (uint32_t)cg_number(parser);
    size = (uint32_t)cg_number(parser);
    if (parser->failed) {
        return;
    }
    if (section != CG_DATA_BSS && cg_accept(parser, "=")) {
        init = calloc(size == 0 ? 1 : size, 1);
        cg_skip_blanks(parser);
        for (uint32_t index = 0; init != nullptr && index < size; ++index) {
            int digits[2];
            for (int half = 0; half < 2; ++half) {
                char c = parser->cursor < parser->end ? *parser->cursor : 0;
                digits[half] = c >= '0' && c <= '9'   ? c - '0'
                               : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                      : -1;
                if (digits[half] < 0) {
                    cg_parse_fail(parser, "hex data expected");
                    free(init);
                    return;
                }
                ++parser->cursor;
            }
            init[index] = (uint8_t)(digits[0] * 16 + digits[1]);
        }
    }
    symbol = cg_module_add_data(parser->module, name, size, align, init, section,
                                binding);
    free(init);
    if (symbol == CG_INVALID) {
        cg_parse_fail(parser, "cannot define data");
    }
}

cg_module_t *cg_parse_module(const char *text, size_t length, cg_diag_t *diag) {
    cg_parser_t parser = {.cursor = text, .end = text + length,
                          .module = cg_module_create(), .diag = diag,
                          .line = 1};
    char word[32];
    if (parser.module == nullptr) {
        return nullptr;
    }
    while (!parser.failed && parser.cursor < parser.end) {
        cg_skip_blanks(&parser);
        if (parser.cursor >= parser.end) {
            break;
        }
        if (*parser.cursor == '\n') {
            cg_skip_line(&parser);
            continue;
        }
        cg_word(&parser, word, sizeof(word));
        if (strcmp(word, "func") == 0) {
            cg_parse_function(&parser);
            continue;
        }
        if (strcmp(word, "global") == 0) {
            cg_parse_global(&parser);
        } else if (strcmp(word, "declare") == 0) {
            cg_symbol_t symbol = cg_parse_symbol(&parser);
            if (symbol != CG_INVALID &&
                parser.module->symbols.data[symbol].kind == CG_SYM_UNKNOWN) {
                parser.module->symbols.data[symbol].kind = CG_SYM_FUNC;
            }
        } else if (strcmp(word, "extern") == 0) {
            (void)cg_parse_symbol(&parser);
        } else if (strcmp(word, "reloc") == 0) {
            cg_symbol_t owner = cg_parse_symbol(&parser);
            uint32_t offset = (uint32_t)cg_number(&parser);
            cg_symbol_t target = cg_parse_symbol(&parser);
            int64_t addend = cg_number(&parser);
            if (!parser.failed) {
                cg_module_add_data_reloc(parser.module, owner, offset, target,
                                         addend);
            }
        } else {
            cg_parse_fail(&parser, "unknown top-level item");
        }
        if (!parser.failed && !cg_at_end_of_line(&parser)) {
            cg_parse_fail(&parser, "trailing text");
        }
        cg_skip_line(&parser);
    }
    if (parser.failed || !cg_module_ok(parser.module)) {
        cg_module_destroy(parser.module);
        return nullptr;
    }
    return parser.module;
}
