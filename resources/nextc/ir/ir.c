#include "ir.h"

#include <string.h>

static const uint8_t cg_type_sizes[CG_T_COUNT] = {0, 1, 1, 2, 4, 8, 4, 8, 8};
static const char *const cg_type_names[CG_T_COUNT] = {
    "void", "i1", "i8", "i16", "i32", "i64", "f32", "f64", "ptr"};

uint32_t cg_type_size(cg_type_t type) {
    return type < CG_T_COUNT ? cg_type_sizes[type] : 0;
}

const char *cg_type_name(cg_type_t type) {
    return type < CG_T_COUNT ? cg_type_names[type] : "?";
}

#define D CG_OPF_DEF
#define T CG_OPF_TERM
#define S CG_OPF_SIDE
#define P CG_OPF_PURE
static const uint8_t cg_op_flag_table[CG_OP_COUNT] = {
#define X(name, text, flags) [CG_OP_##name] = (flags),
#include "ir_ops.def"
#undef X
};
#undef D
#undef T
#undef S
#undef P

static const char *const cg_op_names[CG_OP_COUNT] = {
#define X(name, text, flags) [CG_OP_##name] = text,
#include "ir_ops.def"
#undef X
};

uint32_t cg_op_flags(cg_op_t op) {
    return op < CG_OP_COUNT ? cg_op_flag_table[op] : 0;
}

const char *cg_op_name(cg_op_t op) {
    return op < CG_OP_COUNT ? cg_op_names[op] : "?";
}

/* ---- module ----------------------------------------------------------- */

cg_module_t *cg_module_create(void) { return calloc(1, sizeof(cg_module_t)); }

static void cg_func_destroy(cg_func_t *func) {
    if (func == nullptr) {
        return;
    }
    for (uint32_t index = 0; index < func->blocks.count; ++index) {
        cg_vec_free(&func->blocks.data[index].insts);
    }
    cg_vec_free(&func->values);
    cg_vec_free(&func->insts);
    cg_vec_free(&func->blocks);
    cg_vec_free(&func->operands);
    free(func);
}

void cg_module_destroy(cg_module_t *module) {
    if (module == nullptr) {
        return;
    }
    for (uint32_t index = 0; index < module->funcs.count; ++index) {
        cg_func_destroy(module->funcs.data[index]);
    }
    cg_vec_free(&module->funcs);
    cg_vec_free(&module->symbols);
    cg_vec_free(&module->relocs);
    cg_buffer_free(&module->data);
    cg_strings_free(&module->names);
    free(module);
}

bool cg_module_ok(const cg_module_t *module) {
    if (module->symbols.oom || module->funcs.oom || module->relocs.oom ||
        module->data.bytes.oom || module->names.items.oom) {
        return false;
    }
    for (uint32_t index = 0; index < module->funcs.count; ++index) {
        const cg_func_t *func = module->funcs.data[index];
        if (func->values.oom || func->insts.oom || func->blocks.oom ||
            func->operands.oom) {
            return false;
        }
        for (uint32_t block = 0; block < func->blocks.count; ++block) {
            if (func->blocks.data[block].insts.oom) {
                return false;
            }
        }
    }
    return true;
}

const char *cg_symbol_name(const cg_module_t *module, cg_symbol_t symbol) {
    return cg_strings_get(&module->names, module->symbols.data[symbol].name);
}

cg_symbol_t cg_module_symbol(cg_module_t *module, const char *name) {
    uint32_t id = cg_strings_intern(&module->names, name, strlen(name));
    if (id == CG_INVALID) {
        module->names.items.oom = true;
        return CG_INVALID;
    }
    /* Symbol ids equal string ids: every interned name is a symbol. */
    while (module->symbols.count <= id) {
        cg_symbol_info_t info = {.name = module->symbols.count,
                                 .kind = CG_SYM_UNKNOWN,
                                 .binding = CG_BIND_GLOBAL,
                                 .func_index = CG_INVALID};
        if (!cg_vec_push(&module->symbols, info)) {
            return CG_INVALID;
        }
    }
    return id;
}

cg_symbol_t cg_module_declare_function(cg_module_t *module, const char *name) {
    cg_symbol_t symbol = cg_module_symbol(module, name);
    if (symbol != CG_INVALID && module->symbols.data[symbol].kind ==
                                    CG_SYM_UNKNOWN) {
        module->symbols.data[symbol].kind = CG_SYM_FUNC;
    }
    return symbol;
}

cg_func_t *cg_module_add_function(cg_module_t *module, const char *name,
                                  cg_type_t ret_type,
                                  const cg_type_t *param_types,
                                  uint32_t param_count, bool variadic,
                                  cg_binding_t binding) {
    cg_symbol_t symbol = cg_module_symbol(module, name);
    cg_symbol_info_t *info;
    cg_func_t *func;
    cg_builder_t builder;
    if (symbol == CG_INVALID) {
        return nullptr;
    }
    info = &module->symbols.data[symbol];
    if (info->defined || (info->kind != CG_SYM_UNKNOWN &&
                          info->kind != CG_SYM_FUNC)) {
        return nullptr;
    }
    func = calloc(1, sizeof(*func));
    if (func == nullptr) {
        return nullptr;
    }
    func->symbol = symbol;
    func->ret_type = ret_type;
    func->variadic = variadic;
    func->param_count = param_count;
    info->kind = CG_SYM_FUNC;
    info->defined = true;
    info->binding = binding;
    info->func_index = module->funcs.count;
    if (!cg_vec_push(&module->funcs, func)) {
        free(func);
        return nullptr;
    }
    cg_builder_init(&builder, module, func);
    cg_builder_new_block(&builder);
    for (uint32_t index = 0; index < param_count; ++index) {
        cg_inst_t inst = {.op = CG_OP_PARAM,
                          .type = param_types[index],
                          .imm = index};
        cg_value_t value = (cg_value_t)func->values.count;
        cg_value_info_t value_info = {.type = param_types[index],
                                      .def = func->insts.count};
        inst.dst = value;
        cg_vec_push(&func->values, value_info);
        cg_vec_push(&func->insts, inst);
        cg_vec_push(&func->blocks.data[0].insts, func->insts.count - 1);
    }
    return func;
}

cg_func_t *cg_module_function_of(const cg_module_t *module,
                                 cg_symbol_t symbol) {
    if (symbol >= module->symbols.count ||
        module->symbols.data[symbol].func_index == CG_INVALID) {
        return nullptr;
    }
    return module->funcs.data[module->symbols.data[symbol].func_index];
}

cg_symbol_t cg_module_add_data(cg_module_t *module, const char *name,
                               uint32_t size, uint32_t align, const void *init,
                               cg_datasec_t section, cg_binding_t binding) {
    cg_symbol_t symbol = cg_module_symbol(module, name);
    cg_symbol_info_t *info;
    if (symbol == CG_INVALID) {
        return CG_INVALID;
    }
    info = &module->symbols.data[symbol];
    if (info->defined || (info->kind != CG_SYM_UNKNOWN &&
                          info->kind != CG_SYM_DATA)) {
        return CG_INVALID;
    }
    info->kind = CG_SYM_DATA;
    info->defined = true;
    info->binding = binding;
    info->section = section;
    info->size = size;
    info->align = align == 0 ? 1 : align;
    if (section != CG_DATA_BSS) {
        cg_buffer_align(&module->data, info->align < 16 ? info->align : 16);
        info->data_offset = (uint32_t)cg_buffer_size(&module->data);
        if (init != nullptr) {
            cg_buffer_bytes(&module->data, init, size);
        } else {
            cg_buffer_zeros(&module->data, size);
        }
    }
    return symbol;
}

bool cg_module_add_data_reloc(cg_module_t *module, cg_symbol_t owner,
                              uint32_t offset, cg_symbol_t target,
                              int64_t addend) {
    cg_data_reloc_t reloc = {owner, offset, target, addend};
    return cg_vec_push(&module->relocs, reloc);
}

/* ---- builder ------------------------------------------------------------ */

void cg_builder_init(cg_builder_t *builder, cg_module_t *module,
                     cg_func_t *func) {
    builder->module = module;
    builder->func = func;
    builder->block = 0;
}

cg_block_t cg_builder_new_block(cg_builder_t *builder) {
    cg_ir_block_t block = {};
    cg_block_t id = builder->func->blocks.count;
    if (!cg_vec_push(&builder->func->blocks, block)) {
        return CG_INVALID;
    }
    return id;
}

void cg_builder_set_block(cg_builder_t *builder, cg_block_t block) {
    builder->block = block;
}

cg_value_t cg_func_param(const cg_func_t *func, uint32_t index) {
    return index < func->param_count ? index : CG_INVALID;
}

cg_type_t cg_value_type(const cg_func_t *func, cg_value_t value) {
    return value < func->values.count ? (cg_type_t)func->values.data[value].type
                                      : CG_T_VOID;
}

/* Appends an instruction to the current block; allocates a value when the
 * opcode defines one and `type` is not void. */
static cg_value_t cg_append(cg_builder_t *builder, cg_inst_t inst) {
    cg_func_t *func = builder->func;
    cg_inst_id_t id = func->insts.count;
    if (builder->block >= func->blocks.count) {
        return CG_INVALID;
    }
    inst.dst = CG_INVALID;
    if ((cg_op_flags(inst.op) & CG_OPF_DEF) != 0 && inst.type != CG_T_VOID) {
        cg_value_info_t info = {.type = inst.type, .def = id};
        inst.dst = func->values.count;
        cg_vec_push(&func->values, info);
    }
    cg_vec_push(&func->insts, inst);
    cg_vec_push(&func->blocks.data[builder->block].insts, id);
    return inst.dst;
}

cg_value_t cg_build_iconst(cg_builder_t *builder, cg_type_t type,
                           int64_t value) {
    cg_inst_t inst = {.op = CG_OP_ICONST, .type = type, .imm = value};
    return cg_append(builder, inst);
}

cg_value_t cg_build_fconst(cg_builder_t *builder, cg_type_t type,
                           double value) {
    cg_inst_t inst = {.op = CG_OP_FCONST, .type = type};
    if (type == CG_T_F32) {
        float narrow = (float)value;
        uint32_t bits;
        memcpy(&bits, &narrow, 4);
        inst.imm = bits;
    } else {
        memcpy(&inst.imm, &value, 8);
    }
    return cg_append(builder, inst);
}

static cg_value_t cg_build_atomic(cg_builder_t *builder, cg_op_t op, cg_value_t pointer,
                                 cg_value_t first, cg_value_t second) {
    cg_inst_t inst = {.op = op, .type = (uint8_t)cg_value_type(builder->func, first),
                      .a = pointer, .b = first, .c = second};
    return cg_append(builder, inst);
}

cg_value_t cg_build_atomic_xchg(cg_builder_t *builder, cg_value_t pointer, cg_value_t value) {
    return cg_build_atomic(builder, CG_OP_XCHG, pointer, value, 0);
}

cg_value_t cg_build_atomic_xadd(cg_builder_t *builder, cg_value_t pointer, cg_value_t value) {
    return cg_build_atomic(builder, CG_OP_XADD, pointer, value, 0);
}

cg_value_t cg_build_atomic_cas(cg_builder_t *builder, cg_value_t pointer, cg_value_t expected,
                               cg_value_t desired) {
    return cg_build_atomic(builder, CG_OP_CAS, pointer, expected, desired);
}

void cg_build_fence(cg_builder_t *builder) {
    cg_inst_t inst = {.op = CG_OP_FENCE};
    cg_append(builder, inst);
}

cg_value_t cg_build_stacksave(cg_builder_t *builder) {
    cg_inst_t inst = {.op = CG_OP_STACKSAVE, .type = CG_T_PTR};
    return cg_append(builder, inst);
}

void cg_build_stackrestore(cg_builder_t *builder, cg_value_t saved) {
    cg_inst_t inst = {.op = CG_OP_STACKRESTORE, .a = saved};
    cg_append(builder, inst);
}

cg_value_t cg_build_dynalloca(cg_builder_t *builder, cg_value_t size) {
    cg_inst_t inst = {.op = CG_OP_DYNALLOCA, .type = CG_T_PTR, .a = size};
    return cg_append(builder, inst);
}

cg_value_t cg_build_symaddr(cg_builder_t *builder, cg_symbol_t symbol) {
    cg_inst_t inst = {.op = CG_OP_SYMADDR, .type = CG_T_PTR, .imm = symbol};
    return cg_append(builder, inst);
}

cg_value_t cg_build_vaarea(cg_builder_t *builder) {
    cg_inst_t inst = {.op = CG_OP_VAAREA, .type = CG_T_PTR};
    return cg_append(builder, inst);
}

cg_value_t cg_build_alloca(cg_builder_t *builder, uint32_t size,
                           uint32_t align) {
    uint32_t log2 = 0;
    cg_inst_t inst = {.op = CG_OP_ALLOCA, .type = CG_T_PTR, .imm = size};
    while (((uint32_t)1 << log2) < align && log2 < 31) {
        ++log2;
    }
    inst.a = log2;
    return cg_append(builder, inst);
}

cg_value_t cg_build_alloca_in_entry(cg_builder_t *builder, uint32_t size,
                                    uint32_t align) {
    cg_func_t *func = builder->func;
    cg_block_t saved = builder->block;
    cg_value_t value;
    cg_ir_block_t *entry;
    uint32_t count;
    bool terminated = false;
    if (func->blocks.count == 0) {
        return CG_INVALID;
    }
    entry = &func->blocks.data[0];
    count = entry->insts.count;
    if (count > 0) {
        terminated = (cg_op_flags(func->insts.data[entry->insts.data[count - 1]].op) &
                      CG_OPF_TERM) != 0;
    }
    builder->block = 0;
    value = cg_build_alloca(builder, size, align);
    builder->block = saved;
    if (terminated && value != CG_INVALID && entry->insts.count == count + 1) {
        /* keep the terminator last */
        uint32_t id = entry->insts.data[count];
        entry->insts.data[count] = entry->insts.data[count - 1];
        entry->insts.data[count - 1] = id;
    }
    return value;
}

cg_value_t cg_build_binary(cg_builder_t *builder, cg_op_t op, cg_value_t a,
                           cg_value_t b) {
    cg_inst_t inst = {.op = op, .type = cg_value_type(builder->func, a),
                      .a = a, .b = b};
    return cg_append(builder, inst);
}

cg_value_t cg_build_fneg(cg_builder_t *builder, cg_value_t a) {
    cg_inst_t inst = {.op = CG_OP_FNEG, .type = cg_value_type(builder->func, a),
                      .a = a};
    return cg_append(builder, inst);
}

cg_value_t cg_build_icmp(cg_builder_t *builder, cg_icmp_t cc, cg_value_t a,
                         cg_value_t b) {
    cg_inst_t inst = {.op = CG_OP_ICMP, .type = CG_T_I1, .cc = cc, .a = a,
                      .b = b};
    return cg_append(builder, inst);
}

cg_value_t cg_build_fcmp(cg_builder_t *builder, cg_fcmp_t cc, cg_value_t a,
                         cg_value_t b) {
    cg_inst_t inst = {.op = CG_OP_FCMP, .type = CG_T_I1, .cc = cc, .a = a,
                      .b = b};
    return cg_append(builder, inst);
}

cg_value_t cg_build_select(cg_builder_t *builder, cg_value_t cond,
                           cg_value_t if_true, cg_value_t if_false) {
    cg_inst_t inst = {.op = CG_OP_SELECT,
                      .type = cg_value_type(builder->func, if_true),
                      .a = cond, .b = if_true, .c = if_false};
    return cg_append(builder, inst);
}

cg_value_t cg_build_cast(cg_builder_t *builder, cg_op_t op, cg_type_t type,
                         cg_value_t a) {
    cg_inst_t inst = {.op = op, .type = type, .a = a};
    return cg_append(builder, inst);
}

cg_value_t cg_build_load(cg_builder_t *builder, cg_type_t type,
                         cg_value_t pointer, int64_t offset) {
    cg_inst_t inst = {.op = CG_OP_LOAD, .type = type, .a = pointer,
                      .imm = offset};
    return cg_append(builder, inst);
}

void cg_build_store(cg_builder_t *builder, cg_value_t pointer, cg_value_t value,
                    int64_t offset) {
    cg_inst_t inst = {.op = CG_OP_STORE,
                      .type = cg_value_type(builder->func, value),
                      .a = pointer, .b = value, .imm = offset};
    cg_append(builder, inst);
}

void cg_build_memcpy(cg_builder_t *builder, cg_value_t dst, cg_value_t src,
                     uint32_t length) {
    cg_inst_t inst = {.op = CG_OP_MEMCPY, .a = dst, .b = src, .imm = length};
    cg_append(builder, inst);
}

void cg_build_memset(cg_builder_t *builder, cg_value_t dst, cg_value_t byte,
                     uint32_t length) {
    cg_inst_t inst = {.op = CG_OP_MEMSET, .a = dst, .b = byte, .imm = length};
    cg_append(builder, inst);
}

static cg_value_t cg_build_call_common(cg_builder_t *builder, cg_inst_t inst,
                                       const cg_value_t *args,
                                       uint32_t arg_count) {
    cg_func_t *func = builder->func;
    inst.b = func->operands.count;
    inst.c = arg_count;
    for (uint32_t index = 0; index < arg_count; ++index) {
        cg_vec_push(&func->operands, args[index]);
    }
    return cg_append(builder, inst);
}

cg_value_t cg_build_call(cg_builder_t *builder, cg_symbol_t callee,
                         cg_type_t ret_type, const cg_value_t *args,
                         uint32_t arg_count, bool variadic) {
    cg_inst_t inst = {.op = CG_OP_CALL, .type = ret_type,
                      .flags = variadic ? CG_CALL_VARIADIC : 0,
                      .a = CG_INVALID, .imm = callee};
    if (callee < builder->module->symbols.count &&
        builder->module->symbols.data[callee].kind == CG_SYM_UNKNOWN) {
        builder->module->symbols.data[callee].kind = CG_SYM_FUNC;
    }
    return cg_build_call_common(builder, inst, args, arg_count);
}

cg_value_t cg_build_call_indirect(cg_builder_t *builder, cg_value_t callee,
                                  cg_type_t ret_type, const cg_value_t *args,
                                  uint32_t arg_count, bool variadic) {
    cg_inst_t inst = {.op = CG_OP_CALL, .type = ret_type,
                      .flags = variadic ? CG_CALL_VARIADIC : 0, .a = callee,
                      .imm = -1};
    return cg_build_call_common(builder, inst, args, arg_count);
}

cg_value_t cg_build_phi(cg_builder_t *builder, cg_type_t type) {
    cg_inst_t inst = {.op = CG_OP_PHI, .type = type,
                      .b = builder->func->operands.count};
    return cg_append(builder, inst);
}

bool cg_phi_add_incoming(cg_func_t *func, cg_value_t phi, cg_value_t value,
                         cg_block_t block) {
    cg_inst_t *inst;
    if (phi >= func->values.count) {
        return false;
    }
    inst = &func->insts.data[func->values.data[phi].def];
    if (inst->op != CG_OP_PHI) {
        return false;
    }
    if (inst->b + inst->c * 2 != func->operands.count) {
        /* Not at the end of the pool: move the pairs there. */
        uint32_t start = func->operands.count;
        for (uint32_t index = 0; index < inst->c * 2; ++index) {
            uint32_t word = func->operands.data[inst->b + index];
            cg_vec_push(&func->operands, word);
        }
        inst = &func->insts.data[func->values.data[phi].def];
        inst->b = start;
    }
    cg_vec_push(&func->operands, value);
    cg_vec_push(&func->operands, block);
    ++inst->c;
    return !func->operands.oom;
}

void cg_build_br(cg_builder_t *builder, cg_block_t target) {
    cg_inst_t inst = {.op = CG_OP_BR, .a = target};
    cg_append(builder, inst);
}

void cg_build_condbr(cg_builder_t *builder, cg_value_t cond, cg_block_t if_true,
                     cg_block_t if_false) {
    cg_inst_t inst = {.op = CG_OP_CONDBR, .a = cond, .b = if_true,
                      .c = if_false};
    cg_append(builder, inst);
}

void cg_build_ret(cg_builder_t *builder, cg_value_t value) {
    cg_inst_t inst = {.op = CG_OP_RET, .a = value};
    cg_append(builder, inst);
}

void cg_build_unreachable(cg_builder_t *builder) {
    cg_inst_t inst = {.op = CG_OP_UNREACHABLE};
    cg_append(builder, inst);
}

/* ---- control flow graph --------------------------------------------- */

uint32_t cg_block_successors(const cg_func_t *func, cg_block_t block,
                             cg_block_t out[2]) {
    const cg_ir_block_t *ir_block;
    const cg_inst_t *term;
    if (block >= func->blocks.count) {
        return 0;
    }
    ir_block = &func->blocks.data[block];
    if (ir_block->insts.count == 0) {
        return 0;
    }
    term = &func->insts.data[ir_block->insts.data[ir_block->insts.count - 1]];
    if (term->op == CG_OP_BR) {
        out[0] = term->a;
        return 1;
    }
    if (term->op == CG_OP_CONDBR) {
        out[0] = term->b;
        out[1] = term->c;
        return 2;
    }
    return 0;
}

void cg_cfg_free(cg_cfg_t *cfg) {
    cg_vec_free(&cfg->rpo);
    free(cfg->rpo_index);
    free(cfg->idom);
    free(cfg->pred_start);
    free(cfg->pred_list);
    free(cfg->loop_depth);
    memset(cfg, 0, sizeof(*cfg));
}

static uint32_t cg_cfg_intersect(const cg_cfg_t *cfg, uint32_t a, uint32_t b) {
    while (a != b) {
        while (cfg->rpo_index[a] > cfg->rpo_index[b]) {
            a = cfg->idom[a];
        }
        while (cfg->rpo_index[b] > cfg->rpo_index[a]) {
            b = cfg->idom[b];
        }
    }
    return a;
}

bool cg_cfg_build(cg_cfg_t *cfg, const cg_func_t *func) {
    uint32_t count = func->blocks.count;
    uint32_t *stack = nullptr;
    uint32_t *edge = nullptr;
    uint8_t *state = nullptr;
    cg_u32_vec_t post = {};
    uint32_t sp = 0;
    bool ok = false;
    memset(cfg, 0, sizeof(*cfg));
    cfg->block_count = count;
    if (count == 0) {
        return false;
    }
    cfg->rpo_index = malloc(count * sizeof(uint32_t));
    cfg->idom = malloc(count * sizeof(uint32_t));
    cfg->pred_start = calloc(count + 1, sizeof(uint32_t));
    cfg->loop_depth = calloc(count, sizeof(uint32_t));
    stack = malloc(count * sizeof(uint32_t));
    edge = calloc(count, sizeof(uint32_t));
    state = calloc(count, 1);
    if (!cfg->rpo_index || !cfg->idom || !cfg->pred_start || !cfg->loop_depth ||
        !stack || !edge || !state) {
        goto done;
    }
    memset(cfg->rpo_index, 0xFF, count * sizeof(uint32_t));
    memset(cfg->idom, 0xFF, count * sizeof(uint32_t));
    /* Iterative depth first search for the post order. */
    stack[sp++] = 0;
    state[0] = 1;
    while (sp != 0) {
        cg_block_t top = stack[sp - 1];
        cg_block_t successors[2];
        uint32_t successor_count = cg_block_successors(func, top, successors);
        if (edge[top] < successor_count) {
            cg_block_t next = successors[edge[top]++];
            if (next < count && state[next] == 0) {
                state[next] = 1;
                stack[sp++] = next;
            }
        } else {
            cg_vec_push(&post, top);
            --sp;
        }
    }
    if (post.oom) {
        goto done;
    }
    for (uint32_t index = 0; index < post.count; ++index) {
        cg_vec_push(&cfg->rpo, post.data[post.count - 1 - index]);
        cfg->rpo_index[post.data[post.count - 1 - index]] = index;
    }
    if (cfg->rpo.oom) {
        goto done;
    }
    /* Predecessors (reachable only), CSR layout. */
    for (uint32_t index = 0; index < cfg->rpo.count; ++index) {
        cg_block_t successors[2];
        uint32_t n = cg_block_successors(func, cfg->rpo.data[index], successors);
        for (uint32_t s = 0; s < n; ++s) {
            if (successors[s] < count) {
                ++cfg->pred_start[successors[s] + 1];
            }
        }
    }
    for (uint32_t index = 0; index < count; ++index) {
        cfg->pred_start[index + 1] += cfg->pred_start[index];
    }
    cfg->pred_list = malloc((cfg->pred_start[count] + 1) * sizeof(uint32_t));
    if (cfg->pred_list == nullptr) {
        goto done;
    }
    memset(edge, 0, count * sizeof(uint32_t));
    for (uint32_t index = 0; index < cfg->rpo.count; ++index) {
        cg_block_t block = cfg->rpo.data[index];
        cg_block_t successors[2];
        uint32_t n = cg_block_successors(func, block, successors);
        for (uint32_t s = 0; s < n; ++s) {
            cg_block_t target = successors[s];
            if (target < count) {
                cfg->pred_list[cfg->pred_start[target] + edge[target]++] = block;
            }
        }
    }
    /* Dominators (Cooper, Harvey, Kennedy). */
    cfg->idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t index = 1; index < cfg->rpo.count; ++index) {
            cg_block_t block = cfg->rpo.data[index];
            uint32_t new_idom = CG_INVALID;
            for (uint32_t p = cfg->pred_start[block];
                 p < cfg->pred_start[block + 1]; ++p) {
                cg_block_t pred = cfg->pred_list[p];
                if (cfg->idom[pred] == CG_INVALID) {
                    continue;
                }
                new_idom = new_idom == CG_INVALID
                               ? pred
                               : cg_cfg_intersect(cfg, pred, new_idom);
            }
            if (new_idom != cfg->idom[block]) {
                cfg->idom[block] = new_idom;
                changed = true;
            }
        }
    }
    /* Loop depth: a back edge is an edge whose target dominates its source;
     * the natural loop body is found by walking predecessors. */
    for (uint32_t index = 0; index < cfg->rpo.count; ++index) {
        cg_block_t header = cfg->rpo.data[index];
        uint8_t *in_loop = nullptr;
        for (uint32_t p = cfg->pred_start[header];
             p < cfg->pred_start[header + 1]; ++p) {
            cg_block_t tail = cfg->pred_list[p];
            if (!cg_cfg_dominates(cfg, header, tail)) {
                continue;
            }
            if (in_loop == nullptr) {
                in_loop = calloc(count, 1);
                if (in_loop == nullptr) {
                    goto done;
                }
            }
            sp = 0;
            if (!in_loop[tail]) {
                in_loop[tail] = 1;
                stack[sp++] = tail;
            }
            in_loop[header] = 1;
            while (sp != 0) {
                cg_block_t node = stack[--sp];
                for (uint32_t q = cfg->pred_start[node];
                     q < cfg->pred_start[node + 1]; ++q) {
                    cg_block_t pred = cfg->pred_list[q];
                    if (!in_loop[pred]) {
                        in_loop[pred] = 1;
                        stack[sp++] = pred;
                    }
                }
            }
        }
        if (in_loop != nullptr) {
            for (uint32_t node = 0; node < count; ++node) {
                cfg->loop_depth[node] += in_loop[node];
            }
            free(in_loop);
        }
    }
    ok = true;
done:
    free(stack);
    free(edge);
    free(state);
    cg_vec_free(&post);
    if (!ok) {
        cg_cfg_free(cfg);
    }
    return ok;
}

bool cg_cfg_dominates(const cg_cfg_t *cfg, cg_block_t a, cg_block_t b) {
    if (cfg->rpo_index[a] == CG_INVALID || cfg->rpo_index[b] == CG_INVALID) {
        return false;
    }
    while (true) {
        if (a == b) {
            return true;
        }
        if (b == 0) {
            return false;
        }
        b = cfg->idom[b];
    }
}
