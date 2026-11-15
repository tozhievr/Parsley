#include "parser.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SwiftMetrics {
  size_t imports;
  size_t structs;
  size_t classes;
  size_t enums;
  size_t funcs;
  size_t let_count;
  size_t var_count;
  size_t guard_count;
  size_t branch_points;
  size_t warn_mutable_var;
  size_t warn_div_zero;
  size_t warn_duplicate_decl;
} SwiftMetrics;

typedef struct NameTable {
  const char* names[128];
  size_t count;
} NameTable;

static bool name_table_add(NameTable* tbl, const char* name) {
  if (!name || name[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < tbl->count; ++i) {
    if (strcmp(tbl->names[i], name) == 0) {
      return true; /* duplicate */
    }
  }
  if (tbl->count < 128) {
    tbl->names[tbl->count++] = name;
  }
  return false;
}

static bool is_ctor(const ParsleyValue* v, const char* name) {
  return v != NULL && v->kind == PARSLEY_VAL_CTOR && v->ctor_name != NULL &&
         strcmp(v->ctor_name, name) == 0;
}

static const char* get_string_val(const ParsleyValue* v) {
  if (v != NULL && v->kind == PARSLEY_VAL_STRING && v->string_val != NULL) {
    return v->string_val;
  }
  return "";
}

static bool expr_is_zero_literal(const ParsleyValue* v) {
  if (is_ctor(v, "SwNum") && v->item_count >= 1 && v->items[0] != NULL &&
      v->items[0]->kind == PARSLEY_VAL_NUMBER) {
    return fabs(v->items[0]->number_val) < 1e-12;
  }
  return false;
}

static void walk_expr(const ParsleyValue* v, SwiftMetrics* m) {
  if (v == NULL) {
    return;
  }
  if (v->kind == PARSLEY_VAL_LIST || v->kind == PARSLEY_VAL_TUPLE) {
    for (size_t i = 0; i < v->item_count; ++i) {
      walk_expr(v->items[i], m);
    }
    return;
  }
  if (v->kind != PARSLEY_VAL_CTOR) {
    return;
  }
  if (is_ctor(v, "SwBin") && v->item_count >= 3) {
    const ParsleyValue* op = v->items[0];
    const ParsleyValue* lhs = v->items[1];
    const ParsleyValue* rhs = v->items[2];
    if (is_ctor(op, "SwDiv") && expr_is_zero_literal(rhs)) {
      m->warn_div_zero++;
    }
    if (is_ctor(op, "SwAnd") || is_ctor(op, "SwOr")) {
      m->branch_points++;
    }
    walk_expr(lhs, m);
    walk_expr(rhs, m);
    return;
  }
  for (size_t i = 0; i < v->item_count; ++i) {
    walk_expr(v->items[i], m);
  }
}

static void walk_stmt_list(const ParsleyValue* list, SwiftMetrics* m);

static void walk_stmt(const ParsleyValue* v, SwiftMetrics* m) {
  if (v == NULL || v->kind != PARSLEY_VAL_CTOR) {
    return;
  }
  if (is_ctor(v, "StmtLet") && v->item_count >= 3) {
    m->let_count++;
    walk_expr(v->items[2], m);
  } else if (is_ctor(v, "StmtVar") && v->item_count >= 3) {
    m->var_count++;
    m->warn_mutable_var++;
    walk_expr(v->items[2], m);
  } else if (is_ctor(v, "StmtIf") && v->item_count >= 3) {
    m->branch_points++;
    walk_expr(v->items[0], m);
    walk_stmt_list(v->items[1], m);
    walk_stmt_list(v->items[2], m);
  } else if (is_ctor(v, "StmtGuard") && v->item_count >= 2) {
    m->guard_count++;
    m->branch_points++;
    walk_expr(v->items[0], m);
    walk_stmt_list(v->items[1], m);
  } else if (is_ctor(v, "StmtWhile") && v->item_count >= 2) {
    m->branch_points++;
    walk_expr(v->items[0], m);
    walk_stmt_list(v->items[1], m);
  } else if (is_ctor(v, "StmtFor") && v->item_count >= 3) {
    m->branch_points++;
    walk_expr(v->items[1], m);
    walk_stmt_list(v->items[2], m);
  } else if (is_ctor(v, "StmtReturn") && v->item_count >= 1) {
    walk_expr(v->items[0], m);
  } else if (is_ctor(v, "StmtExpr") && v->item_count >= 1) {
    walk_expr(v->items[0], m);
  }
}

static void walk_stmt_list(const ParsleyValue* list, SwiftMetrics* m) {
  if (list == NULL || list->kind != PARSLEY_VAL_LIST) {
    return;
  }
  for (size_t i = 0; i < list->item_count; ++i) {
    walk_stmt(list->items[i], m);
  }
}

static void walk_decl_list(const ParsleyValue* list, SwiftMetrics* m) {
  if (list == NULL || list->kind != PARSLEY_VAL_LIST) {
    return;
  }
  NameTable scope_names = {0};
  for (size_t i = 0; i < list->item_count; ++i) {
    const ParsleyValue* d = list->items[i];
    if (is_ctor(d, "DeclImport")) {
      m->imports++;
    } else if (is_ctor(d, "DeclStruct") && d->item_count >= 2) {
      m->structs++;
      if (name_table_add(&scope_names, get_string_val(d->items[0]))) {
        m->warn_duplicate_decl++;
      }
      walk_decl_list(d->items[1], m);
    } else if (is_ctor(d, "DeclClass") && d->item_count >= 3) {
      m->classes++;
      if (name_table_add(&scope_names, get_string_val(d->items[0]))) {
        m->warn_duplicate_decl++;
      }
      walk_decl_list(d->items[2], m);
    } else if (is_ctor(d, "DeclEnum") && d->item_count >= 2) {
      m->enums++;
      if (name_table_add(&scope_names, get_string_val(d->items[0]))) {
        m->warn_duplicate_decl++;
      }
      walk_decl_list(d->items[1], m);
    } else if (is_ctor(d, "DeclFunc") && d->item_count >= 4) {
      m->funcs++;
      if (name_table_add(&scope_names, get_string_val(d->items[0]))) {
        m->warn_duplicate_decl++;
      }
      walk_stmt_list(d->items[3], m);
    } else if (is_ctor(d, "DeclField") && d->item_count >= 1) {
      const ParsleyValue* fld = d->items[0];
      if (is_ctor(fld, "SwField") && fld->item_count >= 4) {
        const bool is_var =
            fld->items[0] != NULL && fld->items[0]->kind == PARSLEY_VAL_BOOL &&
            fld->items[0]->bool_val;
        if (is_var) {
          m->var_count++;
          m->warn_mutable_var++;
        } else {
          m->let_count++;
        }
        if (name_table_add(&scope_names, get_string_val(fld->items[1]))) {
          m->warn_duplicate_decl++;
        }
        walk_expr(fld->items[3], m);
      }
    } else if (is_ctor(d, "DeclStmt") && d->item_count >= 1) {
      walk_stmt(d->items[0], m);
    }
  }
}

static SwiftMetrics analyze_swift_ast(const ParsleyValue* root) {
  SwiftMetrics m = {0};
  if (is_ctor(root, "SwFile") && root->item_count >= 1) {
    walk_decl_list(root->items[0], &m);
  }
  return m;
}

int main(void) {
  /* 1. Built-in grammar example self-tests */
  assert(parsley_run_examples() == 0);

  /* 2. Realistic Swift 5 program with structs, classes, enums, guards, loops, and lint triggers */
  const char* swift_program =
      "import Foundation\n"
      "/* Multi-line\n"
      "   Swift analyzer sample */\n"
      "enum Mode {\n"
      "  case fast, safe\n"
      "  case custom(Int)\n"
      "}\n"
      "struct Vector {\n"
      "  let x: Double\n"
      "  var y: Double = 0\n"
      "  func lengthSq() -> Double {\n"
      "    return x * x + y * y\n"
      "  }\n"
      "}\n"
      "class Engine : BaseEngine {\n"
      "  var cycles: Int = 0\n"
      "  func step(limit: Int) -> Int {\n"
      "    guard limit > 0 else {\n"
      "      return 0\n"
      "    }\n"
      "    let factor = 2\n"
      "    var acc = 1\n"
      "    for i in [1, 2, 3] {\n"
      "      if i > 1 && acc < limit {\n"
      "        acc = acc + factor\n"
      "      } else {\n"
      "        acc = acc + 1\n"
      "      }\n"
      "    }\n"
      "    while acc < 10 {\n"
      "      acc = acc + 1\n"
      "    }\n"
      "    return acc\n"
      "  }\n"
      "}\n"
      "func risky(v: Double) -> Double {\n"
      "  let denom = 0\n"
      "  return v / 0 + denom\n"
      "}\n"
      "func risky(v: Double) -> Double {\n"
      "  return v\n"
      "}\n";

  ParsleyResult res = parsley_parse(swift_program);
  if (!res.ok) {
    fprintf(stderr, "Unexpected Swift parse error at (%d, %d): %s\n",
            res.error.line, res.error.col,
            res.error.message ? res.error.message : "(null)");
    parsley_result_free(&res);
    return 1;
  }
  assert(res.root != NULL);
  assert(res.value != NULL);

  char ast_buf[256];
  int formatted_len = parsley_format_ast(&res, ast_buf, sizeof(ast_buf));
  assert(formatted_len > 0);

  SwiftMetrics m = analyze_swift_ast(res.value);
  const size_t total_bindings = m.let_count + m.var_count;
  const double immutability_ratio =
      total_bindings > 0 ? ((double)m.let_count / (double)total_bindings) : 1.0;
  const size_t cyclomatic_complexity = 1 + m.branch_points;

  printf("Swift Static Analysis Report:\n");
  printf("  imports: %zu\n", m.imports);
  printf("  structs: %zu, classes: %zu, enums: %zu, funcs: %zu\n",
         m.structs, m.classes, m.enums, m.funcs);
  printf("  let: %zu, var: %zu (immutability ratio: %.2f)\n",
         m.let_count, m.var_count, immutability_ratio);
  printf("  guards: %zu, cyclomatic complexity: %zu\n",
         m.guard_count, cyclomatic_complexity);
  printf("  warnings: mutable_var=%zu, div_zero=%zu, duplicate_decl=%zu\n",
         m.warn_mutable_var, m.warn_div_zero, m.warn_duplicate_decl);

  assert(m.imports == 1);
  assert(m.structs == 1);
  assert(m.classes == 1);
  assert(m.enums == 1);
  assert(m.funcs == 4); /* lengthSq, step, risky, risky */
  assert(m.let_count == 3); /* Vector.x, factor, denom */
  assert(m.var_count == 3); /* Vector.y, Engine.cycles, acc */
  assert(immutability_ratio >= 0.49 && immutability_ratio <= 0.51);
  assert(m.guard_count == 1);
  assert(cyclomatic_complexity == 6); /* 1 + guard(1) + for(1) + if(1) + &&(1) + while(1) */
  assert(m.warn_mutable_var == 3);
  assert(m.warn_div_zero == 1);
  assert(m.warn_duplicate_decl == 1); /* duplicate `risky` */

  parsley_result_free(&res);

  /* 3. Invalid Swift syntax check */
  const char* bad_swift = "func broken(x: Int {\n  return x\n}\n";
  ParsleyResult bad_res = parsley_parse(bad_swift);
  assert(!bad_res.ok);
  assert(bad_res.error.line == 1);
  assert(bad_res.error.message != NULL);
  parsley_result_free(&bad_res);

  printf("Swift C17 static analyzer: ALL CHECKS PASSED!\n");
  return 0;
}
