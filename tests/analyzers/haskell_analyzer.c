#include "parser.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAMES 128

typedef struct NameList {
  const char* items[MAX_NAMES];
  size_t count;
} NameList;

typedef struct HsMetrics {
  const char* module_name;
  size_t import_count;
  size_t data_count;
  size_t ctor_count;
  size_t type_syn_count;
  size_t type_sig_count;
  size_t fun_bind_count;
  size_t expr_nodes;
  size_t missing_sig_count;
  size_t recursive_fn_count;
  size_t div_zero_count;
  NameList sig_names;
  NameList fun_names;
  NameList missing_sig_names;
  NameList recursive_fn_names;
} HsMetrics;

static void name_list_add_unique(NameList* list, const char* name) {
  if (!list || !name) return;
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->items[i], name) == 0) return;
  }
  if (list->count < MAX_NAMES) {
    list->items[list->count++] = name;
  }
}

static bool name_list_contains(const NameList* list, const char* name) {
  if (!list || !name) return false;
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->items[i], name) == 0) return true;
  }
  return false;
}

static bool is_ctor(const ParsleyValue* v, const char* ctor_name) {
  return v != NULL && v->kind == PARSLEY_VAL_CTOR && v->ctor_name != NULL &&
         strcmp(v->ctor_name, ctor_name) == 0;
}

static bool is_zero_literal(const ParsleyValue* expr) {
  if (!expr) return false;
  if (is_ctor(expr, "HsNum") && expr->item_count == 1) {
    const ParsleyValue* num = expr->items[0];
    return num != NULL && num->kind == PARSLEY_VAL_NUMBER && num->number_val == 0.0;
  }
  if (is_ctor(expr, "HsNeg") && expr->item_count == 1) {
    return is_zero_literal(expr->items[0]);
  }
  return false;
}

static bool expr_has_self_call(const ParsleyValue* expr, const char* fn_name, HsMetrics* m) {
  if (!expr || expr->kind != PARSLEY_VAL_CTOR || !expr->ctor_name) return false;
  m->expr_nodes++;
  bool found = false;

  if (strcmp(expr->ctor_name, "HsVar") == 0 && expr->item_count == 1) {
    const ParsleyValue* id = expr->items[0];
    if (id && id->kind == PARSLEY_VAL_STRING && id->string_val &&
        strcmp(id->string_val, fn_name) == 0) {
      found = true;
    }
    return found;
  }

  if (strcmp(expr->ctor_name, "HsBin") == 0 && expr->item_count == 3) {
    const ParsleyValue* op = expr->items[0];
    const ParsleyValue* lhs = expr->items[1];
    const ParsleyValue* rhs = expr->items[2];
    if (is_ctor(op, "Div") && is_zero_literal(rhs)) {
      m->div_zero_count++;
    }
    if (expr_has_self_call(lhs, fn_name, m)) found = true;
    if (expr_has_self_call(rhs, fn_name, m)) found = true;
    return found;
  }

  if (strcmp(expr->ctor_name, "HsApp") == 0 && expr->item_count == 2) {
    if (expr_has_self_call(expr->items[0], fn_name, m)) found = true;
    const ParsleyValue* args = expr->items[1];
    if (args && args->kind == PARSLEY_VAL_LIST) {
      for (size_t i = 0; i < args->item_count; ++i) {
        if (expr_has_self_call(args->items[i], fn_name, m)) found = true;
      }
    }
    return found;
  }

  if (strcmp(expr->ctor_name, "HsNeg") == 0 && expr->item_count == 1) {
    return expr_has_self_call(expr->items[0], fn_name, m);
  }

  if (strcmp(expr->ctor_name, "HsIf") == 0 && expr->item_count == 3) {
    for (size_t i = 0; i < 3; ++i) {
      if (expr_has_self_call(expr->items[i], fn_name, m)) found = true;
    }
    return found;
  }

  if (strcmp(expr->ctor_name, "HsLet") == 0 && expr->item_count == 2) {
    const ParsleyValue* binds = expr->items[0];
    if (binds && binds->kind == PARSLEY_VAL_LIST) {
      for (size_t i = 0; i < binds->item_count; ++i) {
        const ParsleyValue* b = binds->items[i];
        if (is_ctor(b, "HsBind") && b->item_count == 2) {
          if (expr_has_self_call(b->items[1], fn_name, m)) found = true;
        }
      }
    }
    if (expr_has_self_call(expr->items[1], fn_name, m)) found = true;
    return found;
  }

  if (strcmp(expr->ctor_name, "HsCase") == 0 && expr->item_count == 2) {
    if (expr_has_self_call(expr->items[0], fn_name, m)) found = true;
    const ParsleyValue* alts = expr->items[1];
    if (alts && alts->kind == PARSLEY_VAL_LIST) {
      for (size_t i = 0; i < alts->item_count; ++i) {
        const ParsleyValue* alt = alts->items[i];
        if (is_ctor(alt, "HsAlt") && alt->item_count == 3) {
          if (expr_has_self_call(alt->items[2], fn_name, m)) found = true;
        }
      }
    }
    return found;
  }

  if (strcmp(expr->ctor_name, "HsList") == 0 && expr->item_count == 1) {
    const ParsleyValue* elems = expr->items[0];
    if (elems && elems->kind == PARSLEY_VAL_LIST) {
      for (size_t i = 0; i < elems->item_count; ++i) {
        if (expr_has_self_call(elems->items[i], fn_name, m)) found = true;
      }
    }
    return found;
  }

  return false;
}

static void analyze_module(const ParsleyValue* root, HsMetrics* m) {
  memset(m, 0, sizeof(*m));
  assert(is_ctor(root, "HsModule") && root->item_count == 3);

  const ParsleyValue* mod_name = root->items[0];
  assert(mod_name != NULL && mod_name->kind == PARSLEY_VAL_STRING);
  m->module_name = mod_name->string_val;

  const ParsleyValue* imports = root->items[1];
  assert(imports != NULL && imports->kind == PARSLEY_VAL_LIST);
  m->import_count = imports->item_count;

  const ParsleyValue* decls = root->items[2];
  assert(decls != NULL && decls->kind == PARSLEY_VAL_LIST);

  for (size_t i = 0; i < decls->item_count; ++i) {
    const ParsleyValue* d = decls->items[i];
    if (is_ctor(d, "HsData") && d->item_count == 3) {
      m->data_count++;
      const ParsleyValue* ctors = d->items[2];
      if (ctors && ctors->kind == PARSLEY_VAL_LIST) {
        m->ctor_count += ctors->item_count;
      }
    } else if (is_ctor(d, "HsTypeSyn") && d->item_count == 3) {
      m->type_syn_count++;
    } else if (is_ctor(d, "HsTypeSig") && d->item_count == 2) {
      m->type_sig_count++;
      if (d->items[0] && d->items[0]->kind == PARSLEY_VAL_STRING) {
        name_list_add_unique(&m->sig_names, d->items[0]->string_val);
      }
    } else if (is_ctor(d, "HsFunBind") && d->item_count == 3) {
      m->fun_bind_count++;
      const char* fn_name = (d->items[0] && d->items[0]->kind == PARSLEY_VAL_STRING)
                                ? d->items[0]->string_val
                                : "<anon>";
      name_list_add_unique(&m->fun_names, fn_name);
      if (expr_has_self_call(d->items[2], fn_name, m)) {
        name_list_add_unique(&m->recursive_fn_names, fn_name);
      }
    }
  }

  for (size_t i = 0; i < m->fun_names.count; ++i) {
    if (!name_list_contains(&m->sig_names, m->fun_names.items[i])) {
      name_list_add_unique(&m->missing_sig_names, m->fun_names.items[i]);
    }
  }
  m->missing_sig_count = m->missing_sig_names.count;
  m->recursive_fn_count = m->recursive_fn_names.count;
}

static void print_report(const HsMetrics* m) {
  printf("=== Haskell Static Analysis Report (%s) ===\n",
         m->module_name ? m->module_name : "<unknown>");
  printf("Imports         : %zu\n", m->import_count);
  printf("Data ADTs       : %zu (Constructors: %zu)\n", m->data_count, m->ctor_count);
  printf("Type Synonyms   : %zu\n", m->type_syn_count);
  printf("Type Signatures : %zu\n", m->type_sig_count);
  printf("Function Binds  : %zu\n", m->fun_bind_count);
  printf("Expr AST Nodes  : %zu\n", m->expr_nodes);
  printf("Recursive Fns   : %zu", m->recursive_fn_count);
  for (size_t i = 0; i < m->recursive_fn_names.count; ++i) {
    printf("%s%s", (i == 0 ? " [" : ", "), m->recursive_fn_names.items[i]);
  }
  if (m->recursive_fn_names.count > 0) printf("]");
  printf("\n");
  printf("Missing Sigs    : %zu", m->missing_sig_count);
  for (size_t i = 0; i < m->missing_sig_names.count; ++i) {
    printf("%s%s", (i == 0 ? " [" : ", "), m->missing_sig_names.items[i]);
  }
  if (m->missing_sig_names.count > 0) printf("]");
  printf("\n");
  printf("Div-by-Zero     : %zu\n", m->div_zero_count);
}

int main(void) {
  const char* sample_program =
      "module Analytics.Core (Expr, eval) where\n"
      "import qualified Data.Map as M;\n"
      "import Data.List (foldl, map);\n"
      "type Name = String;\n"
      "data Expr = Lit Int | Var Name | Add Expr Expr | Mul Expr Expr;\n"
      "data Result = Ok Int | Err String;\n"
      "eval :: Expr -> Int;\n"
      "eval e = case e of {\n"
      "  Lit n -> n;\n"
      "  Var x -> 0;\n"
      "  Add l r -> eval l + eval r;\n"
      "  Mul l r -> eval l * eval r\n"
      "};\n"
      "fact :: Int -> Int;\n"
      "fact n = if n <= 1 then 1 else n * fact (n - 1);\n"
      "helper x = let y = x * 2; z = y + 1 in [y, z];\n"
      "badRatio x = x / 0";

  ParsleyResult ok_res = parsley_parse(sample_program);
  if (!ok_res.ok) {
    fprintf(stderr, "Unexpected parse error at %d:%d: %s\n",
            ok_res.error.line, ok_res.error.col,
            ok_res.error.message ? ok_res.error.message : "(null)");
    parsley_result_free(&ok_res);
    return 1;
  }

  HsMetrics metrics;
  analyze_module(ok_res.value, &metrics);
  print_report(&metrics);

  assert(strcmp(metrics.module_name, "Analytics.Core") == 0);
  assert(metrics.import_count == 2);
  assert(metrics.data_count == 2);
  assert(metrics.ctor_count == 6);
  assert(metrics.type_syn_count == 1);
  assert(metrics.type_sig_count == 2);
  assert(metrics.fun_bind_count == 4);
  assert(metrics.expr_nodes >= 20);
  assert(metrics.recursive_fn_count == 2);
  assert(name_list_contains(&metrics.recursive_fn_names, "eval"));
  assert(name_list_contains(&metrics.recursive_fn_names, "fact"));
  assert(metrics.missing_sig_count == 2);
  assert(name_list_contains(&metrics.missing_sig_names, "helper"));
  assert(name_list_contains(&metrics.missing_sig_names, "badRatio"));
  assert(metrics.div_zero_count == 1);

  parsley_result_free(&ok_res);

  const char* invalid_program = "module Broken where bad x = if then else";
  ParsleyResult err_res = parsley_parse(invalid_program);
  assert(!err_res.ok);
  assert(err_res.error.line >= 1 && err_res.error.col >= 1);
  printf("Invalid syntax rejected as expected at %d:%d (expected: %s)\n",
         err_res.error.line, err_res.error.col,
         err_res.error.expected ? err_res.error.expected : "<unknown>");
  parsley_result_free(&err_res);

  assert(parsley_run_examples() == 0);
  printf("Haskell E2E C17 analyzer checks PASSED.\n");
  return 0;
}
