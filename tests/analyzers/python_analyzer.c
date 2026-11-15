/* ISO C17 Static Analyzer for Python 3 powered by Parsley */
#include "parser.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PyMetrics {
  int functions;
  int classes;
  int imports;
  int assignments;
  int cyclomatic_complexity;
  int call_count;
  char call_targets[64][128];

  int div_by_zero_count;
  int empty_pass_funcs;
  int duplicate_funcs;

  char func_names[64][64];
  int func_name_count;
} PyMetrics;

static bool is_ctor(const ParsleyValue* v, const char* name) {
  return v != NULL && v->kind == PARSLEY_VAL_CTOR && v->ctor_name != NULL &&
         strcmp(v->ctor_name, name) == 0;
}

static const char* as_string(const ParsleyValue* v) {
  if (v != NULL && v->kind == PARSLEY_VAL_STRING && v->string_val != NULL) {
    return v->string_val;
  }
  return "";
}

static void record_call_target(PyMetrics* m, const char* name) {
  if (name == NULL || name[0] == '\0' || m->call_count >= 64) {
    return;
  }
  snprintf(m->call_targets[m->call_count], sizeof(m->call_targets[m->call_count]), "%s", name);
  m->call_count++;
}

static void format_callee(const ParsleyValue* expr, char* buf, size_t buf_sz) {
  if (buf_sz == 0) {
    return;
  }
  buf[0] = '\0';
  if (is_ctor(expr, "PyVar") && expr->item_count >= 1) {
    snprintf(buf, buf_sz, "%s", as_string(expr->items[0]));
  } else if (is_ctor(expr, "PyAttr") && expr->item_count >= 2) {
    char base[96];
    format_callee(expr->items[0], base, sizeof(base));
    if (base[0] != '\0') {
      snprintf(buf, buf_sz, "%s.%s", base, as_string(expr->items[1]));
    } else {
      snprintf(buf, buf_sz, "%s", as_string(expr->items[1]));
    }
  } else {
    snprintf(buf, buf_sz, "<expr>");
  }
}

static void walk_expr(const ParsleyValue* expr, PyMetrics* m);

static void walk_expr_list(const ParsleyValue* list, PyMetrics* m) {
  if (list == NULL || list->kind != PARSLEY_VAL_LIST) {
    return;
  }
  for (size_t i = 0; i < list->item_count; ++i) {
    walk_expr(list->items[i], m);
  }
}

static void walk_expr(const ParsleyValue* expr, PyMetrics* m) {
  if (expr == NULL || expr->kind != PARSLEY_VAL_CTOR) {
    return;
  }
  if (is_ctor(expr, "PyBin") && expr->item_count >= 3) {
    const ParsleyValue* op = expr->items[0];
    const ParsleyValue* lhs = expr->items[1];
    const ParsleyValue* rhs = expr->items[2];
    if ((is_ctor(op, "PyDiv") || is_ctor(op, "PyFloorDiv") || is_ctor(op, "PyMod")) &&
        is_ctor(rhs, "PyNum") && rhs->item_count >= 1 &&
        rhs->items[0] != NULL && rhs->items[0]->kind == PARSLEY_VAL_NUMBER &&
        fabs(rhs->items[0]->number_val) < 1e-12) {
      m->div_by_zero_count++;
    }
    walk_expr(lhs, m);
    walk_expr(rhs, m);
  } else if (is_ctor(expr, "PyUnary") && expr->item_count >= 2) {
    walk_expr(expr->items[1], m);
  } else if (is_ctor(expr, "PyCall") && expr->item_count >= 2) {
    char target[128];
    format_callee(expr->items[0], target, sizeof(target));
    record_call_target(m, target);
    walk_expr(expr->items[0], m);
    walk_expr_list(expr->items[1], m);
  } else if (is_ctor(expr, "PyIndex") && expr->item_count >= 2) {
    walk_expr(expr->items[0], m);
    walk_expr(expr->items[1], m);
  } else if (is_ctor(expr, "PyAttr") && expr->item_count >= 2) {
    walk_expr(expr->items[0], m);
  } else if (is_ctor(expr, "PyList") && expr->item_count >= 1) {
    walk_expr_list(expr->items[0], m);
  }
}

static void walk_stmt(const ParsleyValue* stmt, PyMetrics* m);

static void walk_stmt_list(const ParsleyValue* list, PyMetrics* m) {
  if (list == NULL || list->kind != PARSLEY_VAL_LIST) {
    return;
  }
  for (size_t i = 0; i < list->item_count; ++i) {
    walk_stmt(list->items[i], m);
  }
}

static void walk_stmt(const ParsleyValue* stmt, PyMetrics* m) {
  if (stmt == NULL || stmt->kind != PARSLEY_VAL_CTOR) {
    return;
  }
  if (is_ctor(stmt, "PyImport") || is_ctor(stmt, "PyFromImport")) {
    m->imports++;
  } else if (is_ctor(stmt, "PyDef") && stmt->item_count >= 4) {
    const char* name = as_string(stmt->items[0]);
    const ParsleyValue* body = stmt->items[3];
    m->functions++;

    bool seen = false;
    for (int i = 0; i < m->func_name_count; ++i) {
      if (strcmp(m->func_names[i], name) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) {
      m->duplicate_funcs++;
    } else if (m->func_name_count < 64) {
      snprintf(m->func_names[m->func_name_count], sizeof(m->func_names[0]), "%s", name);
      m->func_name_count++;
    }

    if (body != NULL && body->kind == PARSLEY_VAL_LIST && body->item_count == 1 &&
        is_ctor(body->items[0], "PyPass")) {
      m->empty_pass_funcs++;
    }
    walk_stmt_list(body, m);
  } else if (is_ctor(stmt, "PyClass") && stmt->item_count >= 3) {
    m->classes++;
    walk_stmt_list(stmt->items[2], m);
  } else if (is_ctor(stmt, "PyIf") && stmt->item_count >= 3) {
    m->cyclomatic_complexity++;
    walk_expr(stmt->items[0], m);
    walk_stmt_list(stmt->items[1], m);
    walk_stmt_list(stmt->items[2], m);
  } else if (is_ctor(stmt, "PyWhile") && stmt->item_count >= 2) {
    m->cyclomatic_complexity++;
    walk_expr(stmt->items[0], m);
    walk_stmt_list(stmt->items[1], m);
  } else if (is_ctor(stmt, "PyFor") && stmt->item_count >= 3) {
    m->cyclomatic_complexity++;
    walk_expr(stmt->items[1], m);
    walk_stmt_list(stmt->items[2], m);
  } else if (is_ctor(stmt, "PyReturn") && stmt->item_count >= 1) {
    walk_expr(stmt->items[0], m);
  } else if ((is_ctor(stmt, "PyAssign") || is_ctor(stmt, "PyAugAssign")) &&
             stmt->item_count >= 3) {
    m->assignments++;
    walk_expr(stmt->items[0], m);
    walk_expr(stmt->items[2], m);
  } else if (is_ctor(stmt, "PyExprStmt") && stmt->item_count >= 1) {
    walk_expr(stmt->items[0], m);
  }
}

static PyMetrics analyze_python(const char* source) {
  PyMetrics m;
  memset(&m, 0, sizeof(m));
  m.cyclomatic_complexity = 1;

  ParsleyResult res = parsley_parse(source);
  if (!res.ok) {
    fprintf(stderr,
            "Unexpected parse failure at %d:%d: %s\n",
            res.error.line,
            res.error.col,
            res.error.message ? res.error.message : "parse error");
    parsley_result_free(&res);
    exit(1);
  }
  assert(is_ctor(res.value, "PyModule"));
  assert(res.value->item_count >= 1);
  walk_stmt_list(res.value->items[0], &m);
  parsley_result_free(&res);
  return m;
}

int main(void) {
  assert(parsley_run_examples() == 0);

  const char* prog_analysis =
      "import math\n"
      "from typing import List, Optional\n"
      "\n"
      "# Empty stub function\n"
      "def empty_stub():\n"
      "    pass\n"
      "\n"
      "def compute(x: int, y: int) -> int:\n"
      "    total: int = x + y * 2\n"
      "    if x > 0:\n"
      "        return total\n"
      "    elif x == 0:\n"
      "        return 0\n"
      "    else:\n"
      "        return y\n"
      "\n"
      "def loop_sum(n: int) -> int:\n"
      "    acc = 0\n"
      "    for i in range(n):\n"
      "        if i == 3:\n"
      "            continue\n"
      "        elif i == 7:\n"
      "            break\n"
      "        else:\n"
      "            acc += i\n"
      "    return acc\n"
      "\n"
      "def risky(a: int) -> int:\n"
      "    while a > 0:\n"
      "        a = a - 1\n"
      "    return 10 / 0\n"
      "\n"
      "def compute(z: int) -> int:\n"
      "    return abs(z) + 1\n";

  PyMetrics m1 = analyze_python(prog_analysis);
  printf("Python Program 1 Report:\n");
  printf("  imports=%d functions=%d classes=%d assignments=%d cyclomatic=%d calls=%d\n",
         m1.imports,
         m1.functions,
         m1.classes,
         m1.assignments,
         m1.cyclomatic_complexity,
         m1.call_count);
  printf("  lint: div_by_zero=%d empty_pass=%d duplicate_funcs=%d\n",
         m1.div_by_zero_count,
         m1.empty_pass_funcs,
         m1.duplicate_funcs);
  for (int i = 0; i < m1.call_count; ++i) {
    printf("  call[%d]: %s\n", i, m1.call_targets[i]);
  }

  assert(m1.imports == 2);
  assert(m1.functions == 5);
  assert(m1.classes == 0);
  assert(m1.assignments == 4);
  /* 1 base + if + elif + for + if + elif + while = 7 */
  assert(m1.cyclomatic_complexity == 7);
  assert(m1.call_count == 2);
  assert(strcmp(m1.call_targets[0], "range") == 0);
  assert(strcmp(m1.call_targets[1], "abs") == 0);
  assert(m1.div_by_zero_count == 1);
  assert(m1.empty_pass_funcs == 1);
  assert(m1.duplicate_funcs == 1);

  const char* prog_oop =
      "class Calculator(Base):\n"
      "    def __init__(self, initial: int):\n"
      "        self.value = initial\n"
      "\n"
      "    def scale(self, items: List[int], factor: int) -> int:\n"
      "        if not factor == 0 and factor > 0:\n"
      "            return self.value + items[0] * factor\n"
      "        else:\n"
      "            return -self.value\n";

  PyMetrics m2 = analyze_python(prog_oop);
  printf("Python Program 2 (OOP) Report:\n");
  printf("  classes=%d functions=%d assignments=%d cyclomatic=%d lint_issues=%d\n",
         m2.classes,
         m2.functions,
         m2.assignments,
         m2.cyclomatic_complexity,
         m2.div_by_zero_count + m2.empty_pass_funcs + m2.duplicate_funcs);

  assert(m2.classes == 1);
  assert(m2.functions == 2);
  assert(m2.assignments == 1);
  assert(m2.cyclomatic_complexity == 2);
  assert(m2.div_by_zero_count == 0);
  assert(m2.empty_pass_funcs == 0);
  assert(m2.duplicate_funcs == 0);

  const char* prog_invalid =
      "def broken(x)\n"
      "    return x\n";
  ParsleyResult bad = parsley_parse(prog_invalid);
  assert(!bad.ok);
  assert(bad.error.line == 2);
  printf("Invalid Python caught at %d:%d: %s\n",
         bad.error.line,
         bad.error.col,
         bad.error.message ? bad.error.message : "");
  parsley_result_free(&bad);

  printf("All Python C17 static analyzer checks passed!\n");
  return 0;
}
