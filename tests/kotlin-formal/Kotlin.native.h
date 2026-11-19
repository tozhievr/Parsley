/* Kotlin project rule for statically known top-level calls. */
#include <stdlib.h>

#define KT_CALL_DIAG(code_, message_) { \
    .size = sizeof(psl_diag_def), .code = PSL_STR_INIT(code_), \
    .default_sev = PSL_SEV_ERROR, .status = PSL_STATUS_REVIEWED, \
    .template_str = PSL_STR_INIT(message_), \
    .source_ref = PSL_STR_INIT("Kotlin specification: determining function applicability") \
}
static const psl_diag_def kt_missing = KT_CALL_DIAG("NO_VALUE_FOR_PARAMETER", "Missing required argument");
static const psl_diag_def kt_unknown = KT_CALL_DIAG("NAMED_PARAMETER_NOT_FOUND", "Unknown argument name");
static const psl_diag_def kt_duplicate = KT_CALL_DIAG("ARGUMENT_PASSED_TWICE", "Parameter was supplied twice");
static const psl_diag_def kt_extra = KT_CALL_DIAG("TOO_MANY_ARGUMENTS", "Too many arguments");
static const psl_diag_def kt_mismatch = KT_CALL_DIAG("ARGUMENT_TYPE_MISMATCH", "Incompatible argument type");
#undef KT_CALL_DIAG

static void kotlin_call_error(psl_sink sink, void *user, const psl_diag_def *def,
                              const kotlin_node *at) {
    const psl_diag diag = {.size = sizeof(diag), .def = def, .severity = def->default_sev,
                           .span = at->span, .message = def->template_str};
    if (sink) sink(user, &diag);
}

static const kotlin_node *kotlin_call_target(const kotlin_node *decls, psl_str name) {
    const kotlin_node *target = NULL;
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node *decl = decls->children[i];
        if (decl->tag != KOTLIN_NODE_FUNDECL ||
            !psl_str_eq(name, decl->children[0]->text_val)) continue;
        if (target) return NULL;
        target = decl;
    }
    return target;
}

static void kotlin_check_literal_argument(const kotlin_node *argument, const kotlin_node *param,
                                          psl_sink sink, void *user) {
    psl_str actual;
    switch (argument->tag) {
        case KOTLIN_NODE_INTLIT:
            if (argument->children[0]->num_val > INT32_MAX) return;
            actual = psl_str_lit("Int");
            break;
        case KOTLIN_NODE_FLOATLIT: actual = psl_str_lit("Double"); break;
        case KOTLIN_NODE_BOOLLIT: actual = psl_str_lit("Boolean"); break;
        case KOTLIN_NODE_STRLIT: actual = psl_str_lit("String"); break;
        default: return;
    }
    const psl_str expected = param->children[1]->text_val;
    if ((psl_str_eq(expected, psl_str_lit("Int")) ||
         psl_str_eq(expected, psl_str_lit("Double")) ||
         psl_str_eq(expected, psl_str_lit("Boolean")) ||
         psl_str_eq(expected, psl_str_lit("String"))) &&
        !psl_str_eq(expected, actual))
        kotlin_call_error(sink, user, &kt_mismatch, argument);
}

static int kotlin_bind_arguments(const kotlin_node *call, const kotlin_node *target,
                                 psl_sink sink, void *user) {
    const kotlin_node *params = target->children[3]->children[1];
    const kotlin_node *args = call->children[1];
    const size_t count = params->child_count;
    bool *bound = count ? calloc(count, sizeof(*bound)) : NULL;
    if (count && !bound) return -1;
    size_t next = 0;
    for (size_t i = 0; i < args->child_count; ++i) {
        const kotlin_node *arg = args->children[i];
        size_t index;
        if (arg->tag == KOTLIN_NODE_NAMEDARG) {
            const kotlin_node *name = arg->children[0];
            for (index = 0; index < count; ++index) {
                if (psl_str_eq(name->text_val, params->children[index]->children[0]->text_val)) break;
            }
            if (index == count || bound[index]) {
                kotlin_call_error(sink, user, index == count ? &kt_unknown : &kt_duplicate, name);
                continue;
            }
            bound[index] = true;
            if (!params->children[index]->children[2]->bool_val)
                kotlin_check_literal_argument(arg->children[1], params->children[index], sink, user);
            continue;
        }
        while (next < count && bound[next] && !params->children[next]->children[2]->bool_val) ++next;
        if (next == count) {
            kotlin_call_error(sink, user, &kt_extra, arg);
            continue;
        }
        bound[next] = true;
        kotlin_check_literal_argument(arg, params->children[next], sink, user);
    }
    for (size_t i = 0; i < count; ++i) {
        const kotlin_node *param = params->children[i];
        if (!bound[i] && !param->children[2]->bool_val && !param->children[3]->child_count)
            kotlin_call_error(sink, user, &kt_missing, call->children[0]);
    }
    free(bound);
    return 0;
}

/* Bottom types matter for the runtime array used by a generic vararg. */
enum kt_bottom { KT_OTHER, KT_NOTHING, KT_NULLABLE_NOTHING };

static const psl_diag_def kt_vararg_bottom = {
    .size = sizeof(psl_diag_def),
    .code = PSL_STR_INIT("ILLEGAL_TYPE_ARGUMENT_FOR_VARARG_PARAMETER_WARNING"),
    .default_sev = PSL_SEV_WARNING,
    .status = PSL_STATUS_DRAFT,
    .template_str = PSL_STR_INIT("Bottom type used as the element type of a vararg array"),
    .source_ref = PSL_STR_INIT("Kotlin compiler diagnostic KT-78252"),
};

static enum kt_bottom kotlin_bottom_type(const kotlin_node *type) {
    if (!psl_str_eq(type->children[0]->text_val, psl_str_lit("Nothing")) ||
        type->children[1]->child_count ||
        type->children[2]->tag != KOTLIN_NODE_NOTYPEARGUMENTS) return KT_OTHER;
    return type->children[3]->child_count ? KT_NULLABLE_NOTHING : KT_NOTHING;
}

/* Both type-parameter and explicit type-argument lists contain a head and tail. */
static const kotlin_node *kotlin_type_at(const kotlin_node *sequence, size_t index) {
    if (!index) return sequence->children[0];
    const kotlin_node *tail = sequence->children[1];
    return index <= tail->child_count ? tail->children[index - 1] : NULL;
}

static enum kt_bottom kotlin_infer_vararg_bottom(const kotlin_node *params, const kotlin_node *args,
                                                  psl_str type_name, const kotlin_node *decls) {
    enum kt_bottom inferred = KT_OTHER;
    for (size_t i = 0; i < args->child_count; ++i) {
        const kotlin_node *arg = args->children[i];
        if (i >= params->child_count || arg->tag == KOTLIN_NODE_NAMEDARG ||
            params->children[i]->children[2]->bool_val) return KT_OTHER;
        if (!psl_str_eq(params->children[i]->children[1]->text_val, type_name)) continue;
        if (arg->tag != KOTLIN_NODE_CALL) return KT_OTHER;
        const kotlin_node *target = kotlin_call_target(decls, arg->children[0]->text_val);
        if (!target || target->children[1]->tag != KOTLIN_NODE_NOTYPEPARAMETERS) return KT_OTHER;
        const kotlin_node *returns = target->children[2];
        if (returns->child_count != 1) return KT_OTHER;
        const enum kt_bottom current = kotlin_bottom_type(returns->children[0]);
        if (current == KT_OTHER) return KT_OTHER;
        if (current > inferred) inferred = current;
    }
    return inferred;
}

static void kotlin_check_generic_varargs(const kotlin_node *call, const kotlin_node *target,
                                         const kotlin_node *decls, psl_sink sink, void *user) {
    const bool explicit_types = call->tag == KOTLIN_NODE_GENERICCALL;
    const kotlin_node *type_params = target->children[1];
    const kotlin_node *params = target->children[3]->children[1];
    const kotlin_node *args = call->children[explicit_types ? 2 : 1];
    if (explicit_types &&
        call->children[1]->children[1]->child_count != type_params->children[1]->child_count) return;

    for (size_t index = 0; index <= type_params->children[1]->child_count; ++index) {
        const kotlin_node *type_param = kotlin_type_at(type_params, index);
        const psl_str name = type_param->children[0]->text_val;
        bool element_type = false;
        for (size_t i = 0; i < params->child_count; ++i) {
            const kotlin_node *param = params->children[i];
            if (param->children[2]->bool_val && psl_str_eq(param->children[1]->text_val, name)) {
                element_type = true;
                break;
            }
        }
        if (!element_type) continue;
        const kotlin_node *at = call->children[0];
        enum kt_bottom bottom;
        if (explicit_types) {
            at = kotlin_type_at(call->children[1], index);
            const bool placeholder = psl_str_eq(at->children[0]->text_val, psl_str_lit("_")) &&
                !at->children[1]->child_count &&
                at->children[2]->tag == KOTLIN_NODE_NOTYPEARGUMENTS && !at->children[3]->child_count;
            bottom = placeholder ? kotlin_infer_vararg_bottom(params, args, name, decls)
                                 : kotlin_bottom_type(at);
        } else {
            bottom = kotlin_infer_vararg_bottom(params, args, name, decls);
        }
        if (bottom == KT_NOTHING ||
            (bottom == KT_NULLABLE_NOTHING && type_param->children[1]->child_count))
            kotlin_call_error(sink, user, &kt_vararg_bottom, at);
    }
}

static int kotlin_check_call_nodes(const kotlin_node *node, const kotlin_node *decls,
                                   psl_sink sink, void *user) {
    if (node->tag == KOTLIN_NODE_CALL || node->tag == KOTLIN_NODE_GENERICCALL) {
        const kotlin_node *target = kotlin_call_target(decls, node->children[0]->text_val);
        if (target && target->children[1]->tag == KOTLIN_NODE_TYPEPARAMETERS) {
            kotlin_check_generic_varargs(node, target, decls, sink, user);
        } else if (target && node->tag == KOTLIN_NODE_CALL &&
                   kotlin_bind_arguments(node, target, sink, user) < 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < node->child_count; ++i) {
        if (kotlin_check_call_nodes(node->children[i], decls, sink, user) < 0) return -1;
    }
    return 0;
}

static int kotlin_check_calls(const kotlin_node *root, psl_sink sink, void *user) {
    const kotlin_node *decls = root->children[1];
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node *decl = decls->children[i];
        if (decl->tag == KOTLIN_NODE_FUNDECL && kotlin_check_call_nodes(decl, decls, sink, user) < 0)
            return -1;
    }
    return 0;
}

#ifndef KOTLIN_CALL_CHECK_ONLY
#define PARSLEY_NATIVE_CHECK kotlin_check_calls
/* Native entry adapter for the Kotlin conformance project. */
static bool kotlin_lower_entry(kotlin_backend_context *ctx, const kotlin_node *node,
                               cg_value_t *out, void *user) {
    (void)user;
    if (node->tag == KOTLIN_NODE_KTFILE) {
        const kotlin_node *decls = node->children[1];
        const kotlin_node *entry = NULL;
        for (size_t i = 0; i < decls->child_count; ++i) {
            const kotlin_node *decl = decls->children[i];
            if (decl->tag != KOTLIN_NODE_FUNDECL) {
                cg_diag_error(kotlin_backend_diagnostic(ctx), "native Kotlin entry supports only top-level functions");
                *out = CG_INVALID;
                return true;
            }
            if (!psl_str_eq(decl->children[0]->text_val, psl_str_lit("main"))) continue;
            if (entry) {
                cg_diag_error(kotlin_backend_diagnostic(ctx), "multiple Kotlin main declarations");
                *out = CG_INVALID;
                return true;
            }
            entry = decl;
        }
        if (entry) *out = kotlin_backend_node(ctx, entry);
        else {
            cg_diag_error(kotlin_backend_diagnostic(ctx), "Kotlin file has no main function");
            *out = CG_INVALID;
        }
        return true;
    }
    if (node->tag == KOTLIN_NODE_FUNDECL) {
        const kotlin_node *return_types = node->children[2];
        const kotlin_node *scope = node->children[3];
        const kotlin_node *body = scope->children[2];
        if (node->children[1]->tag == KOTLIN_NODE_NOTYPEPARAMETERS &&
            return_types->child_count == 1 &&
            psl_str_eq(return_types->children[0]->children[0]->text_val, psl_str_lit("Int")) &&
            return_types->children[0]->children[1]->child_count == 0 &&
            return_types->children[0]->children[2]->tag == KOTLIN_NODE_NOTYPEARGUMENTS &&
            return_types->children[0]->children[3]->child_count == 0 &&
            scope->children[1]->child_count == 0 && body->child_count == 1 &&
            body->children[0]->tag == KOTLIN_NODE_RETURNSTMT &&
            body->children[0]->children[0]->child_count == 1) {
            *out = kotlin_backend_node(ctx, body->children[0]->children[0]->children[0]);
        } else {
            cg_diag_error(kotlin_backend_diagnostic(ctx),
                          "native Kotlin main requires no parameters, an Int return type and one return expression");
            *out = CG_INVALID;
        }
        return true;
    }
    if (node->tag != KOTLIN_NODE_INTLIT) return false;
    /* Every Int32 is exact in the PSL adapter's double representation. */
    const double value = node->children[0]->num_val;
    if (!(value >= INT32_MIN && value <= INT32_MAX)) {
        cg_diag_error(kotlin_backend_diagnostic(ctx), "integer literal is outside Kotlin Int range");
        *out = CG_INVALID;
    } else {
        *out = cg_build_iconst(kotlin_backend_builder(ctx), CG_T_I32, (int32_t)value);
    }
    return true;
}

static const kotlin_backend_options kotlin_native_options = {.lower = kotlin_lower_entry};
#define PARSLEY_NATIVE_RESULT CG_T_I32
#define PARSLEY_NATIVE_RETURN "i32"
#define PARSLEY_NATIVE_OPTIONS (&kotlin_native_options)
#endif
