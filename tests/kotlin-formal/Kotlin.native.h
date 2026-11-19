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

struct kt_type_scope {
    const struct kt_type_scope *parent;
    const kotlin_node *list;
    size_t prefix;
};

static const kotlin_node *kotlin_local_declaration(const struct kt_type_scope *scope, psl_str name,
                                                   struct kt_type_scope *initializer_scope) {
    for (; scope; scope = scope->parent) {
        for (size_t i = scope->prefix; i > 0; --i) {
            const kotlin_node *node = scope->list->children[i - 1];
            const bool parameter = node->tag == KOTLIN_NODE_FUNCTIONPARAM;
            if ((!parameter && node->tag != KOTLIN_NODE_VALSTMT) ||
                !psl_str_eq(node->children[parameter ? 0 : 1]->text_val, name)) continue;
            *initializer_scope = (struct kt_type_scope){scope->parent, scope->list, i - 1};
            return node;
        }
    }
    return NULL;
}

static bool kotlin_run_is_builtin(const kotlin_node *decls, const struct kt_type_scope *scope) {
    const psl_str run = psl_str_lit("run");
    struct kt_type_scope unused;
    if (kotlin_local_declaration(scope, run, &unused)) return false;
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node *decl = decls->children[i];
        if (decl->tag == KOTLIN_NODE_FUNDECL && psl_str_eq(decl->children[0]->text_val, run))
            return false;
        if ((decl->tag == KOTLIN_NODE_PROPDECL || decl->tag == KOTLIN_NODE_CLASSDECL) &&
            psl_str_eq(decl->children[1]->text_val, run)) return false;
        if (decl->tag == KOTLIN_NODE_IMPORTDECL) return false;
    }
    return true;
}

static psl_str kotlin_argument_type(const kotlin_node *argument, const kotlin_node *decls,
                                   const struct kt_type_scope *scope, unsigned depth) {
    if (depth >= 64) return (psl_str){0};
    switch (argument->tag) {
        case KOTLIN_NODE_INTLIT:
            if (argument->children[0]->num_val <= INT32_MAX) return psl_str_lit("Int");
            break;
        case KOTLIN_NODE_FLOATLIT: return psl_str_lit("Double");
        case KOTLIN_NODE_BOOLLIT: return psl_str_lit("Boolean");
        case KOTLIN_NODE_STRLIT: return psl_str_lit("String");
        case KOTLIN_NODE_VAR: {
            struct kt_type_scope initializer_scope;
            const kotlin_node *local = kotlin_local_declaration(scope, argument->children[0]->text_val,
                                                               &initializer_scope);
            if (!local) break;
            if (local->tag == KOTLIN_NODE_FUNCTIONPARAM)
                return local->children[2]->bool_val ? (psl_str){0} : local->children[1]->text_val;
            const psl_str declared = local->children[2]->text_val;
            if (declared.len) return declared;
            const kotlin_node *init = local->children[3];
            if (init->child_count == 1)
                return kotlin_argument_type(init->children[0], decls, &initializer_scope, depth + 1);
            break;
        }
        case KOTLIN_NODE_CALL: {
            const kotlin_node *args = argument->children[1];
            if (!psl_str_eq(argument->children[0]->text_val, psl_str_lit("run")) ||
                args->child_count != 1 || !kotlin_run_is_builtin(decls, scope)) break;
            const kotlin_node *lambda = args->children[0];
            if (lambda->tag != KOTLIN_NODE_LAMBDAEXPR) break;
            const kotlin_node *lambda_scope = lambda->children[1];
            const kotlin_node *params = lambda_scope->children[0];
            if (params->tag == KOTLIN_NODE_EXPLICITLAMBDAPARAMS && params->children[0]->child_count) break;
            const kotlin_node *body = lambda_scope->children[1];
            if (!body->child_count) break;
            const size_t last = body->child_count - 1;
            for (size_t i = 0; i < last; ++i)
                if (body->children[i]->tag != KOTLIN_NODE_VALSTMT) return (psl_str){0};
            const kotlin_node *stmt = body->children[last];
            if (stmt->tag != KOTLIN_NODE_EXPRSTMT) break;
            const struct kt_type_scope inner = {scope, body, last};
            return kotlin_argument_type(stmt->children[0], decls, &inner, depth + 1);
        }
        default: break;
    }
    return (psl_str){0};
}

static void kotlin_check_argument(const kotlin_node *argument, const kotlin_node *param,
                                  const kotlin_node *decls, const struct kt_type_scope *scope,
                                  psl_sink sink, void *user) {
    const psl_str actual = kotlin_argument_type(argument, decls, scope, 0);
    if (!(psl_str_eq(actual, psl_str_lit("Int")) || psl_str_eq(actual, psl_str_lit("Double")) ||
          psl_str_eq(actual, psl_str_lit("Boolean")) || psl_str_eq(actual, psl_str_lit("String"))))
        return;
    const psl_str expected = param->children[1]->text_val;
    if ((psl_str_eq(expected, psl_str_lit("Int")) ||
         psl_str_eq(expected, psl_str_lit("Double")) ||
         psl_str_eq(expected, psl_str_lit("Boolean")) ||
         psl_str_eq(expected, psl_str_lit("String"))) &&
        !psl_str_eq(expected, actual))
        kotlin_call_error(sink, user, &kt_mismatch, argument);
}

static int kotlin_bind_arguments(const kotlin_node *call, const kotlin_node *target,
                                 const kotlin_node *decls, const struct kt_type_scope *scope,
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
                kotlin_check_argument(arg->children[1], params->children[index], decls, scope, sink, user);
            continue;
        }
        while (next < count && bound[next] && !params->children[next]->children[2]->bool_val) ++next;
        if (next == count) {
            kotlin_call_error(sink, user, &kt_extra, arg);
            continue;
        }
        bound[next] = true;
        kotlin_check_argument(arg, params->children[next], decls, scope, sink, user);
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
                                   const struct kt_type_scope *scope, psl_sink sink, void *user) {
    if (node->tag == KOTLIN_NODE_LAMBDASCOPE || node->tag == KOTLIN_NODE_CATCHCLAUSE ||
        node->tag == KOTLIN_NODE_FORSTMT) scope = NULL;
    if (node->tag == KOTLIN_NODE_CALL || node->tag == KOTLIN_NODE_GENERICCALL) {
        const kotlin_node *target = kotlin_call_target(decls, node->children[0]->text_val);
        if (target && target->children[1]->tag == KOTLIN_NODE_TYPEPARAMETERS) {
            kotlin_check_generic_varargs(node, target, decls, sink, user);
        } else if (target && node->tag == KOTLIN_NODE_CALL &&
                   kotlin_bind_arguments(node, target, decls, scope, sink, user) < 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < node->child_count; ++i) {
        struct kt_type_scope prefix = {scope, node, i};
        const struct kt_type_scope *child_scope = node->tag == KOTLIN_NODE_LIST ? &prefix : scope;
        if (node->tag == KOTLIN_NODE_FUNCTIONSCOPE && i == 2) {
            const kotlin_node *params = node->children[1];
            prefix = (struct kt_type_scope){scope, params, params->child_count};
            child_scope = &prefix;
        }
        if (kotlin_check_call_nodes(node->children[i], decls, child_scope, sink, user) < 0) return -1;
    }
    return 0;
}

static int kotlin_check_calls(const kotlin_node *root, psl_sink sink, void *user) {
    const kotlin_node *decls = root->children[1];
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node *decl = decls->children[i];
        if (decl->tag == KOTLIN_NODE_FUNDECL && kotlin_check_call_nodes(decl, decls, NULL, sink, user) < 0)
            return -1;
    }
    return 0;
}

#ifndef KOTLIN_CALL_CHECK_ONLY
#define PARSLEY_NATIVE_CHECK kotlin_check_calls
/* Evaluate closed, side-effect-free box bodies before emitting their string. */
enum kt_const_kind { KT_CONST_UNSUPPORTED, KT_CONST_INT, KT_CONST_BOOL, KT_CONST_TEXT };

struct kt_const {
    enum kt_const_kind kind;
    int32_t number;
    cg_buffer_t text;
};

struct kt_binding {
    psl_str name;
    bool mutable;
    struct kt_const value;
};

struct kt_constants {
    struct kt_binding bindings[64];
    size_t count;
    const kotlin_node *functions;
    const kotlin_node *declarations;
    unsigned *steps;
};

static bool kotlin_text_append(cg_buffer_t *text, const void *ptr, size_t size) {
    return size <= 65535u - text->bytes.count && (!size || cg_buffer_bytes(text, ptr, size));
}

static const struct kt_const *kotlin_constant(const struct kt_constants *env, psl_str name) {
    for (size_t i = env->count; i > 0; --i)
        if (psl_str_eq(name, env->bindings[i - 1].name)) return &env->bindings[i - 1].value;
    return NULL;
}

enum kt_flow { KT_FAILED, KT_CONTINUE, KT_RETURN };

static enum kt_flow kotlin_eval_box_block(const kotlin_node *, struct kt_constants *,
                                          unsigned, struct kt_const *);

static bool kotlin_eval_constant(const kotlin_node *, const struct kt_constants *,
                                 unsigned, struct kt_const *);

static bool kotlin_append_constant(cg_buffer_t *text, const struct kt_const *value) {
    if (value->kind == KT_CONST_TEXT)
        return kotlin_text_append(text, value->text.bytes.data, value->text.bytes.count);
    if (value->kind == KT_CONST_INT) {
        char digits[16];
        int count = snprintf(digits, sizeof(digits), "%" PRId32, value->number);
        return count > 0 && kotlin_text_append(text, digits, (size_t)count);
    }
    if (value->kind == KT_CONST_BOOL)
        return value->number ? kotlin_text_append(text, "true", 4)
                             : kotlin_text_append(text, "false", 5);
    return false;
}

static bool kotlin_append_interpolation(cg_buffer_t *text, psl_str expr,
                                        const struct kt_constants *env, unsigned depth) {
    static const char prefix[] = "fun interpolated() = ";
    if (expr.len > 4096) return false;
    char source[sizeof(prefix) + 4096];
    memcpy(source, prefix, sizeof(prefix) - 1);
    memcpy(source + sizeof(prefix) - 1, expr.ptr, expr.len);
    void *memory = malloc(256u * 1024u);
    if (!memory) return false;
    psl_arena arena;
    psl_arena_init(&arena, memory, 256u * 1024u);
    const kotlin_node *root = NULL;
    bool ok = false;
    if (kotlin_parse(&arena, psl_str_make(source, sizeof(prefix) - 1 + expr.len), &root, NULL) == 0 &&
        root->children[1]->child_count == 1) {
        const kotlin_node *decl = root->children[1]->children[0];
        if (decl->tag == KOTLIN_NODE_FUNDECL) {
            const kotlin_node *body = decl->children[3]->children[2];
            if (body->child_count == 1 && body->children[0]->tag == KOTLIN_NODE_RETURNSTMT) {
                const kotlin_node *returns = body->children[0]->children[0];
                struct kt_const value = {0};
                ok = returns->child_count == 1 &&
                     kotlin_eval_constant(returns->children[0], env, depth + 1, &value) &&
                     kotlin_append_constant(text, &value);
                cg_buffer_free(&value.text);
            }
        }
    }
    free(memory);
    return ok;
}

static bool kotlin_name_byte(char ch, bool first) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
           (!first && ch >= '0' && ch <= '9');
}

static bool kotlin_eval_template(psl_str literal, const struct kt_constants *env, unsigned depth,
                                 cg_buffer_t *text) {
    if (literal.len < 2 || literal.ptr[0] != '"' || literal.ptr[literal.len - 1] != '"') return false;
    const size_t end = literal.len - 1;
    for (size_t i = 1; i < end; ++i) {
        char ch = literal.ptr[i];
        if (ch == '\\') {
            if (++i >= end) return false;
            ch = literal.ptr[i];
            switch (ch) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'b': ch = '\b'; break;
                case '"': case '\'': case '\\': case '$': break;
                default: return false;
            }
        } else if (ch == '$' && i + 1 < end) {
            size_t start = i + 1;
            if (literal.ptr[start] == '{') {
                ++start;
                size_t stop = start;
                while (stop < end && literal.ptr[stop] != '}') ++stop;
                if (stop == end || !kotlin_append_interpolation(
                        text, psl_str_make(literal.ptr + start, stop - start), env, depth)) return false;
                i = stop;
                continue;
            }
            if (kotlin_name_byte(literal.ptr[start], true)) {
                size_t stop = start + 1;
                while (stop < end && kotlin_name_byte(literal.ptr[stop], false)) ++stop;
                const struct kt_const *value = kotlin_constant(env, psl_str_make(literal.ptr + start, stop - start));
                if (!value || !kotlin_append_constant(text, value)) return false;
                i = stop - 1;
                continue;
            }
        } else if (ch == '\n' || ch == '\r' || ch == '\0') {
            return false;
        }
        if (!kotlin_text_append(text, &ch, 1)) return false;
    }
    return true;
}

static bool kotlin_eval_constant_binary(kotlin_node_tag op, const struct kt_const *left,
                                        const struct kt_const *right, struct kt_const *out) {
    if (left->kind != right->kind) return false;
    if (op == KOTLIN_NODE_EQ || op == KOTLIN_NODE_NEQ) {
        bool equal = left->kind == KT_CONST_TEXT
            ? left->text.bytes.count == right->text.bytes.count &&
                (!left->text.bytes.count || !memcmp(left->text.bytes.data, right->text.bytes.data,
                                                    left->text.bytes.count))
            : left->number == right->number;
        out->kind = KT_CONST_BOOL;
        out->number = op == KOTLIN_NODE_EQ ? equal : !equal;
        return true;
    }
    if (left->kind == KT_CONST_TEXT && op == KOTLIN_NODE_ADD) {
        out->kind = KT_CONST_TEXT;
        return kotlin_append_constant(&out->text, left) && kotlin_append_constant(&out->text, right);
    }
    if (left->kind != KT_CONST_INT) return false;
    uint32_t bits;
    switch (op) {
        case KOTLIN_NODE_ADD: bits = (uint32_t)left->number + (uint32_t)right->number; break;
        case KOTLIN_NODE_SUB: bits = (uint32_t)left->number - (uint32_t)right->number; break;
        case KOTLIN_NODE_MUL: bits = (uint32_t)left->number * (uint32_t)right->number; break;
        default: return false;
    }
    out->kind = KT_CONST_INT;
    out->number = (int32_t)(bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(4294967296));
    return true;
}

static bool kotlin_constant_has_type(const struct kt_const *value, psl_str type) {
    return (value->kind == KT_CONST_INT && psl_str_eq(type, psl_str_lit("Int"))) ||
           (value->kind == KT_CONST_BOOL && psl_str_eq(type, psl_str_lit("Boolean"))) ||
           (value->kind == KT_CONST_TEXT && psl_str_eq(type, psl_str_lit("String")));
}

static bool kotlin_eval_constant_call(const kotlin_node *node, const struct kt_constants *env,
                                      unsigned depth, struct kt_const *out) {
    const kotlin_node *target = kotlin_call_target(env->functions, node->children[0]->text_val);
    if (!target || target->children[1]->tag != KOTLIN_NODE_NOTYPEPARAMETERS) return false;
    const kotlin_node *scope = target->children[3];
    const kotlin_node *params = scope->children[1];
    const kotlin_node *args = node->children[1];
    struct kt_constants callee = {.functions = env->functions, .declarations = env->declarations,
                                  .steps = env->steps};
    if (params->child_count != args->child_count ||
        params->child_count > sizeof(callee.bindings) / sizeof(callee.bindings[0])) return false;

    bool ok = true;
    for (size_t i = 0; i < params->child_count && ok; ++i) {
        const kotlin_node *param = params->children[i];
        if (param->children[2]->bool_val) { ok = false; break; }
        struct kt_binding *binding = &callee.bindings[callee.count++];
        binding->name = param->children[0]->text_val;
        ok = kotlin_eval_constant(args->children[i], env, depth + 1, &binding->value);
        const psl_str type = param->children[1]->text_val;
        ok = ok && kotlin_constant_has_type(&binding->value, type);
    }
    if (ok) ok = kotlin_eval_box_block(scope->children[2], &callee, depth + 1, out) == KT_RETURN;
    const kotlin_node *return_types = target->children[2];
    if (ok && return_types->child_count) {
        const kotlin_node *type = return_types->children[0];
        ok = !type->children[1]->child_count &&
             type->children[2]->tag == KOTLIN_NODE_NOTYPEARGUMENTS &&
             kotlin_constant_has_type(out, type->children[0]->text_val);
    }
    while (callee.count) cg_buffer_free(&callee.bindings[--callee.count].value.text);
    return ok;
}

static const kotlin_node *kotlin_constructor_class(const kotlin_node *decls, psl_str name) {
    for (unsigned hop = 0; hop < 64; ++hop) {
        const kotlin_node *found = NULL;
        for (size_t i = 0; i < decls->child_count; ++i) {
            const kotlin_node *decl = decls->children[i];
            const bool is_class = decl->tag == KOTLIN_NODE_CLASSDECL;
            if (!is_class && decl->tag != KOTLIN_NODE_TYPEALIASDECL) continue;
            if (!psl_str_eq(name, decl->children[is_class ? 1 : 0]->text_val)) continue;
            if (found) return NULL;
            found = decl;
        }
        if (!found || found->tag == KOTLIN_NODE_CLASSDECL) return found;
        const kotlin_node *type = found->children[1];
        if (type->children[1]->child_count || type->children[3]->child_count ||
            type->children[2]->tag != KOTLIN_NODE_NOTYPEARGUMENTS) return NULL;
        name = type->children[0]->text_val;
    }
    return NULL;
}

static bool kotlin_eval_constructor_property(const kotlin_node *node, const struct kt_constants *env,
                                             unsigned depth, struct kt_const *out) {
    const kotlin_node *call = node->children[0], *suffixes = node->children[1];
    if (call->tag != KOTLIN_NODE_CALL || suffixes->child_count != 1) return false;
    const kotlin_node *suffix = suffixes->children[0];
    if (suffix->tag != KOTLIN_NODE_SELECTSUFFIX || suffix->children[0]->bool_val) return false;
    const psl_str name = call->children[0]->text_val;
    /* Calls ambiguous with a top-level function need overload resolution. */
    for (size_t i = 0; i < env->functions->child_count; ++i) {
        const kotlin_node *decl = env->functions->children[i];
        if (decl->tag == KOTLIN_NODE_FUNDECL && psl_str_eq(name, decl->children[0]->text_val))
            return false;
    }
    const kotlin_node *klass = kotlin_constructor_class(env->declarations, name);
    if (!klass || klass->children[5]->bool_val || klass->children[4]->child_count ||
        !psl_str_eq(klass->children[0]->text_val, psl_str_lit("class")) ||
        klass->children[2]->tag != KOTLIN_NODE_NOTYPEPARAMETERS) return false;
    const kotlin_node *params = klass->children[3], *args = call->children[1];
    if (params->child_count != args->child_count) return false;
    const psl_str property = suffix->children[1]->text_val;
    bool selected = false;
    for (size_t i = 0; i < params->child_count; ++i) {
        const kotlin_node *param = params->children[i];
        const bool matches = psl_str_eq(property, param->children[1]->text_val);
        if (matches && (selected || !psl_str_eq(param->children[0]->text_val, psl_str_lit("val"))))
            return false;
        struct kt_const value = {0};
        bool ok = kotlin_eval_constant(args->children[i], env, depth + 1, &value) &&
                  kotlin_constant_has_type(&value, param->children[2]->text_val);
        if (ok && matches) {
            *out = value;
            selected = true;
        } else {
            cg_buffer_free(&value.text);
        }
        if (!ok) return false;
    }
    return selected;
}

static bool kotlin_eval_constant(const kotlin_node *node, const struct kt_constants *env,
                                 unsigned depth, struct kt_const *out) {
    if (depth >= 64 || *env->steps >= 100000) return false;
    ++*env->steps;
    switch (node->tag) {
        case KOTLIN_NODE_STRLIT:
            out->kind = KT_CONST_TEXT;
            return kotlin_eval_template(node->children[0]->text_val, env, depth, &out->text);
        case KOTLIN_NODE_INTLIT: {
            const double number = node->children[0]->num_val;
            if (!(number >= 0 && number <= INT32_MAX)) return false;
            out->kind = KT_CONST_INT;
            out->number = (int32_t)number;
            return true;
        }
        case KOTLIN_NODE_BOOLLIT:
            out->kind = KT_CONST_BOOL;
            out->number = node->children[0]->bool_val;
            return true;
        case KOTLIN_NODE_VAR: {
            const struct kt_const *value = kotlin_constant(env, node->children[0]->text_val);
            if (!value) return false;
            out->kind = value->kind;
            out->number = value->number;
            return value->kind != KT_CONST_TEXT || kotlin_append_constant(&out->text, value);
        }
        case KOTLIN_NODE_POSTFIXCHAIN:
            return kotlin_eval_constructor_property(node, env, depth, out);
        case KOTLIN_NODE_CALL:
            return kotlin_eval_constant_call(node, env, depth, out);
        case KOTLIN_NODE_BIN: {
            struct kt_const left = {0}, right = {0};
            bool ok = kotlin_eval_constant(node->children[1], env, depth + 1, &left) &&
                      kotlin_eval_constant(node->children[2], env, depth + 1, &right) &&
                      kotlin_eval_constant_binary(node->children[0]->tag, &left, &right, out);
            cg_buffer_free(&left.text);
            cg_buffer_free(&right.text);
            return ok;
        }
        default: return false;
    }
}

static enum kt_flow kotlin_eval_box_block(const kotlin_node *body, struct kt_constants *env,
                                          unsigned depth, struct kt_const *out) {
    if (depth >= 64) return KT_FAILED;
    const size_t saved = env->count;
    enum kt_flow flow = KT_CONTINUE;
    for (size_t i = 0; i < body->child_count && flow == KT_CONTINUE; ++i) {
        const kotlin_node *stmt = body->children[i];
        if (stmt->tag == KOTLIN_NODE_VALSTMT &&
            !stmt->children[2]->text_val.len && stmt->children[3]->child_count == 1 &&
            env->count < sizeof(env->bindings) / sizeof(env->bindings[0])) {
            struct kt_binding *binding = &env->bindings[env->count];
            *binding = (struct kt_binding){.name = stmt->children[1]->text_val,
                                           .mutable = stmt->children[0]->bool_val};
            bool ok = kotlin_eval_constant(stmt->children[3]->children[0], env, depth + 1, &binding->value);
            ++env->count;
            if (!ok) flow = KT_FAILED;
        } else if (stmt->tag == KOTLIN_NODE_ASSIGNSTMT &&
                   psl_str_eq(stmt->children[1]->text_val, psl_str_lit("="))) {
            const psl_str name = stmt->children[0]->text_val;
            size_t index = env->count;
            while (index && !psl_str_eq(name, env->bindings[index - 1].name)) --index;
            struct kt_const replacement = {0};
            if (index && env->bindings[index - 1].mutable &&
                kotlin_eval_constant(stmt->children[2], env, depth + 1, &replacement) &&
                replacement.kind == env->bindings[index - 1].value.kind) {
                cg_buffer_free(&env->bindings[index - 1].value.text);
                env->bindings[index - 1].value = replacement;
            } else {
                cg_buffer_free(&replacement.text);
                flow = KT_FAILED;
            }
        } else if (stmt->tag == KOTLIN_NODE_IFSTMT) {
            struct kt_const condition = {0};
            if (kotlin_eval_constant(stmt->children[0], env, depth + 1, &condition) &&
                condition.kind == KT_CONST_BOOL) {
                flow = kotlin_eval_box_block(stmt->children[condition.number ? 1 : 2], env, depth + 1, out);
            } else {
                flow = KT_FAILED;
            }
            cg_buffer_free(&condition.text);
        } else if (stmt->tag == KOTLIN_NODE_RETURNSTMT && stmt->children[0]->child_count == 1) {
            flow = kotlin_eval_constant(stmt->children[0]->children[0], env, depth + 1, out)
                ? KT_RETURN : KT_FAILED;
        } else {
            flow = KT_FAILED;
        }
    }
    while (env->count > saved) cg_buffer_free(&env->bindings[--env->count].value.text);
    return flow;
}

static cg_value_t kotlin_emit_box_text(kotlin_backend_context *ctx, cg_buffer_t *text) {
    if (!cg_buffer_u8(text, 0)) return CG_INVALID;
    cg_builder_t *builder = kotlin_backend_builder(ctx);
    char name[48];
    snprintf(name, sizeof(name), "kotlin_string_%" PRIu32, builder->module->symbols.count);
    cg_symbol_t symbol = cg_module_add_data(builder->module, name, text->bytes.count, 1,
                                            text->bytes.data, CG_DATA_RO, CG_BIND_LOCAL);
    return symbol == CG_INVALID ? CG_INVALID : cg_build_symaddr(builder, symbol);
}


static bool kotlin_box_return_type(const kotlin_node *types) {
    if (!types->child_count) return true;
    const kotlin_node *type = types->children[0];
    return psl_str_eq(type->children[0]->text_val, psl_str_lit("String")) &&
           !type->children[1]->child_count && !type->children[3]->child_count &&
           type->children[2]->tag == KOTLIN_NODE_NOTYPEARGUMENTS;
}

static const kotlin_node *kotlin_companion_members(const kotlin_node *decls, psl_str name) {
    const kotlin_node *members = NULL;
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node *decl = decls->children[i];
        if (decl->tag != KOTLIN_NODE_CLASSDECL ||
            !psl_str_eq(decl->children[1]->text_val, name)) continue;
        if (members) return NULL;
        members = decl->children[4];
    }
    if (!members) return NULL;
    const kotlin_node *companion = NULL;
    for (size_t i = 0; i < members->child_count; ++i) {
        const kotlin_node *member = members->children[i];
        if (member->tag != KOTLIN_NODE_COMPANIONDECL) continue;
        if (companion) return NULL;
        companion = member->children[1];
    }
    if (!companion) return NULL;
    for (size_t i = 0; i < companion->child_count; ++i) {
        /* Initializers may have effects and need companion object storage. */
        if (companion->children[i]->tag != KOTLIN_NODE_FUNDECL) return NULL;
    }
    return companion;
}

static cg_value_t kotlin_lower_box_function(kotlin_backend_context *ctx, const kotlin_node *function,
                                             const kotlin_node *decls, const kotlin_node *scope_decls,
                                             unsigned depth) {
    if (!function || depth >= 64 || function->children[1]->tag != KOTLIN_NODE_NOTYPEPARAMETERS ||
        !kotlin_box_return_type(function->children[2])) return CG_INVALID;
    const kotlin_node *scope = function->children[3];
    const kotlin_node *body = scope->children[2];
    if (scope->children[1]->child_count) return CG_INVALID;
    unsigned steps = 0;
    struct kt_constants env = {.functions = scope_decls, .declarations = decls, .steps = &steps};
    struct kt_const constant = {0};
    const enum kt_flow flow = kotlin_eval_box_block(body, &env, 0, &constant);
    cg_value_t value = flow == KT_RETURN && constant.kind == KT_CONST_TEXT
        ? kotlin_emit_box_text(ctx, &constant.text) : CG_INVALID;
    cg_buffer_free(&constant.text);
    if (value != CG_INVALID) return value;
    if (body->child_count != 1 || body->children[0]->tag != KOTLIN_NODE_RETURNSTMT)
        return CG_INVALID;
    const kotlin_node *returns = body->children[0]->children[0];
    if (returns->child_count != 1) return CG_INVALID;
    const kotlin_node *expr = returns->children[0];

    if (expr->tag == KOTLIN_NODE_CALL && !expr->children[1]->child_count) {
        const kotlin_node *callee = kotlin_call_target(scope_decls, expr->children[0]->text_val);
        return kotlin_lower_box_function(ctx, callee, decls, scope_decls, depth + 1);
    }
    if (expr->tag == KOTLIN_NODE_METHODCALL &&
        expr->children[0]->tag == KOTLIN_NODE_CALLRECEIVER && !expr->children[2]->child_count) {
        const psl_str receiver = expr->children[0]->children[0]->text_val;
        const kotlin_node *members = kotlin_companion_members(decls, receiver);
        if (!members) return CG_INVALID;
        const kotlin_node *callee = kotlin_call_target(members, expr->children[1]->text_val);
        return kotlin_lower_box_function(ctx, callee, decls, members, depth + 1);
    }
    return CG_INVALID;
}

static cg_value_t kotlin_lower_box(kotlin_backend_context *ctx, const kotlin_node *decls) {
    for (size_t i = 0; i < decls->child_count; ++i) {
        const kotlin_node_tag tag = decls->children[i]->tag;
        if (tag != KOTLIN_NODE_FUNDECL && tag != KOTLIN_NODE_IMPORTDECL &&
            tag != KOTLIN_NODE_CLASSDECL && tag != KOTLIN_NODE_TYPEALIASDECL) {
            cg_diag_error(kotlin_backend_diagnostic(ctx), "box lowering supports top-level functions, classes, type aliases and imports");
            return CG_INVALID;
        }
    }
    const kotlin_node *function = kotlin_call_target(decls, psl_str_lit("box"));
    cg_value_t result = kotlin_lower_box_function(ctx, function, decls, decls, 0);
    if (result == CG_INVALID)
        cg_diag_error(kotlin_backend_diagnostic(ctx), "unsupported or missing Kotlin box string entry");
    return result;
}

/* Native entry adapter for the Kotlin conformance project. */
static bool kotlin_lower_entry(kotlin_backend_context *ctx, const kotlin_node *node,
                               cg_value_t *out, void *user) {
    (void)user;
    if (node->tag == KOTLIN_NODE_KTFILE) {
        const kotlin_node *decls = node->children[1];
        if (kotlin_backend_builder(ctx)->func->ret_type == CG_T_PTR) {
            *out = kotlin_lower_box(ctx, decls);
            return true;
        }
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
