module;

#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module parsley.ir;

import parsley.diagnostic;
import parsley.ast;

export namespace parsley::ir {

using TokenId = std::string;
using RuleId = std::string;

enum class Polarity {
  Positive,
  Negative,
};

enum class ParseMode {
  Auto,
  LL1,
  LLk,
  LALR,
  PEG,
  GLL,
};

enum class LexActionKind {
  None,
  Push,
  Pop,
};

struct Type {
  enum class Kind {
    Named,
    Var,
    List,
    Tuple,
    App,
    Arrow,
    Unit,
  };

  Kind kind = Kind::Named;
  std::string name;
  std::vector<Type> args;
};

inline Type from_ast_type(const ast::TypeExpr& te) {
  Type t;
  switch (te.kind) {
    case ast::TypeExpr::Kind::Named:
      if (te.name == "Unit" || te.name == "()") {
        t.kind = Type::Kind::Unit;
        t.name = "Unit";
      } else {
        t.kind = Type::Kind::Named;
        t.name = te.name;
      }
      return t;
    case ast::TypeExpr::Kind::Var:
      t.kind = Type::Kind::Var;
      t.name = te.name;
      return t;
    case ast::TypeExpr::Kind::List:
      t.kind = Type::Kind::List;
      t.name = "List";
      if (te.left) {
        t.args.push_back(from_ast_type(*te.left));
      }
      return t;
    case ast::TypeExpr::Kind::Tuple:
      if (te.args.empty()) {
        t.kind = Type::Kind::Unit;
        t.name = "Unit";
      } else {
        t.kind = Type::Kind::Tuple;
        t.name = "Tuple";
        for (const auto& a : te.args) {
          if (a) {
            t.args.push_back(from_ast_type(*a));
          }
        }
      }
      return t;
    case ast::TypeExpr::Kind::App:
      t.kind = Type::Kind::App;
      t.name = (te.left && te.left->kind == ast::TypeExpr::Kind::Named) ? te.left->name : "App";
      for (const auto& a : te.args) {
        if (a) {
          t.args.push_back(from_ast_type(*a));
        }
      }
      return t;
    case ast::TypeExpr::Kind::Arrow:
      t.kind = Type::Kind::Arrow;
      t.name = "->";
      if (te.left) {
        t.args.push_back(from_ast_type(*te.left));
      }
      if (te.right) {
        t.args.push_back(from_ast_type(*te.right));
      }
      return t;
  }
  return t;
}

inline std::string type_to_string(const Type& t) {
  switch (t.kind) {
    case Type::Kind::Unit:
      return "()";
    case Type::Kind::Named:
    case Type::Kind::Var:
      return t.name;
    case Type::Kind::List:
      return "[" + (t.args.empty() ? std::string("?") : type_to_string(t.args[0])) + "]";
    case Type::Kind::Tuple: {
      if (t.args.empty()) {
        return "()";
      }
      std::string s = "(";
      for (std::size_t i = 0; i < t.args.size(); ++i) {
        if (i > 0) {
          s += ", ";
        }
        s += type_to_string(t.args[i]);
      }
      s += ")";
      return s;
    }
    case Type::Kind::App: {
      std::string s = t.name;
      for (const auto& a : t.args) {
        std::string sub = type_to_string(a);
        if (a.kind == Type::Kind::App || a.kind == Type::Kind::Arrow) {
          sub = "(" + sub + ")";
        }
        s += " " + sub;
      }
      return s;
    }
    case Type::Kind::Arrow: {
      if (t.args.size() < 2) {
        return "->";
      }
      std::string l = type_to_string(t.args[0]);
      if (t.args[0].kind == Type::Kind::Arrow) {
        l = "(" + l + ")";
      }
      return l + " -> " + type_to_string(t.args[1]);
    }
  }
  return "Unit";
}

struct TokenDef {
  TokenId id;
  std::string pattern;
  bool is_regex = true;
  bool is_skip = false;
  bool is_keyword = false;
  bool is_anonymous = false;
  int priority = 0;
  int decl_order = 0;
  std::string mode = "default";
  Type type{Type::Kind::Named, "Text", {}};
  LexActionKind action = LexActionKind::None;
  std::string target_mode;
  Span span;
};

struct LexMode {
  std::string name;
  std::vector<TokenDef> tokens;
  Span span;
};

struct FieldDecl {
  std::string name;
  Type type;
  Span span;
};

struct CtorDecl {
  std::string name;
  std::vector<Type> positional_fields;
  std::vector<FieldDecl> record_fields;
  bool is_record = false;
  Span span;
};

struct DataDecl {
  std::string name;
  std::vector<std::string> type_params;
  std::vector<CtorDecl> constructors;
  bool is_newtype = false;
  bool is_foreign = false;
  std::string foreign_target;
  Span span;
};

struct ActionExpr {
  enum class Kind {
    Ctor,
    Arg,
    List,
    Tuple,
    IntLit,
    FloatLit,
    StringLit,
    BoolLit,
    UnitLit,
    Identity,
  };

  Kind kind = Kind::Identity;
  std::string name;
  int arg_index = 0;
  double number_value = 0.0;
  bool bool_value = false;
  std::vector<ActionExpr> args;
};

struct Prod;
using ProdPtr = std::shared_ptr<Prod>;

struct PrecOp {
  std::vector<ProdPtr> pattern;
  ActionExpr action;
  Span span;
};

struct PrecLevel {
  ast::AssocKind assoc = ast::AssocKind::InfixL;
  int precedence = 0;
  std::vector<PrecOp> ops;
  Span span;
};

struct CtxStep {
  std::string binder;
  ProdPtr prod;
  Span span;
};

struct Prod {
  enum class Kind {
    Tok,
    Ref,
    Seq,
    Alt,
    Rep,
    Look,
    Act,
    PrecTable,
    Ctx,
    Label,
    Recover,
    Eps,
  };

  Kind kind = Kind::Eps;
  TokenId token_id;
  RuleId rule_id;
  std::vector<ProdPtr> items;
  int min_rep = 0;
  std::optional<int> max_rep;
  Polarity polarity = Polarity::Positive;
  ProdPtr inner;
  ProdPtr sync;
  ActionExpr action;
  std::vector<PrecLevel> levels;
  std::string ctx_var;
  std::vector<CtxStep> ctx_steps;
  std::string label;
  Span span;
};

inline ProdPtr make_tok(TokenId id, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Tok;
  p->token_id = std::move(id);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_ref(RuleId id, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Ref;
  p->rule_id = std::move(id);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_seq(std::vector<ProdPtr> items, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Seq;
  p->items = std::move(items);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_alt(std::vector<ProdPtr> items, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Alt;
  p->items = std::move(items);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_rep(int min_rep, std::optional<int> max_rep, ProdPtr inner, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Rep;
  p->min_rep = min_rep;
  p->max_rep = max_rep;
  p->inner = std::move(inner);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_look(Polarity pol, ProdPtr inner, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Look;
  p->polarity = pol;
  p->inner = std::move(inner);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_act(ActionExpr action, std::vector<ProdPtr> items, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Act;
  p->action = std::move(action);
  p->items = std::move(items);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_prec_table(ProdPtr base, std::vector<PrecLevel> levels, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::PrecTable;
  p->inner = std::move(base);
  p->levels = std::move(levels);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_ctx_var(std::string var_name, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Ctx;
  p->ctx_var = std::move(var_name);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_ctx_block(std::vector<CtxStep> steps, ActionExpr action, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Ctx;
  p->ctx_steps = std::move(steps);
  p->action = std::move(action);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_label(std::string label, ProdPtr inner, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Label;
  p->label = std::move(label);
  p->inner = std::move(inner);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_recover(ProdPtr inner, ProdPtr sync, Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Recover;
  p->inner = std::move(inner);
  p->sync = std::move(sync);
  p->span = std::move(sp);
  return p;
}

inline ProdPtr make_eps(Span sp = {}) {
  auto p = std::make_shared<Prod>();
  p->kind = Prod::Kind::Eps;
  p->span = std::move(sp);
  return p;
}

struct Rule {
  Type rType;
  ProdPtr rBody;
  Span rSpan;
  bool is_exported = false;
  bool is_generated = false;
};

struct ExampleCase {
  std::string name;
  std::string input;
  bool expect_success = true;
  std::optional<ast::AstPattern> expected_ast;
  int fail_line = 1;
  int fail_col = 1;
  std::string expecting;
  Span span;
};

struct GrammarPragmas {
  std::optional<std::string> start_rule;
  ParseMode mode = ParseMode::Auto;
  bool no_factor = false;
  std::vector<std::pair<std::string, std::string>> token_precedence;
};

struct Grammar {
  std::string module_name;
  std::vector<TokenDef> gTokens;
  std::vector<LexMode> gModes;
  std::map<RuleId, Rule> gRules;
  std::vector<RuleId> gStarts;
  std::vector<DataDecl> gTypes;
  std::vector<ExampleCase> gExamples;
  GrammarPragmas pragmas;
};

inline const TokenDef* find_token(const Grammar& g, std::string_view id) {
  for (const auto& t : g.gTokens) {
    if (t.id == id) {
      return &t;
    }
  }
  for (const auto& m : g.gModes) {
    for (const auto& t : m.tokens) {
      if (t.id == id) {
        return &t;
      }
    }
  }
  return nullptr;
}

} // namespace parsley::ir

namespace parsley::ir::detail {

inline std::string escape_str(std::string_view s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (c < 0x20) {
          out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
        break;
    }
  }
  return out.str();
}

inline std::string_view assoc_name(ast::AssocKind a) noexcept {
  switch (a) {
    case ast::AssocKind::InfixL:
      return "infixl";
    case ast::AssocKind::InfixR:
      return "infixr";
    case ast::AssocKind::Infix:
      return "infix";
    case ast::AssocKind::Prefix:
      return "prefix";
    case ast::AssocKind::Postfix:
      return "postfix";
    case ast::AssocKind::Mixfix:
      return "mixfix";
  }
  return "infixl";
}

inline std::string_view parse_mode_name(ParseMode m) noexcept {
  switch (m) {
    case ParseMode::Auto:
      return "Auto";
    case ParseMode::LL1:
      return "LL1";
    case ParseMode::LLk:
      return "LLk";
    case ParseMode::LALR:
      return "LALR";
    case ParseMode::PEG:
      return "PEG";
    case ParseMode::GLL:
      return "GLL";
  }
  return "Auto";
}

inline std::string_view lex_action_name(LexActionKind k) noexcept {
  switch (k) {
    case LexActionKind::None:
      return "None";
    case LexActionKind::Push:
      return "Push";
    case LexActionKind::Pop:
      return "Pop";
  }
  return "None";
}

inline void indent_json(std::ostringstream& out, int depth) {
  for (int i = 0; i < depth; ++i) {
    out << "  ";
  }
}

inline void write_action_json(std::ostringstream& out, const ActionExpr& act, int depth) {
  out << "{\n";
  indent_json(out, depth + 1);
  out << "\"kind\": \"";
  switch (act.kind) {
    case ActionExpr::Kind::Ctor:
      out << "Ctor";
      break;
    case ActionExpr::Kind::Arg:
      out << "Arg";
      break;
    case ActionExpr::Kind::List:
      out << "List";
      break;
    case ActionExpr::Kind::Tuple:
      out << "Tuple";
      break;
    case ActionExpr::Kind::IntLit:
      out << "IntLit";
      break;
    case ActionExpr::Kind::FloatLit:
      out << "FloatLit";
      break;
    case ActionExpr::Kind::StringLit:
      out << "StringLit";
      break;
    case ActionExpr::Kind::BoolLit:
      out << "BoolLit";
      break;
    case ActionExpr::Kind::UnitLit:
      out << "UnitLit";
      break;
    case ActionExpr::Kind::Identity:
      out << "Identity";
      break;
  }
  out << "\"";
  if (!act.name.empty()) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"name\": \"" << escape_str(act.name) << "\"";
  }
  if (act.kind == ActionExpr::Kind::Arg || act.kind == ActionExpr::Kind::Identity) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"arg_index\": " << act.arg_index;
  }
  if (act.kind == ActionExpr::Kind::IntLit || act.kind == ActionExpr::Kind::FloatLit) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"number\": " << act.number_value;
  }
  if (act.kind == ActionExpr::Kind::BoolLit) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"bool\": " << (act.bool_value ? "true" : "false");
  }
  if (!act.args.empty()) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"args\": [\n";
    for (std::size_t i = 0; i < act.args.size(); ++i) {
      indent_json(out, depth + 2);
      write_action_json(out, act.args[i], depth + 2);
      if (i + 1 < act.args.size()) {
        out << ",";
      }
      out << "\n";
    }
    indent_json(out, depth + 1);
    out << "]";
  }
  out << "\n";
  indent_json(out, depth);
  out << "}";
}

inline void write_prod_json(std::ostringstream& out, const ProdPtr& p, int depth) {
  if (!p) {
    out << "{\"kind\": \"Eps\"}";
    return;
  }
  out << "{\n";
  indent_json(out, depth + 1);
  out << "\"kind\": \"";
  switch (p->kind) {
    case Prod::Kind::Tok:
      out << "Tok\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"token\": \"" << escape_str(p->token_id) << "\"";
      break;
    case Prod::Kind::Ref:
      out << "Ref\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"rule\": \"" << escape_str(p->rule_id) << "\"";
      break;
    case Prod::Kind::Seq:
    case Prod::Kind::Alt:
      out << (p->kind == Prod::Kind::Seq ? "Seq\"" : "Alt\"");
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"items\": [\n";
      for (std::size_t i = 0; i < p->items.size(); ++i) {
        indent_json(out, depth + 2);
        write_prod_json(out, p->items[i], depth + 2);
        if (i + 1 < p->items.size()) {
          out << ",";
        }
        out << "\n";
      }
      indent_json(out, depth + 1);
      out << "]";
      break;
    case Prod::Kind::Rep:
      out << "Rep\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"min\": " << p->min_rep << ",\n";
      indent_json(out, depth + 1);
      out << "\"max\": " << (p->max_rep ? std::to_string(*p->max_rep) : "null") << ",\n";
      indent_json(out, depth + 1);
      out << "\"inner\": ";
      write_prod_json(out, p->inner, depth + 1);
      break;
    case Prod::Kind::Look:
      out << "Look\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"polarity\": \"" << (p->polarity == Polarity::Positive ? "Positive" : "Negative")
          << "\",\n";
      indent_json(out, depth + 1);
      out << "\"inner\": ";
      write_prod_json(out, p->inner, depth + 1);
      break;
    case Prod::Kind::Act:
      out << "Act\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"action\": ";
      write_action_json(out, p->action, depth + 1);
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"items\": [\n";
      for (std::size_t i = 0; i < p->items.size(); ++i) {
        indent_json(out, depth + 2);
        write_prod_json(out, p->items[i], depth + 2);
        if (i + 1 < p->items.size()) {
          out << ",";
        }
        out << "\n";
      }
      indent_json(out, depth + 1);
      out << "]";
      break;
    case Prod::Kind::PrecTable:
      out << "PrecTable\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"base\": ";
      write_prod_json(out, p->inner, depth + 1);
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"levels\": [\n";
      for (std::size_t i = 0; i < p->levels.size(); ++i) {
        const auto& lvl = p->levels[i];
        indent_json(out, depth + 2);
        out << "{\"assoc\": \"" << assoc_name(lvl.assoc) << "\", \"precedence\": "
            << lvl.precedence << ", \"ops\": [\n";
        for (std::size_t j = 0; j < lvl.ops.size(); ++j) {
          const auto& op = lvl.ops[j];
          indent_json(out, depth + 3);
          out << "{\"pattern\": [";
          for (std::size_t k = 0; k < op.pattern.size(); ++k) {
            if (k > 0) {
              out << ", ";
            }
            write_prod_json(out, op.pattern[k], depth + 3);
          }
          out << "], \"action\": ";
          write_action_json(out, op.action, depth + 3);
          out << "}";
          if (j + 1 < lvl.ops.size()) {
            out << ",";
          }
          out << "\n";
        }
        indent_json(out, depth + 2);
        out << "]}";
        if (i + 1 < p->levels.size()) {
          out << ",";
        }
        out << "\n";
      }
      indent_json(out, depth + 1);
      out << "]";
      break;
    case Prod::Kind::Ctx:
      out << "Ctx\"";
      if (!p->ctx_var.empty()) {
        out << ",\n";
        indent_json(out, depth + 1);
        out << "\"exactly_var\": \"" << escape_str(p->ctx_var) << "\"";
      } else {
        out << ",\n";
        indent_json(out, depth + 1);
        out << "\"steps\": [\n";
        for (std::size_t i = 0; i < p->ctx_steps.size(); ++i) {
          const auto& st = p->ctx_steps[i];
          indent_json(out, depth + 2);
          out << "{\"binder\": \"" << escape_str(st.binder) << "\", \"prod\": ";
          write_prod_json(out, st.prod, depth + 2);
          out << "}";
          if (i + 1 < p->ctx_steps.size()) {
            out << ",";
          }
          out << "\n";
        }
        indent_json(out, depth + 1);
        out << "],\n";
        indent_json(out, depth + 1);
        out << "\"action\": ";
        write_action_json(out, p->action, depth + 1);
      }
      break;
    case Prod::Kind::Label:
      out << "Label\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"label\": \"" << escape_str(p->label) << "\",\n";
      indent_json(out, depth + 1);
      out << "\"inner\": ";
      write_prod_json(out, p->inner, depth + 1);
      break;
    case Prod::Kind::Recover:
      out << "Recover\"";
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"inner\": ";
      write_prod_json(out, p->inner, depth + 1);
      out << ",\n";
      indent_json(out, depth + 1);
      out << "\"sync\": ";
      write_prod_json(out, p->sync, depth + 1);
      break;
    case Prod::Kind::Eps:
      out << "Eps\"";
      break;
  }
  out << "\n";
  indent_json(out, depth);
  out << "}";
}

inline void write_token_json(std::ostringstream& out, const TokenDef& t, int depth) {
  out << "{\n";
  indent_json(out, depth + 1);
  out << "\"id\": \"" << escape_str(t.id) << "\",\n";
  indent_json(out, depth + 1);
  out << "\"pattern\": \"" << escape_str(t.pattern) << "\",\n";
  indent_json(out, depth + 1);
  out << "\"is_regex\": " << (t.is_regex ? "true" : "false") << ",\n";
  indent_json(out, depth + 1);
  out << "\"is_skip\": " << (t.is_skip ? "true" : "false") << ",\n";
  indent_json(out, depth + 1);
  out << "\"is_keyword\": " << (t.is_keyword ? "true" : "false") << ",\n";
  indent_json(out, depth + 1);
  out << "\"is_anonymous\": " << (t.is_anonymous ? "true" : "false") << ",\n";
  indent_json(out, depth + 1);
  out << "\"priority\": " << t.priority << ",\n";
  indent_json(out, depth + 1);
  out << "\"mode\": \"" << escape_str(t.mode) << "\",\n";
  indent_json(out, depth + 1);
  out << "\"type\": \"" << escape_str(type_to_string(t.type)) << "\",\n";
  indent_json(out, depth + 1);
  out << "\"action\": \"" << lex_action_name(t.action) << "\"";
  if (!t.target_mode.empty()) {
    out << ",\n";
    indent_json(out, depth + 1);
    out << "\"target_mode\": \"" << escape_str(t.target_mode) << "\"";
  }
  out << "\n";
  indent_json(out, depth);
  out << "}";
}

inline void write_action_sexpr(std::ostringstream& out, const ActionExpr& act) {
  switch (act.kind) {
    case ActionExpr::Kind::Ctor:
      out << "(ctor \"" << escape_str(act.name) << "\"";
      for (const auto& a : act.args) {
        out << " ";
        write_action_sexpr(out, a);
      }
      out << ")";
      break;
    case ActionExpr::Kind::Arg:
      if (!act.name.empty()) {
        out << "(arg " << act.arg_index << " \"" << escape_str(act.name) << "\")";
      } else {
        out << "(arg " << act.arg_index << ")";
      }
      break;
    case ActionExpr::Kind::List:
      out << "(list";
      if (!act.name.empty()) {
        out << " \"" << escape_str(act.name) << "\"";
      }
      for (const auto& a : act.args) {
        out << " ";
        write_action_sexpr(out, a);
      }
      out << ")";
      break;
    case ActionExpr::Kind::Tuple:
      out << "(tuple";
      for (const auto& a : act.args) {
        out << " ";
        write_action_sexpr(out, a);
      }
      out << ")";
      break;
    case ActionExpr::Kind::IntLit:
      out << "(int " << static_cast<long long>(act.number_value) << ")";
      break;
    case ActionExpr::Kind::FloatLit:
      out << "(float " << act.number_value << ")";
      break;
    case ActionExpr::Kind::StringLit:
      out << "(str \"" << escape_str(act.name) << "\")";
      break;
    case ActionExpr::Kind::BoolLit:
      out << "(bool " << (act.bool_value ? "#t" : "#f") << ")";
      break;
    case ActionExpr::Kind::UnitLit:
      out << "(unit)";
      break;
    case ActionExpr::Kind::Identity:
      out << "(id " << act.arg_index << ")";
      break;
  }
}

inline void write_prod_sexpr(std::ostringstream& out, const ProdPtr& p) {
  if (!p) {
    out << "(eps)";
    return;
  }
  switch (p->kind) {
    case Prod::Kind::Tok:
      out << "(tok \"" << escape_str(p->token_id) << "\")";
      break;
    case Prod::Kind::Ref:
      out << "(ref \"" << escape_str(p->rule_id) << "\")";
      break;
    case Prod::Kind::Seq:
      out << "(seq";
      for (const auto& it : p->items) {
        out << " ";
        write_prod_sexpr(out, it);
      }
      out << ")";
      break;
    case Prod::Kind::Alt:
      out << "(alt";
      for (const auto& it : p->items) {
        out << " ";
        write_prod_sexpr(out, it);
      }
      out << ")";
      break;
    case Prod::Kind::Rep:
      out << "(rep " << p->min_rep << " ";
      if (p->max_rep) {
        out << *p->max_rep;
      } else {
        out << "inf";
      }
      out << " ";
      write_prod_sexpr(out, p->inner);
      out << ")";
      break;
    case Prod::Kind::Look:
      out << "(look " << (p->polarity == Polarity::Positive ? "+" : "-") << " ";
      write_prod_sexpr(out, p->inner);
      out << ")";
      break;
    case Prod::Kind::Act:
      out << "(act ";
      write_action_sexpr(out, p->action);
      for (const auto& it : p->items) {
        out << " ";
        write_prod_sexpr(out, it);
      }
      out << ")";
      break;
    case Prod::Kind::PrecTable:
      out << "(prec-table ";
      write_prod_sexpr(out, p->inner);
      for (const auto& lvl : p->levels) {
        out << " (" << assoc_name(lvl.assoc) << " " << lvl.precedence;
        for (const auto& op : lvl.ops) {
          out << " (op (";
          for (std::size_t i = 0; i < op.pattern.size(); ++i) {
            if (i > 0) {
              out << " ";
            }
            write_prod_sexpr(out, op.pattern[i]);
          }
          out << ") ";
          write_action_sexpr(out, op.action);
          out << ")";
        }
        out << ")";
      }
      out << ")";
      break;
    case Prod::Kind::Ctx:
      if (!p->ctx_var.empty()) {
        out << "(ctx-exactly \"" << escape_str(p->ctx_var) << "\")";
      } else {
        out << "(ctx";
        for (const auto& st : p->ctx_steps) {
          out << " (step \"" << escape_str(st.binder) << "\" ";
          write_prod_sexpr(out, st.prod);
          out << ")";
        }
        out << " ";
        write_action_sexpr(out, p->action);
        out << ")";
      }
      break;
    case Prod::Kind::Label:
      out << "(label \"" << escape_str(p->label) << "\" ";
      write_prod_sexpr(out, p->inner);
      out << ")";
      break;
    case Prod::Kind::Recover:
      out << "(recover ";
      write_prod_sexpr(out, p->inner);
      out << " ";
      write_prod_sexpr(out, p->sync);
      out << ")";
      break;
    case Prod::Kind::Eps:
      out << "(eps)";
      break;
  }
}

} // namespace parsley::ir::detail

export namespace parsley::ir {

inline std::string to_json(const Grammar& g) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"module\": \"" << detail::escape_str(g.module_name) << "\",\n";
  out << "  \"mode\": \"" << detail::parse_mode_name(g.pragmas.mode) << "\",\n";
  out << "  \"no_factor\": " << (g.pragmas.no_factor ? "true" : "false") << ",\n";

  out << "  \"gStarts\": [";
  for (std::size_t i = 0; i < g.gStarts.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << "\"" << detail::escape_str(g.gStarts[i]) << "\"";
  }
  out << "],\n";

  out << "  \"gTokens\": [\n";
  for (std::size_t i = 0; i < g.gTokens.size(); ++i) {
    detail::indent_json(out, 2);
    detail::write_token_json(out, g.gTokens[i], 2);
    if (i + 1 < g.gTokens.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"gModes\": [\n";
  for (std::size_t i = 0; i < g.gModes.size(); ++i) {
    const auto& m = g.gModes[i];
    out << "    {\n";
    out << "      \"name\": \"" << detail::escape_str(m.name) << "\",\n";
    out << "      \"tokens\": [\n";
    for (std::size_t j = 0; j < m.tokens.size(); ++j) {
      detail::indent_json(out, 4);
      detail::write_token_json(out, m.tokens[j], 4);
      if (j + 1 < m.tokens.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "      ]\n";
    out << "    }";
    if (i + 1 < g.gModes.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"gTypes\": [\n";
  for (std::size_t i = 0; i < g.gTypes.size(); ++i) {
    const auto& dd = g.gTypes[i];
    out << "    {\n";
    out << "      \"name\": \"" << detail::escape_str(dd.name) << "\",\n";
    out << "      \"is_newtype\": " << (dd.is_newtype ? "true" : "false") << ",\n";
    out << "      \"is_foreign\": " << (dd.is_foreign ? "true" : "false") << ",\n";
    out << "      \"constructors\": [\n";
    for (std::size_t j = 0; j < dd.constructors.size(); ++j) {
      const auto& c = dd.constructors[j];
      out << "        {\"name\": \"" << detail::escape_str(c.name) << "\", \"fields\": [";
      if (c.is_record) {
        for (std::size_t k = 0; k < c.record_fields.size(); ++k) {
          if (k > 0) {
            out << ", ";
          }
          out << "\"" << detail::escape_str(c.record_fields[k].name) << ": "
              << detail::escape_str(type_to_string(c.record_fields[k].type)) << "\"";
        }
      } else {
        for (std::size_t k = 0; k < c.positional_fields.size(); ++k) {
          if (k > 0) {
            out << ", ";
          }
          out << "\"" << detail::escape_str(type_to_string(c.positional_fields[k])) << "\"";
        }
      }
      out << "]}";
      if (j + 1 < dd.constructors.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "      ]\n";
    out << "    }";
    if (i + 1 < g.gTypes.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"gRules\": {\n";
  std::size_t r_idx = 0;
  for (const auto& [rid, rule] : g.gRules) {
    out << "    \"" << detail::escape_str(rid) << "\": {\n";
    out << "      \"type\": \"" << detail::escape_str(type_to_string(rule.rType)) << "\",\n";
    out << "      \"exported\": " << (rule.is_exported ? "true" : "false") << ",\n";
    out << "      \"body\": ";
    detail::write_prod_json(out, rule.rBody, 3);
    out << "\n    }";
    if (++r_idx < g.gRules.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  },\n";

  out << "  \"gExamples\": [\n";
  for (std::size_t i = 0; i < g.gExamples.size(); ++i) {
    const auto& ex = g.gExamples[i];
    out << "    {\"name\": \"" << detail::escape_str(ex.name) << "\", \"input\": \""
        << detail::escape_str(ex.input) << "\", \"expect_success\": "
        << (ex.expect_success ? "true" : "false") << "}";
    if (i + 1 < g.gExamples.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";

  out << "}\n";
  return out.str();
}

inline std::string to_sexpr(const Grammar& g) {
  std::ostringstream out;
  out << "(grammar\n";
  out << "  (module \"" << detail::escape_str(g.module_name) << "\")\n";
  out << "  (mode " << detail::parse_mode_name(g.pragmas.mode) << ")\n";

  out << "  (starts";
  for (const auto& s : g.gStarts) {
    out << " \"" << detail::escape_str(s) << "\"";
  }
  out << ")\n";

  out << "  (tokens\n";
  for (const auto& t : g.gTokens) {
    out << "    (token \"" << detail::escape_str(t.id) << "\" \""
        << detail::escape_str(t.pattern) << "\" :prio " << t.priority
        << (t.is_skip ? " :skip" : "") << (t.is_regex ? " :regex" : " :lit") << ")\n";
  }
  out << "  )\n";

  if (!g.gModes.empty()) {
    out << "  (modes\n";
    for (const auto& m : g.gModes) {
      out << "    (mode \"" << detail::escape_str(m.name) << "\"";
      for (const auto& t : m.tokens) {
        out << " (token \"" << detail::escape_str(t.id) << "\" \""
            << detail::escape_str(t.pattern) << "\")";
      }
      out << ")\n";
    }
    out << "  )\n";
  }

  out << "  (types\n";
  for (const auto& dd : g.gTypes) {
    out << "    (data \"" << detail::escape_str(dd.name) << "\"";
    for (const auto& c : dd.constructors) {
      out << " (ctor \"" << detail::escape_str(c.name) << "\"";
      if (c.is_record) {
        for (const auto& f : c.record_fields) {
          out << " (" << f.name << " " << type_to_string(f.type) << ")";
        }
      } else {
        for (const auto& f : c.positional_fields) {
          out << " " << type_to_string(f);
        }
      }
      out << ")";
    }
    out << ")\n";
  }
  out << "  )\n";

  out << "  (rules\n";
  for (const auto& [rid, rule] : g.gRules) {
    out << "    (rule \"" << detail::escape_str(rid) << "\" \""
        << detail::escape_str(type_to_string(rule.rType)) << "\" ";
    detail::write_prod_sexpr(out, rule.rBody);
    out << ")\n";
  }
  out << "  )\n";

  out << ")\n";
  return out.str();
}

} // namespace parsley::ir
