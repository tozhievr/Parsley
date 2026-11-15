module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.desugar;

import parsley.diagnostic;
import parsley.ast;
import parsley.resolve;
import parsley.typecheck;
import parsley.ir;

namespace parsley::desugar::detail {

inline std::string sanitize_ident(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(c));
    } else if (c == '_') {
      out.push_back('_');
    } else {
      switch (c) {
        case '+':
          out += "plus";
          break;
        case '-':
          out += "minus";
          break;
        case '*':
          out += "star";
          break;
        case '/':
          out += "slash";
          break;
        case '^':
          out += "caret";
          break;
        case '=':
          out += "eq";
          break;
        case '<':
          out += "lt";
          break;
        case '>':
          out += "gt";
          break;
        case '(':
          out += "lparen";
          break;
        case ')':
          out += "rparen";
          break;
        case '[':
          out += "lbrack";
          break;
        case ']':
          out += "rbrack";
          break;
        case '{':
          out += "lbrace";
          break;
        case '}':
          out += "rbrace";
          break;
        case ',':
          out += "comma";
          break;
        case ';':
          out += "semi";
          break;
        case ':':
          out += "colon";
          break;
        case '.':
          out += "dot";
          break;
        case '"':
          out += "quote";
          break;
        default:
          out += "x" + std::to_string(static_cast<int>(c));
          break;
      }
    }
  }
  if (out.empty()) {
    out = "empty";
  }
  return out;
}

class Desugarer {
public:
  Desugarer(const ast::Module& mod,
            const resolve::ResolvedModule& res,
            const typecheck::TypedModule& typed,
            DiagnosticBag& diags)
      : mod_(mod), res_(res), typed_(typed), diags_(diags) {}

  ir::Grammar run() {
    ir::Grammar g;
    g.module_name = mod_.name;

    process_pragmas(g);
    process_types(g);
    process_tokens_and_modes(g);
    process_examples(g);
    index_rules();
    desugar_non_param_rules(g);
    apply_token_precedence(g);
    sort_tokens(g);
    determine_starts(g);

    return g;
  }

private:
  const ast::Module& mod_;
  const resolve::ResolvedModule& res_;
  const typecheck::TypedModule& typed_;
  DiagnosticBag& diags_;

  int next_decl_order_ = 0;
  std::unordered_map<std::string, ir::TokenId> lit_to_token_;
  std::unordered_set<std::string> known_token_ids_;
  std::vector<ir::TokenDef> extra_tokens_;

  std::unordered_map<std::string, ast::RuleDef> all_rule_defs_;
  std::unordered_map<std::string, std::string> mono_cache_;

  struct MonoFrame {
    std::string rule_name;
    std::string key;
    std::string mangled;
  };
  std::vector<MonoFrame> active_mono_;
  std::map<ir::RuleId, ir::Rule> generated_rules_;

  using ParamEnv = std::unordered_map<std::string, ast::RuleExprPtr>;

  void process_pragmas(ir::Grammar& g) {
    for (const auto& p : mod_.pragmas) {
      if (p.name == "START" && !p.args.empty()) {
        g.pragmas.start_rule = p.args[0];
      } else if (p.name == "MODE" && !p.args.empty()) {
        const std::string& m = p.args[0];
        if (m == "LL1" || m == "LL(1)") {
          g.pragmas.mode = ir::ParseMode::LL1;
        } else if (m == "LLk" || m == "LL(k)") {
          g.pragmas.mode = ir::ParseMode::LLk;
        } else if (m == "LALR" || m == "LR") {
          g.pragmas.mode = ir::ParseMode::LALR;
        } else if (m == "PEG" || m == "Packrat") {
          g.pragmas.mode = ir::ParseMode::PEG;
        } else if (m == "GLL" || m == "GLR") {
          g.pragmas.mode = ir::ParseMode::GLL;
        }
      } else if (p.name == "NOFACTOR") {
        g.pragmas.no_factor = true;
      } else if (p.name == "PREC" && p.args.size() >= 3) {
        if (p.args[1] == "<") {
          g.pragmas.token_precedence.emplace_back(p.args[0], p.args[2]);
        } else if (p.args[1] == ">") {
          g.pragmas.token_precedence.emplace_back(p.args[2], p.args[0]);
        }
      }
    }
  }

  void process_types(ir::Grammar& g) {
    for (const auto& dd : mod_.data_decls) {
      ir::DataDecl idd;
      idd.name = dd.name;
      idd.type_params = dd.type_params;
      idd.is_newtype = dd.is_newtype;
      idd.is_foreign = dd.is_foreign;
      idd.foreign_target = dd.foreign_target;
      idd.span = dd.span;
      for (const auto& c : dd.constructors) {
        ir::CtorDecl ic;
        ic.name = c.name;
        ic.is_record = c.is_record;
        ic.span = c.span;
        for (const auto& pf : c.positional_fields) {
          ic.positional_fields.push_back(ir::from_ast_type(pf));
        }
        for (const auto& rf : c.record_fields) {
          ic.record_fields.push_back(ir::FieldDecl{
              .name = rf.name,
              .type = ir::from_ast_type(rf.type),
              .span = rf.span,
          });
        }
        idd.constructors.push_back(std::move(ic));
      }
      g.gTypes.push_back(std::move(idd));
    }
  }

  ir::TokenDef convert_token_decl(const ast::TokenDecl& td, const std::string& mode_name) {
    ir::TokenDef t;
    t.id = td.name;
    t.pattern = td.pattern;
    t.is_regex = td.is_regex;
    t.is_skip = false;
    t.is_keyword = false;
    t.is_anonymous = false;
    t.priority = td.is_regex ? 10 : 100;
    t.decl_order = next_decl_order_++;
    t.mode = mode_name;
    if (td.type_ann) {
      t.type = ir::from_ast_type(*td.type_ann);
    } else {
      t.type = ir::Type{ir::Type::Kind::Named, "Text", {}};
    }
    switch (td.action.kind) {
      case ast::LexActionKind::None:
        t.action = ir::LexActionKind::None;
        break;
      case ast::LexActionKind::Push:
        t.action = ir::LexActionKind::Push;
        t.target_mode = td.action.target_mode;
        break;
      case ast::LexActionKind::Pop:
        t.action = ir::LexActionKind::Pop;
        break;
    }
    t.span = td.span;
    return t;
  }

  void process_tokens_and_modes(ir::Grammar& g) {
    // Keywords in default mode: higher priority than regex tokens
    for (const auto& kw_decl : mod_.keywords) {
      for (const auto& w : kw_decl.words) {
        if (lit_to_token_.contains(w)) {
          continue;
        }
        std::string id = "kw_" + sanitize_ident(w);
        while (known_token_ids_.contains(id)) {
          id += "_";
        }
        known_token_ids_.insert(id);
        lit_to_token_[w] = id;

        ir::TokenDef td;
        td.id = id;
        td.pattern = w;
        td.is_regex = false;
        td.is_keyword = true;
        td.is_anonymous = false;
        td.priority = 100;
        td.decl_order = next_decl_order_++;
        td.mode = "default";
        td.span = kw_decl.span;
        g.gTokens.push_back(std::move(td));
      }
    }

    // Skip declarations in default mode
    for (const auto& sk : mod_.skips) {
      ir::TokenDef td;
      td.id = sk.name;
      td.pattern = sk.pattern;
      td.is_regex = sk.is_regex;
      td.is_skip = true;
      td.priority = sk.is_regex ? 10 : 100;
      td.decl_order = next_decl_order_++;
      td.mode = "default";
      td.span = sk.span;
      known_token_ids_.insert(td.id);
      g.gTokens.push_back(std::move(td));
    }

    // Explicit tokens in default mode
    for (const auto& td : mod_.tokens) {
      ir::TokenDef it = convert_token_decl(td, "default");
      known_token_ids_.insert(it.id);
      g.gTokens.push_back(std::move(it));
    }

    // Mode declarations
    for (const auto& mode : mod_.modes) {
      ir::LexMode lm;
      lm.name = mode.name;
      lm.span = mode.span;

      for (const auto& kw_decl : mode.keywords) {
        for (const auto& w : kw_decl.words) {
          ir::TokenDef td;
          td.id = "kw_" + mode.name + "_" + sanitize_ident(w);
          td.pattern = w;
          td.is_regex = false;
          td.is_keyword = true;
          td.priority = 100;
          td.decl_order = next_decl_order_++;
          td.mode = mode.name;
          td.span = kw_decl.span;
          lm.tokens.push_back(std::move(td));
        }
      }
      for (const auto& sk : mode.skips) {
        ir::TokenDef td;
        td.id = sk.name;
        td.pattern = sk.pattern;
        td.is_regex = sk.is_regex;
        td.is_skip = true;
        td.priority = sk.is_regex ? 10 : 100;
        td.decl_order = next_decl_order_++;
        td.mode = mode.name;
        td.span = sk.span;
        lm.tokens.push_back(std::move(td));
      }
      for (const auto& td : mode.tokens) {
        ir::TokenDef it = convert_token_decl(td, mode.name);
        known_token_ids_.insert(it.id);
        lm.tokens.push_back(std::move(it));
      }

      g.gModes.push_back(std::move(lm));
    }
  }

  ir::TokenId intern_anon_literal(const std::string& lit, const Span& sp) {
    auto it = lit_to_token_.find(lit);
    if (it != lit_to_token_.end()) {
      return it->second;
    }
    std::string id = "lit_" + sanitize_ident(lit);
    while (known_token_ids_.contains(id)) {
      id += "_";
    }
    known_token_ids_.insert(id);
    lit_to_token_[lit] = id;

    ir::TokenDef td;
    td.id = id;
    td.pattern = lit;
    td.is_regex = false;
    td.is_anonymous = true;
    td.priority = 100;
    td.decl_order = next_decl_order_++;
    td.mode = "default";
    td.span = sp;
    extra_tokens_.push_back(std::move(td));
    return id;
  }

  void process_examples(ir::Grammar& g) {
    for (const auto& ex : mod_.examples) {
      ir::ExampleCase ec;
      ec.name = ex.name;
      ec.input = ex.input;
      ec.expect_success = (ex.kind == ast::ExampleDecl::Kind::ParsesTo);
      ec.expected_ast = ex.expected_ast;
      ec.fail_line = ex.fail_line;
      ec.fail_col = ex.fail_col;
      ec.expecting = ex.expecting;
      ec.span = ex.span;
      g.gExamples.push_back(std::move(ec));
    }
  }

  void index_rules() {
    for (const auto& def : mod_.rule_defs) {
      if (!def.name.empty() && def.name != "<error>") {
        all_rule_defs_[def.name] = def;
      }
    }
  }

  ir::Type lookup_rule_ir_type(const std::string& name) const {
    if (auto res_ty = typed_.lookup_rule_result_type_string(name)) {
      auto it = typed_.rule_result_types.find(name);
      if (it != typed_.rule_result_types.end() && it->second) {
        return ir::from_ast_type(typecheck::to_ast_type(it->second));
      }
    }
    return ir::Type{ir::Type::Kind::Named, "Unit", {}};
  }

  void desugar_non_param_rules(ir::Grammar& g) {
    for (const auto& def : mod_.rule_defs) {
      if (def.name.empty() || def.name == "<error>") {
        continue;
      }
      if (!def.params.empty()) {
        // Parameterized rules are monomorphized on demand.
        continue;
      }
      ir::Rule r;
      r.rType = lookup_rule_ir_type(def.name);
      r.rSpan = def.span;
      r.is_exported = res_.is_exported(def.name);
      ParamEnv empty_env;
      r.rBody = desugar_expr(def.body, empty_env, false);
      g.gRules[def.name] = std::move(r);
    }

    for (auto& td : extra_tokens_) {
      g.gTokens.push_back(std::move(td));
    }
    for (auto& [rid, r] : generated_rules_) {
      g.gRules[rid] = std::move(r);
    }
  }

  void apply_token_precedence(ir::Grammar& g) {
    auto find_prio = [&](const std::string& name) -> int* {
      for (auto& t : g.gTokens) {
        if (t.id == name) {
          return &t.priority;
        }
      }
      return nullptr;
    };
    for (const auto& [lower, higher] : g.pragmas.token_precedence) {
      int* p_low = find_prio(lower);
      int* p_high = find_prio(higher);
      if (p_low && p_high && *p_high <= *p_low) {
        *p_high = *p_low + 1;
      }
    }
  }

  void sort_tokens(ir::Grammar& g) {
    auto cmp = [](const ir::TokenDef& a, const ir::TokenDef& b) {
      if (a.priority != b.priority) {
        return a.priority > b.priority;
      }
      return a.decl_order < b.decl_order;
    };
    std::stable_sort(g.gTokens.begin(), g.gTokens.end(), cmp);
    for (auto& m : g.gModes) {
      std::stable_sort(m.tokens.begin(), m.tokens.end(), cmp);
    }
  }

  void determine_starts(ir::Grammar& g) {
    if (g.pragmas.start_rule && g.gRules.contains(*g.pragmas.start_rule)) {
      g.gStarts.push_back(*g.pragmas.start_rule);
      return;
    }
    if (mod_.has_export_list) {
      for (const auto& exp : mod_.exports) {
        if (g.gRules.contains(exp.name)) {
          g.gStarts.push_back(exp.name);
        }
      }
    }
    if (g.gStarts.empty()) {
      if (g.gRules.contains("program")) {
        g.gStarts.push_back("program");
      } else if (!g.gRules.empty()) {
        g.gStarts.push_back(g.gRules.begin()->first);
      }
    }
  }

  ast::RuleExprPtr substitute_params(const ast::RuleExprPtr& expr, const ParamEnv& env) {
    if (!expr || env.empty()) {
      return expr;
    }
    if (expr->kind == ast::RuleExpr::Kind::Var && !expr->is_upper) {
      auto it = env.find(expr->text);
      if (it != env.end()) {
        return it->second;
      }
      return expr;
    }
    auto copy = std::make_shared<ast::RuleExpr>(*expr);
    copy->lhs = substitute_params(expr->lhs, env);
    copy->rhs = substitute_params(expr->rhs, env);
    for (auto& item : copy->items) {
      item = substitute_params(item, env);
    }
    for (auto& st : copy->do_stmts) {
      if (st.kind == ast::DoStmt::Kind::Bind) {
        // Bound variables shadow outer params in subsequent steps; simple copy here
      }
      st.expr = substitute_params(st.expr, env);
    }
    for (auto& lvl : copy->op_levels) {
      for (auto& spec : lvl.specs) {
        for (auto& pat : spec.pattern) {
          pat = substitute_params(pat, env);
        }
        spec.action = substitute_params(spec.action, env);
      }
    }
    return copy;
  }

  std::string expr_key(const ast::RuleExprPtr& expr, const ParamEnv& env) {
    if (!expr) {
      return "nil";
    }
    switch (expr->kind) {
      case ast::RuleExpr::Kind::Lit:
        return "lit_" + sanitize_ident(expr->text);
      case ast::RuleExpr::Kind::IntLit:
      case ast::RuleExpr::Kind::FloatLit:
        return "num_" + sanitize_ident(expr->text);
      case ast::RuleExpr::Kind::Var: {
        if (!expr->is_upper) {
          auto it = env.find(expr->text);
          if (it != env.end()) {
            return expr_key(it->second, {});
          }
        }
        return sanitize_ident(expr->text);
      }
      case ast::RuleExpr::Kind::Paren:
        return expr_key(expr->lhs, env);
      case ast::RuleExpr::Kind::App:
        return expr_key(expr->lhs, env) + "__" + expr_key(expr->rhs, env);
      case ast::RuleExpr::Kind::InfixOp:
        return expr_key(expr->lhs, env) + "_" + sanitize_ident(expr->text) + "_" +
               expr_key(expr->rhs, env);
      case ast::RuleExpr::Kind::Alt: {
        std::string s = "alt";
        for (const auto& it : expr->items) {
          s += "_" + expr_key(it, env);
        }
        return s;
      }
      default:
        return "expr";
    }
  }

  // Unwrap parentheses
  ast::RuleExprPtr unwrap_paren(ast::RuleExprPtr expr) const {
    while (expr && expr->kind == ast::RuleExpr::Kind::Paren) {
      expr = expr->lhs;
    }
    return expr;
  }

  // Flatten `f <$> a <*> b ...` or `pure f <*> a ...`
  bool try_extract_applicative_chain(const ast::RuleExprPtr& expr,
                                     ast::RuleExprPtr& out_fn,
                                     std::vector<ast::RuleExprPtr>& out_args) {
    auto e = unwrap_paren(expr);
    if (!e || e->kind != ast::RuleExpr::Kind::InfixOp) {
      return false;
    }
    if (e->op_kind == ast::InfixOpKind::Fmap) {
      out_fn = e->lhs;
      out_args.push_back(e->rhs);
      return true;
    }
    if (e->op_kind == ast::InfixOpKind::Ap) {
      if (try_extract_applicative_chain(e->lhs, out_fn, out_args)) {
        out_args.push_back(e->rhs);
        return true;
      }
      auto lhs_u = unwrap_paren(e->lhs);
      if (lhs_u && lhs_u->kind == ast::RuleExpr::Kind::App && lhs_u->lhs &&
          lhs_u->lhs->kind == ast::RuleExpr::Kind::Var && lhs_u->lhs->text == "pure") {
        out_fn = lhs_u->rhs;
        out_args.push_back(e->rhs);
        return true;
      }
    }
    return false;
  }

  // Convert a pure AST expression into an ActionExpr
  ir::ActionExpr build_action_expr(
      const ast::RuleExprPtr& raw_expr,
      const std::unordered_map<std::string, int>& binder_indices,
      const std::vector<ir::ActionExpr>& extra_args = {}) {
    auto expr = unwrap_paren(raw_expr);
    if (!expr) {
      ir::ActionExpr act;
      act.kind = ir::ActionExpr::Kind::UnitLit;
      return act;
    }

    switch (expr->kind) {
      case ast::RuleExpr::Kind::IntLit: {
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::IntLit;
        act.number_value = expr->number_value;
        return act;
      }
      case ast::RuleExpr::Kind::FloatLit: {
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::FloatLit;
        act.number_value = expr->number_value;
        return act;
      }
      case ast::RuleExpr::Kind::Lit: {
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::StringLit;
        act.name = expr->text;
        return act;
      }
      case ast::RuleExpr::Kind::List: {
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::List;
        for (const auto& item : expr->items) {
          act.args.push_back(build_action_expr(item, binder_indices));
        }
        return act;
      }
      case ast::RuleExpr::Kind::Tuple: {
        if (expr->items.empty()) {
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::UnitLit;
          return act;
        }
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::Tuple;
        for (const auto& item : expr->items) {
          act.args.push_back(build_action_expr(item, binder_indices));
        }
        return act;
      }
      case ast::RuleExpr::Kind::Var: {
        if (expr->is_upper) {
          if (expr->text == "True" || expr->text == "False") {
            if (extra_args.empty()) {
              ir::ActionExpr act;
              act.kind = ir::ActionExpr::Kind::BoolLit;
              act.bool_value = (expr->text == "True");
              return act;
            }
          }
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Ctor;
          act.name = expr->text;
          act.args = extra_args;
          return act;
        }
        auto it = binder_indices.find(expr->text);
        if (it != binder_indices.end()) {
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Arg;
          act.name = expr->text;
          act.arg_index = it->second;
          return act;
        }
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::Arg;
        act.name = expr->text;
        act.arg_index = 0;
        return act;
      }
      case ast::RuleExpr::Kind::App: {
        // Flatten `Ctor arg1 arg2 ...`
        std::vector<ast::RuleExprPtr> call_args;
        ast::RuleExprPtr cur = expr;
        while (cur && cur->kind == ast::RuleExpr::Kind::App) {
          call_args.push_back(cur->rhs);
          cur = unwrap_paren(cur->lhs);
        }
        std::reverse(call_args.begin(), call_args.end());
        std::vector<ir::ActionExpr> built_args;
        for (const auto& a : call_args) {
          built_args.push_back(build_action_expr(a, binder_indices));
        }
        for (const auto& ea : extra_args) {
          built_args.push_back(ea);
        }
        if (cur && cur->kind == ast::RuleExpr::Kind::Var) {
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Ctor;
          act.name = cur->text;
          act.args = std::move(built_args);
          return act;
        }
        return build_action_expr(cur, binder_indices, built_args);
      }
      case ast::RuleExpr::Kind::Pure: {
        return build_action_expr(expr->lhs, binder_indices, extra_args);
      }
      default: {
        ir::ActionExpr act;
        act.kind = ir::ActionExpr::Kind::Identity;
        act.arg_index = 0;
        return act;
      }
    }
  }

  // Estimate whether a Prod definitely consumes at least 1 token (not trivially nullable)
  bool is_non_nullable_hint(const ast::RuleExprPtr& raw, const ParamEnv& env) {
    auto e = unwrap_paren(raw);
    if (!e) {
      return false;
    }
    switch (e->kind) {
      case ast::RuleExpr::Kind::Lit:
        return true;
      case ast::RuleExpr::Kind::Var: {
        if (res_.token_decls.contains(e->text)) {
          return true;
        }
        auto it = env.find(e->text);
        if (it != env.end()) {
          return is_non_nullable_hint(it->second, {});
        }
        return false;
      }
      case ast::RuleExpr::Kind::InfixOp:
        if (e->op_kind == ast::InfixOpKind::SeqR || e->op_kind == ast::InfixOpKind::SeqL ||
            e->op_kind == ast::InfixOpKind::Ap || e->op_kind == ast::InfixOpKind::Fmap) {
          if (e->op_kind == ast::InfixOpKind::Fmap) {
            return is_non_nullable_hint(e->rhs, env);
          }
          return is_non_nullable_hint(e->lhs, env) || is_non_nullable_hint(e->rhs, env);
        }
        if (e->op_kind == ast::InfixOpKind::Alt) {
          return is_non_nullable_hint(e->lhs, env) && is_non_nullable_hint(e->rhs, env);
        }
        return false;
      case ast::RuleExpr::Kind::Alt:
        for (const auto& item : e->items) {
          if (!is_non_nullable_hint(item, env)) {
            return false;
          }
        }
        return !e->items.empty();
      case ast::RuleExpr::Kind::Do:
        for (const auto& st : e->do_stmts) {
          if (st.kind != ast::DoStmt::Kind::Pure && is_non_nullable_hint(st.expr, env)) {
            return true;
          }
        }
        return false;
      default:
        return false;
    }
  }

  ir::ProdPtr desugar_expr(const ast::RuleExprPtr& raw, const ParamEnv& env, bool guarded) {
    auto expr = unwrap_paren(raw);
    if (!expr) {
      return ir::make_eps();
    }

    switch (expr->kind) {
      case ast::RuleExpr::Kind::Lit: {
        ir::TokenId tid = intern_anon_literal(expr->text, expr->span);
        return ir::make_tok(std::move(tid), expr->span);
      }

      case ast::RuleExpr::Kind::Var: {
        if (expr->text == "<error>" || expr->text.empty()) {
          return ir::make_eps(expr->span);
        }
        if (!expr->is_upper) {
          if (auto pit = env.find(expr->text); pit != env.end()) {
            return desugar_expr(pit->second, {}, guarded);
          }
        }
        if (expr->text == "eof") {
          return ir::make_tok("eof", expr->span);
        }
        if (res_.token_decls.contains(expr->text)) {
          return ir::make_tok(expr->text, expr->span);
        }
        if (auto vsym = res_.lookup_value(expr->text)) {
          if (vsym->kind == resolve::SymbolKind::Token ||
              vsym->source_module == "Parsley.Unicode" ||
              vsym->source_module == "Parsley.Layout") {
            return ir::make_tok(vsym->name, expr->span);
          }
        }
        if (expr->is_upper) {
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Ctor;
          act.name = expr->text;
          return ir::make_act(std::move(act), {}, expr->span);
        }
        return ir::make_ref(expr->text, expr->span);
      }

      case ast::RuleExpr::Kind::Pure: {
        ir::ActionExpr act = build_action_expr(expr->lhs, {});
        return ir::make_act(std::move(act), {}, expr->span);
      }

      case ast::RuleExpr::Kind::App: {
        // Collect `head arg0 arg1 ...`
        std::vector<ast::RuleExprPtr> args;
        ast::RuleExprPtr cur = expr;
        while (cur && cur->kind == ast::RuleExpr::Kind::App) {
          args.push_back(cur->rhs);
          cur = unwrap_paren(cur->lhs);
        }
        std::reverse(args.begin(), args.end());

        if (cur && cur->kind == ast::RuleExpr::Kind::Var) {
          const std::string& fn = cur->text;

          // `pure <val>`
          if (fn == "pure" && args.size() == 1) {
            ir::ActionExpr act = build_action_expr(args[0], {});
            return ir::make_act(std::move(act), {}, expr->span);
          }

          // `exactly <var>`
          if (fn == "exactly" && args.size() == 1) {
            auto arg_u = unwrap_paren(args[0]);
            std::string var_name = (arg_u && arg_u->kind == ast::RuleExpr::Kind::Var)
                                       ? arg_u->text
                                       : "_ctx";
            return ir::make_ctx_var(std::move(var_name), expr->span);
          }

          // Built-in repetition / lookahead / error recovery combinators
          if (fn == "many" && args.size() == 1) {
            return ir::make_rep(0, std::nullopt, desugar_expr(args[0], env, guarded), expr->span);
          }
          if (fn == "some" && args.size() == 1) {
            return ir::make_rep(1, std::nullopt, desugar_expr(args[0], env, guarded), expr->span);
          }
          if (fn == "optional" && args.size() == 1) {
            return ir::make_rep(0, 1, desugar_expr(args[0], env, guarded), expr->span);
          }
          if (fn == "lookahead" && args.size() == 1) {
            return ir::make_look(ir::Polarity::Positive,
                                 desugar_expr(args[0], env, guarded),
                                 expr->span);
          }
          if (fn == "notFollowedBy" && args.size() == 1) {
            return ir::make_look(ir::Polarity::Negative,
                                 desugar_expr(args[0], env, guarded),
                                 expr->span);
          }
          if (fn == "recover" && args.size() == 2) {
            return ir::make_recover(desugar_expr(args[0], env, guarded),
                                    desugar_expr(args[1], env, true),
                                    expr->span);
          }

          // Built-in parameterized combinators & user parameterized rules -> monomorphize
          if (fn == "sepBy" || fn == "sepEndBy" || fn == "between" || fn == "block" ||
              all_rule_defs_.contains(fn)) {
            return monomorphize_call(fn, args, env, guarded, expr->span);
          }
        }

        // Fallback: pure constructor application or rule ref
        ir::ActionExpr act = build_action_expr(expr, {});
        return ir::make_act(std::move(act), {}, expr->span);
      }

      case ast::RuleExpr::Kind::InfixOp: {
        if (expr->op_kind == ast::InfixOpKind::Fmap ||
            expr->op_kind == ast::InfixOpKind::Ap) {
          ast::RuleExprPtr fn_expr;
          std::vector<ast::RuleExprPtr> parser_args;
          if (try_extract_applicative_chain(expr, fn_expr, parser_args)) {
            std::vector<ir::ActionExpr> extra;
            std::vector<ir::ProdPtr> children;
            bool step_guarded = guarded;
            for (std::size_t i = 0; i < parser_args.size(); ++i) {
              ir::ActionExpr arg_ref;
              arg_ref.kind = ir::ActionExpr::Kind::Arg;
              arg_ref.arg_index = static_cast<int>(i);
              extra.push_back(std::move(arg_ref));
              children.push_back(desugar_expr(parser_args[i], env, step_guarded));
              if (is_non_nullable_hint(parser_args[i], env)) {
                step_guarded = true;
              }
            }
            ir::ActionExpr act = build_action_expr(fn_expr, {}, extra);
            return ir::make_act(std::move(act), std::move(children), expr->span);
          }
        }

        if (expr->op_kind == ast::InfixOpKind::SeqR) {
          auto l = desugar_expr(expr->lhs, env, guarded);
          const bool rhs_guarded = guarded || is_non_nullable_hint(expr->lhs, env);
          auto r = desugar_expr(expr->rhs, env, rhs_guarded);
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Arg;
          act.arg_index = 1;
          return ir::make_act(std::move(act), {std::move(l), std::move(r)}, expr->span);
        }

        if (expr->op_kind == ast::InfixOpKind::SeqL) {
          auto l = desugar_expr(expr->lhs, env, guarded);
          const bool rhs_guarded = guarded || is_non_nullable_hint(expr->lhs, env);
          auto r = desugar_expr(expr->rhs, env, rhs_guarded);
          ir::ActionExpr act;
          act.kind = ir::ActionExpr::Kind::Arg;
          act.arg_index = 0;
          return ir::make_act(std::move(act), {std::move(l), std::move(r)}, expr->span);
        }

        if (expr->op_kind == ast::InfixOpKind::Alt) {
          return ir::make_alt(
              {desugar_expr(expr->lhs, env, guarded), desugar_expr(expr->rhs, env, guarded)},
              expr->span);
        }

        if (expr->op_kind == ast::InfixOpKind::Label) {
          std::string lbl = (expr->rhs && expr->rhs->kind == ast::RuleExpr::Kind::Lit)
                                ? expr->rhs->text
                                : "expression";
          return ir::make_label(std::move(lbl), desugar_expr(expr->lhs, env, guarded), expr->span);
        }

        if (expr->op_kind == ast::InfixOpKind::Backtick) {
          return monomorphize_call(expr->text, {expr->lhs, expr->rhs}, env, guarded, expr->span);
        }

        return ir::make_eps(expr->span);
      }

      case ast::RuleExpr::Kind::Alt: {
        std::vector<ir::ProdPtr> alts;
        for (const auto& item : expr->items) {
          auto p = desugar_expr(item, env, guarded);
          if (p && p->kind == ir::Prod::Kind::Alt) {
            alts.insert(alts.end(), p->items.begin(), p->items.end());
          } else {
            alts.push_back(std::move(p));
          }
        }
        return ir::make_alt(std::move(alts), expr->span);
      }

      case ast::RuleExpr::Kind::Do: {
        if (expr->is_context) {
          return desugar_context_do(expr, env, guarded);
        }
        return desugar_applicative_do(expr, env, guarded);
      }

      case ast::RuleExpr::Kind::Operators: {
        auto base = desugar_expr(expr->lhs, env, guarded);
        std::vector<ir::PrecLevel> levels;
        for (const auto& lvl : expr->op_levels) {
          ir::PrecLevel il;
          il.assoc = lvl.assoc;
          il.precedence = lvl.precedence;
          il.span = lvl.span;
          for (const auto& spec : lvl.specs) {
            ir::PrecOp iop;
            iop.span = spec.span;
            std::size_t non_lit_count = 0;
            for (const auto& pat_elem : spec.pattern) {
              if (pat_elem && pat_elem->kind != ast::RuleExpr::Kind::Lit) {
                ++non_lit_count;
              }
              iop.pattern.push_back(desugar_expr(pat_elem, env, true));
            }

            std::vector<ir::ActionExpr> extra;
            std::size_t arity = 0;
            if (lvl.assoc == ast::AssocKind::InfixL || lvl.assoc == ast::AssocKind::InfixR ||
                lvl.assoc == ast::AssocKind::Infix) {
              arity = 2;
            } else if (lvl.assoc == ast::AssocKind::Prefix ||
                       lvl.assoc == ast::AssocKind::Postfix) {
              arity = 1;
            } else if (lvl.assoc == ast::AssocKind::Mixfix) {
              arity = non_lit_count;
            }
            for (std::size_t i = 0; i < arity; ++i) {
              ir::ActionExpr arg;
              arg.kind = ir::ActionExpr::Kind::Arg;
              arg.arg_index = static_cast<int>(i);
              extra.push_back(std::move(arg));
            }
            if (spec.action) {
              iop.action = build_action_expr(spec.action, {}, extra);
            } else {
              iop.action.kind = ir::ActionExpr::Kind::Identity;
              iop.action.arg_index = 0;
            }
            il.ops.push_back(std::move(iop));
          }
          levels.push_back(std::move(il));
        }
        std::stable_sort(levels.begin(),
                         levels.end(),
                         [](const ir::PrecLevel& a, const ir::PrecLevel& b) {
                           return a.precedence < b.precedence;
                         });
        return ir::make_prec_table(std::move(base), std::move(levels), expr->span);
      }

      case ast::RuleExpr::Kind::List:
      case ast::RuleExpr::Kind::Tuple:
      case ast::RuleExpr::Kind::IntLit:
      case ast::RuleExpr::Kind::FloatLit: {
        ir::ActionExpr act = build_action_expr(expr, {});
        return ir::make_act(std::move(act), {}, expr->span);
      }

      case ast::RuleExpr::Kind::Paren:
        return desugar_expr(expr->lhs, env, guarded);
    }

    return ir::make_eps(expr->span);
  }

  ir::ProdPtr desugar_applicative_do(const ast::RuleExprPtr& expr,
                                     const ParamEnv& env,
                                     bool guarded) {
    std::vector<ir::ProdPtr> children;
    std::unordered_map<std::string, int> binder_indices;
    bool step_guarded = guarded;
    ast::RuleExprPtr pure_expr;

    for (std::size_t i = 0; i < expr->do_stmts.size(); ++i) {
      const auto& stmt = expr->do_stmts[i];
      const bool is_last = (i + 1 == expr->do_stmts.size());
      if (stmt.kind == ast::DoStmt::Kind::Pure && is_last) {
        pure_expr = stmt.expr;
        break;
      }
      const int idx = static_cast<int>(children.size());
      if (stmt.kind == ast::DoStmt::Kind::Bind && !stmt.binder.empty()) {
        binder_indices[stmt.binder] = idx;
      }
      children.push_back(desugar_expr(stmt.expr, env, step_guarded));
      if (is_non_nullable_hint(stmt.expr, env)) {
        step_guarded = true;
      }
    }

    if (pure_expr) {
      ir::ActionExpr act = build_action_expr(pure_expr, binder_indices);
      if (children.size() == 1) {
        return ir::make_act(std::move(act), std::move(children), expr->span);
      }
      // Explicitly group sequential steps via Seq inside Act when multiple steps exist
      // OR keep direct Act children for uniform indexing:
      // Use Act(action, [Seq(children)])? Keeping flat items on Act matches Section 6 (`Act Action [Prod]`),
      // and we can keep children directly on Act (or Seq if no bindings).
      if (binder_indices.empty() && pure_expr->kind == ast::RuleExpr::Kind::Tuple &&
          pure_expr->items.empty()) {
        return ir::make_seq(std::move(children), expr->span);
      }
      return ir::make_act(std::move(act), std::move(children), expr->span);
    }

    if (children.empty()) {
      return ir::make_eps(expr->span);
    }
    if (children.size() == 1) {
      return children[0];
    }
    return ir::make_seq(std::move(children), expr->span);
  }

  ir::ProdPtr desugar_context_do(const ast::RuleExprPtr& expr,
                                 const ParamEnv& env,
                                 bool guarded) {
    std::vector<ir::CtxStep> steps;
    std::unordered_map<std::string, int> binder_indices;
    bool step_guarded = guarded;
    ast::RuleExprPtr pure_expr;

    for (std::size_t i = 0; i < expr->do_stmts.size(); ++i) {
      const auto& stmt = expr->do_stmts[i];
      const bool is_last = (i + 1 == expr->do_stmts.size());
      if (stmt.kind == ast::DoStmt::Kind::Pure && is_last) {
        pure_expr = stmt.expr;
        break;
      }
      ir::CtxStep cs;
      cs.binder = (stmt.kind == ast::DoStmt::Kind::Bind) ? stmt.binder : "";
      cs.prod = desugar_expr(stmt.expr, env, step_guarded);
      cs.span = stmt.span;
      if (!cs.binder.empty()) {
        binder_indices[cs.binder] = static_cast<int>(steps.size());
      }
      steps.push_back(std::move(cs));
      if (is_non_nullable_hint(stmt.expr, env)) {
        step_guarded = true;
      }
    }

    ir::ActionExpr act;
    if (pure_expr) {
      act = build_action_expr(pure_expr, binder_indices);
    } else if (!steps.empty()) {
      act.kind = ir::ActionExpr::Kind::Arg;
      act.arg_index = static_cast<int>(steps.size() - 1);
    }
    return ir::make_ctx_block(std::move(steps), std::move(act), expr->span);
  }

  std::unordered_set<std::string> e0400_reported_rules_;

  ir::ProdPtr monomorphize_call(const std::string& fn,
                                const std::vector<ast::RuleExprPtr>& raw_args,
                                const ParamEnv& caller_env,
                                bool guarded,
                                const Span& call_span) {
    std::vector<ast::RuleExprPtr> args;
    args.reserve(raw_args.size());
    for (const auto& a : raw_args) {
      args.push_back(substitute_params(a, caller_env));
    }

    std::string key = fn;
    for (const auto& a : args) {
      key += "__" + expr_key(a, {});
    }

    // Check active monomorphization stack BEFORE cache to detect recursion cycles (E0400)
    int same_fn_depth = 0;
    for (const auto& frame : active_mono_) {
      if (frame.rule_name == fn) {
        ++same_fn_depth;
        if (frame.key == key) {
          if (!guarded) {
            if (e0400_reported_rules_.insert(fn).second) {
              Diagnostic d;
              d.code = "E0400";
              d.severity = Severity::Error;
              d.message = "unguarded recursive expansion in parameterized rule `" + fn + "`";
              d.span = call_span;
              d.label = "expands `" + fn + "` without consuming a token first";
              d.helps.push_back("ensure a token or non-nullable parser precedes the recursive call");
              diags_.push(std::move(d));
            }
          }
          return ir::make_ref(frame.mangled, call_span);
        }
      }
    }
    if (same_fn_depth >= 4 || (same_fn_depth >= 1 && !guarded)) {
      if (e0400_reported_rules_.insert(fn).second) {
        Diagnostic d;
        d.code = "E0400";
        d.severity = Severity::Error;
        d.message = "unguarded or non-terminating recursion in parameterized rule `" + fn + "`";
        d.span = call_span;
        d.label = "causes infinite monomorphization";
        d.helps.push_back("guard recursive calls with a token and avoid growing rule arguments");
        diags_.push(std::move(d));
      }
      return ir::make_eps(call_span);
    }

    if (auto mit = mono_cache_.find(key); mit != mono_cache_.end()) {
      return ir::make_ref(mit->second, call_span);
    }

    // Allocate unique mangled rule ID
    std::string mangled = key;
    int suffix = 1;
    while (generated_rules_.contains(mangled) || all_rule_defs_.contains(mangled)) {
      mangled = key + "_" + std::to_string(suffix++);
    }
    mono_cache_[key] = mangled;

    active_mono_.push_back(MonoFrame{fn, key, mangled});

    ir::Rule specialized;
    specialized.rSpan = call_span;
    specialized.is_generated = true;

    if (fn == "sepBy" && args.size() >= 2) {
      // sepBy p sep = (:) <$> p <*> many (sep *> p) <|> pure []
      auto dp = desugar_expr(args[0], {}, false);
      auto dsep = desugar_expr(args[1], {}, true);
      ir::ActionExpr skip_sep;
      skip_sep.kind = ir::ActionExpr::Kind::Arg;
      skip_sep.arg_index = 1;
      auto sep_p = ir::make_act(std::move(skip_sep), {dsep, dp}, call_span);
      auto tail = ir::make_rep(0, std::nullopt, std::move(sep_p), call_span);

      ir::ActionExpr cons_act;
      cons_act.kind = ir::ActionExpr::Kind::List;
      cons_act.name = ":";
      cons_act.args = {
          ir::ActionExpr{ir::ActionExpr::Kind::Arg, "", 0, 0.0, false, {}},
          ir::ActionExpr{ir::ActionExpr::Kind::Arg, "", 1, 0.0, false, {}},
      };
      auto non_empty = ir::make_act(std::move(cons_act), {dp, std::move(tail)}, call_span);

      ir::ActionExpr empty_list;
      empty_list.kind = ir::ActionExpr::Kind::List;
      auto empty = ir::make_act(std::move(empty_list), {}, call_span);

      specialized.rBody = ir::make_alt({std::move(non_empty), std::move(empty)}, call_span);
      specialized.rType = ir::Type{ir::Type::Kind::List, "List", {}};
    } else if (fn == "sepEndBy" && args.size() >= 2) {
      auto base_sep_by = monomorphize_call("sepBy", args, {}, false, call_span);
      auto dsep = desugar_expr(args[1], {}, true);
      auto opt_sep = ir::make_rep(0, 1, std::move(dsep), call_span);
      ir::ActionExpr keep_first;
      keep_first.kind = ir::ActionExpr::Kind::Arg;
      keep_first.arg_index = 0;
      specialized.rBody = ir::make_act(
          std::move(keep_first), {std::move(base_sep_by), std::move(opt_sep)}, call_span);
      specialized.rType = ir::Type{ir::Type::Kind::List, "List", {}};
    } else if (fn == "between" && args.size() >= 3) {
      auto dopen = desugar_expr(args[0], {}, false);
      auto dp = desugar_expr(args[2], {}, true);
      auto dclose = desugar_expr(args[1], {}, true);
      ir::ActionExpr keep_mid;
      keep_mid.kind = ir::ActionExpr::Kind::Arg;
      keep_mid.arg_index = 1;
      specialized.rBody = ir::make_act(
          std::move(keep_mid), {std::move(dopen), std::move(dp), std::move(dclose)}, call_span);
    } else if (fn == "block" && args.size() >= 1) {
      auto dind = ir::make_tok("indent", call_span);
      auto dmany = ir::make_rep(0, std::nullopt, desugar_expr(args[0], {}, true), call_span);
      auto dded = ir::make_tok("dedent", call_span);
      ir::ActionExpr keep_mid;
      keep_mid.kind = ir::ActionExpr::Kind::Arg;
      keep_mid.arg_index = 1;
      specialized.rBody = ir::make_act(
          std::move(keep_mid), {std::move(dind), std::move(dmany), std::move(dded)}, call_span);
      specialized.rType = ir::Type{ir::Type::Kind::List, "List", {}};
    } else if (auto rit = all_rule_defs_.find(fn); rit != all_rule_defs_.end()) {
      const auto& def = rit->second;
      ParamEnv sub_env;
      for (std::size_t i = 0; i < def.params.size() && i < args.size(); ++i) {
        sub_env[def.params[i]] = args[i];
      }
      specialized.rSpan = def.span;
      specialized.rType = lookup_rule_ir_type(fn);
      specialized.rBody = desugar_expr(def.body, sub_env, false);
    } else {
      specialized.rBody = ir::make_ref(fn, call_span);
    }

    active_mono_.pop_back();
    generated_rules_[mangled] = std::move(specialized);
    return ir::make_ref(mangled, call_span);
  }
};

} // namespace parsley::desugar::detail

export namespace parsley::desugar {

inline ir::Grammar desugar_module(const ast::Module& mod,
                                  const resolve::ResolvedModule& res,
                                  const typecheck::TypedModule& typed,
                                  DiagnosticBag& diags) {
  detail::Desugarer d(mod, res, typed, diags);
  return d.run();
}

} // namespace parsley::desugar
