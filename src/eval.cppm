module;

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <locale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.eval;

import parsley.ast;
import parsley.diagnostic;
import parsley.ir;
import parsley.regex;
import parsley.c17_text;

export namespace parsley::eval {

struct EvalValue {
  enum class Kind {
    Unit,
    Bool,
    Number,
    String,
    List,
    Tuple,
    Ctor,
  };

  Kind kind = Kind::Unit;
  bool bool_value = false;
  double number_value = 0.0;
  std::int64_t integer_value = 0;
  bool is_int = false;
  std::string string_value;
  std::string ctor_name;
  std::vector<EvalValue> elements;

  static EvalValue make_unit() {
    EvalValue v;
    v.kind = Kind::Unit;
    return v;
  }

  static EvalValue make_bool(bool b) {
    EvalValue v;
    v.kind = Kind::Bool;
    v.bool_value = b;
    return v;
  }

  static EvalValue make_number(double n, bool as_int = false) {
    EvalValue v;
    v.kind = Kind::Number;
    v.number_value = n;
    v.is_int = as_int;
    if (as_int) {
      if (!std::isfinite(n) || n < -9223372036854775808.0 || n >= 9223372036854775808.0) throw std::invalid_argument("integer outside Int64 range");
      v.integer_value = static_cast<std::int64_t>(n);
    }
    return v;
  }

  static EvalValue make_string(std::string s) {
    EvalValue v;
    v.kind = Kind::String;
    v.string_value = std::move(s);
    return v;
  }

  static EvalValue make_list(std::vector<EvalValue> elems) {
    EvalValue v;
    v.kind = Kind::List;
    v.elements = std::move(elems);
    return v;
  }

  static EvalValue make_tuple(std::vector<EvalValue> elems) {
    EvalValue v;
    v.kind = Kind::Tuple;
    v.elements = std::move(elems);
    return v;
  }

  static EvalValue make_ctor(std::string name, std::vector<EvalValue> fields = {}) {
    EvalValue v;
    v.kind = Kind::Ctor;
    v.ctor_name = std::move(name);
    v.elements = std::move(fields);
    return v;
  }

  bool operator==(const EvalValue& other) const {
    if (kind != other.kind) {
      return false;
    }
    switch (kind) {
      case Kind::Unit:
        return true;
      case Kind::Bool:
        return bool_value == other.bool_value;
      case Kind::Number:
        if (is_int && other.is_int) return integer_value == other.integer_value;
        return std::abs(number_value - other.number_value) < 1e-9;
      case Kind::String:
        return string_value == other.string_value;
      case Kind::List:
      case Kind::Tuple:
        return elements == other.elements;
      case Kind::Ctor:
        return ctor_name == other.ctor_name && elements == other.elements;
    }
    return false;
  }
};

inline std::string format_value(const EvalValue& v, bool wrap_parens = false) {
  switch (v.kind) {
    case EvalValue::Kind::Unit:
      return "()";
    case EvalValue::Kind::Bool:
      return v.bool_value ? "True" : "False";
    case EvalValue::Kind::Number: {
      std::ostringstream out;
      out.imbue(std::locale::classic());
      if (v.is_int) return std::to_string(v.integer_value);
      if ((std::isfinite(v.number_value) && std::floor(v.number_value) == v.number_value &&
           v.number_value >= -9e15 && v.number_value <= 9e15)) {
        out << static_cast<long long>(v.number_value);
      } else {
        out << std::setprecision(15) << v.number_value;
      }
      return out.str();
    }
    case EvalValue::Kind::String: return c17_text::display_string(v.string_value);
    case EvalValue::Kind::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < v.elements.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += format_value(v.elements[i], false);
      }
      out.push_back(']');
      return out;
    }
    case EvalValue::Kind::Tuple: {
      std::string out = "(";
      for (std::size_t i = 0; i < v.elements.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += format_value(v.elements[i], false);
      }
      out.push_back(')');
      return out;
    }
    case EvalValue::Kind::Ctor: {
      if (v.elements.empty()) {
        return v.ctor_name;
      }
      std::string out;
      if (wrap_parens) {
        out.push_back('(');
      }
      out += v.ctor_name;
      for (const auto& el : v.elements) {
        out.push_back(' ');
        out += format_value(el, true);
      }
      if (wrap_parens) {
        out.push_back(')');
      }
      return out;
    }
  }
  return "()";
}

inline bool matches_pattern(const EvalValue& val, const ast::AstPattern& pat) {
  switch (pat.kind) {
    case ast::AstPattern::Kind::Wildcard:
      return true;
    case ast::AstPattern::Kind::Number:
      if (val.is_int && !pat.name.empty() && pat.name.find_first_of(".eE") == std::string::npos) {
        std::int64_t expected;auto [end,ec]=std::from_chars(pat.name.data(),pat.name.data()+pat.name.size(),expected);
        return ec==std::errc{}&&end==pat.name.data()+pat.name.size()&&expected==val.integer_value;
      }
      return val.kind == EvalValue::Kind::Number &&
             std::abs(val.number_value - pat.number_value) < 1e-9;
    case ast::AstPattern::Kind::String:
      return val.kind == EvalValue::Kind::String && val.string_value == pat.string_value;
    case ast::AstPattern::Kind::List:
      if (val.kind != EvalValue::Kind::List || val.elements.size() != pat.args.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pat.args.size(); ++i) {
        if (!matches_pattern(val.elements[i], pat.args[i])) {
          return false;
        }
      }
      return true;
    case ast::AstPattern::Kind::Ctor:
      if (pat.args.empty() && (pat.name == "True" || pat.name == "False") &&
          val.kind == EvalValue::Kind::Bool) {
        return val.bool_value == (pat.name == "True");
      }
      if (val.kind != EvalValue::Kind::Ctor || val.ctor_name != pat.name ||
          val.elements.size() != pat.args.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pat.args.size(); ++i) {
        if (!matches_pattern(val.elements[i], pat.args[i])) {
          return false;
        }
      }
      return true;
    case ast::AstPattern::Kind::Ident:
      if (pat.name == "True" || pat.name == "False") {
        if (val.kind == EvalValue::Kind::Bool) {
          return val.bool_value == (pat.name == "True");
        }
      }
      if (val.kind == EvalValue::Kind::Ctor && val.elements.empty() && val.ctor_name == pat.name) {
        return true;
      }
      if (val.kind == EvalValue::Kind::String && val.string_value == pat.name) {
        return true;
      }
      return false;
  }
  return false;
}

struct EvalParseError {
  int line = 1;
  int col = 1;
  std::string expected;
  std::string found;
  std::string message;
};

struct EvalResult {
  bool ok = false;
  EvalValue value;
  std::optional<EvalParseError> error;
};

struct ExampleRunResult {
  std::string name;
  bool passed = false;
  std::string message;
};

struct ExampleRunSummary {
  std::size_t total = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<ExampleRunResult> cases;
};

struct FuzzSummary {
  std::size_t attempts = 0;
  std::size_t generated = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
};

} // namespace parsley::eval

namespace parsley::eval::detail {

// ---------------------------------------------------------------------------
// Cross-platform Thompson NFA regex engine (with POSIX [:space:] etc.)
// ---------------------------------------------------------------------------

using regex::CharMatcher;
using regex::RegexAst;
using regex::RegexAstPtr;
using regex::RegexParser;

using regex::NfaState;
using regex::CompiledRegex;

inline std::string sample_from_regex_ast(const RegexAstPtr& node, std::mt19937_64& rng) {
  if (!node) {
    return "";
  }
  switch (node->kind) {
    case RegexAst::Kind::Empty:
      return "";
    case RegexAst::Kind::Char: {
      auto cands = node->matcher.candidate_chars();
      if (cands.empty()) {
        return "a";
      }
      // Prefer printable alphanumeric characters if available
      std::vector<unsigned char> nice;
      for (unsigned char c : cands) {
        if (std::isalnum(c) || c == '_') {
          nice.push_back(c);
        }
      }
      const auto& pool = nice.empty() ? cands : nice;
      std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
      return std::string(1, static_cast<char>(pool[dist(rng)]));
    }
    case RegexAst::Kind::Seq: {
      std::string out;
      for (const auto& it : node->items) {
        out += sample_from_regex_ast(it, rng);
      }
      return out;
    }
    case RegexAst::Kind::Alt: {
      if (node->items.empty()) {
        return "";
      }
      std::uniform_int_distribution<std::size_t> dist(0, node->items.size() - 1);
      return sample_from_regex_ast(node->items[dist(rng)], rng);
    }
    case RegexAst::Kind::Repeat: {
      int lo = node->min_rep;
      int hi = (node->max_rep < 0) ? (lo + 3) : std::min(node->max_rep, lo + 3);
      std::uniform_int_distribution<int> dist(lo, std::max(lo, hi));
      int count = dist(rng);
      std::string out;
      for (int i = 0; i < count; ++i) {
        out += sample_from_regex_ast(node->inner, rng);
      }
      return out;
    }
  }
  return "";
}

// ---------------------------------------------------------------------------
// Lexer & Runtime Evaluator
// ---------------------------------------------------------------------------

struct LexedToken {
  std::string id;
  std::string lexeme;
  int line = 1;
  int col = 1;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  bool is_eof = false;
  bool is_lex_error = false;
};

struct CompiledToken {
  const ir::TokenDef* def = nullptr;
  std::optional<CompiledRegex> regex;
};

inline void advance_pos(std::string_view text, int& line, int& col) noexcept {
  for (char c : text) {
    if (c == '\n') {
      ++line;
      col = 1;
    } else if ((static_cast<unsigned char>(c) & 0xc0) != 0x80 && c != '\r') {
      ++col;
    }
  }
}

inline std::string display_token_name(const ir::Grammar& g, const std::string& id) {
  if (id == "eof") {
    return "end of input";
  }
  if (const auto* td = ir::find_token(g, id)) {
    if (!td->is_regex && (td->is_anonymous || td->is_keyword)) {
      return c17_text::display_string(td->pattern);
    }
  }
  return id;
}

inline EvalValue token_to_value(const ir::TokenDef* td, const std::string& lexeme) {
  if (!td) {
    return EvalValue::make_string(lexeme);
  }
  if (td->type.kind == ir::Type::Kind::Unit) {
    return EvalValue::make_unit();
  }
  if (td->type.kind == ir::Type::Kind::Named) {
    if (td->type.name == "Double" || td->type.name == "Float") {
      double d;
      auto [end, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), d);
      if (ec != std::errc{} || end != lexeme.data() + lexeme.size() || !std::isfinite(d)) throw std::invalid_argument("invalid or out-of-range Double token");
      return EvalValue::make_number(d, false);
    }
    if (td->type.name == "Int") {
      std::int64_t n;
      auto [end, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), n);
      if (ec != std::errc{} || end != lexeme.data() + lexeme.size()) throw std::invalid_argument("invalid or out-of-range Int64 token");
      auto v = EvalValue::make_number(0, true); v.integer_value = n; v.number_value = static_cast<double>(n); return v;
    }
    if (td->type.name == "Bool") {
      return EvalValue::make_bool(lexeme == "true" || lexeme == "True");
    }
  }
  return EvalValue::make_string(lexeme);
}

inline bool grammar_has_context(const ir::ProdPtr& p) {
  if (!p) {
    return false;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Ctx:
      return true;
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Alt:
    case ir::Prod::Kind::Act:
      for (const auto& it : p->items) {
        if (grammar_has_context(it)) {
          return true;
        }
      }
      return false;
    case ir::Prod::Kind::Rep:
    case ir::Prod::Kind::Look:
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::PrecTable:
      return grammar_has_context(p->inner);
    case ir::Prod::Kind::Recover:
      return grammar_has_context(p->inner) || grammar_has_context(p->sync);
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Ref:
    case ir::Prod::Kind::Eps:
      return false;
  }
  return false;
}

class Interpreter {
public:
  Interpreter(const ir::Grammar& grammar, std::string_view input)
      : grammar_(grammar), input_(input) {
    compile_modes();
    for (const auto& [_, rule] : grammar_.gRules) {
      if (grammar_has_context(rule.rBody)) {
        scannerless_ = true;
        break;
      }
    }
    if (!scannerless_) {
      lex_all();
    }
  }

  EvalResult run(std::string_view start_rule) {
    std::string entry = std::string(start_rule);
    if (entry.empty()) {
      if (!grammar_.gStarts.empty()) {
        entry = grammar_.gStarts[0];
      } else if (grammar_.gRules.contains("program")) {
        entry = "program";
      } else if (!grammar_.gRules.empty()) {
        entry = grammar_.gRules.begin()->first;
      }
    }
    if (entry.empty() || !grammar_.gRules.contains(entry)) {
      EvalResult r;
      r.ok = false;
      r.error = EvalParseError{1, 1, "start rule", "none", "grammar has no start rule"};
      return r;
    }

    EvalResult res;
    auto val = eval_rule(entry);
    if (val && !had_recovery_error_) {
      res.ok = true;
      res.value = std::move(*val);
      return res;
    }

    res.ok = false;
    res.error = build_parse_error();
    return res;
  }

private:
  struct DepthGuard {
    std::size_t& depth;
    explicit DepthGuard(std::size_t& value) : depth(value) { if (++depth > 128) throw std::runtime_error("parse nesting exceeds 128"); }
    ~DepthGuard() { --depth; }
  };
  std::size_t depth_ = 0, steps_ = 0;
  const ir::Grammar& grammar_;
  std::string_view input_;
  bool scannerless_ = false;

  std::unordered_map<std::string, std::vector<CompiledToken>> compiled_modes_;
  std::vector<LexedToken> tokens_;

  // Parser cursor state
  std::size_t tok_pos_ = 0;

  // Scannerless cursor state
  std::size_t char_pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  std::vector<std::string> mode_stack_ = {"default"};

  // Dynamic environments
  std::vector<EvalValue> prefix_stack_;
  std::unordered_map<std::string, EvalValue> ctx_env_;

  // Left-recursion seed table for (rule_id, cursor_pos)
  struct MemoSeed {
    bool active = false;
    bool hit_recursion = false;
    bool ok = false;
    EvalValue value;
    std::size_t end_tok_pos = 0;
    std::size_t end_char_pos = 0;
    int end_line = 1;
    int end_col = 1;
    std::vector<std::string> end_modes;
  };
  std::map<std::pair<std::string, std::size_t>, MemoSeed> lr_table_;

  // Furthest failure tracking
  bool has_err_ = false;
  bool suppress_err_ = false;
  bool had_recovery_error_ = false;
  std::size_t err_offset_ = 0;
  int err_line_ = 1;
  int err_col_ = 1;
  std::string err_found_ = "end of input";
  std::vector<std::string> err_expected_;

  struct SavedCursor {
    std::size_t tok_pos = 0;
    std::size_t char_pos = 0;
    int line = 1;
    int col = 1;
    std::vector<std::string> modes;
    std::unordered_map<std::string, EvalValue> ctx_env;
  };

  SavedCursor save_cursor() const {
    return SavedCursor{tok_pos_, char_pos_, line_, col_, mode_stack_, ctx_env_};
  }

  void restore_cursor(const SavedCursor& s) {
    tok_pos_ = s.tok_pos;
    char_pos_ = s.char_pos;
    line_ = s.line;
    col_ = s.col;
    mode_stack_ = s.modes;
    ctx_env_ = s.ctx_env;
  }

  std::size_t current_offset() const noexcept {
    if (!scannerless_) {
      if (tok_pos_ < tokens_.size()) {
        return tokens_[tok_pos_].start_offset;
      }
      return input_.size();
    }
    return char_pos_;
  }

  void current_line_col(int& out_line, int& out_col, std::string& out_found) const {
    if (!scannerless_) {
      if (tok_pos_ < tokens_.size()) {
        const auto& t = tokens_[tok_pos_];
        out_line = t.line;
        out_col = t.col;
        out_found = t.is_eof ? "end of input" : ("\"" + t.lexeme + "\"");
        return;
      }
    }
    out_line = line_;
    out_col = col_;
    if (char_pos_ >= input_.size()) {
      out_found = "end of input";
    } else {
      out_found = std::string("\"") + input_[char_pos_] + "\"";
    }
  }

  void record_failure(std::string expected_label, bool override_at_same_offset = false) {
    if (suppress_err_) {
      return;
    }
    const std::size_t off = current_offset();
    int ln = 1;
    int cl = 1;
    std::string fnd;
    current_line_col(ln, cl, fnd);

    if (!has_err_ || off > err_offset_) {
      has_err_ = true;
      err_offset_ = off;
      err_line_ = ln;
      err_col_ = cl;
      err_found_ = std::move(fnd);
      err_expected_.clear();
      if (!expected_label.empty()) {
        err_expected_.push_back(std::move(expected_label));
      }
    } else if (off == err_offset_) {
      if (override_at_same_offset) {
        err_expected_.clear();
      }
      if (!expected_label.empty() &&
          std::find(err_expected_.begin(), err_expected_.end(), expected_label) ==
              err_expected_.end()) {
        err_expected_.push_back(std::move(expected_label));
      }
    }
  }

  EvalParseError build_parse_error() const {
    EvalParseError e;
    if (!has_err_) {
      e.line = 1;
      e.col = 1;
      e.expected = "expression";
      e.found = "end of input";
      e.message = "parse error";
      return e;
    }
    e.line = err_line_;
    e.col = err_col_;
    e.found = err_found_;
    if (err_expected_.empty()) {
      e.expected = "valid token";
    } else if (err_expected_.size() == 1) {
      e.expected = err_expected_[0];
    } else {
      for (std::size_t i = 0; i < err_expected_.size(); ++i) {
        if (i > 0) {
          e.expected += (i + 1 == err_expected_.size()) ? " or " : ", ";
        }
        e.expected += err_expected_[i];
      }
    }
    e.message = "unexpected " + e.found + ", expecting " + e.expected;
    return e;
  }

  void compile_modes() {
    auto compile_list = [](const std::vector<ir::TokenDef>& defs) {
      std::vector<CompiledToken> out;
      out.reserve(defs.size());
      for (const auto& td : defs) {
        CompiledToken ct;
        ct.def = &td;
        if (td.is_regex) {
          ct.regex = CompiledRegex::compile(td.pattern);
        }
        out.push_back(std::move(ct));
      }
      return out;
    };
    compiled_modes_["default"] = compile_list(grammar_.gTokens);
    for (const auto& m : grammar_.gModes) {
      compiled_modes_[m.name] = compile_list(m.tokens);
    }
  }

  const std::vector<CompiledToken>& active_tokens() const {
    const std::string& m = mode_stack_.empty() ? std::string("default") : mode_stack_.back();
    if (auto it = compiled_modes_.find(m); it != compiled_modes_.end()) {
      return it->second;
    }
    return compiled_modes_.at("default");
  }

  static std::optional<std::size_t> match_compiled(const CompiledToken& ct,
                                                   std::string_view input,
                                                   std::size_t offset) {
    if (!ct.def->is_regex) {
      if (offset + ct.def->pattern.size() <= input.size() &&
          input.substr(offset, ct.def->pattern.size()) == ct.def->pattern) {
        return ct.def->pattern.size();
      }
      return std::nullopt;
    }
    if (ct.regex) {
      auto m = ct.regex->match_prefix(input, offset);
      if (m && *m > 0) {
        return m;
      }
    }
    return std::nullopt;
  }

  void apply_mode_action(const ir::TokenDef* td, std::vector<std::string>& modes) const {
    if (!td) {
      return;
    }
    if (td->action == ir::LexActionKind::Push && !td->target_mode.empty()) {
      if(modes.size()>=64)throw std::runtime_error("lexer mode depth exceeds 64");
      modes.push_back(td->target_mode);
    } else if (td->action == ir::LexActionKind::Pop && modes.size() > 1) {
      modes.pop_back();
    }
  }

  void lex_all() {
    std::size_t pos = 0;
    int ln = 1;
    int cl = 1;
    std::vector<std::string> modes = {"default"};

    while (pos < input_.size()) {
      const std::string& cur_mode = modes.empty() ? std::string("default") : modes.back();
      const auto& list = compiled_modes_.contains(cur_mode) ? compiled_modes_.at(cur_mode)
                                                            : compiled_modes_.at("default");

      const ir::TokenDef* best_def = nullptr;
      std::size_t best_len = 0;

      for (const auto& ct : list) {
        auto mlen = match_compiled(ct, input_, pos);
        if (!mlen || *mlen == 0) {
          continue;
        }
        bool better = false;
        if (!best_def || *mlen > best_len) {
          better = true;
        } else if (*mlen == best_len) {
          if (ct.def->priority > best_def->priority) {
            better = true;
          } else if (ct.def->priority == best_def->priority &&
                     ct.def->decl_order < best_def->decl_order) {
            better = true;
          }
        }
        if (better) {
          best_def = ct.def;
          best_len = *mlen;
        }
      }

      if (!best_def) {
        LexedToken err_tok;
        err_tok.id = "<lex_error>";
        err_tok.lexeme = std::string(1, input_[pos]);
        err_tok.line = ln;
        err_tok.col = cl;
        err_tok.start_offset = pos;
        err_tok.end_offset = pos + 1;
        err_tok.is_lex_error = true;
        tokens_.push_back(std::move(err_tok));
        advance_pos(input_.substr(pos, 1), ln, cl);
        ++pos;
        continue;
      }

      std::string_view slice = input_.substr(pos, best_len);
      if (!best_def->is_skip) {
        LexedToken tok;
        tok.id = best_def->id;
        tok.lexeme = std::string(slice);
        tok.line = ln;
        tok.col = cl;
        tok.start_offset = pos;
        tok.end_offset = pos + best_len;
        tokens_.push_back(std::move(tok));
      }
      advance_pos(slice, ln, cl);
      pos += best_len;
      apply_mode_action(best_def, modes);
    }

    LexedToken eof_tok;
    eof_tok.id = "eof";
    eof_tok.line = ln;
    eof_tok.col = cl;
    eof_tok.start_offset = pos;
    eof_tok.end_offset = pos;
    eof_tok.is_eof = true;
    tokens_.push_back(std::move(eof_tok));
  }

  void scannerless_skip() {
    while (char_pos_ < input_.size()) {
      const auto& list = active_tokens();
      const ir::TokenDef* best_skip = nullptr;
      std::size_t best_len = 0;
      for (const auto& ct : list) {
        if (!ct.def->is_skip) {
          continue;
        }
        auto mlen = match_compiled(ct, input_, char_pos_);
        if (mlen && *mlen > best_len) {
          best_skip = ct.def;
          best_len = *mlen;
        }
      }
      if (!best_skip) {
        break;
      }
      advance_pos(input_.substr(char_pos_, best_len), line_, col_);
      char_pos_ += best_len;
      apply_mode_action(best_skip, mode_stack_);
    }
  }

  std::optional<EvalValue> match_token(const std::string& token_id) {
    if (!scannerless_) {
      if (tok_pos_ >= tokens_.size()) {
        record_failure(display_token_name(grammar_, token_id));
        return std::nullopt;
      }
      const auto& cur = tokens_[tok_pos_];
      if (token_id == "eof") {
        if (cur.is_eof) {
          return EvalValue::make_unit();
        }
        record_failure("end of input");
        return std::nullopt;
      }
      if (!cur.is_eof && !cur.is_lex_error && cur.id == token_id) {
        const ir::TokenDef* td = ir::find_token(grammar_, token_id);
        EvalValue val = token_to_value(td, cur.lexeme);
        ++tok_pos_;
        return val;
      }
      record_failure(display_token_name(grammar_, token_id));
      return std::nullopt;
    }

    // Scannerless mode (used when grammar has `context do` rules)
    scannerless_skip();
    if (token_id == "eof") {
      if (char_pos_ >= input_.size()) {
        return EvalValue::make_unit();
      }
      record_failure("end of input");
      return std::nullopt;
    }

    const auto& list = active_tokens();
    const CompiledToken* target = nullptr;
    for (const auto& ct : list) {
      if (ct.def->id == token_id && !ct.def->is_skip) {
        target = &ct;
        break;
      }
    }
    if (!target) {
      // Check global token table fallback
      if (const auto* td = ir::find_token(grammar_, token_id)) {
        CompiledToken tmp;
        tmp.def = td;
        if (td->is_regex) {
          tmp.regex = CompiledRegex::compile(td->pattern);
        }
        auto mlen = match_compiled(tmp, input_, char_pos_);
        if (mlen && *mlen > 0) {
          std::string lexeme(input_.substr(char_pos_, *mlen));
          advance_pos(lexeme, line_, col_);
          char_pos_ += *mlen;
          apply_mode_action(td, mode_stack_);
          return token_to_value(td, lexeme);
        }
      }
      record_failure(display_token_name(grammar_, token_id));
      return std::nullopt;
    }

    auto mlen = match_compiled(*target, input_, char_pos_);
    if (!mlen || *mlen == 0) {
      record_failure(display_token_name(grammar_, token_id));
      return std::nullopt;
    }

    // Respect higher-priority keywords/literals shadowing regex tokens at the same or longer span
    for (const auto& ct : list) {
      if (ct.def == target->def || ct.def->is_skip) {
        continue;
      }
      {
        auto other_len = match_compiled(ct, input_, char_pos_);
        if (other_len && (*other_len > *mlen || (*other_len == *mlen &&
            (ct.def->priority > target->def->priority ||
             (ct.def->priority == target->def->priority && ct.def->decl_order < target->def->decl_order))))) {
          record_failure(display_token_name(grammar_, token_id));
          return std::nullopt;
        }
      }
    }

    std::string lexeme(input_.substr(char_pos_, *mlen));
    advance_pos(lexeme, line_, col_);
    char_pos_ += *mlen;
    apply_mode_action(target->def, mode_stack_);
    return token_to_value(target->def, lexeme);
  }

  std::optional<EvalValue> match_exactly(const std::string& expected_text) {
    if (!scannerless_) {
      if (tok_pos_ < tokens_.size() && !tokens_[tok_pos_].is_eof &&
          tokens_[tok_pos_].lexeme == expected_text) {
        EvalValue val = EvalValue::make_string(tokens_[tok_pos_].lexeme);
        ++tok_pos_;
        return val;
      }
      record_failure("\"" + expected_text + "\"");
      return std::nullopt;
    }
    scannerless_skip();
    if (char_pos_ + expected_text.size() <= input_.size() &&
        input_.substr(char_pos_, expected_text.size()) == expected_text) {
      std::string lexeme(input_.substr(char_pos_, expected_text.size()));
      advance_pos(lexeme, line_, col_);
      char_pos_ += expected_text.size();
      return EvalValue::make_string(std::move(lexeme));
    }
    record_failure("\"" + expected_text + "\"");
    return std::nullopt;
  }

  EvalValue eval_action(const ir::ActionExpr& act, const std::vector<EvalValue>& child_vals) {
    switch (act.kind) {
      case ir::ActionExpr::Kind::UnitLit:
        return EvalValue::make_unit();
      case ir::ActionExpr::Kind::BoolLit:
        return EvalValue::make_bool(act.bool_value);
      case ir::ActionExpr::Kind::IntLit:
        if (!act.name.empty()) {
          ir::TokenDef token;token.type.kind=ir::Type::Kind::Named;token.type.name="Int";
          return token_to_value(&token,act.name);
        }
        return EvalValue::make_number(act.number_value, true);
      case ir::ActionExpr::Kind::FloatLit:
        return EvalValue::make_number(act.number_value, false);
      case ir::ActionExpr::Kind::StringLit:
        return EvalValue::make_string(act.name);
      case ir::ActionExpr::Kind::Arg:
      case ir::ActionExpr::Kind::Identity: {
        if (act.arg_index >= 0) {
          const std::size_t idx = static_cast<std::size_t>(act.arg_index);
          if (idx < child_vals.size()) {
            return child_vals[idx];
          }
        } else {
          const std::ptrdiff_t pos =
              static_cast<std::ptrdiff_t>(prefix_stack_.size()) + act.arg_index;
          if (pos >= 0 && static_cast<std::size_t>(pos) < prefix_stack_.size()) {
            return prefix_stack_[static_cast<std::size_t>(pos)];
          }
        }
        return EvalValue::make_unit();
      }
      case ir::ActionExpr::Kind::List: {
        if (act.name == ":" && act.args.size() == 2) {
          EvalValue head = eval_action(act.args[0], child_vals);
          EvalValue tail = eval_action(act.args[1], child_vals);
          std::vector<EvalValue> combined;
          combined.push_back(std::move(head));
          if (tail.kind == EvalValue::Kind::List) {
            combined.insert(combined.end(), tail.elements.begin(), tail.elements.end());
          }
          return EvalValue::make_list(std::move(combined));
        }
        std::vector<EvalValue> elems;
        elems.reserve(act.args.size());
        for (const auto& a : act.args) {
          elems.push_back(eval_action(a, child_vals));
        }
        return EvalValue::make_list(std::move(elems));
      }
      case ir::ActionExpr::Kind::Tuple: {
        std::vector<EvalValue> elems;
        elems.reserve(act.args.size());
        for (const auto& a : act.args) {
          elems.push_back(eval_action(a, child_vals));
        }
        return EvalValue::make_tuple(std::move(elems));
      }
      case ir::ActionExpr::Kind::Ctor: {
        std::vector<EvalValue> fields;
        fields.reserve(act.args.size());
        for (const auto& a : act.args) {
          fields.push_back(eval_action(a, child_vals));
        }
        return EvalValue::make_ctor(act.name, std::move(fields));
      }
    }
    return EvalValue::make_unit();
  }

  std::optional<EvalValue> eval_rule(const std::string& rule_id) {
    DepthGuard guard(depth_);
    auto it = grammar_.gRules.find(rule_id);
    if (it == grammar_.gRules.end()) {
      record_failure(rule_id);
      return std::nullopt;
    }

    const std::size_t start_off = current_offset();
    const auto key = std::make_pair(rule_id, start_off);
    auto lr_it = lr_table_.find(key);
    if (lr_it != lr_table_.end() && lr_it->second.active) {
      lr_it->second.hit_recursion = true;
      if (!lr_it->second.ok) {
        return std::nullopt;
      }
      tok_pos_ = lr_it->second.end_tok_pos;
      char_pos_ = lr_it->second.end_char_pos;
      line_ = lr_it->second.end_line;
      col_ = lr_it->second.end_col;
      mode_stack_ = lr_it->second.end_modes;
      return lr_it->second.value;
    }

    const std::size_t saved_prefix_size = prefix_stack_.size();
    const SavedCursor entry_cursor = save_cursor();

    MemoSeed& seed = lr_table_[key];
    seed.active = true;
    seed.hit_recursion = false;
    seed.ok = false;

    auto res = eval_prod(it->second.rBody);
    if (!seed.hit_recursion) {
      lr_table_.erase(key);
      prefix_stack_.resize(saved_prefix_size);
      return res;
    }

    // Left-recursive seed growing (Warth's algorithm)
    while (res) {
      const std::size_t new_off = current_offset();
      const std::size_t prev_off =
          scannerless_ ? seed.end_char_pos : (seed.ok ? seed.end_tok_pos : 0);
      const std::size_t cur_unit_off = scannerless_ ? new_off : tok_pos_;
      if (seed.ok && cur_unit_off <= prev_off) {
        break;
      }
      seed.ok = true;
      seed.value = *res;
      seed.end_tok_pos = tok_pos_;
      seed.end_char_pos = char_pos_;
      seed.end_line = line_;
      seed.end_col = col_;
      seed.end_modes = mode_stack_;

      restore_cursor(entry_cursor);
      prefix_stack_.resize(saved_prefix_size);
      res = eval_prod(it->second.rBody);
    }

    prefix_stack_.resize(saved_prefix_size);
    if (seed.ok) {
      EvalValue final_val = seed.value;
      tok_pos_ = seed.end_tok_pos;
      char_pos_ = seed.end_char_pos;
      line_ = seed.end_line;
      col_ = seed.end_col;
      mode_stack_ = seed.end_modes;
      lr_table_.erase(key);
      return final_val;
    }
    lr_table_.erase(key);
    return std::nullopt;
  }

  std::optional<EvalValue> eval_prec_table(const ir::ProdPtr& p, int min_prec) {
    DepthGuard guard(depth_);
    const std::size_t start_off = current_offset();
    std::optional<EvalValue> lhs;

    // Try prefix operators (highest precedence first)
    for (std::size_t li = p->levels.size(); li > 0; --li) {
      const auto& lvl = p->levels[li - 1];
      if (lvl.assoc != ast::AssocKind::Prefix) {
        continue;
      }
      for (const auto& op : lvl.ops) {
        SavedCursor saved = save_cursor();
        bool matched = !op.pattern.empty();
        for (const auto& pat : op.pattern) {
          if (!eval_prod(pat)) {
            matched = false;
            break;
          }
        }
        if (matched) {
          auto operand = eval_prec_table(p, lvl.precedence);
          if (!operand) {
            restore_cursor(saved);
            return std::nullopt;
          }
          lhs = eval_action(op.action, {*operand});
          break;
        }
        restore_cursor(saved);
      }
      if (lhs) {
        break;
      }
    }

    if (!lhs) {
      lhs = eval_prod(p->inner);
      if (!lhs) {
        if (current_offset() == start_off) {
          record_failure("expression", true);
        }
        return std::nullopt;
      }
    }

    // Climb infix, postfix, and mixfix operators
    while (true) {
      bool advanced = false;
      for (std::size_t li = p->levels.size(); li > 0; --li) {
        const auto& lvl = p->levels[li - 1];
        if (lvl.assoc == ast::AssocKind::Prefix || lvl.precedence < min_prec) {
          continue;
        }
        for (const auto& op : lvl.ops) {
          SavedCursor saved = save_cursor();
          std::vector<EvalValue> captured;
          captured.push_back(*lhs);
          bool pat_ok = !op.pattern.empty();
          for (std::size_t pi = 0; pi < op.pattern.size(); ++pi) {
            auto pv = eval_prod(op.pattern[pi]);
            if (!pv) {
              pat_ok = false;
              break;
            }
            if (op.pattern[pi] && op.pattern[pi]->kind != ir::Prod::Kind::Tok) {
              captured.push_back(std::move(*pv));
            }
          }
          if (!pat_ok) {
            restore_cursor(saved);
            continue;
          }

          if (lvl.assoc == ast::AssocKind::Postfix) {
            lhs = eval_action(op.action, captured);
            advanced = true;
            break;
          }

          const int next_min = (lvl.assoc == ast::AssocKind::InfixR ||
                                lvl.assoc == ast::AssocKind::Mixfix)
                                   ? lvl.precedence
                                   : (lvl.precedence + 1);
          auto rhs = eval_prec_table(p, next_min);
          if (!rhs) {
            return std::nullopt;
          }
          captured.push_back(std::move(*rhs));
          lhs = eval_action(op.action, captured);
          advanced = true;
          if (lvl.assoc == ast::AssocKind::Infix) {
            return lhs;
          }
          break;
        }
        if (advanced) {
          break;
        }
      }
      if (!advanced) {
        break;
      }
    }

    return lhs;
  }

  std::optional<EvalValue> eval_prod(const ir::ProdPtr& p) {
    if (++steps_ > 10000000) throw std::runtime_error("parse step budget exceeded");
    if (!p) {
      return EvalValue::make_unit();
    }
    switch (p->kind) {
      case ir::Prod::Kind::Eps:
        return EvalValue::make_unit();

      case ir::Prod::Kind::Tok:
        return match_token(p->token_id);

      case ir::Prod::Kind::Ref:
        return eval_rule(p->rule_id);

      case ir::Prod::Kind::Seq: {
        if (p->items.empty()) {
          return EvalValue::make_unit();
        }
        const bool is_factored =
            p->items.size() >= 2 && p->items.back() && p->items.back()->kind == ir::Prod::Kind::Alt;
        const std::size_t prefix_count = is_factored ? (p->items.size() - 1) : 0;
        const std::size_t saved_prefix_size = prefix_stack_.size();

        EvalValue last = EvalValue::make_unit();
        for (std::size_t i = 0; i < p->items.size(); ++i) {
          auto item_val = eval_prod(p->items[i]);
          if (!item_val) {
            prefix_stack_.resize(saved_prefix_size);
            return std::nullopt;
          }
          if (i < prefix_count) {
            if(prefix_stack_.size()>=64)throw std::runtime_error("factored prefix depth exceeds 64");
            prefix_stack_.push_back(*item_val);
          }
          last = std::move(*item_val);
        }
        prefix_stack_.resize(saved_prefix_size);
        return last;
      }

      case ir::Prod::Kind::Alt: {
        SavedCursor saved = save_cursor();
        for (const auto& item : p->items) {
          auto res = eval_prod(item);
          if (res) {
            return res;
          }
          restore_cursor(saved);
        }
        return std::nullopt;
      }

      case ir::Prod::Kind::Rep: {
        std::vector<EvalValue> elems;
        int count = 0;
        while (!p->max_rep || count < *p->max_rep) {
          SavedCursor saved = save_cursor();
          const std::size_t off_before = current_offset();
          auto item = eval_prod(p->inner);
          if (!item) {
            restore_cursor(saved);
            break;
          }
          elems.push_back(std::move(*item));
          ++count;
          if (current_offset() == off_before) {
            break;
          }
        }
        if (count < p->min_rep) {
          return std::nullopt;
        }
        return EvalValue::make_list(std::move(elems));
      }

      case ir::Prod::Kind::Look: {
        SavedCursor saved = save_cursor();
        const bool prev_suppress = suppress_err_;
        if (p->polarity == ir::Polarity::Negative) {
          suppress_err_ = true;
        }
        auto res = eval_prod(p->inner);
        suppress_err_ = prev_suppress;
        restore_cursor(saved);
        if (p->polarity == ir::Polarity::Positive) {
          return res ? std::optional<EvalValue>(EvalValue::make_unit()) : std::nullopt;
        }
        if (!res) {
          return EvalValue::make_unit();
        }
        record_failure("not followed by lookahead");
        return std::nullopt;
      }

      case ir::Prod::Kind::Act: {
        std::vector<EvalValue> child_vals;
        child_vals.reserve(p->items.size());
        for (const auto& item : p->items) {
          auto v = eval_prod(item);
          if (!v) {
            return std::nullopt;
          }
          child_vals.push_back(std::move(*v));
        }
        return eval_action(p->action, child_vals);
      }

      case ir::Prod::Kind::PrecTable:
        return eval_prec_table(p, 0);

      case ir::Prod::Kind::Ctx: {
        if (!p->ctx_var.empty()) {
          auto it = ctx_env_.find(p->ctx_var);
          if (it == ctx_env_.end() || it->second.kind != EvalValue::Kind::String) {
            record_failure(p->ctx_var);
            return std::nullopt;
          }
          return match_exactly(it->second.string_value);
        }
        SavedCursor saved = save_cursor();
        std::vector<EvalValue> step_vals;
        step_vals.reserve(p->ctx_steps.size());
        for (const auto& step : p->ctx_steps) {
          auto sv = eval_prod(step.prod);
          if (!sv) {
            restore_cursor(saved);
            return std::nullopt;
          }
          if (!step.binder.empty()) {
            ctx_env_[step.binder] = *sv;
          }
          step_vals.push_back(std::move(*sv));
        }
        EvalValue result = eval_action(p->action, step_vals);
        ctx_env_ = std::move(saved.ctx_env);
        return result;
      }

      case ir::Prod::Kind::Label: {
        const std::size_t start_off = current_offset();
        auto res = eval_prod(p->inner);
        if (!res && current_offset() == start_off) {
          record_failure(p->label, true);
        }
        return res;
      }

      case ir::Prod::Kind::Recover: {
        auto res = eval_prod(p->inner);
        if (res) {
          return res;
        }
        had_recovery_error_ = true;
        // Skip tokens/characters until sync matches or EOF
        while (true) {
          SavedCursor saved = save_cursor();
          const bool prev_suppress = suppress_err_;
          suppress_err_ = true;
          auto sync_ok = eval_prod(p->sync);
          suppress_err_ = prev_suppress;
          if (sync_ok) {
            return EvalValue::make_unit();
          }
          restore_cursor(saved);
          if (!scannerless_) {
            if (tok_pos_ >= tokens_.size() || tokens_[tok_pos_].is_eof) {
              return std::nullopt;
            }
            ++tok_pos_;
          } else {
            if (char_pos_ >= input_.size()) {
              return std::nullopt;
            }
            advance_pos(input_.substr(char_pos_, 1), line_, col_);
            ++char_pos_;
          }
        }
      }
    }
    return std::nullopt;
  }
};

// ---------------------------------------------------------------------------
// Grammar-guided deterministic sentence generator for fuzzing
// ---------------------------------------------------------------------------

class SentenceGenerator {
public:
  SentenceGenerator(const ir::Grammar& grammar, std::uint64_t seed)
      : grammar_(grammar), rng_(seed) {
    compile_regexes();
    for (const auto& t : grammar_.gTokens) {
      if (t.is_skip) {
        has_default_skip_ = true;
      }
      if (!t.is_regex && t.priority >= 100) {
        default_high_literals_.push_back(t.pattern);
      }
    }
  }

  std::string generate_start() {
    mode_stack_ = {"default"};
    ctx_env_.clear();
    out_.clear();
    last_had_sep_ = true;

    std::string start = "program";
    if (!grammar_.gStarts.empty()) {
      start = grammar_.gStarts[0];
    } else if (!grammar_.gRules.empty()) {
      start = grammar_.gRules.begin()->first;
    }
    gen_rule(start, 0);
    return out_;
  }

private:
  const ir::Grammar& grammar_;
  std::mt19937_64 rng_;
  std::unordered_map<std::string, RegexAstPtr> regex_asts_;
  bool has_default_skip_ = false;
  std::vector<std::string> default_high_literals_;
  std::vector<std::string> mode_stack_ = {"default"};
  std::unordered_map<std::string, std::string> ctx_env_;
  std::string out_;
  bool last_had_sep_ = true;

  void compile_regexes() {
    auto add_list = [&](const std::vector<ir::TokenDef>& toks) {
      for (const auto& t : toks) {
        if (t.is_regex) {
          RegexParser p(t.pattern);
          regex_asts_[t.id] = p.parse();
        }
      }
    };
    add_list(grammar_.gTokens);
    for (const auto& m : grammar_.gModes) {
      add_list(m.tokens);
    }
  }

  const ir::TokenDef* find_in_active_mode(const std::string& id) const {
    const std::string& m = mode_stack_.empty() ? std::string("default") : mode_stack_.back();
    if (m != "default") {
      for (const auto& mode : grammar_.gModes) {
        if (mode.name == m) {
          for (const auto& t : mode.tokens) {
            if (t.id == id) {
              return &t;
            }
          }
        }
      }
    }
    return ir::find_token(grammar_, id);
  }

  bool is_shadowed_by_literal(const std::string& s) const {
    if (mode_stack_.back() != "default") {
      return false;
    }
    for (const auto& lit : default_high_literals_) {
      if (lit == s) {
        return true;
      }
    }
    return false;
  }

  std::string eval_action_capture(const ir::ActionExpr& act,
                                  const std::vector<std::string>& child_caps) const {
    switch (act.kind) {
      case ir::ActionExpr::Kind::Arg:
      case ir::ActionExpr::Kind::Identity:
        if (act.arg_index >= 0 &&
            static_cast<std::size_t>(act.arg_index) < child_caps.size()) {
          return child_caps[static_cast<std::size_t>(act.arg_index)];
        }
        return "";
      case ir::ActionExpr::Kind::StringLit:
        return act.name;
      default:
        return child_caps.empty() ? std::string{} : child_caps[0];
    }
  }

  std::string emit_token(const std::string& token_id) {
    if (token_id == "eof") {
      return "";
    }
    const ir::TokenDef* td = find_in_active_mode(token_id);
    std::string text;
    if (!td) {
      text = token_id;
    } else if (!td->is_regex) {
      text = td->pattern;
    } else {
      auto ast_it = regex_asts_.find(td->id);
      if (ast_it != regex_asts_.end()) {
        for (int attempt = 0; attempt < 5; ++attempt) {
          text = sample_from_regex_ast(ast_it->second, rng_);
          if (!text.empty() && !is_shadowed_by_literal(text)) {
            break;
          }
        }
        if (text.empty() || is_shadowed_by_literal(text)) {
          text += "_v";
        }
      } else {
        text = "x";
      }
    }

    if (has_default_skip_ && mode_stack_.back() == "default" && !out_.empty() && !last_had_sep_) {
      out_.push_back(' ');
    }
    out_ += text;
    last_had_sep_ = false;

    if (td) {
      if (td->action == ir::LexActionKind::Push && !td->target_mode.empty()) {
        mode_stack_.push_back(td->target_mode);
      } else if (td->action == ir::LexActionKind::Pop && mode_stack_.size() > 1) {
        mode_stack_.pop_back();
      }
    }
    return text;
  }

  std::string gen_rule(const std::string& rule_id, int depth) {
    auto it = grammar_.gRules.find(rule_id);
    if (it == grammar_.gRules.end()) {
      return "";
    }
    return gen_prod(it->second.rBody, depth + 1);
  }

  std::string gen_prod(const ir::ProdPtr& p, int depth) {
    if (!p) {
      return "";
    }
    switch (p->kind) {
      case ir::Prod::Kind::Eps:
      case ir::Prod::Kind::Look:
        return "";
      case ir::Prod::Kind::Tok:
        return emit_token(p->token_id);
      case ir::Prod::Kind::Ref:
        return gen_rule(p->rule_id, depth);
      case ir::Prod::Kind::Seq: {
        std::string last;
        for (const auto& it : p->items) {
          last = gen_prod(it, depth);
        }
        return last;
      }
      case ir::Prod::Kind::Act: {
        std::vector<std::string> child_caps;
        child_caps.reserve(p->items.size());
        for (const auto& it : p->items) {
          child_caps.push_back(gen_prod(it, depth));
        }
        return eval_action_capture(p->action, child_caps);
      }
      case ir::Prod::Kind::Alt: {
        if (p->items.empty()) {
          return "";
        }
        std::size_t idx = 0;
        if (depth >= 6) {
          // Prefer a simple non-recursive / token-only branch at high depth
          std::vector<std::size_t> safe;
          for (std::size_t i = 0; i < p->items.size(); ++i) {
            if (p->items[i] &&
                (p->items[i]->kind == ir::Prod::Kind::Tok ||
                 p->items[i]->kind == ir::Prod::Kind::Eps ||
                 (p->items[i]->kind == ir::Prod::Kind::Act &&
                  (p->items[i]->items.empty() ||
                   p->items[i]->items[0]->kind == ir::Prod::Kind::Tok)))) {
              safe.push_back(i);
            }
          }
          if (!safe.empty()) {
            std::uniform_int_distribution<std::size_t> dist(0, safe.size() - 1);
            idx = safe[dist(rng_)];
          } else {
            idx = p->items.size() - 1;
          }
        } else {
          std::uniform_int_distribution<std::size_t> dist(0, p->items.size() - 1);
          idx = dist(rng_);
        }
        return gen_prod(p->items[idx], depth);
      }
      case ir::Prod::Kind::Rep: {
        int lo = p->min_rep;
        int hi = p->max_rep ? *p->max_rep : (depth >= 5 ? lo : lo + 2);
        std::uniform_int_distribution<int> dist(lo, std::max(lo, hi));
        int n = dist(rng_);
        for (int i = 0; i < n; ++i) {
          gen_prod(p->inner, depth + 1);
        }
        return "";
      }
      case ir::Prod::Kind::PrecTable: {
        std::uniform_int_distribution<int> coin(0, 99);
        if (depth >= 5 || p->levels.empty() || coin(rng_) < 65) {
          return gen_prod(p->inner, depth + 1);
        }
        std::uniform_int_distribution<std::size_t> ldist(0, p->levels.size() - 1);
        const auto& lvl = p->levels[ldist(rng_)];
        if (lvl.ops.empty()) {
          return gen_prod(p->inner, depth + 1);
        }
        std::uniform_int_distribution<std::size_t> odist(0, lvl.ops.size() - 1);
        const auto& op = lvl.ops[odist(rng_)];
        if (lvl.assoc == ast::AssocKind::Prefix) {
          for (const auto& pat : op.pattern) {
            gen_prod(pat, depth + 1);
          }
          gen_prod(p->inner, depth + 1);
        } else if (lvl.assoc == ast::AssocKind::Postfix) {
          gen_prod(p->inner, depth + 1);
          for (const auto& pat : op.pattern) {
            gen_prod(pat, depth + 1);
          }
        } else {
          gen_prod(p->inner, depth + 1);
          for (const auto& pat : op.pattern) {
            gen_prod(pat, depth + 1);
          }
          gen_prod(p->inner, depth + 1);
        }
        return "";
      }
      case ir::Prod::Kind::Ctx: {
        if (!p->ctx_var.empty()) {
          std::string val = ctx_env_.contains(p->ctx_var) ? ctx_env_.at(p->ctx_var) : "x";
          if (has_default_skip_ && mode_stack_.back() == "default" && !out_.empty() &&
              !last_had_sep_) {
            out_.push_back(' ');
          }
          out_ += val;
          last_had_sep_ = false;
          return val;
        }
        auto saved_ctx = ctx_env_;
        std::vector<std::string> step_caps;
        step_caps.reserve(p->ctx_steps.size());
        for (const auto& st : p->ctx_steps) {
          std::string cap = gen_prod(st.prod, depth + 1);
          if (!st.binder.empty()) {
            ctx_env_[st.binder] = cap;
          }
          step_caps.push_back(std::move(cap));
        }
        std::string res = eval_action_capture(p->action, step_caps);
        ctx_env_ = std::move(saved_ctx);
        return res;
      }
      case ir::Prod::Kind::Label:
      case ir::Prod::Kind::Recover:
        return gen_prod(p->inner, depth);
    }
    return "";
  }
};

} // namespace parsley::eval::detail

export namespace parsley::eval {

inline EvalResult parse_input(const ir::Grammar& grammar,
                              std::string_view input,
                              std::string_view start_rule = "") {
  try {
    detail::Interpreter interp(grammar, input);
    return interp.run(start_rule);
  } catch (const std::exception& e) {
    EvalResult r; r.error = EvalParseError{1, 1, "valid input within resource limits", "input", e.what()}; return r;
  }
}

inline ExampleRunSummary run_examples(const ir::Grammar& grammar, DiagnosticBag& diags) {
  ExampleRunSummary summary;
  summary.total = grammar.gExamples.size();

  for (const auto& ex : grammar.gExamples) {
    EvalResult res = parse_input(grammar, ex.input);
    ExampleRunResult er;
    er.name = ex.name;

    if (ex.expect_success) {
      if (!res.ok) {
        er.passed = false;
        const std::string msg = res.error ? res.error->message : "parse failed";
        er.message = "expected success, but parse failed at (" +
                     std::to_string(res.error ? res.error->line : 1) + ", " +
                     std::to_string(res.error ? res.error->col : 1) + "): " + msg;
        Diagnostic d;
        d.code = "E0600";
        d.severity = Severity::Error;
        d.message = "example `" + ex.name + "` failed: " + er.message;
        d.span = ex.span;
        d.label = "expected successful parse";
        diags.push(std::move(d));
        ++summary.failed;
      } else if (ex.expected_ast && !matches_pattern(res.value, *ex.expected_ast)) {
        er.passed = false;
        er.message = "AST mismatch: got `" + format_value(res.value) + "`";
        Diagnostic d;
        d.code = "E0600";
        d.severity = Severity::Error;
        d.message = "example `" + ex.name + "` failed: " + er.message;
        d.span = ex.span;
        d.label = "parsed AST did not match `parses-to` pattern";
        diags.push(std::move(d));
        ++summary.failed;
      } else {
        er.passed = true;
        er.message = "ok: " + format_value(res.value);
        ++summary.passed;
      }
    } else {
      if (res.ok) {
        er.passed = false;
        er.message = "expected failure at (" + std::to_string(ex.fail_line) + ", " +
                     std::to_string(ex.fail_col) + "), but input parsed to `" +
                     format_value(res.value) + "`";
        Diagnostic d;
        d.code = "E0600";
        d.severity = Severity::Error;
        d.message = "example `" + ex.name + "` failed: " + er.message;
        d.span = ex.span;
        d.label = "expected parse error here";
        diags.push(std::move(d));
        ++summary.failed;
      } else {
        const EvalParseError err =
            res.error.value_or(EvalParseError{1, 1, "", "", "unknown error"});
        const bool pos_ok = (err.line == ex.fail_line && err.col == ex.fail_col);
        const bool exp_ok =
            ex.expecting.empty() || err.expected == ex.expecting ||
            err.expected.find(ex.expecting) != std::string::npos ||
            err.message.find(ex.expecting) != std::string::npos;
        if (!pos_ok || !exp_ok) {
          er.passed = false;
          er.message = "expected failure at (" + std::to_string(ex.fail_line) + ", " +
                       std::to_string(ex.fail_col) + ") expecting \"" + ex.expecting +
                       "\", got (" + std::to_string(err.line) + ", " +
                       std::to_string(err.col) + ") expecting \"" + err.expected + "\"";
          Diagnostic d;
          d.code = "E0600";
          d.severity = Severity::Error;
          d.message = "example `" + ex.name + "` failed: " + er.message;
          d.span = ex.span;
          d.label = "error location or expected token mismatch";
          diags.push(std::move(d));
          ++summary.failed;
        } else {
          er.passed = true;
          er.message = "failed as expected at (" + std::to_string(err.line) + ", " +
                       std::to_string(err.col) + ") expecting \"" + err.expected + "\"";
          ++summary.passed;
        }
      }
    }

    summary.cases.push_back(std::move(er));
  }

  return summary;
}

inline FuzzSummary fuzz_grammar(const ir::Grammar& grammar,
                                std::size_t count,
                                std::uint64_t seed = 42) {
  FuzzSummary summary;
  summary.attempts = count;
  detail::SentenceGenerator gen(grammar, seed);

  for (std::size_t i = 0; i < count; ++i) {
    std::string sentence = gen.generate_start();
    ++summary.generated;
    EvalResult res = parse_input(grammar, sentence);
    if (res.ok) {
      ++summary.passed;
    } else {
      ++summary.failed;
      std::string err_msg = res.error ? res.error->message : "parse failed";
      summary.failures.push_back("input=\"" + sentence + "\" -> " + err_msg);
    }
  }

  return summary;
}

} // namespace parsley::eval
