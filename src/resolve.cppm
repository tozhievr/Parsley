module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.resolve;

import parsley.diagnostic;
import parsley.ast;
import parsley.stdlib;

export namespace parsley::resolve {

enum class SymbolKind {
  Token,
  Skip,
  Rule,
  Type,
  Ctor,
  Field,
  BuiltinCombinator,
};

struct Symbol {
  std::string name;
  std::string qualified_name;
  SymbolKind kind = SymbolKind::Rule;
  Span span;
  std::string source_module;
  std::optional<ast::TypeExpr> type_ann;
  std::vector<std::string> params;
  std::string parent_type;
  bool is_builtin = false;
  bool is_exported = false;
};

struct ResolvedImport {
  std::string module_name;
  std::string qualifier;
  bool is_qualified = false;
  bool is_hiding = false;
  std::vector<std::string> imported_rules;
  std::vector<std::string> imported_types;
  bool from_stdlib = false;
  Span span;
};

struct SymbolRef {
  std::string name;
  Span span;
  std::string target_name;
  SymbolKind target_kind = SymbolKind::Rule;
  Span target_span;
};

struct ResolvedModule {
  std::string module_name;
  std::vector<ResolvedImport> imports;

  // Separate namespaces matching Haskell/Parsley lexical rules
  std::unordered_map<std::string, Symbol> values;
  std::unordered_map<std::string, Symbol> types;
  std::unordered_map<std::string, Symbol> ctors;

  std::unordered_map<std::string, std::vector<std::string>> type_ctors;
  std::unordered_map<std::string, std::vector<std::string>> ctor_fields;

  std::unordered_map<std::string, ast::DataDecl> data_decls;
  std::unordered_map<std::string, ast::TokenDecl> token_decls;
  std::unordered_map<std::string, ast::SkipDecl> skip_decls;
  std::unordered_map<std::string, ast::RuleDef> rule_defs;
  std::unordered_map<std::string, ast::RuleSig> rule_sigs;

  std::vector<std::string> exported_names;
  std::vector<SymbolRef> references;

  std::optional<Symbol> lookup_value(std::string_view name) const {
    auto it = values.find(std::string(name));
    if (it != values.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::optional<Symbol> lookup_type(std::string_view name) const {
    auto it = types.find(std::string(name));
    if (it != types.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::optional<Symbol> lookup_ctor(std::string_view name) const {
    auto it = ctors.find(std::string(name));
    if (it != ctors.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  bool is_exported(std::string_view name) const {
    for (const auto& exp : exported_names) {
      if (exp == name) {
        return true;
      }
    }
    return false;
  }
};

} // namespace parsley::resolve

namespace parsley::resolve::detail {

inline std::vector<std::string> extract_imports_from_source(std::string_view source) {
  std::vector<std::string> deps;
  std::istringstream iss{std::string(source)};
  std::string line;
  while (std::getline(iss, line)) {
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (line.compare(i, 7, "import ") != 0) {
      continue;
    }
    i += 7;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (line.compare(i, 10, "qualified ") == 0) {
      i += 10;
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
      }
    }
    const std::size_t start = i;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == '_')) {
      ++i;
    }
    if (i > start) {
      deps.push_back(line.substr(start, i - start));
    }
  }
  return deps;
}

inline bool detect_circular_import(const std::string& start_mod,
                                   const std::string& target_mod,
                                   std::vector<std::string>& cycle_path) {
  if (target_mod == start_mod) {
    cycle_path = {start_mod, target_mod};
    return true;
  }
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack_path = {start_mod, target_mod};

  auto dfs = [&](auto& self, const std::string& current) -> bool {
    if (!visited.insert(current).second) {
      return false;
    }
    const auto builtin = stdlib::find_builtin_module(current);
    if (!builtin) {
      return false;
    }
    for (const auto& dep : extract_imports_from_source(builtin->source)) {
      stack_path.push_back(dep);
      if (dep == start_mod) {
        cycle_path = stack_path;
        return true;
      }
      if (self(self, dep)) {
        return true;
      }
      stack_path.pop_back();
    }
    return false;
  };

  return dfs(dfs, target_mod);
}

inline std::vector<std::string> stdlib_type_ctors(std::string_view type_name) {
  if (type_name == "JsonValue") {
    return {"JObject", "JArray", "JString", "JNumber", "JBool", "JNull"};
  }
  if (type_name == "SqlStmt") {
    return {"Select", "Insert", "Update", "Delete"};
  }
  if (type_name == "SqlExpr") {
    return {"Col", "LitNum", "LitStr"};
  }
  if (type_name == "Maybe") {
    return {"Nothing", "Just"};
  }
  if (type_name == "Bool") {
    return {"True", "False"};
  }
  return {};
}

inline void register_builtins(ResolvedModule& out) {
  const std::vector<std::string> builtin_types = {
      "Text", "Double", "Int", "Bool", "Unit", "Maybe", "Rule",
  };
  for (const auto& t : builtin_types) {
    Symbol sym;
    sym.name = t;
    sym.qualified_name = t;
    sym.kind = SymbolKind::Type;
    sym.source_module = "<builtin>";
    sym.is_builtin = true;
    out.types[t] = sym;
  }

  auto add_ctor = [&](std::string name, std::string parent) {
    Symbol sym;
    sym.name = name;
    sym.qualified_name = name;
    sym.kind = SymbolKind::Ctor;
    sym.source_module = "<builtin>";
    sym.parent_type = parent;
    sym.is_builtin = true;
    out.ctors[name] = sym;
    out.type_ctors[parent].push_back(name);
  };
  add_ctor("True", "Bool");
  add_ctor("False", "Bool");
  add_ctor("Nothing", "Maybe");
  add_ctor("Just", "Maybe");

  for (const std::string& core_fn : {"pure", "exactly"}) {
    Symbol sym;
    sym.name = core_fn;
    sym.qualified_name = core_fn;
    sym.kind = SymbolKind::BuiltinCombinator;
    sym.source_module = "Parsley.Prelude";
    sym.is_builtin = true;
    out.values[core_fn] = sym;
  }
}

inline void check_type_expr(const ast::TypeExpr& te,
                            const ResolvedModule& mod,
                            const std::unordered_set<std::string>& type_params,
                            DiagnosticBag& diags) {
  switch (te.kind) {
    case ast::TypeExpr::Kind::Named: {
      if (te.name.empty() || te.name == "<error>") {
        return;
      }
      if (!mod.types.contains(te.name)) {
        diags.error("E0200",
                    "unknown type `" + te.name + "`",
                    te.span,
                    "not in scope");
      }
      return;
    }
    case ast::TypeExpr::Kind::Var: {
      if (!type_params.empty() && !type_params.contains(te.name)) {
        // In rule signatures, lowercase vars are implicitly universally quantified;
        // in `data` declarations, undeclared type variables are flagged.
        diags.error("E0200",
                    "undeclared type variable `" + te.name + "` in data declaration",
                    te.span,
                    "not listed in data type parameters");
      }
      return;
    }
    case ast::TypeExpr::Kind::List:
      if (te.left) {
        check_type_expr(*te.left, mod, type_params, diags);
      }
      return;
    case ast::TypeExpr::Kind::Tuple:
      for (const auto& arg : te.args) {
        if (arg) {
          check_type_expr(*arg, mod, type_params, diags);
        }
      }
      return;
    case ast::TypeExpr::Kind::App:
      if (te.left) {
        check_type_expr(*te.left, mod, type_params, diags);
      }
      for (const auto& arg : te.args) {
        if (arg) {
          check_type_expr(*arg, mod, type_params, diags);
        }
      }
      return;
    case ast::TypeExpr::Kind::Arrow:
      if (te.left) {
        check_type_expr(*te.left, mod, type_params, diags);
      }
      if (te.right) {
        check_type_expr(*te.right, mod, type_params, diags);
      }
      return;
  }
}

inline void resolve_rule_expr(const ast::RuleExprPtr& expr,
                              ResolvedModule& out,
                              std::unordered_set<std::string>& local_vars,
                              DiagnosticBag& diags) {
  if (!expr) {
    return;
  }
  switch (expr->kind) {
    case ast::RuleExpr::Kind::Lit:
    case ast::RuleExpr::Kind::IntLit:
    case ast::RuleExpr::Kind::FloatLit:
      return;

    case ast::RuleExpr::Kind::Var: {
      if (expr->text.empty() || expr->text == "<error>" || expr->text == "_") {
        return;
      }
      if (expr->is_upper) {
        auto ctor = out.lookup_ctor(expr->text);
        if (!ctor) {
          diags.error("E0200",
                      "undefined constructor `" + expr->text + "`",
                      expr->span,
                      "constructor not in scope");
        } else {
          out.references.push_back(SymbolRef{
              .name = expr->text,
              .span = expr->span,
              .target_name = ctor->qualified_name,
              .target_kind = ctor->kind,
              .target_span = ctor->span,
          });
        }
      } else {
        if (local_vars.contains(expr->text)) {
          return;
        }
        auto val = out.lookup_value(expr->text);
        if (!val) {
          diags.error("E0200",
                      "undefined identifier `" + expr->text + "`",
                      expr->span,
                      "not in scope");
        } else {
          out.references.push_back(SymbolRef{
              .name = expr->text,
              .span = expr->span,
              .target_name = val->qualified_name,
              .target_kind = val->kind,
              .target_span = val->span,
          });
        }
      }
      return;
    }

    case ast::RuleExpr::Kind::App:
    case ast::RuleExpr::Kind::Pure:
      resolve_rule_expr(expr->lhs, out, local_vars, diags);
      resolve_rule_expr(expr->rhs, out, local_vars, diags);
      return;

    case ast::RuleExpr::Kind::Paren:
      resolve_rule_expr(expr->lhs, out, local_vars, diags);
      return;

    case ast::RuleExpr::Kind::InfixOp: {
      resolve_rule_expr(expr->lhs, out, local_vars, diags);
      if (expr->op_kind == ast::InfixOpKind::Backtick) {
        if (!expr->text.empty() && !local_vars.contains(expr->text)) {
          auto val = out.lookup_value(expr->text);
          if (!val) {
            diags.error("E0200",
                        "undefined infix operator `" + expr->text + "`",
                        expr->span,
                        "not in scope");
          } else {
            out.references.push_back(SymbolRef{
                .name = expr->text,
                .span = expr->span,
                .target_name = val->qualified_name,
                .target_kind = val->kind,
                .target_span = val->span,
            });
          }
        }
      }
      if (expr->op_kind != ast::InfixOpKind::Label) {
        resolve_rule_expr(expr->rhs, out, local_vars, diags);
      }
      return;
    }

    case ast::RuleExpr::Kind::Alt:
    case ast::RuleExpr::Kind::List:
    case ast::RuleExpr::Kind::Tuple:
      for (const auto& item : expr->items) {
        resolve_rule_expr(item, out, local_vars, diags);
      }
      return;

    case ast::RuleExpr::Kind::Do: {
      std::unordered_set<std::string> do_locals = local_vars;
      for (const auto& stmt : expr->do_stmts) {
        resolve_rule_expr(stmt.expr, out, do_locals, diags);
        if (stmt.kind == ast::DoStmt::Kind::Bind && !stmt.binder.empty()) {
          do_locals.insert(stmt.binder);
        }
      }
      return;
    }

    case ast::RuleExpr::Kind::Operators: {
      resolve_rule_expr(expr->lhs, out, local_vars, diags);
      for (const auto& lvl : expr->op_levels) {
        for (const auto& spec : lvl.specs) {
          for (const auto& pat_elem : spec.pattern) {
            resolve_rule_expr(pat_elem, out, local_vars, diags);
          }
          resolve_rule_expr(spec.action, out, local_vars, diags);
        }
      }
      return;
    }
  }
}

inline void resolve_ast_pattern(const ast::AstPattern& pat,
                                ResolvedModule& out,
                                DiagnosticBag& diags) {
  switch (pat.kind) {
    case ast::AstPattern::Kind::Ctor: {
      if (!pat.name.empty() && pat.name != "<error>") {
        auto ctor = out.lookup_ctor(pat.name);
        if (!ctor) {
          diags.error("E0200",
                      "undefined constructor `" + pat.name + "` in example pattern",
                      pat.span,
                      "constructor not in scope");
        } else {
          out.references.push_back(SymbolRef{
              .name = pat.name,
              .span = pat.span,
              .target_name = ctor->qualified_name,
              .target_kind = ctor->kind,
              .target_span = ctor->span,
          });
        }
      }
      for (const auto& arg : pat.args) {
        resolve_ast_pattern(arg, out, diags);
      }
      return;
    }
    case ast::AstPattern::Kind::List:
      for (const auto& arg : pat.args) {
        resolve_ast_pattern(arg, out, diags);
      }
      return;
    case ast::AstPattern::Kind::Number:
    case ast::AstPattern::Kind::String:
    case ast::AstPattern::Kind::Wildcard:
    case ast::AstPattern::Kind::Ident:
      return;
  }
}

} // namespace parsley::resolve::detail

export namespace parsley::resolve {

inline ResolvedModule resolve_module(const ast::Module& mod, DiagnosticBag& diags) {
  ResolvedModule out;
  out.module_name = mod.name;

  detail::register_builtins(out);

  // 1. Resolve imports and check circular imports
  for (const auto& imp : mod.imports) {
    if (imp.module_name == "<error>") {
      continue;
    }

    std::vector<std::string> cycle;
    if (detail::detect_circular_import(mod.name, imp.module_name, cycle)) {
      std::string chain;
      for (std::size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) {
          chain += " -> ";
        }
        chain += cycle[i];
      }
      diags.error("E0200",
                  "circular import detected: " + chain,
                  imp.span,
                  "circular module dependency");
      continue;
    }

    const auto std_mod = stdlib::find_builtin_module(imp.module_name);
    if (!std_mod) {
      diags.error("E0200",
                  "unknown module `" + imp.module_name + "`",
                  imp.span,
                  "module not found in stdlib");
      continue;
    }

    ResolvedImport r_imp;
    r_imp.module_name = imp.module_name;
    r_imp.qualifier = imp.alias.value_or(imp.module_name);
    r_imp.is_qualified = imp.is_qualified;
    r_imp.is_hiding = imp.is_hiding;
    r_imp.from_stdlib = true;
    r_imp.span = imp.span;

    const auto& avail_rules = std_mod->exported_rules;
    const auto& avail_types = std_mod->exported_types;

    auto is_rule_avail = [&](std::string_view n) {
      return std::find(avail_rules.begin(), avail_rules.end(), n) != avail_rules.end();
    };
    auto is_type_avail = [&](std::string_view n) {
      return std::find(avail_types.begin(), avail_types.end(), n) != avail_types.end();
    };

    std::unordered_set<std::string> selected_rules;
    std::unordered_set<std::string> selected_types;
    std::unordered_set<std::string> selected_ctors;

    if (imp.items.empty()) {
      selected_rules.insert(avail_rules.begin(), avail_rules.end());
      selected_types.insert(avail_types.begin(), avail_types.end());
      for (const auto& t : avail_types) {
        for (const auto& c : detail::stdlib_type_ctors(t)) {
          selected_ctors.insert(c);
        }
      }
    } else if (imp.is_hiding) {
      selected_rules.insert(avail_rules.begin(), avail_rules.end());
      selected_types.insert(avail_types.begin(), avail_types.end());
      for (const auto& t : avail_types) {
        for (const auto& c : detail::stdlib_type_ctors(t)) {
          selected_ctors.insert(c);
        }
      }
      for (const auto& item : imp.items) {
        if (item.name == "<error>") {
          continue;
        }
        if (is_rule_avail(item.name)) {
          selected_rules.erase(item.name);
        } else if (is_type_avail(item.name)) {
          selected_types.erase(item.name);
          for (const auto& c : detail::stdlib_type_ctors(item.name)) {
            selected_ctors.erase(c);
          }
        } else {
          diags.error("E0200",
                      "module `" + imp.module_name + "` does not export `" + item.name + "`",
                      item.span,
                      "unknown symbol in hiding list");
        }
      }
    } else {
      for (const auto& item : imp.items) {
        if (item.name == "<error>") {
          continue;
        }
        if (is_rule_avail(item.name)) {
          selected_rules.insert(item.name);
        } else if (is_type_avail(item.name)) {
          selected_types.insert(item.name);
          const auto tctors = detail::stdlib_type_ctors(item.name);
          if (item.all_ctors) {
            selected_ctors.insert(tctors.begin(), tctors.end());
          } else if (!item.subitems.empty()) {
            for (const auto& sub : item.subitems) {
              if (std::find(tctors.begin(), tctors.end(), sub) != tctors.end()) {
                selected_ctors.insert(sub);
              } else {
                diags.error("E0200",
                            "type `" + item.name + "` has no constructor `" + sub + "`",
                            item.span,
                            "unknown constructor in import list");
              }
            }
          }
        } else {
          diags.error("E0200",
                      "module `" + imp.module_name + "` does not export `" + item.name + "`",
                      item.span,
                      "not exported by `" + imp.module_name + "`");
        }
      }
    }

    for (const auto& r : selected_rules) {
      r_imp.imported_rules.push_back(r);
      Symbol sym;
      sym.name = r;
      sym.qualified_name = imp.module_name + "." + r;
      sym.kind = SymbolKind::BuiltinCombinator;
      sym.source_module = imp.module_name;
      sym.span = imp.span;
      sym.is_builtin = true;

      out.values[r_imp.qualifier + "." + r] = sym;
      if (!imp.is_qualified) {
        out.values[r] = sym;
      }
    }

    for (const auto& t : selected_types) {
      r_imp.imported_types.push_back(t);
      Symbol sym;
      sym.name = t;
      sym.qualified_name = imp.module_name + "." + t;
      sym.kind = SymbolKind::Type;
      sym.source_module = imp.module_name;
      sym.span = imp.span;
      sym.is_builtin = true;

      out.types[r_imp.qualifier + "." + t] = sym;
      if (!imp.is_qualified) {
        out.types[t] = sym;
      }
    }

    for (const auto& c : selected_ctors) {
      Symbol sym;
      sym.name = c;
      sym.qualified_name = imp.module_name + "." + c;
      sym.kind = SymbolKind::Ctor;
      sym.source_module = imp.module_name;
      sym.span = imp.span;
      sym.is_builtin = true;

      out.ctors[r_imp.qualifier + "." + c] = sym;
      if (!imp.is_qualified) {
        out.ctors[c] = sym;
      }
    }

    out.imports.push_back(std::move(r_imp));
  }

  // 2. Register modes & verify mode targets
  std::unordered_set<std::string> declared_modes;
  for (const auto& mode : mod.modes) {
    if (!declared_modes.insert(mode.name).second) {
      diags.error("E0200",
                  "duplicate lexer mode `" + mode.name + "`",
                  mode.span,
                  "already declared");
    }
  }

  auto register_token = [&](const ast::TokenDecl& tok, const std::string& mode_name) {
    if (tok.name.empty() || tok.name == "<error>") {
      return;
    }
    if (tok.action.kind == ast::LexActionKind::Push &&
        !declared_modes.contains(tok.action.target_mode)) {
      diags.error("E0200",
                  "unknown lexer mode `" + tok.action.target_mode + "` in `push` action",
                  tok.action.span,
                  "mode is not declared");
    }
    if (out.token_decls.contains(tok.name)) {
      diags.error("E0200",
                  "duplicate token declaration `" + tok.name + "`",
                  tok.span,
                  "token already defined");
      return;
    }
    out.token_decls[tok.name] = tok;

    Symbol sym;
    sym.name = tok.name;
    sym.qualified_name = mod.name + "." + tok.name;
    sym.kind = SymbolKind::Token;
    sym.span = tok.span;
    sym.source_module = mode_name.empty() ? mod.name : (mod.name + "." + mode_name);
    sym.type_ann = tok.type_ann;
    out.values[tok.name] = sym;
  };

  for (const auto& tok : mod.tokens) {
    register_token(tok, "");
  }
  for (const auto& sk : mod.skips) {
    if (!sk.name.empty() && sk.name != "<error>") {
      if (out.skip_decls.contains(sk.name)) {
        diags.error("E0200",
                    "duplicate skip declaration `" + sk.name + "`",
                    sk.span,
                    "skip already defined");
      } else {
        out.skip_decls[sk.name] = sk;
      }
    }
  }
  for (const auto& mode : mod.modes) {
    for (const auto& tok : mode.tokens) {
      register_token(tok, mode.name);
    }
  }

  // 3. Register data/newtype/foreign declarations & constructors
  for (const auto& dd : mod.data_decls) {
    if (dd.name.empty() || dd.name == "<error>") {
      continue;
    }
    if (out.data_decls.contains(dd.name)) {
      diags.error("E0200",
                  "duplicate data type `" + dd.name + "`",
                  dd.span,
                  "type already defined");
      continue;
    }
    out.data_decls[dd.name] = dd;

    Symbol tsym;
    tsym.name = dd.name;
    tsym.qualified_name = mod.name + "." + dd.name;
    tsym.kind = SymbolKind::Type;
    tsym.span = dd.span;
    tsym.source_module = mod.name;
    tsym.params = dd.type_params;
    out.types[dd.name] = tsym;

    for (const auto& ctor : dd.constructors) {
      if (ctor.name.empty() || ctor.name == "<error>") {
        continue;
      }
      if (out.ctors.contains(ctor.name) && !out.ctors[ctor.name].is_builtin) {
        diags.error("E0200",
                    "duplicate constructor `" + ctor.name + "`",
                    ctor.span,
                    "constructor already defined");
        continue;
      }
      Symbol csym;
      csym.name = ctor.name;
      csym.qualified_name = mod.name + "." + ctor.name;
      csym.kind = SymbolKind::Ctor;
      csym.span = ctor.span;
      csym.source_module = mod.name;
      csym.parent_type = dd.name;
      out.ctors[ctor.name] = csym;
      out.type_ctors[dd.name].push_back(ctor.name);

      if (ctor.is_record) {
        for (const auto& fld : ctor.record_fields) {
          out.ctor_fields[ctor.name].push_back(fld.name);
          Symbol fsym;
          fsym.name = fld.name;
          fsym.qualified_name = mod.name + "." + fld.name;
          fsym.kind = SymbolKind::Field;
          fsym.span = fld.span;
          fsym.source_module = mod.name;
          fsym.type_ann = fld.type;
          fsym.parent_type = ctor.name;
          out.values[fld.name] = fsym;
        }
      }
    }
  }

  // Validate types inside data declarations & token annotations
  for (const auto& dd : mod.data_decls) {
    std::unordered_set<std::string> tparams(dd.type_params.begin(), dd.type_params.end());
    // Ensure non-empty set sentinel so Var check knows we're inside a data decl
    if (tparams.empty()) {
      tparams.insert("<none>");
    }
    for (const auto& ctor : dd.constructors) {
      for (const auto& pf : ctor.positional_fields) {
        detail::check_type_expr(pf, out, tparams, diags);
      }
      for (const auto& rf : ctor.record_fields) {
        detail::check_type_expr(rf.type, out, tparams, diags);
      }
    }
  }
  const std::unordered_set<std::string> empty_tparams;
  for (const auto& tok : mod.tokens) {
    if (tok.type_ann) {
      detail::check_type_expr(*tok.type_ann, out, empty_tparams, diags);
    }
  }

  // 4. Register rule signatures & rule definitions
  for (const auto& sig : mod.rule_sigs) {
    if (sig.name.empty() || sig.name == "<error>") {
      continue;
    }
    if (out.rule_sigs.contains(sig.name)) {
      diags.error("E0200",
                  "duplicate type signature for rule `" + sig.name + "`",
                  sig.span,
                  "signature already declared");
    } else {
      out.rule_sigs[sig.name] = sig;
      detail::check_type_expr(sig.type, out, empty_tparams, diags);
    }
  }

  for (const auto& def : mod.rule_defs) {
    if (def.name.empty() || def.name == "<error>") {
      continue;
    }
    if (out.rule_defs.contains(def.name)) {
      diags.error("E0200",
                  "duplicate definition of rule `" + def.name + "`",
                  def.span,
                  "rule already defined");
      continue;
    }
    if (out.token_decls.contains(def.name)) {
      diags.error("E0200",
                  "rule `" + def.name + "` conflicts with token of the same name",
                  def.span,
                  "already defined as token");
      continue;
    }
    out.rule_defs[def.name] = def;

    Symbol rsym;
    rsym.name = def.name;
    rsym.qualified_name = mod.name + "." + def.name;
    rsym.kind = SymbolKind::Rule;
    rsym.span = def.span;
    rsym.source_module = mod.name;
    rsym.params = def.params;
    if (def.signature) {
      rsym.type_ann = def.signature;
    } else if (auto it = out.rule_sigs.find(def.name); it != out.rule_sigs.end()) {
      rsym.type_ann = it->second.type;
    }
    out.values[def.name] = rsym;
  }

  // Check orphan type signatures
  for (const auto& sig : mod.rule_sigs) {
    if (!out.rule_defs.contains(sig.name)) {
      diags.error("E0200",
                  "type signature for `" + sig.name + "` has no accompanying rule definition",
                  sig.span,
                  "missing rule body");
    }
  }

  // 5. Validate export list
  if (mod.has_export_list) {
    for (const auto& exp : mod.exports) {
      if (exp.name == "<error>") {
        continue;
      }
      const bool in_values = out.values.contains(exp.name);
      const bool in_types = out.types.contains(exp.name);
      if (!in_values && !in_types) {
        diags.error("E0200",
                    "exported symbol `" + exp.name + "` is not defined in module `" + mod.name + "`",
                    exp.span,
                    "undefined export");
      } else {
        out.exported_names.push_back(exp.name);
        if (auto it = out.values.find(exp.name); it != out.values.end()) {
          it->second.is_exported = true;
        }
        if (auto it = out.types.find(exp.name); it != out.types.end()) {
          it->second.is_exported = true;
        }
      }
    }
  } else {
    for (const auto& def : mod.rule_defs) {
      out.exported_names.push_back(def.name);
      if (auto it = out.values.find(def.name); it != out.values.end()) {
        it->second.is_exported = true;
      }
    }
  }

  // 6. Resolve names inside rule bodies
  for (const auto& def : mod.rule_defs) {
    std::unordered_set<std::string> local_vars(def.params.begin(), def.params.end());
    detail::resolve_rule_expr(def.body, out, local_vars, diags);
  }

  // 7. Resolve constructor names in example patterns
  for (const auto& ex : mod.examples) {
    if (ex.expected_ast) {
      detail::resolve_ast_pattern(*ex.expected_ast, out, diags);
    }
  }

  return out;
}

} // namespace parsley::resolve
