/* Integration driver for the Kotlin Double expression-function subset. */
#include "kotlin_backend.h"
#include "kotlin_opt.h"
#include "kotlin_parse.h"
#include "kotlin_sem.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

struct parameter_names {
    psl_str first;
    psl_str second;
};

static int shape(const kotlin_node *node, kotlin_node_tag tag, size_t count) {
    return node && node->tag == tag && node->child_count == count;
}

static void diagnostic(void *user, const psl_diag *diag) {
    int *errors = user;
    if (diag->severity != PSL_SEV_ERROR) return;
    ++*errors;
    fprintf(stderr, "%.*s\n", (int)diag->message.len, diag->message.ptr);
}

static bool lower(kotlin_backend_context *context, const kotlin_node *node,
                  cg_value_t *result, void *user) {
    cg_builder_t *builder = kotlin_backend_builder(context);
    const struct parameter_names *names = user;
    if (node->tag == KOTLIN_NODE_VAR) {
        if (psl_str_eq(node->children[0]->text_val, names->first))
            *result = cg_func_param(builder->func, 0);
        else if (psl_str_eq(node->children[0]->text_val, names->second))
            *result = cg_func_param(builder->func, 1);
        else
            *result = CG_INVALID;
        return true;
    }
    if (node->tag == KOTLIN_NODE_UNARY && node->children[0]->tag == KOTLIN_NODE_NEG) {
        cg_value_t operand = kotlin_backend_node(context, node->children[1]);
        *result = operand == CG_INVALID ? CG_INVALID : cg_build_fneg(builder, operand);
        return true;
    }
    if (node->tag != KOTLIN_NODE_BIN) return false;
    cg_op_t op;
    switch (node->children[0]->tag) {
        case KOTLIN_NODE_ADD: op = CG_OP_FADD; break;
        case KOTLIN_NODE_SUB: op = CG_OP_FSUB; break;
        case KOTLIN_NODE_MUL: op = CG_OP_FMUL; break;
        case KOTLIN_NODE_DIV: op = CG_OP_FDIV; break;
        default: return false;
    }
    cg_value_t lhs = kotlin_backend_node(context, node->children[1]);
    cg_value_t rhs = kotlin_backend_node(context, node->children[2]);
    *result = lhs == CG_INVALID || rhs == CG_INVALID ? CG_INVALID
                                                   : cg_build_binary(builder, op, lhs, rhs);
    return true;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Kotlin integration failed at line %d: %s\n%s", __LINE__, #expr, diag.text); \
    goto cleanup; \
} } while (0)

int main(void) {
    static unsigned char parse_memory[4 << 20], semantic_memory[8 << 20], opt_memory[8 << 20];
    psl_arena parse, semantic, optimization;
    psl_arena_init(&parse, parse_memory, sizeof(parse_memory));
    psl_arena_init(&semantic, semantic_memory, sizeof(semantic_memory));
    psl_arena_init(&optimization, opt_memory, sizeof(opt_memory));
    cg_diag_t diag = {0};
    cg_module_t *module = NULL;
    cg_jit_t *jit = NULL;
    cg_buffer_t output = {0};
    int status = 1, errors = 0;
    const kotlin_node *root = NULL;
    psl_diag parse_error = {0};
    CHECK(kotlin_parse(&parse, psl_str_lit("fun evaluate(a: Double, b: Double): Double = -(a * 2.5) + b / 2.0"),
                       &root, &parse_error) >= 0);
    kotlin_ctx *ctx = kotlin_ctx_new(&semantic, NULL);
    CHECK(ctx && kotlin_analyze(ctx, root, diagnostic, &errors) >= 0 && errors == 0);
    const kotlin_opt_result optimized = kotlin_optimize(&optimization, root, NULL, NULL, 0);
    CHECK(!optimized.status);
    root = optimized.root;

    CHECK(shape(root, KOTLIN_NODE_KTFILE, 2));
    CHECK(shape(root->children[1], KOTLIN_NODE_LIST, 1));
    const kotlin_node *function = root->children[1]->children[0];
    CHECK(shape(function, KOTLIN_NODE_FUNDECL, 4));
    CHECK(psl_str_eq(function->children[2]->text_val, psl_str_lit("Double")));
    const kotlin_node *scope = function->children[3];
    CHECK(shape(scope, KOTLIN_NODE_FUNCTIONSCOPE, 3));
    const kotlin_node *params = scope->children[1];
    CHECK(shape(params, KOTLIN_NODE_LIST, 2));
    CHECK(shape(params->children[0], KOTLIN_NODE_FUNCTIONPARAM, 4));
    CHECK(shape(params->children[1], KOTLIN_NODE_FUNCTIONPARAM, 4));
    for (size_t i = 0; i < 2; ++i)
        CHECK(psl_str_eq(params->children[i]->children[1]->text_val, psl_str_lit("Double")));
    CHECK(shape(scope->children[2], KOTLIN_NODE_LIST, 1));
    const kotlin_node *statement = scope->children[2]->children[0];
    CHECK(shape(statement, KOTLIN_NODE_RETURNSTMT, 1));
    CHECK(shape(statement->children[0], KOTLIN_NODE_LIST, 1));
    const kotlin_node *body = statement->children[0]->children[0];

    struct parameter_names names = {params->children[0]->children[0]->text_val,
                                    params->children[1]->children[0]->text_val};
    const cg_type_t parameter_types[] = {CG_T_F64, CG_T_F64};
    const kotlin_backend_options options = {.lower = lower, .user = &names};
    module = kotlin_backend_lower(body, "evaluate", CG_T_F64, parameter_types, 2, &options, &diag);
    CHECK(module);
    cg_options_t target = cg_default_options();
    target.opt_level = 0;
    target.optimize_ir = false;
    CHECK(cg_compile_to_object(module, &target, &output, &diag));
    CHECK(output.bytes.count > 20 && output.bytes.data[0] == 0x64 && output.bytes.data[1] == 0x86);
    const cg_symbol_t symbol = cg_module_symbol(module, "evaluate");
    for (unsigned level = 0; level <= 1; ++level) {
        target.opt_level = level;
        target.optimize_ir = level != 0;
        jit = cg_jit_create(module, &target, NULL, &diag);
        CHECK(jit);
        void *address = cg_jit_symbol(jit, "evaluate");
        double (*native)(double, double);
        _Static_assert(sizeof(native) == sizeof(address), "compatible address representation");
        CHECK(address);
        memcpy(&native, &address, sizeof(native));
        for (int i = -24; i <= 24; ++i) {
            double a = i * 0.25, b = 7.0 - i * 0.5;
            const double expected = -(a * 2.5) + b / 2.0;
            uint64_t args[2], result;
            memcpy(&args[0], &a, sizeof(a));
            memcpy(&args[1], &b, sizeof(b));
            CHECK(cg_interp_run(module, symbol, args, 2, &result, NULL, &diag));
            double interpreted;
            memcpy(&interpreted, &result, sizeof(interpreted));
            CHECK(interpreted == expected && native(a, b) == expected);
        }
        cg_jit_destroy(jit);
        jit = NULL;
    }
    puts("Kotlin to nextc: verified MIR, COFF object and 98 interpreter/JIT comparisons");
    status = 0;
cleanup:
    if (jit) cg_jit_destroy(jit);
    if (module) cg_module_destroy(module);
    cg_buffer_free(&output);
    return status;
}
