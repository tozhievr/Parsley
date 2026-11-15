module;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.typecheck;

import parsley.diagnostic;
import parsley.ast;
import parsley.resolve;

export namespace parsley::typecheck {

struct Ty;
using TyPtr = std::shared_ptr<Ty>;

struct Ty {
  enum class Kind {
    Var,
    Con,
    Arrow,
  };

  Kind kind = Kind::Con;
  int var_id = 0;
  std::string name;
  bool is_numeric = false;
  TyPtr instance;
  std::vector<TyPtr> args;
};

struct TypeScheme {
  std::vector<int> quantified;
  TyPtr type;
};

inline TyPtr prune(const TyPtr& t) {
  if (!t) {
    return t;
  }
  if (t->kind == Ty::Kind::Var && t->instance) {
    t->instance = prune(t->instance);
    return t->instance;
  }
  return t;
}

inline std::string format_type(const TyPtr& raw);

inline ast::TypeExpr to_ast_type(const TyPtr& raw) {
  const TyPtr t = prune(raw);
  ast::TypeExpr out;
  if (!t) {
    out.kind = ast::TypeExpr::Kind::Named;
    out.name = "Unit";
    return out;
  }
  switch (t->kind) {
    case Ty::Kind::Var: {
      if (t->is_numeric) {
        out.kind = ast::TypeExpr::Kind::Named;
        out.name = "Int";
      } else {
        out.kind = ast::TypeExpr::Kind::Var;
        out.name = t->name.empty() ? ("t" + std::to_string(t->var_id)) : t->name;
      }
      return out;
    }
    case Ty::Kind::Arrow: {
      out.kind = ast::TypeExpr::Kind::Arrow;
      if (t->args.size() >= 2) {
        out.left = std::make_shared<ast::TypeExpr>(to_ast_type(t->args[0]));
        out.right = std::make_shared<ast::TypeExpr>(to_ast_type(t->args[1]));
      }
      return out;
    }
    case Ty::Kind::Con: {
      if (t->name == "List" && t->args.size() == 1) {
        out.kind = ast::TypeExpr::Kind::List;
        out.left = std::make_shared<ast::TypeExpr>(to_ast_type(t->args[0]));
        return out;
      }
      if (t->name == "Tuple") {
        out.kind = ast::TypeExpr::Kind::Tuple;
        if (t->args.empty()) {
          out.name = "Unit";
        } else {
          for (const auto& a : t->args) {
            out.args.push_back(std::make_shared<ast::TypeExpr>(to_ast_type(a)));
          }
        }
        return out;
      }
      if (t->name == "Unit") {
        out.kind = ast::TypeExpr::Kind::Named;
        out.name = "Unit";
        return out;
      }
      if (t->args.empty()) {
        out.kind = ast::TypeExpr::Kind::Named;
        out.name = t->name;
        return out;
      }
      out.kind = ast::TypeExpr::Kind::App;
      ast::TypeExpr head;
      head.kind = ast::TypeExpr::Kind::Named;
      head.name = t->name;
      out.left = std::make_shared<ast::TypeExpr>(std::move(head));
      for (const auto& a : t->args) {
        out.args.push_back(std::make_shared<ast::TypeExpr>(to_ast_type(a)));
      }
      return out;
    }
  }
  return out;
}

inline std::string format_type(const TyPtr& raw) {
  const TyPtr t = prune(raw);
  if (!t) {
    return "Unit";
  }
  switch (t->kind) {
    case Ty::Kind::Var: {
      if (t->is_numeric) {
        return "Int";
      }
      if (!t->name.empty()) {
        return t->name;
      }
      return "t" + std::to_string(t->var_id);
    }
    case Ty::Kind::Arrow: {
      if (t->args.size() < 2) {
        return "->";
      }
      const TyPtr lhs = prune(t->args[0]);
      std::string left_str = format_type(lhs);
      if (lhs && lhs->kind == Ty::Kind::Arrow) {
        left_str = "(" + left_str + ")";
      }
      return left_str + " -> " + format_type(t->args[1]);
    }
    case Ty::Kind::Con: {
      if (t->name == "Unit") {
        return "()";
      }
      if (t->name == "List" && t->args.size() == 1) {
        return "[" + format_type(t->args[0]) + "]";
      }
      if (t->name == "Tuple") {
        if (t->args.empty()) {
          return "()";
        }
        std::string out = "(";
        for (std::size_t i = 0; i < t->args.size(); ++i) {
          if (i > 0) {
            out += ", ";
          }
          out += format_type(t->args[i]);
        }
        out += ")";
        return out;
      }
      if (t->args.empty()) {
        return t->name;
      }
      std::string out = t->name;
      for (const auto& a : t->args) {
        const TyPtr pa = prune(a);
        std::string s = format_type(pa);
        if (pa && ((pa->kind == Ty::Kind::Con && !pa->args.empty() && pa->name != "List" &&
                    pa->name != "Tuple") ||
                   pa->kind == Ty::Kind::Arrow)) {
          s = "(" + s + ")";
        }
        out += " " + s;
      }
      return out;
    }
  }
  return "Unit";
}

struct TypedModule {
  std::unordered_map<std::string, TyPtr> rule_types;
  std::unordered_map<std::string, TyPtr> rule_result_types;
  std::unordered_map<std::string, std::string> rule_type_strings;
  std::unordered_map<std::string, std::string> rule_result_type_strings;
  std::unordered_map<std::string, bool> rule_is_context;

  std::optional<std::string> lookup_rule_type_string(std::string_view name) const {
    auto it = rule_type_strings.find(std::string(name));
    if (it != rule_type_strings.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::optional<std::string> lookup_rule_result_type_string(std::string_view name) const {
    auto it = rule_result_type_strings.find(std::string(name));
    if (it != rule_result_type_strings.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::optional<TyPtr> lookup_rule_type(std::string_view name) const {
    auto it = rule_types.find(std::string(name));
    if (it != rule_types.end()) {
      return it->second;
    }
    return std::nullopt;
  }
};

} // namespace parsley::typecheck

namespace parsley::typecheck::detail {

class TypeChecker {
public:
  TypeChecker(const ast::Module& mod,
              const resolve::ResolvedModule& resolved,
              DiagnosticBag& diags)
      : mod_(mod), resolved_(resolved), diags_(diags) {}

  TypedModule check() {
    TypedModule out;

    init_builtin_schemes();
    init_data_schemes();
    init_token_schemes();
    check_exported_signatures();
    register_rule_preliminary_schemes();

    for (const auto& def : mod_.rule_defs) {
      if (def.name.empty() || def.name == "<error>") {
        continue;
      }
      check_applicative_do(def.body);
      check_rule_def(def);
      out.rule_is_context[def.name] = contains_context_do(def.body);
    }

    for (const auto& def : mod_.rule_defs) {
      if (def.name.empty() || def.name == "<error>") {
        continue;
      }
      auto it = rule_final_types_.find(def.name);
      if (it != rule_final_types_.end()) {
        TyPtr full = prune(it->second);
        out.rule_types[def.name] = full;
        out.rule_type_strings[def.name] = format_type(full);

        TyPtr res_ty = extract_rule_result_type(full);
        out.rule_result_types[def.name] = res_ty;
        out.rule_result_type_strings[def.name] = format_type(res_ty);
      }
    }

    return out;
  }

private:
  const ast::Module& mod_;
  const resolve::ResolvedModule& resolved_;
  DiagnosticBag& diags_;

  int next_var_id_ = 1;
  std::unordered_map<std::string, TypeScheme> env_;
  std::unordered_map<std::string, TyPtr> rule_final_types_;

  TyPtr fresh_var(std::string hint = "", bool is_numeric = false) {
    auto t = std::make_shared<Ty>();
    t->kind = Ty::Kind::Var;
    t->var_id = next_var_id_++;
    t->name = std::move(hint);
    t->is_numeric = is_numeric;
    return t;
  }

  TyPtr make_con(std::string name, std::vector<TyPtr> args = {}) {
    auto t = std::make_shared<Ty>();
    t->kind = Ty::Kind::Con;
    t->name = std::move(name);
    t->args = std::move(args);
    return t;
  }

  TyPtr make_arrow(TyPtr lhs, TyPtr rhs) {
    auto t = std::make_shared<Ty>();
    t->kind = Ty::Kind::Arrow;
    t->args = {std::move(lhs), std::move(rhs)};
    return t;
  }

  TyPtr make_rule(TyPtr inner) {
    return make_con("Rule", {std::move(inner)});
  }

  TyPtr make_list(TyPtr elem) {
    return make_con("List", {std::move(elem)});
  }

  TyPtr make_unit() {
    return make_con("Unit", {});
  }

  bool is_rule_con(const TyPtr& raw) const {
    const TyPtr t = prune(raw);
    return t && t->kind == Ty::Kind::Con && t->name == "Rule" && t->args.size() == 1;
  }

  TyPtr extract_rule_result_type(const TyPtr& raw) {
    TyPtr t = prune(raw);
    while (t && t->kind == Ty::Kind::Arrow && t->args.size() == 2) {
      t = prune(t->args[1]);
    }
    if (is_rule_con(t)) {
      return prune(t->args[0]);
    }
    return t;
  }

  bool occurs_in(int var_id, const TyPtr& raw) {
    const TyPtr t = prune(raw);
    if (!t) {
      return false;
    }
    if (t->kind == Ty::Kind::Var) {
      return t->var_id == var_id;
    }
    for (const auto& a : t->args) {
      if (occurs_in(var_id, a)) {
        return true;
      }
    }
    return false;
  }

  bool unify(const TyPtr& raw_a, const TyPtr& raw_b, const Span& span) {
    const TyPtr a = prune(raw_a);
    const TyPtr b = prune(raw_b);
    if (!a || !b) {
      return true;
    }
    if (a == b) {
      return true;
    }

    if (a->kind == Ty::Kind::Var && b->kind == Ty::Kind::Var && a->var_id == b->var_id) {
      return true;
    }

    auto bind_var = [&](const TyPtr& v, const TyPtr& other) -> bool {
      if (occurs_in(v->var_id, other)) {
        diags_.error("E0300",
                     "infinite type: cannot unify `" + format_type(v) + "` with `" +
                         format_type(other) + "`",
                     span,
                     "occurs check failed");
        return false;
      }
      if (v->is_numeric) {
        if (other->kind == Ty::Kind::Con &&
            other->name != "Int" && other->name != "Double") {
          diags_.error("E0300",
                       "type mismatch: numeric literal cannot unify with `" +
                           format_type(other) + "`",
                       span,
                       "expected `" + format_type(other) + "`");
          return false;
        }
        if (other->kind == Ty::Kind::Var && !other->is_numeric) {
          // Bind the non-numeric var to the numeric var or keep numeric constraint
          other->instance = v;
          return true;
        }
      }
      v->instance = other;
      return true;
    };

    if (a->kind == Ty::Kind::Var) {
      return bind_var(a, b);
    }
    if (b->kind == Ty::Kind::Var) {
      return bind_var(b, a);
    }

    if (a->kind == Ty::Kind::Arrow && b->kind == Ty::Kind::Arrow) {
      return unify(a->args[0], b->args[0], span) &&
             unify(a->args[1], b->args[1], span);
    }

    if (a->kind == Ty::Kind::Con && b->kind == Ty::Kind::Con) {
      // Allow Int <-> Double? No, keep strict except numeric literals.
      if (a->name == b->name && a->args.size() == b->args.size()) {
        for (std::size_t i = 0; i < a->args.size(); ++i) {
          if (!unify(a->args[i], b->args[i], span)) {
            return false;
          }
        }
        return true;
      }
    }

    diags_.error("E0300",
                 "type mismatch: expected `" + format_type(a) + "`, got `" +
                     format_type(b) + "`",
                 span,
                 "expected `" + format_type(a) + "`");
    return false;
  }

  void collect_free_vars(const TyPtr& raw,
                         const std::unordered_set<int>& bound,
                         std::vector<int>& out,
                         std::unordered_set<int>& seen) {
    const TyPtr t = prune(raw);
    if (!t) {
      return;
    }
    if (t->kind == Ty::Kind::Var) {
      if (!bound.contains(t->var_id) && seen.insert(t->var_id).second) {
        out.push_back(t->var_id);
      }
      return;
    }
    for (const auto& a : t->args) {
      collect_free_vars(a, bound, out, seen);
    }
  }

  TypeScheme generalize(const TyPtr& t) {
    std::vector<int> fvs;
    std::unordered_set<int> seen;
    std::unordered_set<int> empty_bound;
    collect_free_vars(t, empty_bound, fvs, seen);
    return TypeScheme{std::move(fvs), t};
  }

  TyPtr instantiate(const TypeScheme& scheme) {
    if (scheme.quantified.empty()) {
      return scheme.type;
    }
    std::unordered_map<int, TyPtr> subst;
    for (int qv : scheme.quantified) {
      subst[qv] = fresh_var();
    }
    return copy_with_subst(scheme.type, subst);
  }

  TyPtr copy_with_subst(const TyPtr& raw, const std::unordered_map<int, TyPtr>& subst) {
    const TyPtr t = prune(raw);
    if (!t) {
      return t;
    }
    if (t->kind == Ty::Kind::Var) {
      auto it = subst.find(t->var_id);
      if (it != subst.end()) {
        return it->second;
      }
      return t;
    }
    auto cloned = std::make_shared<Ty>();
    cloned->kind = t->kind;
    cloned->name = t->name;
    for (const auto& a : t->args) {
      cloned->args.push_back(copy_with_subst(a, subst));
    }
    return cloned;
  }

  TyPtr convert_ast_type(const ast::TypeExpr& te,
                         std::unordered_map<std::string, TyPtr>& tvars,
                         std::vector<int>& collected_vars) {
    switch (te.kind) {
      case ast::TypeExpr::Kind::Named: {
        if (te.name == "()" || te.name == "Unit") {
          return make_unit();
        }
        return make_con(te.name);
      }
      case ast::TypeExpr::Kind::Var: {
        auto it = tvars.find(te.name);
        if (it != tvars.end()) {
          return it->second;
        }
        TyPtr v = fresh_var(te.name);
        tvars[te.name] = v;
        collected_vars.push_back(v->var_id);
        return v;
      }
      case ast::TypeExpr::Kind::List: {
        TyPtr elem = te.left ? convert_ast_type(*te.left, tvars, collected_vars) : fresh_var();
        return make_list(std::move(elem));
      }
      case ast::TypeExpr::Kind::Tuple: {
        if (te.args.empty()) {
          return make_unit();
        }
        std::vector<TyPtr> elems;
        for (const auto& a : te.args) {
          elems.push_back(a ? convert_ast_type(*a, tvars, collected_vars) : fresh_var());
        }
        return make_con("Tuple", std::move(elems));
      }
      case ast::TypeExpr::Kind::App: {
        std::string head_name = "App";
        if (te.left && te.left->kind == ast::TypeExpr::Kind::Named) {
          head_name = te.left->name;
        }
        std::vector<TyPtr> args;
        for (const auto& a : te.args) {
          args.push_back(a ? convert_ast_type(*a, tvars, collected_vars) : fresh_var());
        }
        return make_con(std::move(head_name), std::move(args));
      }
      case ast::TypeExpr::Kind::Arrow: {
        TyPtr lhs = te.left ? convert_ast_type(*te.left, tvars, collected_vars) : fresh_var();
        TyPtr rhs = te.right ? convert_ast_type(*te.right, tvars, collected_vars) : fresh_var();
        return make_arrow(std::move(lhs), std::move(rhs));
      }
    }
    return make_unit();
  }

  TypeScheme make_rule_scheme_from_ast(const ast::TypeExpr& te, std::size_t param_count) {
    std::unordered_map<std::string, TyPtr> tvars;
    std::vector<int> qvars;
    TyPtr raw = convert_ast_type(te, tvars, qvars);

    // Ensure the final return type after `param_count` arrows is `Rule _`
    if (param_count == 0) {
      if (!is_rule_con(raw)) {
        raw = make_rule(raw);
      }
    } else {
      // Walk `param_count` arrows; wrap result in `Rule` if not already `Rule _`
      std::function<TyPtr(const TyPtr&, std::size_t)> wrap_ret =
          [&](const TyPtr& cur, std::size_t depth) -> TyPtr {
        if (depth == 0) {
          return is_rule_con(cur) ? cur : make_rule(cur);
        }
        if (cur->kind == Ty::Kind::Arrow && cur->args.size() == 2) {
          return make_arrow(cur->args[0], wrap_ret(cur->args[1], depth - 1));
        }
        return cur;
      };
      raw = wrap_ret(raw, param_count);
    }

    return TypeScheme{std::move(qvars), std::move(raw)};
  }

  void init_builtin_schemes() {
    auto text_t = make_con("Text");
    auto double_t = make_con("Double");
    auto bool_t = make_con("Bool");
    auto unit_t = make_unit();

    // Prelude combinators
    {
      auto a = fresh_var("a");
      auto sep = fresh_var("sep");
      env_["sepBy"] = TypeScheme{
          {a->var_id, sep->var_id},
          make_arrow(make_rule(a), make_arrow(make_rule(sep), make_rule(make_list(a)))),
      };
    }
    {
      auto a = fresh_var("a");
      auto sep = fresh_var("sep");
      env_["sepEndBy"] = TypeScheme{
          {a->var_id, sep->var_id},
          make_arrow(make_rule(a), make_arrow(make_rule(sep), make_rule(make_list(a)))),
      };
    }
    {
      auto a = fresh_var("a");
      env_["many"] = TypeScheme{
          {a->var_id},
          make_arrow(make_rule(a), make_rule(make_list(a))),
      };
    }
    {
      auto a = fresh_var("a");
      env_["some"] = TypeScheme{
          {a->var_id},
          make_arrow(make_rule(a), make_rule(make_list(a))),
      };
    }
    {
      auto a = fresh_var("a");
      env_["optional"] = TypeScheme{
          {a->var_id},
          make_arrow(make_rule(a), make_rule(make_con("Maybe", {a}))),
      };
    }
    {
      auto o = fresh_var("open");
      auto c = fresh_var("close");
      auto a = fresh_var("a");
      env_["between"] = TypeScheme{
          {o->var_id, c->var_id, a->var_id},
          make_arrow(make_rule(o),
                     make_arrow(make_rule(c), make_arrow(make_rule(a), make_rule(a)))),
      };
    }
    env_["eof"] = TypeScheme{{}, make_rule(unit_t)};
    {
      auto a = fresh_var("a");
      env_["lookahead"] = TypeScheme{
          {a->var_id},
          make_arrow(make_rule(a), make_rule(a)),
      };
    }
    {
      auto a = fresh_var("a");
      env_["notFollowedBy"] = TypeScheme{
          {a->var_id},
          make_arrow(make_rule(a), make_rule(unit_t)),
      };
    }
    {
      auto a = fresh_var("a");
      auto s = fresh_var("sync");
      env_["recover"] = TypeScheme{
          {a->var_id, s->var_id},
          make_arrow(make_rule(a), make_arrow(make_rule(s), make_rule(a))),
      };
    }
    env_["exactly"] = TypeScheme{{}, make_arrow(text_t, make_rule(text_t))};
    {
      auto a = fresh_var("a");
      env_["pure"] = TypeScheme{{a->var_id}, make_arrow(a, make_rule(a))};
    }

    // Built-in value constructors
    env_["True"] = TypeScheme{{}, bool_t};
    env_["False"] = TypeScheme{{}, bool_t};
    {
      auto a = fresh_var("a");
      env_["Nothing"] = TypeScheme{{a->var_id}, make_con("Maybe", {a})};
    }
    {
      auto a = fresh_var("a");
      env_["Just"] = TypeScheme{{a->var_id}, make_arrow(a, make_con("Maybe", {a}))};
    }

    // Unicode / Layout / Json / Sql imported symbols
    for (const auto& [name, sym] : resolved_.values) {
      if (sym.is_builtin && !env_.contains(name)) {
        if (sym.source_module == "Parsley.Unicode" || sym.source_module == "Parsley.Layout") {
          if (sym.name == "block") {
            auto a = fresh_var("a");
            env_[name] = TypeScheme{{a->var_id}, make_arrow(make_rule(a), make_rule(make_list(a)))};
          } else {
            env_[name] = TypeScheme{{}, make_rule(text_t)};
          }
        } else if (sym.source_module == "Json") {
          if (sym.name == "value") {
            env_[name] = TypeScheme{{}, make_rule(make_con("JsonValue"))};
          } else if (sym.name == "object") {
            env_[name] = TypeScheme{
                {}, make_rule(make_list(make_con("Tuple", {text_t, make_con("JsonValue")})))};
          } else if (sym.name == "array") {
            env_[name] = TypeScheme{{}, make_rule(make_list(make_con("JsonValue")))};
          } else if (sym.name == "number") {
            env_[name] = TypeScheme{{}, make_rule(double_t)};
          } else if (sym.name == "boolean") {
            env_[name] = TypeScheme{{}, make_rule(bool_t)};
          } else if (sym.name == "null") {
            env_[name] = TypeScheme{{}, make_rule(unit_t)};
          } else {
            env_[name] = TypeScheme{{}, make_rule(text_t)};
          }
        } else if (sym.source_module == "Sql") {
          if (sym.name == "expr") {
            env_[name] = TypeScheme{{}, make_rule(make_con("SqlExpr"))};
          } else {
            env_[name] = TypeScheme{{}, make_rule(make_con("SqlStmt"))};
          }
        }
      }
    }

    // Stdlib Json/Sql constructors if imported
    auto json_t = make_con("JsonValue");
    auto sql_stmt_t = make_con("SqlStmt");
    auto sql_expr_t = make_con("SqlExpr");
    auto reg_if_present = [&](const std::string& name, TypeScheme scheme) {
      for (const auto& [k, sym] : resolved_.ctors) {
        if (sym.name == name) {
          env_[k] = scheme;
        }
      }
    };
    reg_if_present("JObject", TypeScheme{{}, make_arrow(make_list(make_con("Tuple", {text_t, json_t})), json_t)});
    reg_if_present("JArray", TypeScheme{{}, make_arrow(make_list(json_t), json_t)});
    reg_if_present("JString", TypeScheme{{}, make_arrow(text_t, json_t)});
    reg_if_present("JNumber", TypeScheme{{}, make_arrow(double_t, json_t)});
    reg_if_present("JBool", TypeScheme{{}, make_arrow(bool_t, json_t)});
    reg_if_present("JNull", TypeScheme{{}, json_t});
    reg_if_present("Col", TypeScheme{{}, make_arrow(text_t, sql_expr_t)});
    reg_if_present("LitNum", TypeScheme{{}, make_arrow(double_t, sql_expr_t)});
    reg_if_present("LitStr", TypeScheme{{}, make_arrow(text_t, sql_expr_t)});
    reg_if_present("Select", TypeScheme{{}, make_arrow(make_list(text_t), make_arrow(text_t, make_arrow(make_con("Maybe", {sql_expr_t}), sql_stmt_t)))});
    reg_if_present("Insert", TypeScheme{{}, make_arrow(text_t, make_arrow(make_list(text_t), make_arrow(make_list(sql_expr_t), sql_stmt_t)))});
    reg_if_present("Update", TypeScheme{{}, make_arrow(text_t, make_arrow(make_list(make_con("Tuple", {text_t, sql_expr_t})), make_arrow(make_con("Maybe", {sql_expr_t}), sql_stmt_t)))});
    reg_if_present("Delete", TypeScheme{{}, make_arrow(text_t, make_arrow(make_con("Maybe", {sql_expr_t}), sql_stmt_t))});
  }

  void init_data_schemes() {
    for (const auto& dd : mod_.data_decls) {
      if (dd.name.empty() || dd.name == "<error>") {
        continue;
      }
      std::unordered_map<std::string, TyPtr> tvars;
      std::vector<int> qvars;
      std::vector<TyPtr> result_args;
      for (const auto& tp : dd.type_params) {
        TyPtr v = fresh_var(tp);
        tvars[tp] = v;
        qvars.push_back(v->var_id);
        result_args.push_back(v);
      }
      TyPtr result_ty = make_con(dd.name, std::move(result_args));

      for (const auto& ctor : dd.constructors) {
        if (ctor.name.empty() || ctor.name == "<error>") {
          continue;
        }
        std::vector<TyPtr> field_types;
        if (ctor.is_record) {
          for (const auto& rf : ctor.record_fields) {
            field_types.push_back(convert_ast_type(rf.type, tvars, qvars));
          }
        } else {
          for (const auto& pf : ctor.positional_fields) {
            field_types.push_back(convert_ast_type(pf, tvars, qvars));
          }
        }

        TyPtr ctor_ty = result_ty;
        for (auto it = field_types.rbegin(); it != field_types.rend(); ++it) {
          ctor_ty = make_arrow(*it, ctor_ty);
        }
        env_[ctor.name] = TypeScheme{qvars, ctor_ty};
      }
    }
  }

  void init_token_schemes() {
    auto register_tok = [&](const ast::TokenDecl& tok) {
      if (tok.name.empty() || tok.name == "<error>") {
        return;
      }
      TyPtr val_ty = make_con("Text");
      if (tok.type_ann) {
        std::unordered_map<std::string, TyPtr> tvars;
        std::vector<int> qvars;
        val_ty = convert_ast_type(*tok.type_ann, tvars, qvars);
      }
      env_[tok.name] = TypeScheme{{}, make_rule(val_ty)};
    };

    for (const auto& tok : mod_.tokens) {
      register_tok(tok);
    }
    for (const auto& mode : mod_.modes) {
      for (const auto& tok : mode.tokens) {
        register_tok(tok);
      }
    }
  }

  void check_exported_signatures() {
    if (!mod_.has_export_list) {
      return;
    }
    for (const auto& exp : mod_.exports) {
      if (exp.name == "<error>") {
        continue;
      }
      auto it = resolved_.rule_defs.find(exp.name);
      if (it == resolved_.rule_defs.end()) {
        continue;
      }
      const auto& def = it->second;
      if (!def.signature.has_value() && !resolved_.rule_sigs.contains(def.name)) {
        Diagnostic d;
        d.code = "E0301";
        d.severity = Severity::Error;
        d.message = "missing explicit type signature on exported rule `" + def.name + "`";
        d.span = def.span;
        d.label = "exported rule requires a `:: Type` signature";
        d.helps.push_back("add `" + def.name + " :: <Type>` above the rule definition");
        diags_.push(std::move(d));
      }
    }
  }

  void register_rule_preliminary_schemes() {
    for (const auto& def : mod_.rule_defs) {
      if (def.name.empty() || def.name == "<error>") {
        continue;
      }
      std::optional<ast::TypeExpr> sig = def.signature;
      if (!sig) {
        if (auto it = resolved_.rule_sigs.find(def.name); it != resolved_.rule_sigs.end()) {
          sig = it->second.type;
        }
      }
      if (sig) {
        TypeScheme scheme = make_rule_scheme_from_ast(*sig, def.params.size());
        env_[def.name] = scheme;
        rule_final_types_[def.name] = scheme.type;
      } else {
        TyPtr ret = make_rule(fresh_var());
        TyPtr full = ret;
        for (std::size_t i = 0; i < def.params.size(); ++i) {
          full = make_arrow(fresh_var(), full);
        }
        env_[def.name] = TypeScheme{{}, full};
        rule_final_types_[def.name] = full;
      }
    }
  }

  void check_rule_def(const ast::RuleDef& def) {
    auto saved_env = env_;

    TyPtr expected_full = instantiate(env_[def.name]);
    // For annotated rules, bind signature type vars rigidly via the scheme's own type
    if (def.signature.has_value() || resolved_.rule_sigs.contains(def.name)) {
      expected_full = env_[def.name].type;
    }

    TyPtr cur = expected_full;
    for (const auto& param : def.params) {
      if (cur->kind == Ty::Kind::Arrow && cur->args.size() == 2) {
        env_[param] = TypeScheme{{}, cur->args[0]};
        cur = cur->args[1];
      } else {
        TyPtr pty = fresh_var();
        env_[param] = TypeScheme{{}, pty};
      }
    }

    // Ensure body is checked as a Rule
    if (!is_rule_con(cur) && prune(cur)->kind == Ty::Kind::Var) {
      unify(cur, make_rule(fresh_var()), def.span);
    }

    TyPtr body_ty = infer_expr(def.body, cur);
    unify(cur, body_ty, def.body ? def.body->span : def.span);

    env_ = std::move(saved_env);

    // If unannotated, generalize for downstream callers
    if (!def.signature.has_value() && !resolved_.rule_sigs.contains(def.name)) {
      env_[def.name] = generalize(rule_final_types_[def.name]);
    }
  }

  TyPtr infer_expr(const ast::RuleExprPtr& expr, const TyPtr& expected) {
    if (!expr) {
      return fresh_var();
    }
    const TyPtr exp = prune(expected);

    switch (expr->kind) {
      case ast::RuleExpr::Kind::Lit: {
        // String literals act as `Text` in pure/text-expected positions,
        // and as anonymous `Rule Text` tokens in combinator positions.
        if (exp && exp->kind == Ty::Kind::Con && exp->name == "Text") {
          return make_con("Text");
        }
        return make_rule(make_con("Text"));
      }

      case ast::RuleExpr::Kind::IntLit: {
        return fresh_var("", true);
      }

      case ast::RuleExpr::Kind::FloatLit: {
        return make_con("Double");
      }

      case ast::RuleExpr::Kind::Var: {
        if (expr->text == "<error>" || expr->text.empty()) {
          return fresh_var();
        }
        auto it = env_.find(expr->text);
        if (it != env_.end()) {
          return instantiate(it->second);
        }
        return fresh_var();
      }

      case ast::RuleExpr::Kind::Paren: {
        return infer_expr(expr->lhs, expected);
      }

      case ast::RuleExpr::Kind::App: {
        TyPtr fn_ty = prune(infer_expr(expr->lhs, nullptr));
        if (fn_ty && fn_ty->kind == Ty::Kind::Arrow && fn_ty->args.size() == 2) {
          TyPtr arg_ty = infer_expr(expr->rhs, fn_ty->args[0]);
          unify(fn_ty->args[0], arg_ty, expr->rhs ? expr->rhs->span : expr->span);
          return prune(fn_ty->args[1]);
        }
        TyPtr arg_ty = infer_expr(expr->rhs, nullptr);
        TyPtr res_ty = expected ? expected : fresh_var();
        unify(fn_ty, make_arrow(arg_ty, res_ty), expr->span);
        return prune(res_ty);
      }

      case ast::RuleExpr::Kind::Pure: {
        TyPtr inner_exp = nullptr;
        if (is_rule_con(exp)) {
          inner_exp = prune(exp->args[0]);
        }
        TyPtr val_ty = infer_expr(expr->lhs, inner_exp);
        return make_rule(val_ty);
      }

      case ast::RuleExpr::Kind::InfixOp: {
        switch (expr->op_kind) {
          case ast::InfixOpKind::Fmap: {
            // (<$>) :: (a -> b) -> Rule a -> Rule b
            TyPtr a = fresh_var();
            TyPtr b = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
            TyPtr fn_exp = make_arrow(a, b);
            TyPtr fn_ty = infer_expr(expr->lhs, fn_exp);
            unify(fn_exp, fn_ty, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr rhs_exp = make_rule(prune(a));
            TyPtr rhs_ty = infer_expr(expr->rhs, rhs_exp);
            unify(rhs_exp, rhs_ty, expr->rhs ? expr->rhs->span : expr->span);
            return make_rule(prune(b));
          }
          case ast::InfixOpKind::Ap: {
            // (<*>) :: Rule (a -> b) -> Rule a -> Rule b
            TyPtr a = fresh_var();
            TyPtr b = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
            TyPtr lhs_exp = make_rule(make_arrow(a, b));
            TyPtr lhs_ty = infer_expr(expr->lhs, lhs_exp);
            unify(lhs_exp, lhs_ty, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr rhs_exp = make_rule(prune(a));
            TyPtr rhs_ty = infer_expr(expr->rhs, rhs_exp);
            unify(rhs_exp, rhs_ty, expr->rhs ? expr->rhs->span : expr->span);
            return make_rule(prune(b));
          }
          case ast::InfixOpKind::SeqR: {
            // (*>) :: Rule a -> Rule b -> Rule b
            TyPtr a = fresh_var();
            TyPtr lhs_ty = infer_expr(expr->lhs, make_rule(a));
            unify(make_rule(a), lhs_ty, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr b = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
            TyPtr rhs_ty = infer_expr(expr->rhs, make_rule(b));
            unify(make_rule(b), rhs_ty, expr->rhs ? expr->rhs->span : expr->span);
            return make_rule(prune(b));
          }
          case ast::InfixOpKind::SeqL: {
            // (<*) :: Rule a -> Rule b -> Rule a
            TyPtr a = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
            TyPtr lhs_ty = infer_expr(expr->lhs, make_rule(a));
            unify(make_rule(a), lhs_ty, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr b = fresh_var();
            TyPtr rhs_ty = infer_expr(expr->rhs, make_rule(b));
            unify(make_rule(b), rhs_ty, expr->rhs ? expr->rhs->span : expr->span);
            return make_rule(prune(a));
          }
          case ast::InfixOpKind::Alt: {
            TyPtr res = is_rule_con(exp) ? exp : make_rule(fresh_var());
            TyPtr lt = infer_expr(expr->lhs, res);
            unify(res, lt, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr rt = infer_expr(expr->rhs, res);
            unify(res, rt, expr->rhs ? expr->rhs->span : expr->span);
            return prune(res);
          }
          case ast::InfixOpKind::Label: {
            TyPtr res = is_rule_con(exp) ? exp : make_rule(fresh_var());
            TyPtr lt = infer_expr(expr->lhs, res);
            unify(res, lt, expr->lhs ? expr->lhs->span : expr->span);
            if (expr->rhs && expr->rhs->kind != ast::RuleExpr::Kind::Lit) {
              TyPtr rt = infer_expr(expr->rhs, make_con("Text"));
              unify(make_con("Text"), rt, expr->rhs->span);
            }
            return prune(res);
          }
          case ast::InfixOpKind::Backtick: {
            auto it = env_.find(expr->text);
            if (it == env_.end()) {
              return fresh_var();
            }
            TyPtr fn_ty = instantiate(it->second);
            TyPtr a = fresh_var();
            TyPtr b = fresh_var();
            TyPtr r = expected ? expected : fresh_var();
            unify(fn_ty, make_arrow(a, make_arrow(b, r)), expr->span);
            TyPtr lt = infer_expr(expr->lhs, prune(a));
            unify(prune(a), lt, expr->lhs ? expr->lhs->span : expr->span);
            TyPtr rt = infer_expr(expr->rhs, prune(b));
            unify(prune(b), rt, expr->rhs ? expr->rhs->span : expr->span);
            return prune(r);
          }
        }
        return fresh_var();
      }

      case ast::RuleExpr::Kind::Alt: {
        TyPtr res = is_rule_con(exp) ? exp : make_rule(fresh_var());
        for (const auto& item : expr->items) {
          TyPtr it_ty = infer_expr(item, res);
          unify(res, it_ty, item ? item->span : expr->span);
        }
        return prune(res);
      }

      case ast::RuleExpr::Kind::List: {
        TyPtr elem_exp = fresh_var();
        if (exp && exp->kind == Ty::Kind::Con && exp->name == "List" && exp->args.size() == 1) {
          elem_exp = exp->args[0];
        }
        for (const auto& item : expr->items) {
          TyPtr it_ty = infer_expr(item, elem_exp);
          unify(elem_exp, it_ty, item ? item->span : expr->span);
        }
        return make_list(prune(elem_exp));
      }

      case ast::RuleExpr::Kind::Tuple: {
        if (expr->items.empty()) {
          return make_unit();
        }
        std::vector<TyPtr> elem_types;
        for (std::size_t i = 0; i < expr->items.size(); ++i) {
          TyPtr elem_exp = nullptr;
          if (exp && exp->kind == Ty::Kind::Con && exp->name == "Tuple" &&
              i < exp->args.size()) {
            elem_exp = exp->args[i];
          }
          elem_types.push_back(infer_expr(expr->items[i], elem_exp));
        }
        return make_con("Tuple", std::move(elem_types));
      }

      case ast::RuleExpr::Kind::Do: {
        auto saved_env = env_;
        TyPtr result_val = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
        TyPtr last_stmt_ty = make_rule(result_val);

        for (std::size_t i = 0; i < expr->do_stmts.size(); ++i) {
          const auto& stmt = expr->do_stmts[i];
          const bool is_last = (i + 1 == expr->do_stmts.size());
          switch (stmt.kind) {
            case ast::DoStmt::Kind::Bind: {
              TyPtr val_ty = fresh_var();
              TyPtr step_ty = infer_expr(stmt.expr, make_rule(val_ty));
              unify(make_rule(val_ty), step_ty, stmt.span);
              if (!stmt.binder.empty()) {
                env_[stmt.binder] = TypeScheme{{}, prune(val_ty)};
              }
              last_stmt_ty = prune(step_ty);
              break;
            }
            case ast::DoStmt::Kind::Expr: {
              TyPtr step_exp = is_last ? make_rule(result_val) : make_rule(fresh_var());
              TyPtr step_ty = infer_expr(stmt.expr, step_exp);
              unify(step_exp, step_ty, stmt.span);
              last_stmt_ty = prune(step_ty);
              break;
            }
            case ast::DoStmt::Kind::Pure: {
              TyPtr pure_exp = is_last ? result_val : fresh_var();
              TyPtr pure_ty = infer_expr(stmt.expr, pure_exp);
              unify(pure_exp, pure_ty, stmt.span);
              last_stmt_ty = make_rule(prune(pure_ty));
              break;
            }
          }
        }

        env_ = std::move(saved_env);
        return prune(last_stmt_ty);
      }

      case ast::RuleExpr::Kind::Operators: {
        TyPtr base_val = is_rule_con(exp) ? prune(exp->args[0]) : fresh_var();
        TyPtr base_rule = make_rule(base_val);
        TyPtr actual_base = infer_expr(expr->lhs, base_rule);
        unify(base_rule, actual_base, expr->lhs ? expr->lhs->span : expr->span);
        TyPtr val_t = prune(base_val);

        for (const auto& lvl : expr->op_levels) {
          for (const auto& spec : lvl.specs) {
            std::vector<TyPtr> pat_val_types;
            for (const auto& pat_elem : spec.pattern) {
              if (pat_elem && pat_elem->kind == ast::RuleExpr::Kind::Lit) {
                infer_expr(pat_elem, make_rule(make_con("Text")));
              } else {
                TyPtr pv = fresh_var();
                TyPtr pt = infer_expr(pat_elem, make_rule(pv));
                if (is_rule_con(pt)) {
                  pat_val_types.push_back(prune(pt->args[0]));
                } else {
                  pat_val_types.push_back(prune(pt));
                }
              }
            }

            if (spec.action) {
              TyPtr expected_act = val_t;
              if (lvl.assoc == ast::AssocKind::InfixL ||
                  lvl.assoc == ast::AssocKind::InfixR ||
                  lvl.assoc == ast::AssocKind::Infix) {
                expected_act = make_arrow(val_t, make_arrow(val_t, val_t));
              } else if (lvl.assoc == ast::AssocKind::Prefix ||
                         lvl.assoc == ast::AssocKind::Postfix) {
                expected_act = make_arrow(val_t, val_t);
              } else if (lvl.assoc == ast::AssocKind::Mixfix) {
                expected_act = val_t;
                for (auto it = pat_val_types.rbegin(); it != pat_val_types.rend(); ++it) {
                  expected_act = make_arrow(*it, expected_act);
                }
              }
              TyPtr act_ty = infer_expr(spec.action, expected_act);
              unify(expected_act, act_ty, spec.action->span);
            }
          }
        }

        return make_rule(prune(val_t));
      }
    }

    return fresh_var();
  }

  bool contains_context_do(const ast::RuleExprPtr& expr) const {
    if (!expr) {
      return false;
    }
    if (expr->kind == ast::RuleExpr::Kind::Do) {
      if (expr->is_context) {
        return true;
      }
      for (const auto& st : expr->do_stmts) {
        if (contains_context_do(st.expr)) {
          return true;
        }
      }
      return false;
    }
    if (contains_context_do(expr->lhs) || contains_context_do(expr->rhs)) {
      return true;
    }
    for (const auto& item : expr->items) {
      if (contains_context_do(item)) {
        return true;
      }
    }
    for (const auto& lvl : expr->op_levels) {
      for (const auto& spec : lvl.specs) {
        for (const auto& pat : spec.pattern) {
          if (contains_context_do(pat)) {
            return true;
          }
        }
        if (contains_context_do(spec.action)) {
          return true;
        }
      }
    }
    return false;
  }

  void scan_bound_var_uses(const ast::RuleExprPtr& expr,
                           const std::unordered_map<std::string, Span>& bound_vars) {
    if (!expr) {
      return;
    }
    switch (expr->kind) {
      case ast::RuleExpr::Kind::Lit:
      case ast::RuleExpr::Kind::IntLit:
      case ast::RuleExpr::Kind::FloatLit:
        return;

      case ast::RuleExpr::Kind::Var: {
        if (!expr->is_upper) {
          auto it = bound_vars.find(expr->text);
          if (it != bound_vars.end()) {
            Diagnostic d;
            d.code = "E0310";
            d.severity = Severity::Error;
            d.message = "bound variable `" + expr->text +
                        "` used inside a parser combinator in a non-`context` `do` block";
            d.span = expr->span;
            d.label = "context-sensitive use requires `context do`";
            d.related.push_back(RelatedSpan{
                .span = it->second,
                .label = "`" + expr->text + "` was bound here",
            });
            d.notes.push_back(
                "regular `do` blocks desugar to static ApplicativeDo and can only reference bound variables in the trailing `pure` action");
            d.helps.push_back("replace `do` with `context do` to enable monadic parsing");
            diags_.push(std::move(d));
          }
        }
        return;
      }

      case ast::RuleExpr::Kind::Paren:
        scan_bound_var_uses(expr->lhs, bound_vars);
        return;

      case ast::RuleExpr::Kind::App:
      case ast::RuleExpr::Kind::Pure:
        scan_bound_var_uses(expr->lhs, bound_vars);
        scan_bound_var_uses(expr->rhs, bound_vars);
        return;

      case ast::RuleExpr::Kind::InfixOp:
        scan_bound_var_uses(expr->lhs, bound_vars);
        if (expr->op_kind == ast::InfixOpKind::Backtick) {
          auto it = bound_vars.find(expr->text);
          if (it != bound_vars.end()) {
            Diagnostic d;
            d.code = "E0310";
            d.severity = Severity::Error;
            d.message = "bound variable `" + expr->text +
                        "` used inside a parser combinator in a non-`context` `do` block";
            d.span = expr->span;
            d.label = "context-sensitive use requires `context do`";
            d.related.push_back(RelatedSpan{
                .span = it->second,
                .label = "`" + expr->text + "` was bound here",
            });
            d.helps.push_back("replace `do` with `context do`");
            diags_.push(std::move(d));
          }
        }
        if (expr->op_kind != ast::InfixOpKind::Label) {
          scan_bound_var_uses(expr->rhs, bound_vars);
        }
        return;

      case ast::RuleExpr::Kind::Alt:
      case ast::RuleExpr::Kind::List:
      case ast::RuleExpr::Kind::Tuple:
        for (const auto& item : expr->items) {
          scan_bound_var_uses(item, bound_vars);
        }
        return;

      case ast::RuleExpr::Kind::Do: {
        auto inner_bounds = bound_vars;
        for (const auto& st : expr->do_stmts) {
          if (st.kind == ast::DoStmt::Kind::Bind && !st.binder.empty()) {
            inner_bounds.erase(st.binder);
          }
          scan_bound_var_uses(st.expr, inner_bounds);
        }
        return;
      }

      case ast::RuleExpr::Kind::Operators:
        scan_bound_var_uses(expr->lhs, bound_vars);
        for (const auto& lvl : expr->op_levels) {
          for (const auto& spec : lvl.specs) {
            for (const auto& pat : spec.pattern) {
              scan_bound_var_uses(pat, bound_vars);
            }
            scan_bound_var_uses(spec.action, bound_vars);
          }
        }
        return;
    }
  }

  void check_applicative_do(const ast::RuleExprPtr& expr) {
    if (!expr) {
      return;
    }
    switch (expr->kind) {
      case ast::RuleExpr::Kind::Do: {
        if (!expr->is_context) {
          std::unordered_map<std::string, Span> bound_vars;
          for (std::size_t i = 0; i < expr->do_stmts.size(); ++i) {
            const auto& stmt = expr->do_stmts[i];
            const bool is_last = (i + 1 == expr->do_stmts.size());
            if (stmt.kind != ast::DoStmt::Kind::Pure || !is_last) {
              scan_bound_var_uses(stmt.expr, bound_vars);
            }
            if (stmt.kind == ast::DoStmt::Kind::Bind && !stmt.binder.empty()) {
              bound_vars[stmt.binder] = stmt.span;
            }
          }
        }
        for (const auto& stmt : expr->do_stmts) {
          check_applicative_do(stmt.expr);
        }
        return;
      }
      case ast::RuleExpr::Kind::Paren:
        check_applicative_do(expr->lhs);
        return;
      case ast::RuleExpr::Kind::App:
      case ast::RuleExpr::Kind::Pure:
      case ast::RuleExpr::Kind::InfixOp:
        check_applicative_do(expr->lhs);
        check_applicative_do(expr->rhs);
        return;
      case ast::RuleExpr::Kind::Alt:
      case ast::RuleExpr::Kind::List:
      case ast::RuleExpr::Kind::Tuple:
        for (const auto& item : expr->items) {
          check_applicative_do(item);
        }
        return;
      case ast::RuleExpr::Kind::Operators:
        check_applicative_do(expr->lhs);
        for (const auto& lvl : expr->op_levels) {
          for (const auto& spec : lvl.specs) {
            for (const auto& pat : spec.pattern) {
              check_applicative_do(pat);
            }
            check_applicative_do(spec.action);
          }
        }
        return;
      case ast::RuleExpr::Kind::Lit:
      case ast::RuleExpr::Kind::IntLit:
      case ast::RuleExpr::Kind::FloatLit:
      case ast::RuleExpr::Kind::Var:
        return;
    }
  }
};

} // namespace parsley::typecheck::detail

export namespace parsley::typecheck {

inline TypedModule check_module(const ast::Module& mod,
                                const resolve::ResolvedModule& resolved,
                                DiagnosticBag& diags) {
  detail::TypeChecker checker(mod, resolved, diags);
  return checker.check();
}

} // namespace parsley::typecheck
