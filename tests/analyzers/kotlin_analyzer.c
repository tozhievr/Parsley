/* ISO C17 static analyzer for Kotlin using Parsley's generated parser.h */
#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct KtMetrics {
  size_t package_segments;
  size_t imports;
  size_t classes;
  size_t interfaces;
  size_t functions;
  size_t val_count;
  size_t var_count;
  size_t when_branches;
  size_t div_zero_warnings;
  size_t duplicate_decls;
} KtMetrics;

static void analyze_stmt_list(const KtStmtList* list, KtMetrics* m);
static void analyze_decl_list(const KtDeclList* list, KtMetrics* m);

static int is_zero_literal(const KtExpr* expr) {
  if (!expr) return 0;
  if (expr->tag == KTEXPR_INTLIT) return expr->as.intlit.f0 == 0.0;
  if (expr->tag == KTEXPR_FLOATLIT) return expr->as.floatlit.f0 == 0.0;
  return 0;
}

static void analyze_expr(const KtExpr* expr, KtMetrics* m) {
  if (!expr) return;
  switch (expr->tag) {
    case KTEXPR_INTLIT:
    case KTEXPR_FLOATLIT:
    case KTEXPR_STRLIT:
    case KTEXPR_BOOLLIT:
    case KTEXPR_NULLLIT:
    case KTEXPR_VAR:
      break;
    case KTEXPR_BIN:
      if ((expr->as.bin.f0.tag == KTOP_DIV || expr->as.bin.f0.tag == KTOP_MOD) &&
          is_zero_literal(expr->as.bin.f2)) {
        m->div_zero_warnings++;
      }
      analyze_expr(expr->as.bin.f1, m);
      analyze_expr(expr->as.bin.f2, m);
      break;
    case KTEXPR_UNARY:
      analyze_expr(expr->as.unary.f1, m);
      break;
    case KTEXPR_CALL:
      for (size_t i = 0; i < expr->as.call.f1.len; ++i) {
        analyze_expr(expr->as.call.f1.items[i], m);
      }
      break;
    case KTEXPR_MEMBER:
      analyze_expr(expr->as.member.f0, m);
      break;
    case KTEXPR_METHODCALL:
      analyze_expr(expr->as.methodcall.f0, m);
      for (size_t i = 0; i < expr->as.methodcall.f2.len; ++i) {
        analyze_expr(expr->as.methodcall.f2.items[i], m);
      }
      break;
    case KTEXPR_INDEX:
      analyze_expr(expr->as.index.f0, m);
      analyze_expr(expr->as.index.f1, m);
      break;
    case KTEXPR_IFEXPR:
      analyze_expr(expr->as.ifexpr.f0, m);
      analyze_stmt_list(&expr->as.ifexpr.f1, m);
      analyze_stmt_list(&expr->as.ifexpr.f2, m);
      break;
    case KTEXPR_WHENEXPR:
      for (size_t i = 0; i < expr->as.whenexpr.f0.len; ++i) {
        analyze_expr(expr->as.whenexpr.f0.items[i], m);
      }
      for (size_t i = 0; i < expr->as.whenexpr.f1.len; ++i) {
        const KtWhenBranch* br = expr->as.whenexpr.f1.items[i];
        if (!br) continue;
        m->when_branches++;
        if (br->tag == KTWHENBRANCH_WHENBRANCH) {
          for (size_t j = 0; j < br->as.whenbranch.f0.len; ++j) {
            analyze_expr(br->as.whenbranch.f0.items[j], m);
          }
          analyze_stmt_list(&br->as.whenbranch.f1, m);
        } else if (br->tag == KTWHENBRANCH_WHENELSE) {
          analyze_stmt_list(&br->as.whenelse.f0, m);
        }
      }
      break;
  }
}

static size_t count_duplicates_in_names(const char* const* names, size_t count) {
  size_t dups = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!names[i] || names[i][0] == '\0') continue;
    int seen_before = 0;
    for (size_t j = 0; j < i; ++j) {
      if (names[j] && strcmp(names[i], names[j]) == 0) {
        seen_before = 1;
        break;
      }
    }
    if (seen_before) dups++;
  }
  return dups;
}

static void analyze_stmt(const KtStmt* st, KtMetrics* m) {
  if (!st) return;
  switch (st->tag) {
    case KTSTMT_VALSTMT:
      if (st->as.valstmt.f0) {
        m->var_count++;
      } else {
        m->val_count++;
      }
      for (size_t i = 0; i < st->as.valstmt.f3.len; ++i) {
        analyze_expr(st->as.valstmt.f3.items[i], m);
      }
      break;
    case KTSTMT_ASSIGNSTMT:
      analyze_expr(st->as.assignstmt.f2, m);
      break;
    case KTSTMT_IFSTMT:
      analyze_expr(st->as.ifstmt.f0, m);
      analyze_stmt_list(&st->as.ifstmt.f1, m);
      analyze_stmt_list(&st->as.ifstmt.f2, m);
      break;
    case KTSTMT_WHENSTMT:
      for (size_t i = 0; i < st->as.whenstmt.f0.len; ++i) {
        analyze_expr(st->as.whenstmt.f0.items[i], m);
      }
      for (size_t i = 0; i < st->as.whenstmt.f1.len; ++i) {
        const KtWhenBranch* br = st->as.whenstmt.f1.items[i];
        if (!br) continue;
        m->when_branches++;
        if (br->tag == KTWHENBRANCH_WHENBRANCH) {
          for (size_t j = 0; j < br->as.whenbranch.f0.len; ++j) {
            analyze_expr(br->as.whenbranch.f0.items[j], m);
          }
          analyze_stmt_list(&br->as.whenbranch.f1, m);
        } else if (br->tag == KTWHENBRANCH_WHENELSE) {
          analyze_stmt_list(&br->as.whenelse.f0, m);
        }
      }
      break;
    case KTSTMT_WHILESTMT:
      analyze_expr(st->as.whilestmt.f0, m);
      analyze_stmt_list(&st->as.whilestmt.f1, m);
      break;
    case KTSTMT_FORSTMT:
      analyze_expr(st->as.forstmt.f1, m);
      analyze_stmt_list(&st->as.forstmt.f2, m);
      break;
    case KTSTMT_RETURNSTMT:
      for (size_t i = 0; i < st->as.returnstmt.f0.len; ++i) {
        analyze_expr(st->as.returnstmt.f0.items[i], m);
      }
      break;
    case KTSTMT_EXPRSTMT:
      analyze_expr(st->as.exprstmt.f0, m);
      break;
  }
}

static void analyze_stmt_list(const KtStmtList* list, KtMetrics* m) {
  if (!list || list->len == 0) return;
  const char** local_names = calloc(list->len, sizeof(char*));
  assert(local_names != NULL);
  size_t name_count = 0;

  for (size_t i = 0; i < list->len; ++i) {
    const KtStmt* st = list->items[i];
    if (!st) continue;
    if (st->tag == KTSTMT_VALSTMT && st->as.valstmt.f1) {
      local_names[name_count++] = st->as.valstmt.f1;
    }
    analyze_stmt(st, m);
  }
  m->duplicate_decls += count_duplicates_in_names(local_names, name_count);
  free((void*)local_names);
}

static void analyze_decl(const KtDecl* decl, KtMetrics* m) {
  if (!decl) return;
  switch (decl->tag) {
    case KTDECL_IMPORTDECL:
      m->imports++;
      break;
    case KTDECL_CLASSDECL:
      m->classes++;
      for (size_t i = 0; i < decl->as.classdecl.f2.len; ++i) {
        const KtParam* p = decl->as.classdecl.f2.items[i];
        if (!p || !p->as.param.f0) continue;
        if (strcmp(p->as.param.f0, "val") == 0) m->val_count++;
        else if (strcmp(p->as.param.f0, "var") == 0) m->var_count++;
      }
      analyze_decl_list(&decl->as.classdecl.f3, m);
      break;
    case KTDECL_INTERFACEDECL:
      m->interfaces++;
      analyze_decl_list(&decl->as.interfacedecl.f1, m);
      break;
    case KTDECL_FUNDECL:
      m->functions++;
      analyze_stmt_list(&decl->as.fundecl.f3, m);
      break;
    case KTDECL_PROPDECL:
      if (decl->as.propdecl.f0) m->var_count++;
      else m->val_count++;
      for (size_t i = 0; i < decl->as.propdecl.f3.len; ++i) {
        analyze_expr(decl->as.propdecl.f3.items[i], m);
      }
      break;
  }
}

static const char* decl_name(const KtDecl* decl) {
  if (!decl) return NULL;
  switch (decl->tag) {
    case KTDECL_IMPORTDECL:
      return NULL;
    case KTDECL_CLASSDECL:
      return decl->as.classdecl.f1;
    case KTDECL_INTERFACEDECL:
      return decl->as.interfacedecl.f0;
    case KTDECL_FUNDECL:
      return decl->as.fundecl.f0;
    case KTDECL_PROPDECL:
      return decl->as.propdecl.f1;
  }
  return NULL;
}

static void analyze_decl_list(const KtDeclList* list, KtMetrics* m) {
  if (!list || list->len == 0) return;
  const char** names = calloc(list->len, sizeof(char*));
  assert(names != NULL);
  size_t name_count = 0;

  for (size_t i = 0; i < list->len; ++i) {
    const KtDecl* d = list->items[i];
    const char* nm = decl_name(d);
    if (nm) names[name_count++] = nm;
    analyze_decl(d, m);
  }
  m->duplicate_decls += count_duplicates_in_names(names, name_count);
  free((void*)names);
}

static void analyze_file(const KtFile* file, KtMetrics* m) {
  assert(file != NULL);
  assert(file->tag == KTFILE_KTFILE);
  m->package_segments = file->as.ktfile.f0.len;
  analyze_decl_list(&file->as.ktfile.f1, m);
}

int main(void) {
  int ex_failures = parsley_run_examples();
  assert(ex_failures == 0);

  const char* valid_kotlin =
      "package com.parsley.demo\n"
      "import kotlin.math.*\n"
      "import kotlin.collections.List\n"
      "\n"
      "interface Greeter {\n"
      "  fun greet(name: String): String\n"
      "}\n"
      "\n"
      "data class Counter(var count: Int, val limit: Int) {\n"
      "  val tag: String = \"counter\"\n"
      "  fun step(delta: Int): Int {\n"
      "    var acc = count + delta\n"
      "    if (acc > limit) {\n"
      "      acc = limit\n"
      "    } else {\n"
      "      while (acc < 0) {\n"
      "        acc += 1\n"
      "      }\n"
      "    }\n"
      "    for (i in 1..3) {\n"
      "      acc += i\n"
      "    }\n"
      "    when (acc) {\n"
      "      0 -> return 0\n"
      "      1, 2 -> return 10\n"
      "      else -> return acc\n"
      "    }\n"
      "  }\n"
      "}\n"
      "\n"
      "fun risky(x: Int): Int {\n"
      "  val dup = 1\n"
      "  val dup = 2\n"
      "  return x / 0 + dup\n"
      "}\n"
      "\n"
      "fun helper(n: Int): Int = n * 2\n"
      "fun helper(n: Int): Int = n + 1\n";

  ParsleyResult ok_res = parsley_parse(valid_kotlin);
  if (!ok_res.ok) {
    fprintf(stderr, "Unexpected parse failure at %d:%d: %s\n",
            ok_res.error.line, ok_res.error.col,
            ok_res.error.message ? ok_res.error.message : "(null)");
    parsley_result_free(&ok_res);
    return 1;
  }
  assert(ok_res.root != NULL);
  assert(ok_res.value != NULL);
  assert(ok_res.value->kind == PARSLEY_VAL_CTOR);

  KtMetrics m = {0};
  analyze_file(ok_res.root, &m);

  printf("Kotlin Static Analysis Report\n");
  printf("  package_segments : %zu\n", m.package_segments);
  printf("  imports          : %zu\n", m.imports);
  printf("  interfaces       : %zu\n", m.interfaces);
  printf("  classes          : %zu\n", m.classes);
  printf("  functions        : %zu\n", m.functions);
  printf("  val_count        : %zu\n", m.val_count);
  printf("  var_count        : %zu\n", m.var_count);
  printf("  when_branches    : %zu\n", m.when_branches);
  printf("  div_zero_warnings: %zu\n", m.div_zero_warnings);
  printf("  duplicate_decls  : %zu\n", m.duplicate_decls);

  assert(m.package_segments == 3);
  assert(m.imports == 2);
  assert(m.interfaces == 1);
  assert(m.classes == 1);
  assert(m.functions == 5);
  assert(m.val_count == 4);
  assert(m.var_count == 2);
  assert(m.when_branches == 3);
  assert(m.div_zero_warnings == 1);
  assert(m.duplicate_decls == 2);

  parsley_result_free(&ok_res);

  const char* invalid_kotlin = "fun broken(x: Int): Int = 1 +";
  ParsleyResult err_res = parsley_parse(invalid_kotlin);
  assert(!err_res.ok);
  assert(err_res.error.line == 1);
  assert(err_res.error.col > 0);
  assert(err_res.error.message != NULL);
  printf("  invalid_parse    : failed as expected at (%d, %d): %s\n",
         err_res.error.line, err_res.error.col, err_res.error.message);
  parsley_result_free(&err_res);

  printf("ALL KOTLIN ANALYZER CHECKS PASSED\n");
  return 0;
}
