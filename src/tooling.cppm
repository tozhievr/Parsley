module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.tooling;

import parsley.diagnostic;
import parsley.ast;
import parsley.ir;
import parsley.analysis;
import parsley.stdlib;
import parsley.lexer;
import parsley.parser;
import parsley.resolve;
import parsley.typecheck;
import parsley.desugar;

namespace parsley::tooling::detail {

// ---------------------------------------------------------------------------
// Ormolu-like Formatter Helpers
// ---------------------------------------------------------------------------

inline std::string escape_parsley_str(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('"');
  return out;
}

inline std::string format_type_expr(const ast::TypeExpr& te, bool wrap_arrow = false) {
  switch (te.kind) {
    case ast::TypeExpr::Kind::Named:
    case ast::TypeExpr::Kind::Var:
      return te.name.empty() ? "()" : te.name;
    case ast::TypeExpr::Kind::List:
      return "[" + (te.left ? format_type_expr(*te.left, false) : std::string("()")) + "]";
    case ast::TypeExpr::Kind::Tuple: {
      if (te.args.empty()) {
        return "()";
      }
      std::string out = "(";
      for (std::size_t i = 0; i < te.args.size(); ++i) {
        if (i > 0) out += ", ";
        if (te.args[i]) out += format_type_expr(*te.args[i], false);
      }
      out.push_back(')');
      return out;
    }
    case ast::TypeExpr::Kind::App: {
      std::string out = te.left ? format_type_expr(*te.left, false) : te.name;
      for (const auto& a : te.args) {
        if (!a) continue;
        std::string sub = format_type_expr(*a, true);
        if (a->kind == ast::TypeExpr::Kind::App || a->kind == ast::TypeExpr::Kind::Arrow) {
          sub = "(" + sub + ")";
        }
        out += " " + sub;
      }
      return out;
    }
    case ast::TypeExpr::Kind::Arrow: {
      std::string l = te.left ? format_type_expr(*te.left, true) : "()";
      if (te.left && te.left->kind == ast::TypeExpr::Kind::Arrow) {
        l = "(" + l + ")";
      }
      std::string r = te.right ? format_type_expr(*te.right, false) : "()";
      std::string s = l + " -> " + r;
      return wrap_arrow ? ("(" + s + ")") : s;
    }
  }
  return "()";
}

inline std::string format_ast_pattern(const ast::AstPattern& pat, bool wrap_parens = false) {
  switch (pat.kind) {
    case ast::AstPattern::Kind::Wildcard:
      return "_";
    case ast::AstPattern::Kind::Number: {
      std::ostringstream os;
      os << pat.number_value;
      return os.str();
    }
    case ast::AstPattern::Kind::String:
      return escape_parsley_str(pat.string_value);
    case ast::AstPattern::Kind::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < pat.args.size(); ++i) {
        if (i > 0) out += ", ";
        out += format_ast_pattern(pat.args[i], false);
      }
      out.push_back(']');
      return out;
    }
    case ast::AstPattern::Kind::Ident:
      return pat.name;
    case ast::AstPattern::Kind::Ctor: {
      if (pat.args.empty()) {
        return pat.name;
      }
      std::string out;
      if (wrap_parens) out.push_back('(');
      out += pat.name;
      for (const auto& a : pat.args) {
        out.push_back(' ');
        out += format_ast_pattern(a, true);
      }
      if (wrap_parens) out.push_back(')');
      return out;
    }
  }
  return "_";
}

inline std::string_view assoc_keyword(ast::AssocKind a) {
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

inline std::string_view infix_op_str(ast::InfixOpKind k, const std::string& text) {
  switch (k) {
    case ast::InfixOpKind::Fmap:
      return "<$>";
    case ast::InfixOpKind::Ap:
      return "<*>";
    case ast::InfixOpKind::SeqR:
      return "*>";
    case ast::InfixOpKind::SeqL:
      return "<*";
    case ast::InfixOpKind::Alt:
      return "<|>";
    case ast::InfixOpKind::Label:
      return "<?>";
    case ast::InfixOpKind::Backtick:
      return text;
  }
  return "<*>";
}

inline std::string format_rule_expr(const ast::RuleExprPtr& expr, int indent = 0);

inline std::string format_rule_expr_inline(const ast::RuleExprPtr& expr) {
  if (!expr) {
    return "()";
  }
  switch (expr->kind) {
    case ast::RuleExpr::Kind::Lit:
      return escape_parsley_str(expr->text);
    case ast::RuleExpr::Kind::IntLit:
    case ast::RuleExpr::Kind::FloatLit:
    case ast::RuleExpr::Kind::Var:
      return expr->text;
    case ast::RuleExpr::Kind::Paren:
      return "(" + format_rule_expr_inline(expr->lhs) + ")";
    case ast::RuleExpr::Kind::Pure:
      return "pure " + format_rule_expr_inline(expr->lhs);
    case ast::RuleExpr::Kind::App: {
      std::string l = format_rule_expr_inline(expr->lhs);
      std::string r = format_rule_expr_inline(expr->rhs);
      if (expr->rhs && (expr->rhs->kind == ast::RuleExpr::Kind::App ||
                        expr->rhs->kind == ast::RuleExpr::Kind::InfixOp)) {
        r = "(" + r + ")";
      }
      return l + " " + r;
    }
    case ast::RuleExpr::Kind::InfixOp: {
      std::string l = format_rule_expr_inline(expr->lhs);
      std::string r = format_rule_expr_inline(expr->rhs);
      if (expr->op_kind == ast::InfixOpKind::Backtick) {
        return l + " `" + expr->text + "` " + r;
      }
      return l + " " + std::string(infix_op_str(expr->op_kind, expr->text)) + " " + r;
    }
    case ast::RuleExpr::Kind::Alt: {
      std::string out;
      for (std::size_t i = 0; i < expr->items.size(); ++i) {
        if (i > 0) out += " <|> ";
        out += format_rule_expr_inline(expr->items[i]);
      }
      return out;
    }
    case ast::RuleExpr::Kind::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < expr->items.size(); ++i) {
        if (i > 0) out += ", ";
        out += format_rule_expr_inline(expr->items[i]);
      }
      out.push_back(']');
      return out;
    }
    case ast::RuleExpr::Kind::Tuple: {
      std::string out = "(";
      for (std::size_t i = 0; i < expr->items.size(); ++i) {
        if (i > 0) out += ", ";
        out += format_rule_expr_inline(expr->items[i]);
      }
      out.push_back(')');
      return out;
    }
    default:
      return format_rule_expr(expr, 0);
  }
}

inline std::string format_rule_expr(const ast::RuleExprPtr& expr, int indent) {
  if (!expr) {
    return "()";
  }
  const std::string ind(static_cast<std::size_t>(indent), ' ');
  switch (expr->kind) {
    case ast::RuleExpr::Kind::Do: {
      std::ostringstream out;
      out << (expr->is_context ? "context do" : "do");
      std::size_t max_binder = 0;
      for (const auto& st : expr->do_stmts) {
        if (st.kind == ast::DoStmt::Kind::Bind) {
          max_binder = std::max(max_binder, st.binder.size());
        }
      }
      for (const auto& st : expr->do_stmts) {
        out << "\n" << ind << "  ";
        if (st.kind == ast::DoStmt::Kind::Bind) {
          out << st.binder << std::string(max_binder - st.binder.size(), ' ') << " <- "
              << format_rule_expr(st.expr, indent + 2);
        } else if (st.kind == ast::DoStmt::Kind::Pure) {
          out << "pure " << format_rule_expr(st.expr, indent + 2);
        } else {
          out << format_rule_expr(st.expr, indent + 2);
        }
      }
      return out.str();
    }
    case ast::RuleExpr::Kind::Operators: {
      std::ostringstream out;
      out << "operators " << format_rule_expr_inline(expr->lhs) << "\n" << ind << "  [ ";
      for (std::size_t li = 0; li < expr->op_levels.size(); ++li) {
        const auto& lvl = expr->op_levels[li];
        if (li > 0) {
          out << "\n" << ind << "  , ";
        }
        out << assoc_keyword(lvl.assoc) << " " << lvl.precedence << " [";
        for (std::size_t si = 0; si < lvl.specs.size(); ++si) {
          if (si > 0) out << ", ";
          const auto& sp = lvl.specs[si];
          for (std::size_t pi = 0; pi < sp.pattern.size(); ++pi) {
            if (pi > 0) out << " ";
            out << format_rule_expr_inline(sp.pattern[pi]);
          }
          if (sp.action) {
            out << " => " << format_rule_expr_inline(sp.action);
          }
        }
        out << "]";
      }
      out << "\n" << ind << "  ]";
      return out.str();
    }
    default:
      return format_rule_expr_inline(expr);
  }
}

// ---------------------------------------------------------------------------
// Minimal self-contained JSON parser/builder for LSP JSON-RPC 2.0
// ---------------------------------------------------------------------------

struct Json {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool bool_val = false;
  double num_val = 0.0;
  bool is_int = false;
  std::int64_t int_val = 0;
  std::string str_val;
  std::vector<Json> arr_val;
  std::vector<std::pair<std::string, Json>> obj_val;

  static Json make_null() { return Json{}; }
  static Json make_bool(bool b) {
    Json j;
    j.kind = Kind::Bool;
    j.bool_val = b;
    return j;
  }
  static Json make_int(std::int64_t v) {
    Json j;
    j.kind = Kind::Number;
    j.is_int = true;
    j.int_val = v;
    j.num_val = static_cast<double>(v);
    return j;
  }
  static Json make_string(std::string s) {
    Json j;
    j.kind = Kind::String;
    j.str_val = std::move(s);
    return j;
  }
  static Json make_array(std::vector<Json> a = {}) {
    Json j;
    j.kind = Kind::Array;
    j.arr_val = std::move(a);
    return j;
  }
  static Json make_object(std::vector<std::pair<std::string, Json>> o = {}) {
    Json j;
    j.kind = Kind::Object;
    j.obj_val = std::move(o);
    return j;
  }

  const Json* get(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& [k, v] : obj_val) {
      if (k == key) return &v;
    }
    return nullptr;
  }

  std::string as_string(std::string_view def = "") const {
    return kind == Kind::String ? str_val : std::string(def);
  }

  std::int64_t as_int(std::int64_t def = 0) const {
    if (kind != Kind::Number) return def;
    return is_int ? int_val : static_cast<std::int64_t>(num_val);
  }

  void set(std::string key, Json val) {
    if (kind != Kind::Object) kind = Kind::Object;
    for (auto& [k, v] : obj_val) {
      if (k == key) {
        v = std::move(val);
        return;
      }
    }
    obj_val.emplace_back(std::move(key), std::move(val));
  }
};

inline std::string json_escape(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

inline std::string json_stringify(const Json& j) {
  switch (j.kind) {
    case Json::Kind::Null:
      return "null";
    case Json::Kind::Bool:
      return j.bool_val ? "true" : "false";
    case Json::Kind::Number:
      if (j.is_int) {
        return std::to_string(j.int_val);
      } else {
        std::ostringstream os;
        os << j.num_val;
        return os.str();
      }
    case Json::Kind::String:
      return json_escape(j.str_val);
    case Json::Kind::Array: {
      std::string out = "[";
      for (std::size_t i = 0; i < j.arr_val.size(); ++i) {
        if (i > 0) out.push_back(',');
        out += json_stringify(j.arr_val[i]);
      }
      out.push_back(']');
      return out;
    }
    case Json::Kind::Object: {
      std::string out = "{";
      for (std::size_t i = 0; i < j.obj_val.size(); ++i) {
        if (i > 0) out.push_back(',');
        out += json_escape(j.obj_val[i].first);
        out.push_back(':');
        out += json_stringify(j.obj_val[i].second);
      }
      out.push_back('}');
      return out;
    }
  }
  return "null";
}

class JsonParser {
public:
  explicit JsonParser(std::string_view src) : src_(src) {}

  std::optional<Json> parse() {
    skip_ws();
    auto v = parse_val();
    if (!v) return std::nullopt;
    skip_ws();
    return v;
  }

private:
  std::string_view src_;
  std::size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
      ++pos_;
    }
  }

  std::optional<Json> parse_val() {
    skip_ws();
    if (pos_ >= src_.size()) return std::nullopt;
    char c = src_[pos_];
    if (c == '{') return parse_obj();
    if (c == '[') return parse_arr();
    if (c == '"') {
      auto s = parse_str();
      if (!s) return std::nullopt;
      return Json::make_string(*s);
    }
    if (c == 't' && src_.substr(pos_, 4) == "true") {
      pos_ += 4;
      return Json::make_bool(true);
    }
    if (c == 'f' && src_.substr(pos_, 5) == "false") {
      pos_ += 5;
      return Json::make_bool(false);
    }
    if (c == 'n' && src_.substr(pos_, 4) == "null") {
      pos_ += 4;
      return Json::make_null();
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return parse_num();
    }
    return std::nullopt;
  }

  std::optional<std::string> parse_str() {
    if (pos_ >= src_.size() || src_[pos_] != '"') return std::nullopt;
    ++pos_;
    std::string out;
    while (pos_ < src_.size()) {
      char c = src_[pos_++];
      if (c == '"') return out;
      if (c == '\\' && pos_ < src_.size()) {
        char e = src_[pos_++];
        switch (e) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            if (pos_ + 4 <= src_.size()) {
              unsigned int cp = 0;
              for (int k = 0; k < 4; ++k) {
                char hc = src_[pos_++];
                cp <<= 4;
                if (hc >= '0' && hc <= '9') cp |= static_cast<unsigned int>(hc - '0');
                else if (hc >= 'a' && hc <= 'f') cp |= static_cast<unsigned int>(hc - 'a' + 10);
                else if (hc >= 'A' && hc <= 'F') cp |= static_cast<unsigned int>(hc - 'A' + 10);
              }
              if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
              } else {
                out.push_back('?');
              }
            }
            break;
          default:
            out.push_back(e);
            break;
        }
      } else {
        out.push_back(c);
      }
    }
    return std::nullopt;
  }

  std::optional<Json> parse_num() {
    std::size_t start = pos_;
    if (src_[pos_] == '-') ++pos_;
    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    bool is_float = false;
    if (pos_ < src_.size() && src_[pos_] == '.') {
      is_float = true;
      ++pos_;
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }
    std::string tmp(src_.substr(start, pos_ - start));
    if (!is_float) {
      return Json::make_int(std::stoll(tmp));
    }
    Json j;
    j.kind = Json::Kind::Number;
    j.is_int = false;
    j.num_val = std::stod(tmp);
    return j;
  }

  std::optional<Json> parse_arr() {
    ++pos_;
    Json arr = Json::make_array();
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == ']') {
      ++pos_;
      return arr;
    }
    while (pos_ < src_.size()) {
      auto v = parse_val();
      if (!v) return std::nullopt;
      arr.arr_val.push_back(std::move(*v));
      skip_ws();
      if (pos_ < src_.size() && src_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < src_.size() && src_[pos_] == ']') {
        ++pos_;
        return arr;
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

  std::optional<Json> parse_obj() {
    ++pos_;
    Json obj = Json::make_object();
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == '}') {
      ++pos_;
      return obj;
    }
    while (pos_ < src_.size()) {
      skip_ws();
      auto k = parse_str();
      if (!k) return std::nullopt;
      skip_ws();
      if (pos_ >= src_.size() || src_[pos_] != ':') return std::nullopt;
      ++pos_;
      auto v = parse_val();
      if (!v) return std::nullopt;
      obj.obj_val.emplace_back(std::move(*k), std::move(*v));
      skip_ws();
      if (pos_ < src_.size() && src_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < src_.size() && src_[pos_] == '}') {
        ++pos_;
        return obj;
      }
      return std::nullopt;
    }
    return std::nullopt;
  }
};

inline std::string ident_at_position(std::string_view source, int line0, int char0) {
  int cur_line = 0;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < source.size() && cur_line < line0; ++i) {
    if (source[i] == '\n') {
      ++cur_line;
      line_start = i + 1;
    }
  }
  if (cur_line != line0) return "";
  std::size_t line_end = source.find('\n', line_start);
  if (line_end == std::string_view::npos) line_end = source.size();
  std::size_t pos = line_start + static_cast<std::size_t>(std::max(0, char0));
  if (pos >= line_end && pos > line_start) pos = line_end - 1;
  if (pos >= line_end) return "";

  auto is_id_ch = [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '\'';
  };
  if (!is_id_ch(static_cast<unsigned char>(source[pos]))) {
    if (pos > line_start && is_id_ch(static_cast<unsigned char>(source[pos - 1]))) {
      --pos;
    } else {
      return "";
    }
  }
  std::size_t s = pos;
  while (s > line_start && is_id_ch(static_cast<unsigned char>(source[s - 1]))) --s;
  std::size_t e = pos;
  while (e < line_end && is_id_ch(static_cast<unsigned char>(source[e]))) ++e;
  return std::string(source.substr(s, e - s));
}

inline Json span_to_lsp_range(const Span& sp) {
  int sl = std::max(0, sp.start_line - 1);
  int sc = std::max(0, sp.start_col - 1);
  int el = std::max(sl, sp.end_line - 1);
  int ec = std::max(0, sp.end_col - 1);
  if (el == sl && ec <= sc) ec = sc + 1;
  Json start = Json::make_object({{"line", Json::make_int(sl)}, {"character", Json::make_int(sc)}});
  Json end = Json::make_object({{"line", Json::make_int(el)}, {"character", Json::make_int(ec)}});
  return Json::make_object({{"start", start}, {"end", end}});
}

} // namespace parsley::tooling::detail

export namespace parsley::tooling {

// ---------------------------------------------------------------------------
// 1. Canonical Ormolu-like Formatter
// ---------------------------------------------------------------------------

inline std::string format_module(const ast::Module& mod, std::string_view /*original_source*/ = "") {
  std::ostringstream out;

  // Gather comments sorted by line/col so we can interleave standalone comments deterministically
  std::vector<ast::Comment> comments = mod.comments;
  std::sort(comments.begin(), comments.end(), [](const ast::Comment& a, const ast::Comment& b) {
    if (a.span.start_line != b.span.start_line) return a.span.start_line < b.span.start_line;
    return a.span.start_col < b.span.start_col;
  });
  std::size_t next_comment = 0;

  auto emit_comments_before = [&](int line_limit) {
    if (line_limit <= 0) return;
    while (next_comment < comments.size() &&
           comments[next_comment].span.start_line > 0 &&
           comments[next_comment].span.start_line < line_limit) {
      out << comments[next_comment].text << "\n";
      ++next_comment;
    }
  };

  for (const auto& pr : mod.pragmas) {
    emit_comments_before(pr.span.start_line);
    out << "{-# " << pr.name;
    for (const auto& a : pr.args) {
      out << " " << a;
    }
    out << " #-}\n";
  }

  if (mod.has_module_header) {
    emit_comments_before(mod.span.start_line);
    out << "module " << mod.name;
    if (mod.has_export_list) {
      out << " (";
      for (std::size_t i = 0; i < mod.exports.size(); ++i) {
        if (i > 0) out << ", ";
        out << mod.exports[i].name;
        if (mod.exports[i].all_ctors) out << "(..)";
      }
      out << ")";
    }
    out << " where\n\n";
  }

  if (!mod.imports.empty()) {
    for (const auto& imp : mod.imports) {
      emit_comments_before(imp.span.start_line);
      out << "import ";
      if (imp.is_qualified) out << "qualified ";
      out << imp.module_name;
      if (imp.alias) out << " as " << *imp.alias;
      if (imp.is_hiding) out << " hiding";
      if (!imp.items.empty()) {
        out << " (";
        for (std::size_t i = 0; i < imp.items.size(); ++i) {
          if (i > 0) out << ", ";
          out << imp.items[i].name;
          if (imp.items[i].all_ctors) out << "(..)";
        }
        out << ")";
      }
      out << "\n";
    }
    out << "\n";
  }

  // Lexer declarations
  if (!mod.skips.empty() || !mod.tokens.empty() || !mod.keywords.empty() || !mod.modes.empty()) {
    std::size_t max_tok_name = 0;
    for (const auto& sk : mod.skips) max_tok_name = std::max(max_tok_name, sk.name.size());
    for (const auto& tk : mod.tokens) max_tok_name = std::max(max_tok_name, tk.name.size());

    for (const auto& sk : mod.skips) {
      emit_comments_before(sk.span.start_line);
      out << "skip  " << sk.name << std::string(max_tok_name - sk.name.size(), ' ') << " = "
          << (sk.is_regex ? ("/" + sk.pattern + "/") : detail::escape_parsley_str(sk.pattern))
          << "\n";
    }
    for (const auto& tk : mod.tokens) {
      emit_comments_before(tk.span.start_line);
      out << "token " << tk.name << std::string(max_tok_name - tk.name.size(), ' ') << " = "
          << (tk.is_regex ? ("/" + tk.pattern + "/") : detail::escape_parsley_str(tk.pattern));
      if (tk.type_ann) {
        out << " :: " << detail::format_type_expr(*tk.type_ann);
      }
      if (tk.action.kind == ast::LexActionKind::Push) {
        out << " -> push " << tk.action.target_mode;
      } else if (tk.action.kind == ast::LexActionKind::Pop) {
        out << " -> pop";
      }
      out << "\n";
    }
    for (const auto& kw : mod.keywords) {
      emit_comments_before(kw.span.start_line);
      out << "keywords = [";
      for (std::size_t i = 0; i < kw.words.size(); ++i) {
        if (i > 0) out << ", ";
        out << detail::escape_parsley_str(kw.words[i]);
      }
      out << "]\n";
    }
    for (const auto& md : mod.modes) {
      emit_comments_before(md.span.start_line);
      out << "\nmode " << md.name << " where\n";
      for (const auto& sk : md.skips) {
        out << "  skip  " << sk.name << " = "
            << (sk.is_regex ? ("/" + sk.pattern + "/") : detail::escape_parsley_str(sk.pattern))
            << "\n";
      }
      for (const auto& tk : md.tokens) {
        out << "  token " << tk.name << " = "
            << (tk.is_regex ? ("/" + tk.pattern + "/") : detail::escape_parsley_str(tk.pattern));
        if (tk.type_ann) {
          out << " :: " << detail::format_type_expr(*tk.type_ann);
        }
        if (tk.action.kind == ast::LexActionKind::Push) {
          out << " -> push " << tk.action.target_mode;
        } else if (tk.action.kind == ast::LexActionKind::Pop) {
          out << " -> pop";
        }
        out << "\n";
      }
    }
    out << "\n";
  }

  // Data declarations
  for (const auto& dd : mod.data_decls) {
    emit_comments_before(dd.span.start_line);
    if (dd.is_foreign) {
      out << "foreign data " << dd.name;
      for (const auto& tp : dd.type_params) out << " " << tp;
      out << " = " << detail::escape_parsley_str(dd.foreign_target) << "\n\n";
      continue;
    }
    out << (dd.is_newtype ? "newtype " : "data ") << dd.name;
    for (const auto& tp : dd.type_params) out << " " << tp;

    bool all_zero_arity = !dd.constructors.empty();
    for (const auto& c : dd.constructors) {
      if (!c.positional_fields.empty() || !c.record_fields.empty()) {
        all_zero_arity = false;
        break;
      }
    }

    auto format_ctor = [](const ast::CtorDecl& c) {
      std::string s = c.name;
      if (c.is_record) {
        s += " { ";
        for (std::size_t i = 0; i < c.record_fields.size(); ++i) {
          if (i > 0) s += ", ";
          s += c.record_fields[i].name + " :: " + detail::format_type_expr(c.record_fields[i].type);
        }
        s += " }";
      } else {
        for (const auto& pf : c.positional_fields) {
          std::string ft = detail::format_type_expr(pf, true);
          if (pf.kind == ast::TypeExpr::Kind::App) ft = "(" + ft + ")";
          s += " " + ft;
        }
      }
      return s;
    };

    if (all_zero_arity && dd.constructors.size() <= 6) {
      out << " = ";
      for (std::size_t ci = 0; ci < dd.constructors.size(); ++ci) {
        if (ci > 0) out << " | ";
        out << dd.constructors[ci].name;
      }
      out << "\n\n";
    } else if (!dd.constructors.empty()) {
      for (std::size_t ci = 0; ci < dd.constructors.size(); ++ci) {
        out << "\n  " << (ci == 0 ? "= " : "| ") << format_ctor(dd.constructors[ci]);
      }
      out << "\n\n";
    } else {
      out << "\n\n";
    }
  }

  // Rule signatures + definitions
  std::unordered_set<std::string> emitted_sigs;
  for (const auto& def : mod.rule_defs) {
    emit_comments_before(def.span.start_line);
    if (def.signature) {
      out << def.name << " :: " << detail::format_type_expr(*def.signature) << "\n";
      emitted_sigs.insert(def.name);
    } else {
      for (const auto& sig : mod.rule_sigs) {
        if (sig.name == def.name) {
          out << sig.name << " :: " << detail::format_type_expr(sig.type) << "\n";
          emitted_sigs.insert(sig.name);
          break;
        }
      }
    }

    out << def.name;
    for (const auto& p : def.params) {
      out << " " << p;
    }
    if (def.body && def.body->kind == ast::RuleExpr::Kind::Alt && def.body->items.size() > 1) {
      for (std::size_t i = 0; i < def.body->items.size(); ++i) {
        out << "\n  " << (i == 0 ? "=  " : "|  ")
            << detail::format_rule_expr(def.body->items[i], 2);
      }
      out << "\n\n";
    } else {
      out << " = " << detail::format_rule_expr(def.body, 0) << "\n\n";
    }
  }

  for (const auto& sig : mod.rule_sigs) {
    if (!emitted_sigs.contains(sig.name)) {
      emit_comments_before(sig.span.start_line);
      out << sig.name << " :: " << detail::format_type_expr(sig.type) << "\n";
    }
  }

  // Examples
  for (const auto& ex : mod.examples) {
    emit_comments_before(ex.span.start_line);
    out << "example " << detail::escape_parsley_str(ex.name) << " : "
        << detail::escape_parsley_str(ex.input);
    if (ex.kind == ast::ExampleDecl::Kind::ParsesTo) {
      out << " parses-to ";
      if (ex.expected_ast) {
        out << detail::format_ast_pattern(*ex.expected_ast);
      } else {
        out << "_";
      }
    } else {
      out << " fails-at (" << ex.fail_line << ", " << ex.fail_col << ")";
      if (!ex.expecting.empty()) {
        out << " expecting " << detail::escape_parsley_str(ex.expecting);
      }
    }
    out << "\n";
  }

  while (next_comment < comments.size()) {
    out << comments[next_comment].text << "\n";
    ++next_comment;
  }

  std::string result = out.str();
  while (result.size() >= 2 && result[result.size() - 1] == '\n' &&
         result[result.size() - 2] == '\n') {
    result.pop_back();
  }
  if (!result.empty() && result.back() != '\n') {
    result.push_back('\n');
  }
  return result;
}

// ---------------------------------------------------------------------------
// 2. Graphviz DOT & Railroad SVG Generators
// ---------------------------------------------------------------------------

inline std::string generate_dot(const ir::Grammar& g) {
  std::ostringstream out;
  std::string mod = g.module_name.empty() ? "ParsleyGrammar" : g.module_name;
  out << "digraph " << mod << " {\n";
  out << "  rankdir=LR;\n";
  out << "  node [fontname=\"Helvetica\", fontsize=11];\n";

  std::unordered_set<std::string> starts(g.gStarts.begin(), g.gStarts.end());
  for (const auto& [rid, rule] : g.gRules) {
    std::string shape = starts.contains(rid) ? "doubleoctagon" : "box";
    out << "  \"rule:" << rid << "\" [label=\"" << rid << " :: " << ir::type_to_string(rule.rType)
        << "\", shape=" << shape << "];\n";
  }

  std::set<std::pair<std::string, std::string>> edges;
  std::set<std::string> used_tokens;

  auto walk = [&](auto& self, const std::string& from, const ir::ProdPtr& p) -> void {
    if (!p) return;
    switch (p->kind) {
      case ir::Prod::Kind::Tok:
        used_tokens.insert(p->token_id);
        edges.emplace("rule:" + from, "tok:" + p->token_id);
        break;
      case ir::Prod::Kind::Ref:
        edges.emplace("rule:" + from, "rule:" + p->rule_id);
        break;
      case ir::Prod::Kind::Seq:
      case ir::Prod::Kind::Alt:
      case ir::Prod::Kind::Act:
        for (const auto& it : p->items) self(self, from, it);
        break;
      case ir::Prod::Kind::Rep:
      case ir::Prod::Kind::Look:
      case ir::Prod::Kind::Label:
        self(self, from, p->inner);
        break;
      case ir::Prod::Kind::Recover:
        self(self, from, p->inner);
        self(self, from, p->sync);
        break;
      case ir::Prod::Kind::PrecTable:
        self(self, from, p->inner);
        for (const auto& lvl : p->levels) {
          for (const auto& op : lvl.ops) {
            for (const auto& pat : op.pattern) self(self, from, pat);
          }
        }
        break;
      case ir::Prod::Kind::Ctx:
        for (const auto& st : p->ctx_steps) self(self, from, st.prod);
        break;
      case ir::Prod::Kind::Eps:
        break;
    }
  };

  for (const auto& [rid, rule] : g.gRules) {
    walk(walk, rid, rule.rBody);
  }

  for (const auto& tid : used_tokens) {
    std::string lbl = tid;
    if (const auto* td = ir::find_token(g, tid)) {
      if (!td->is_regex && (td->is_anonymous || td->is_keyword)) {
        lbl = "\\\"" + td->pattern + "\\\"";
      }
    }
    out << "  \"tok:" << tid << "\" [label=\"" << lbl << "\", shape=ellipse, style=filled, fillcolor=\"#eef6ff\"];\n";
  }

  for (const auto& [src, dst] : edges) {
    out << "  \"" << src << "\" -> \"" << dst << "\";\n";
  }

  out << "}\n";
  return out.str();
}

inline std::string generate_railroad_svg(const ir::Grammar& g) {
  auto xml_esc = [](std::string_view s) {
    std::string out;
    for (char c : s) {
      if (c == '&') out += "&amp;";
      else if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;";
      else if (c == '"') out += "&quot;";
      else out.push_back(c);
    }
    return out;
  };

  struct Box {
    int w = 0;
    int h = 0;
    std::string svg;
  };

  auto render_prod = [&](auto& self, const ir::ProdPtr& p, int x, int y) -> Box {
    if (!p || p->kind == ir::Prod::Kind::Eps) {
      std::ostringstream s;
      s << "<line x1=\"" << x << "\" y1=\"" << y << "\" x2=\"" << (x + 24) << "\" y2=\"" << y
        << "\" stroke=\"#334155\" stroke-width=\"2\"/>";
      return {24, 28, s.str()};
    }
    switch (p->kind) {
      case ir::Prod::Kind::Tok: {
        std::string lbl = p->token_id;
        if (const auto* td = ir::find_token(g, p->token_id)) {
          if (!td->is_regex && (td->is_anonymous || td->is_keyword)) {
            lbl = "\"" + td->pattern + "\"";
          }
        }
        int w = std::max(54, static_cast<int>(lbl.size()) * 8 + 20);
        std::ostringstream s;
        s << "<rect x=\"" << x << "\" y=\"" << (y - 14) << "\" width=\"" << w
          << "\" height=\"28\" rx=\"14\" fill=\"#e0f2fe\" stroke=\"#0284c7\" stroke-width=\"1.5\"/>"
          << "<text x=\"" << (x + w / 2) << "\" y=\"" << (y + 4)
          << "\" text-anchor=\"middle\" font-family=\"monospace\" font-size=\"12\" fill=\"#0f172a\">"
          << xml_esc(lbl) << "</text>";
        return {w, 32, s.str()};
      }
      case ir::Prod::Kind::Ref: {
        int w = std::max(60, static_cast<int>(p->rule_id.size()) * 8 + 20);
        std::ostringstream s;
        s << "<rect x=\"" << x << "\" y=\"" << (y - 14) << "\" width=\"" << w
          << "\" height=\"28\" rx=\"4\" fill=\"#f1f5f9\" stroke=\"#334155\" stroke-width=\"1.5\"/>"
          << "<text x=\"" << (x + w / 2) << "\" y=\"" << (y + 4)
          << "\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"12\" fill=\"#0f172a\">"
          << xml_esc(p->rule_id) << "</text>";
        return {w, 32, s.str()};
      }
      case ir::Prod::Kind::Seq: {
        int cur_x = x;
        int max_h = 32;
        std::ostringstream s;
        for (std::size_t i = 0; i < p->items.size(); ++i) {
          if (i > 0) {
            s << "<line x1=\"" << cur_x << "\" y1=\"" << y << "\" x2=\"" << (cur_x + 16)
              << "\" y2=\"" << y << "\" stroke=\"#334155\" stroke-width=\"2\"/>";
            cur_x += 16;
          }
          Box b = self(self, p->items[i], cur_x, y);
          s << b.svg;
          cur_x += b.w;
          max_h = std::max(max_h, b.h);
        }
        return {std::max(24, cur_x - x), max_h, s.str()};
      }
      case ir::Prod::Kind::Alt: {
        if (p->items.empty()) return {24, 32, ""};
        std::vector<Box> branches;
        int max_bw = 0;
        int total_h = 0;
        int cur_y = y;
        for (std::size_t i = 0; i < p->items.size(); ++i) {
          Box b = self(self, p->items[i], x + 24, cur_y);
          max_bw = std::max(max_bw, b.w);
          total_h += b.h + (i > 0 ? 10 : 0);
          cur_y += b.h + 10;
          branches.push_back(std::move(b));
        }
        int total_w = max_bw + 48;
        std::ostringstream s;
        int branch_y = y;
        for (std::size_t i = 0; i < branches.size(); ++i) {
          s << "<path d=\"M " << x << " " << y << " L " << (x + 12) << " " << branch_y << " L "
            << (x + 24) << " " << branch_y << "\" fill=\"none\" stroke=\"#334155\" stroke-width=\"1.5\"/>";
          s << branches[i].svg;
          s << "<path d=\"M " << (x + 24 + branches[i].w) << " " << branch_y << " L "
            << (x + 24 + max_bw) << " " << branch_y << " L " << (x + total_w) << " " << y
            << "\" fill=\"none\" stroke=\"#334155\" stroke-width=\"1.5\"/>";
          branch_y += branches[i].h + 10;
        }
        return {total_w, std::max(36, total_h), s.str()};
      }
      case ir::Prod::Kind::Rep: {
        Box inner = self(self, p->inner, x + 16, y);
        int w = inner.w + 32;
        std::ostringstream s;
        s << "<line x1=\"" << x << "\" y1=\"" << y << "\" x2=\"" << (x + 16) << "\" y2=\"" << y
          << "\" stroke=\"#334155\" stroke-width=\"2\"/>";
        s << inner.svg;
        s << "<line x1=\"" << (x + 16 + inner.w) << "\" y1=\"" << y << "\" x2=\"" << (x + w)
          << "\" y2=\"" << y << "\" stroke=\"#334155\" stroke-width=\"2\"/>";
        s << "<path d=\"M " << (x + 16 + inner.w) << " " << y << " C " << (x + 16 + inner.w + 10)
          << " " << (y + 22) << ", " << (x + 6) << " " << (y + 22) << ", " << (x + 16) << " " << y
          << "\" fill=\"none\" stroke=\"#64748b\" stroke-width=\"1.5\" stroke-dasharray=\"4,2\"/>";
        return {w, inner.h + 24, s.str()};
      }
      case ir::Prod::Kind::Act:
        if (!p->items.empty()) {
          ir::Prod seq_view;
          seq_view.kind = ir::Prod::Kind::Seq;
          seq_view.items = p->items;
          return self(self, std::make_shared<ir::Prod>(seq_view), x, y);
        }
        return {24, 32, ""};
      case ir::Prod::Kind::PrecTable: {
        Box inner = self(self, p->inner, x + 12, y);
        int w = std::max(inner.w + 24, 140);
        std::ostringstream s;
        s << "<rect x=\"" << x << "\" y=\"" << (y - 22) << "\" width=\"" << w
          << "\" height=\"44\" rx=\"6\" fill=\"#fef3c7\" stroke=\"#d97706\" stroke-width=\"1.5\" stroke-dasharray=\"4,2\"/>"
          << inner.svg;
        return {w, 52, s.str()};
      }
      case ir::Prod::Kind::Label:
      case ir::Prod::Kind::Look:
      case ir::Prod::Kind::Recover:
        return self(self, p->inner, x, y);
      case ir::Prod::Kind::Ctx: {
        std::string lbl = p->ctx_var.empty() ? "context do" : ("exactly " + p->ctx_var);
        int w = std::max(80, static_cast<int>(lbl.size()) * 8 + 20);
        std::ostringstream s;
        s << "<rect x=\"" << x << "\" y=\"" << (y - 14) << "\" width=\"" << w
          << "\" height=\"28\" rx=\"6\" fill=\"#fce7f3\" stroke=\"#db2777\" stroke-width=\"1.5\"/>"
          << "<text x=\"" << (x + w / 2) << "\" y=\"" << (y + 4)
          << "\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"12\" fill=\"#831843\">"
          << xml_esc(lbl) << "</text>";
        return {w, 32, s.str()};
      }
    }
    return {24, 32, ""};
  };

  std::ostringstream body;
  int cur_y = 48;
  int max_width = 520;
  for (const auto& [rid, rule] : g.gRules) {
    body << "<text x=\"20\" y=\"" << (cur_y - 22)
         << "\" font-family=\"sans-serif\" font-weight=\"bold\" font-size=\"13\" fill=\"#1e293b\">"
         << xml_esc(rid + " :: " + ir::type_to_string(rule.rType)) << "</text>\n";
    body << "<circle cx=\"24\" cy=\"" << cur_y << "\" r=\"4\" fill=\"#0f172a\"/>\n";
    body << "<line x1=\"28\" y1=\"" << cur_y << "\" x2=\"48\" y2=\"" << cur_y
         << "\" stroke=\"#334155\" stroke-width=\"2\"/>\n";
    Box b = render_prod(render_prod, rule.rBody, 48, cur_y);
    body << b.svg << "\n";
    int end_x = 48 + b.w;
    body << "<line x1=\"" << end_x << "\" y1=\"" << cur_y << "\" x2=\"" << (end_x + 20)
         << "\" y2=\"" << cur_y << "\" stroke=\"#334155\" stroke-width=\"2\"/>\n";
    body << "<circle cx=\"" << (end_x + 24) << "\" cy=\"" << cur_y
         << "\" r=\"4\" fill=\"#0f172a\"/>\n";
    max_width = std::max(max_width, end_x + 56);
    cur_y += std::max(56, b.h + 36);
  }

  std::ostringstream svg;
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << max_width << "\" height=\""
      << (cur_y + 20) << "\" viewBox=\"0 0 " << max_width << " " << (cur_y + 20) << "\">\n";
  svg << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
  svg << body.str();
  svg << "</svg>\n";
  return svg.str();
}

// ---------------------------------------------------------------------------
// 3. LSP JSON-RPC 2.0 Server
// ---------------------------------------------------------------------------

class LspServer {
public:
  std::vector<std::string> process_message(std::string_view json_rpc) {
    std::vector<std::string> out;
    detail::JsonParser parser(json_rpc);
    auto msg_opt = parser.parse();
    if (!msg_opt || msg_opt->kind != detail::Json::Kind::Object) {
      return out;
    }
    const detail::Json& msg = *msg_opt;
    const detail::Json* id_ptr = msg.get("id");
    std::string method;
    if (const auto* m = msg.get("method")) {
      method = m->as_string();
    }
    const detail::Json* params = msg.get("params");

    auto make_response = [&](detail::Json result) -> std::string {
      detail::Json resp = detail::Json::make_object({
          {"jsonrpc", detail::Json::make_string("2.0")},
          {"id", id_ptr ? *id_ptr : detail::Json::make_null()},
          {"result", std::move(result)},
      });
      return detail::json_stringify(resp);
    };

    if (method == "initialize") {
      detail::Json caps = detail::Json::make_object({
          {"textDocumentSync", detail::Json::make_int(1)},
          {"hoverProvider", detail::Json::make_bool(true)},
          {"definitionProvider", detail::Json::make_bool(true)},
          {"referencesProvider", detail::Json::make_bool(true)},
      });
      detail::Json info = detail::Json::make_object({
          {"name", detail::Json::make_string("parsley-lsp")},
          {"version", detail::Json::make_string("0.1.0")},
      });
      out.push_back(make_response(detail::Json::make_object({
          {"capabilities", std::move(caps)},
          {"serverInfo", std::move(info)},
      })));
      return out;
    }

    if (method == "initialized") {
      return out;
    }

    if (method == "shutdown") {
      shutdown_requested_ = true;
      out.push_back(make_response(detail::Json::make_null()));
      return out;
    }

    if (method == "exit") {
      should_exit_ = true;
      exit_code_ = shutdown_requested_ ? 0 : 1;
      return out;
    }

    if (method == "textDocument/didOpen" && params) {
      if (const auto* td = params->get("textDocument")) {
        std::string uri = td->get("uri") ? td->get("uri")->as_string() : "";
        std::string text = td->get("text") ? td->get("text")->as_string() : "";
        if (!uri.empty()) {
          update_document(uri, text);
          out.push_back(build_publish_diagnostics(uri));
        }
      }
      return out;
    }

    if (method == "textDocument/didChange" && params) {
      std::string uri;
      if (const auto* td = params->get("textDocument"); td && td->get("uri")) {
        uri = td->get("uri")->as_string();
      }
      if (const auto* changes = params->get("contentChanges");
          changes && changes->kind == detail::Json::Kind::Array && !changes->arr_val.empty()) {
        if (const auto* txt = changes->arr_val.back().get("text")) {
          if (!uri.empty()) {
            update_document(uri, txt->as_string());
            out.push_back(build_publish_diagnostics(uri));
          }
        }
      }
      return out;
    }

    if (method == "textDocument/hover" && params) {
      out.push_back(make_response(handle_hover(*params)));
      return out;
    }

    if (method == "textDocument/definition" && params) {
      out.push_back(make_response(handle_definition(*params)));
      return out;
    }

    if (method == "textDocument/references" && params) {
      out.push_back(make_response(handle_references(*params)));
      return out;
    }

    if (id_ptr) {
      out.push_back(make_response(detail::Json::make_null()));
    }
    return out;
  }

  bool should_exit() const noexcept { return should_exit_; }
  int exit_code() const noexcept { return exit_code_; }

private:
  struct DocState {
    std::string uri;
    std::string source;
    DiagnosticBag diags;
    ast::Module ast_mod;
    resolve::ResolvedModule resolved;
    typecheck::TypedModule typed;
    ir::Grammar grammar;
    analysis::AnalysisResult analysis;
  };

  std::unordered_map<std::string, DocState> docs_;
  bool shutdown_requested_ = false;
  bool should_exit_ = false;
  int exit_code_ = 0;

  void update_document(const std::string& uri, std::string text) {
    DocState st;
    st.uri = uri;
    st.source = std::move(text);
    st.ast_mod = parser::parse_module(st.source, uri, st.diags);
    st.resolved = resolve::resolve_module(st.ast_mod, st.diags);
    st.typed = typecheck::check_module(st.ast_mod, st.resolved, st.diags);
    st.grammar = desugar::desugar_module(st.ast_mod, st.resolved, st.typed, st.diags);
    st.analysis = analysis::analyze_grammar(st.grammar, st.diags);
    docs_[uri] = std::move(st);
  }

  std::string build_publish_diagnostics(const std::string& uri) const {
    auto it = docs_.find(uri);
    detail::Json diag_arr = detail::Json::make_array();
    if (it != docs_.end()) {
      for (const auto& d : it->second.diags.items()) {
        int sev = 1;
        if (d.severity == Severity::Warning) sev = 2;
        else if (d.severity == Severity::Note) sev = 3;
        detail::Json dj = detail::Json::make_object({
            {"range", detail::span_to_lsp_range(d.span)},
            {"severity", detail::Json::make_int(sev)},
            {"code", detail::Json::make_string(d.code)},
            {"source", detail::Json::make_string("parsley")},
            {"message", detail::Json::make_string(d.message)},
        });
        diag_arr.arr_val.push_back(std::move(dj));
      }
    }
    detail::Json notif = detail::Json::make_object({
        {"jsonrpc", detail::Json::make_string("2.0")},
        {"method", detail::Json::make_string("textDocument/publishDiagnostics")},
        {"params", detail::Json::make_object({
                       {"uri", detail::Json::make_string(uri)},
                       {"diagnostics", std::move(diag_arr)},
                   })},
    });
    return detail::json_stringify(notif);
  }

  static std::pair<std::string, std::pair<int, int>> extract_doc_pos(const detail::Json& params) {
    std::string uri;
    int line = 0;
    int ch = 0;
    if (const auto* td = params.get("textDocument"); td && td->get("uri")) {
      uri = td->get("uri")->as_string();
    }
    if (const auto* pos = params.get("position")) {
      if (pos->get("line")) line = static_cast<int>(pos->get("line")->as_int(0));
      if (pos->get("character")) ch = static_cast<int>(pos->get("character")->as_int(0));
    }
    return {uri, {line, ch}};
  }

  detail::Json handle_hover(const detail::Json& params) const {
    auto [uri, pos] = extract_doc_pos(params);
    auto it = docs_.find(uri);
    if (it == docs_.end()) return detail::Json::make_null();
    const DocState& doc = it->second;
    const std::string sym = detail::ident_at_position(doc.source, pos.first, pos.second);
    if (sym.empty()) return detail::Json::make_null();

    std::ostringstream md;
    bool found = false;

    if (auto ts = doc.typed.lookup_rule_type_string(sym)) {
      found = true;
      md << "```parsley\n" << sym << " :: " << *ts << "\n```\n";
    } else if (auto vs = doc.resolved.lookup_value(sym)) {
      found = true;
      md << "```parsley\n" << sym;
      if (vs->type_ann) {
        md << " :: " << detail::format_type_expr(*vs->type_ann);
      }
      md << "\n```\n";
    } else if (auto cs = doc.resolved.lookup_ctor(sym)) {
      found = true;
      md << "Constructor `" << sym << "` of `" << cs->parent_type << "`\n";
    } else if (auto ty = doc.resolved.lookup_type(sym)) {
      found = true;
      md << "Data type `" << sym << "`\n";
    }

    if (auto ra_it = doc.analysis.rules.find(sym); ra_it != doc.analysis.rules.end()) {
      found = true;
      const auto& ra = ra_it->second;
      md << "\n- **Strategy**: `" << analysis::strategy_name(doc.analysis.selected_strategy) << "`";
      md << "\n- **Nullable**: `" << (ra.nullable ? "true" : "false") << "`";
      md << "\n- **FIRST**: `{";
      bool first_item = true;
      for (const auto& t : ra.first) {
        if (!first_item) md << ", ";
        md << "`" << t << "`";
        first_item = false;
      }
      md << "}`";
      md << "\n- **FOLLOW**: `{";
      first_item = true;
      for (const auto& t : ra.follow) {
        if (!first_item) md << ", ";
        md << "`" << t << "`";
        first_item = false;
      }
      md << "}`";
    }

    if (!found) return detail::Json::make_null();
    return detail::Json::make_object({
        {"contents", detail::Json::make_object({
                         {"kind", detail::Json::make_string("markdown")},
                         {"value", detail::Json::make_string(md.str())},
                     })},
    });
  }

  detail::Json handle_definition(const detail::Json& params) const {
    auto [uri, pos] = extract_doc_pos(params);
    auto it = docs_.find(uri);
    if (it == docs_.end()) return detail::Json::make_null();
    const DocState& doc = it->second;
    const std::string sym = detail::ident_at_position(doc.source, pos.first, pos.second);
    if (sym.empty()) return detail::Json::make_null();

    std::optional<Span> target;
    if (auto vs = doc.resolved.lookup_value(sym)) {
      if (vs->span.is_valid()) target = vs->span;
    } else if (auto ts = doc.resolved.lookup_type(sym)) {
      if (ts->span.is_valid()) target = ts->span;
    } else if (auto cs = doc.resolved.lookup_ctor(sym)) {
      if (cs->span.is_valid()) target = cs->span;
    }

    if (!target) return detail::Json::make_null();
    return detail::Json::make_object({
        {"uri", detail::Json::make_string(uri)},
        {"range", detail::span_to_lsp_range(*target)},
    });
  }

  detail::Json handle_references(const detail::Json& params) const {
    auto [uri, pos] = extract_doc_pos(params);
    auto it = docs_.find(uri);
    if (it == docs_.end()) return detail::Json::make_array();
    const DocState& doc = it->second;
    const std::string sym = detail::ident_at_position(doc.source, pos.first, pos.second);
    if (sym.empty()) return detail::Json::make_array();

    detail::Json locs = detail::Json::make_array();
    if (auto vs = doc.resolved.lookup_value(sym); vs && vs->span.is_valid()) {
      locs.arr_val.push_back(detail::Json::make_object({
          {"uri", detail::Json::make_string(uri)},
          {"range", detail::span_to_lsp_range(vs->span)},
      }));
    }
    for (const auto& ref : doc.resolved.references) {
      if ((ref.target_name == sym || ref.name == sym) && ref.span.is_valid()) {
        locs.arr_val.push_back(detail::Json::make_object({
            {"uri", detail::Json::make_string(uri)},
            {"range", detail::span_to_lsp_range(ref.span)},
        }));
      }
    }
    return locs;
  }
};

inline int run_lsp_server() {
  LspServer server;
  std::string line;
  while (!server.should_exit()) {
    std::size_t content_length = 0;
    bool got_header = false;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        if (got_header) break;
        continue;
      }
      got_header = true;
      constexpr std::string_view kPrefix = "Content-Length:";
      if (line.size() >= kPrefix.size() &&
          std::equal(kPrefix.begin(), kPrefix.end(), line.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
          })) {
        std::string val = line.substr(kPrefix.size());
        std::size_t idx = 0;
        while (idx < val.size() && std::isspace(static_cast<unsigned char>(val[idx]))) ++idx;
        if (idx < val.size()) {
          content_length = static_cast<std::size_t>(std::stoull(val.substr(idx)));
        }
      }
    }
    if (!std::cin || content_length == 0) {
      break;
    }
    std::string payload(content_length, '\0');
    std::cin.read(payload.data(), static_cast<std::streamsize>(content_length));
    if (std::cin.gcount() < static_cast<std::streamsize>(content_length)) {
      break;
    }
    for (const auto& out_msg : server.process_message(payload)) {
      std::cout << "Content-Length: " << out_msg.size() << "\r\n\r\n" << out_msg << std::flush;
    }
  }
  return server.exit_code();
}

// ---------------------------------------------------------------------------
// 4. Package Manager Helpers
// ---------------------------------------------------------------------------

inline std::string list_packages() {
  std::ostringstream out;
  out << "Available Parsley standard packages:\n";
  for (const auto& m : stdlib::builtin_modules()) {
    out << "  - " << m.name << ": " << m.description << " ("
        << m.exported_rules.size() << " rules, " << m.exported_types.size() << " types)\n";
  }
  return out.str();
}

inline std::optional<std::string> show_package(std::string_view name) {
  auto mod = stdlib::find_builtin_module(name);
  if (!mod) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << "Package: " << mod->name << "\n";
  out << "Description: " << mod->description << "\n";
  out << "Exported types: ";
  if (mod->exported_types.empty()) {
    out << "(none)";
  } else {
    for (std::size_t i = 0; i < mod->exported_types.size(); ++i) {
      if (i > 0) out << ", ";
      out << mod->exported_types[i];
    }
  }
  out << "\nExported rules: ";
  for (std::size_t i = 0; i < mod->exported_rules.size(); ++i) {
    if (i > 0) out << ", ";
    out << mod->exported_rules[i];
  }
  out << "\n\n" << mod->source;
  return out.str();
}

struct InitPackageResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> created_files;
};

inline InitPackageResult init_package(std::string_view name, std::string_view target_dir = ".") {
  InitPackageResult res;
  std::string pkg_name = name.empty() ? "MyGrammar" : std::string(name);
  std::string mod_name = pkg_name;
  mod_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mod_name[0])));

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir(target_dir);
  fs::create_directories(dir / "src", ec);
  if (ec) {
    res.ok = false;
    res.message = "Failed to create package directory: " + ec.message();
    return res;
  }

  fs::path manifest = dir / "parsley.toml";
  {
    std::ofstream mf(manifest);
    if (!mf) {
      res.ok = false;
      res.message = "Failed to write " + manifest.string();
      return res;
    }
    mf << "[package]\n";
    mf << "name = \"" << pkg_name << "\"\n";
    mf << "version = \"0.1.0\"\n";
    mf << "entry = \"src/" << mod_name << ".parsley\"\n";
    mf << "target = \"c17\"\n";
    res.created_files.push_back(manifest.string());
  }

  fs::path grammar_file = dir / "src" / (mod_name + ".parsley");
  if (!fs::exists(grammar_file, ec)) {
    std::ofstream gf(grammar_file);
    if (!gf) {
      res.ok = false;
      res.message = "Failed to write " + grammar_file.string();
      return res;
    }
    gf << "module " << mod_name << " (program) where\n\n";
    gf << "import Parsley.Prelude (eof)\n\n";
    gf << "skip  ws    = /[[:space:]]+/\n";
    gf << "token ident = /[a-zA-Z_][a-zA-Z0-9_]*/ :: Text\n\n";
    gf << "data Ast = Word Text\n\n";
    gf << "program :: Ast\n";
    gf << "program = (Word <$> ident) <* eof\n\n";
    gf << "example \"basic\" : \"hello\" parses-to Word \"hello\"\n";
    res.created_files.push_back(grammar_file.string());
  }

  res.ok = true;
  res.message = "Initialized Parsley package `" + pkg_name + "` in " + dir.string();
  return res;
}

} // namespace parsley::tooling
