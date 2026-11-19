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
        const kotlin_node *scope = node->children[3];
        const kotlin_node *body = scope->children[2];
        if (node->children[1]->tag == KOTLIN_NODE_NOTYPEPARAMETERS &&
            psl_str_eq(node->children[2]->text_val, psl_str_lit("Int")) &&
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
