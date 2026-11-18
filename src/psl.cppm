module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.psl;

import parsley.diagnostic;
import parsley.ast;
import parsley.ir;
import parsley.eval;
import parsley.c23_text;

export namespace parsley::psl {

// ---------------------------------------------------------------------------
// Provenance, status, and severity metadata
// ---------------------------------------------------------------------------

enum class PslStatus {
  Draft = 0,
  Reviewed = 1,
  Verified = 2,
};

inline std::string_view status_to_string(PslStatus s) noexcept {
  switch (s) {
    case PslStatus::Draft:
      return "draft";
    case PslStatus::Reviewed:
      return "reviewed";
    case PslStatus::Verified:
      return "verified";
  }
  return "draft";
}

inline std::optional<PslStatus> parse_status_name(std::string_view s) noexcept {
  if (s == "draft") {
    return PslStatus::Draft;
  }
  if (s == "reviewed") {
    return PslStatus::Reviewed;
  }
  if (s == "verified") {
    return PslStatus::Verified;
  }
  return std::nullopt;
}

enum class PslSeverity {
  Error,
  Warning,
  Note,
};

inline std::string_view psl_severity_to_string(PslSeverity s) noexcept {
  switch (s) {
    case PslSeverity::Error:
      return "Error";
    case PslSeverity::Warning:
      return "Warning";
    case PslSeverity::Note:
      return "Note";
  }
  return "Error";
}

enum class PslLintCategory {
  General,
  Style,
  Correctness,
  Performance,
  Complexity,
  Security,
};

inline std::string_view lint_category_to_string(PslLintCategory c) noexcept {
  switch (c) {
    case PslLintCategory::General:
      return "General";
    case PslLintCategory::Style:
      return "Style";
    case PslLintCategory::Correctness:
      return "Correctness";
    case PslLintCategory::Performance:
      return "Performance";
    case PslLintCategory::Complexity:
      return "Complexity";
    case PslLintCategory::Security:
      return "Security";
  }
  return "General";
}

struct PslProvenance {
  std::string source_name;
  std::string source_ref;
  std::string source_hash_or_date;
  PslStatus status = PslStatus::Draft;
  bool has_status = false;
  double confidence = 1.0;
  bool has_confidence = false;
  bool locked = false;
  std::string supersedes_old;
  std::string supersedes_reason;
  std::string raw_pragmas;
  Span span;
};

// ---------------------------------------------------------------------------
// PSL Surface AST / IR types
// ---------------------------------------------------------------------------

struct PslImportDecl {
  std::string module_name;
  bool is_qualified = false;
  std::optional<std::string> alias;
  bool is_hiding = false;
  std::vector<std::string> items;
  Span span;
};

struct PslSourceDecl {
  std::string name;
  std::string origin_kind;
  std::string title;
  std::string revision;
  std::string raw_text;
  PslProvenance provenance;
  Span span;
};

struct PslTypeExpr;
using PslTypeExprPtr = std::shared_ptr<PslTypeExpr>;

struct PslTypeExpr {
  enum class Kind {
    Named,
    Var,
    App,
    List,
    Tuple,
    Arrow,
    Forall,
  };

  Kind kind = Kind::Named;
  std::string name;
  std::vector<std::string> forall_vars;
  PslTypeExprPtr left;
  PslTypeExprPtr right;
  std::vector<PslTypeExprPtr> args;
  Span span;
};

struct PslTypeDecl {
  std::string name;
  std::vector<std::string> params;
  PslTypeExprPtr target;
  std::vector<std::string> union_ctors;
  PslProvenance provenance;
  Span span;
};

struct PslTyconDecl {
  std::string name;
  std::vector<std::string> params;
  PslProvenance provenance;
  Span span;
};

enum class ShadowingPolicy {
  Allow,
  Warn,
  Error,
};

struct PslKindDecl {
  std::string name;
  ShadowingPolicy shadowing = ShadowingPolicy::Allow;
  PslProvenance provenance;
  Span span;
};

struct PslBuiltinDecl {
  std::string name;
  std::string symbol_kind;
  PslTypeExprPtr value_type;
  PslProvenance provenance;
  Span span;
};

struct PslDiagDecl {
  std::string code;
  PslSeverity severity = PslSeverity::Error;
  std::string template_text;
  PslProvenance provenance;
  Span span;
};

struct PslPattern;
using PslPatternPtr = std::shared_ptr<PslPattern>;

struct PslPattern {
  enum class Kind {
    Wildcard,
    Var,
    TypedVar,
    Number,
    String,
    Ctor,
    List,
  };

  Kind kind = Kind::Wildcard;
  std::string name;
  std::string type_name;
  double number_value = 0.0;
  std::vector<PslPatternPtr> args;
  Span span;
};

struct PslExpr;
using PslExprPtr = std::shared_ptr<PslExpr>;

struct PslExpr {
  enum class Kind {
    Var,
    Number,
    String,
    Bool,
    List,
    ListComp,
    Call,
    BinOp,
    UnaryOp,
    FieldAccess,
    IfThenElse,
    RecordLit,
    Ctor,
  };

  Kind kind = Kind::Var;
  std::string name;
  double number_value = 0.0;
  bool bool_value = false;
  std::vector<PslExprPtr> args;
  std::vector<std::pair<std::string, PslExprPtr>> fields;
  // List comprehension: [head | bind_var <- source, filter]
  std::string comp_var;
  PslExprPtr comp_source;
  PslExprPtr comp_filter;
  Span span;
};

struct PslOrFail {
  bool present = false;
  std::string diag_code;
  std::vector<std::pair<std::string, PslExprPtr>> args;
  Span span;
};

struct PslNodeStmt {
  enum class Kind {
    AssignAttr,   // type = <expr> or custom_attr = <expr>
    LookupBind,   // sym <- lookup Kind x `orFail` E001 { ... }
    FreshBind,    // a <- fresh
    Declare,      // declare Kind name_expr type_expr
    Scope,        // scope child_var $ <nested_stmt>
    Require,      // require cond `orFail` E004 { ... }
    Unify,        // unify t1 t2 `orFail` E010 { ... }
  };

  Kind kind = Kind::AssignAttr;
  std::string target_var;
  std::string sym_kind;
  PslExprPtr expr;
  PslExprPtr expr2;
  std::vector<std::shared_ptr<PslNodeStmt>> nested;
  PslOrFail or_fail;
  Span span;
};

struct PslNodeRule {
  PslPatternPtr pattern;
  std::vector<PslNodeStmt> stmts;
  PslProvenance provenance;
  Span span;
};

struct PslFlowExpr {
  enum class Kind {
    Empty,
    Chain,
    Branch,
    Loop,
    Jump,
    JumpOut,
    JumpTo,
    Linear,
  };

  Kind kind = Kind::Linear;
  std::string arg_name;
  std::vector<std::string> list_args;
  std::string target_ctor;
  bool target_exit = false;
  Span span;
};

struct PslFlowRule {
  PslPatternPtr pattern;
  PslFlowExpr expr;
  PslProvenance provenance;
  Span span;
};

enum class AnalysisDir {
  Forward,
  Backward,
};

enum class AnalysisMeet {
  Must,
  May,
};

struct PslTransferClause {
  PslPatternPtr pattern;
  PslExprPtr expr;
  Span span;
};

struct PslAnalysisRule {
  std::string name;
  AnalysisDir direction = AnalysisDir::Forward;
  AnalysisMeet meet = AnalysisMeet::Must;
  std::string domain_type;
  PslExprPtr entry_expr;
  std::vector<PslTransferClause> gen_clauses;
  std::vector<PslTransferClause> kill_clauses;
  PslProvenance provenance;
  Span span;
};

struct PslLintFix {
  bool present = false;
  std::string title;
  bool is_safe = true;
  PslExprPtr target_span_expr;
  PslExprPtr replacement_expr;
  Span span;
};

struct PslLintRule {
  std::string name;
  PslSeverity severity = PslSeverity::Warning;
  PslLintCategory category = PslLintCategory::General;
  PslPatternPtr match_pattern;
  PslExprPtr when_expr;
  std::string say_template;
  PslLintFix fix;
  PslProvenance provenance;
  Span span;
};

struct PslExampleCase {
  bool expect_fire = true;
  std::string input;
  Span span;
};

struct PslExampleBlock {
  std::string target_name;
  std::vector<PslExampleCase> cases;
  Span span;
};

struct PslProfileDecl {
  std::string name;
  std::vector<std::string> lint_names;
  PslProvenance provenance;
  Span span;
};

struct PslProjectDecl {
  std::string unit_ctor;
  std::string import_ctor;
  std::string export_ctor;
  std::string prelude;
  std::vector<std::string> search_paths;
  bool cycles_allowed = false;
  PslProvenance provenance;
  Span span;
};

struct PslCoerceDecl {
  PslTypeExprPtr from_type;
  PslTypeExprPtr to_type;
  PslProvenance provenance;
  Span span;
};

struct PslOverloadDecl {
  std::string name;
  std::vector<PslTypeExprPtr> signatures;
  PslProvenance provenance;
  Span span;
};

// ---------------------------------------------------------------------------
// 5-Pass Execution Plan
// ---------------------------------------------------------------------------

enum class PslPassKind {
  ScopeAndDeclare = 1,
  NameResolution = 2,
  TypeInference = 3,
  Dataflow = 4,
  LintAndFix = 5,
};

struct PslPassStep {
  PslPassKind pass = PslPassKind::ScopeAndDeclare;
  std::string description;
  std::vector<std::string> active_ctors;
};

struct PslExecutionPlan {
  std::array<PslPassStep, 5> passes;
  std::vector<std::string> attribute_eval_order;
  bool verified_acyclic = false;
};

struct PslModule {
  std::string module_name;
  std::string language_version = "Sem1";
  PslProvenance module_provenance;
  std::vector<PslImportDecl> imports;
  std::vector<PslSourceDecl> sources;
  std::vector<PslTypeDecl> types;
  std::vector<PslTyconDecl> tycons;
  std::vector<PslKindDecl> kinds;
  std::vector<PslBuiltinDecl> builtins;
  std::vector<PslDiagDecl> diags;
  std::vector<PslCoerceDecl> coercions;
  std::vector<PslOverloadDecl> overloads;
  std::vector<PslNodeRule> node_rules;
  std::vector<PslFlowRule> flow_rules;
  std::vector<PslAnalysisRule> analyses;
  std::vector<PslLintRule> lints;
  std::vector<PslExampleBlock> examples;
  std::vector<PslProfileDecl> profiles;
  std::optional<PslProjectDecl> project;
  PslExecutionPlan pass_plan;
  Span span;
};

// ---------------------------------------------------------------------------
// Checker, Interpreter, Example Harness, Lockfile & Codegen Types
// ---------------------------------------------------------------------------

struct PslCheckOptions {
  PslStatus min_status = PslStatus::Draft;
  bool require_examples = true;
  bool cross_check_grammar = true;
};

struct PslDiagnostic {
  std::string code;
  std::string lint_name;
  PslSeverity severity = PslSeverity::Error;
  std::string message;
  std::string fix_title;
  std::string fix_replacement;
  bool has_fix = false;
  bool fix_safe = true;
  Span span;
};

struct PslAnalysisResult {
  bool ok = true;
  std::vector<PslDiagnostic> diagnostics;
  std::size_t nodes_visited = 0;
  std::size_t symbols_declared = 0;
  std::size_t type_unifications = 0;
  std::size_t cfg_blocks = 0;
  std::size_t lints_evaluated = 0;
  std::unordered_map<int, std::string> inferred_types;
};

struct PslExampleResult {
  std::string lint_name;
  bool expect_fire = true;
  std::string input;
  bool passed = false;
  std::string message;
};

struct PslExampleSummary {
  std::size_t total = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<PslExampleResult> cases;
};

struct PslLockData {
  std::string module_name;
  std::string language_version = "Sem1";
  std::string ast_types_hash;
  std::map<std::string, std::string> type_hashes;
};

struct PslLockDiff {
  bool matches = true;
  std::vector<std::string> changed_types;
  std::vector<std::string> added_types;
  std::vector<std::string> removed_types;
};

struct PslGeneratedFile {
  std::string relative_path;
  std::string content;
};

struct PslCodegenOptions {
  bool emit_tests = true;
  PslStatus min_status = PslStatus::Draft;
  std::string profile;
  std::string module_override;
};

} // namespace parsley::psl

namespace parsley::psl::detail {

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

inline bool is_ident_start(unsigned char c) noexcept {
  return std::isalpha(c) || c == '_';
}

inline bool is_ident_cont(unsigned char c) noexcept {
  return std::isalnum(c) || c == '_' || c == '\'';
}

inline std::string to_lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

inline std::string trim_ascii(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return std::string(s.substr(b, e - b));
}

// ---------------------------------------------------------------------------
// .psem Lexer
// ---------------------------------------------------------------------------

enum class PslTokKind {
  Eof,
  Ident,
  UpperIdent,
  Number,
  String,
  Pragma,
  BacktickOp,
  ColonColon,
  Arrow,
  LeftArrow,
  Eq,
  Neq,
  Le,
  Ge,
  AndAnd,
  OrOr,
  Append,
  EqualSign,
  Pipe,
  Dollar,
  Dot,
  Comma,
  Lt,
  Gt,
  Plus,
  Minus,
  Star,
  Slash,
  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  Semicolon,
};

struct PslToken {
  PslTokKind kind = PslTokKind::Eof;
  std::string text;
  double number_value = 0.0;
  Span span;
};

inline std::vector<PslToken> lex_psem(std::string_view src,
                                      std::string_view filename,
                                      DiagnosticBag& diags) {
  std::vector<PslToken> out;
  std::size_t pos = 0;
  int line = 1;
  int col = 1;

  auto advance = [&](std::size_t n = 1) {
    for (std::size_t i = 0; i < n && pos < src.size(); ++i) {
      if (src[pos] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
      ++pos;
    }
  };

  auto make_span = [&](int sl, int sc, std::size_t so) {
    Span sp;
    sp.file = std::string(filename);
    sp.start_line = sl;
    sp.start_col = sc;
    sp.end_line = line;
    sp.end_col = col;
    sp.start_offset = so;
    sp.end_offset = pos;
    return sp;
  };

  while (pos < src.size()) {
    const unsigned char c = static_cast<unsigned char>(src[pos]);
    if (std::isspace(c)) {
      advance();
      continue;
    }

    // Line comment --
    if (c == '-' && pos + 1 < src.size() && src[pos + 1] == '-') {
      while (pos < src.size() && src[pos] != '\n') {
        advance();
      }
      continue;
    }

    // Pragma {-# ... #-}
    if (c == '{' && pos + 2 < src.size() && src[pos + 1] == '-' && src[pos + 2] == '#') {
      const int sl = line;
      const int sc = col;
      const std::size_t so = pos;
      advance(3);
      std::string body;
      bool closed = false;
      while (pos < src.size()) {
        if (src[pos] == '#' && pos + 2 < src.size() && src[pos + 1] == '-' && src[pos + 2] == '}') {
          advance(3);
          closed = true;
          break;
        }
        body.push_back(src[pos]);
        advance();
      }
      if (!closed) {
        diags.error("E0100", "unterminated PSL pragma `{-#`", make_span(sl, sc, so));
      }
      PslToken tok;
      tok.kind = PslTokKind::Pragma;
      tok.text = trim_ascii(body);
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    // Nested block comment {- ... -}
    if (c == '{' && pos + 1 < src.size() && src[pos + 1] == '-') {
      const int sl = line;
      const int sc = col;
      const std::size_t so = pos;
      advance(2);
      int depth = 1;
      while (pos < src.size() && depth > 0) {
        if (src[pos] == '{' && pos + 1 < src.size() && src[pos + 1] == '-') {
          ++depth;
          advance(2);
        } else if (src[pos] == '-' && pos + 1 < src.size() && src[pos + 1] == '}') {
          --depth;
          advance(2);
        } else {
          advance();
        }
      }
      if (depth > 0) {
        diags.error("E0100", "unterminated block comment `{-`", make_span(sl, sc, so));
      }
      continue;
    }

    const int sl = line;
    const int sc = col;
    const std::size_t so = pos;

    // String literal "..."
    if (c == '"') {
      advance();
      std::string s;
      bool closed = false;
      while (pos < src.size()) {
        char ch = src[pos];
        if (ch == '"') {
          advance();
          closed = true;
          break;
        }
        if (ch == '\\' && pos + 1 < src.size()) {
          advance();
          char esc = src[pos];
          switch (esc) {
            case 'n': s.push_back('\n'); break;
            case 'r': s.push_back('\r'); break;
            case 't': s.push_back('\t'); break;
            case '\\': s.push_back('\\'); break;
            case '"': s.push_back('"'); break;
            default: s.push_back(esc); break;
          }
          advance();
          continue;
        }
        s.push_back(ch);
        advance();
      }
      if (!closed) {
        diags.error("E0100", "unterminated string literal", make_span(sl, sc, so));
      }
      PslToken tok;
      tok.kind = PslTokKind::String;
      tok.text = std::move(s);
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    // Backtick operator `op`
    if (c == '`') {
      advance();
      std::string op;
      while (pos < src.size() && src[pos] != '`' && src[pos] != '\n') {
        op.push_back(src[pos]);
        advance();
      }
      if (pos < src.size() && src[pos] == '`') {
        advance();
      } else {
        diags.error("E0100", "unterminated backtick operator", make_span(sl, sc, so));
      }
      PslToken tok;
      tok.kind = PslTokKind::BacktickOp;
      tok.text = trim_ascii(op);
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    // Number literal
    if (std::isdigit(c)) {
      std::string raw;
      while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) {
        raw.push_back(src[pos]);
        advance();
      }
      if (pos + 1 < src.size() && src[pos] == '.' &&
          std::isdigit(static_cast<unsigned char>(src[pos + 1]))) {
        raw.push_back('.');
        advance();
        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) {
          raw.push_back(src[pos]);
          advance();
        }
      }
      PslToken tok;
      tok.kind = PslTokKind::Number;
      tok.text = raw;
      try {
        tok.number_value = std::stod(raw);
      } catch (...) {
        tok.number_value = 0.0;
      }
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    // Identifier / UpperIdent
    if (is_ident_start(c)) {
      std::string id;
      while (pos < src.size() && is_ident_cont(static_cast<unsigned char>(src[pos]))) {
        id.push_back(src[pos]);
        advance();
      }
      PslToken tok;
      tok.kind = std::isupper(static_cast<unsigned char>(id[0])) ? PslTokKind::UpperIdent
                                                                : PslTokKind::Ident;
      tok.text = std::move(id);
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    // Two-char operators
    auto match2 = [&](char a, char b, PslTokKind k, std::string_view txt) -> bool {
      if (pos + 1 < src.size() && src[pos] == a && src[pos + 1] == b) {
        advance(2);
        PslToken tok;
        tok.kind = k;
        tok.text = std::string(txt);
        tok.span = make_span(sl, sc, so);
        out.push_back(std::move(tok));
        return true;
      }
      return false;
    };

    if (match2(':', ':', PslTokKind::ColonColon, "::")) continue;
    if (match2('-', '>', PslTokKind::Arrow, "->")) continue;
    if (match2('<', '-', PslTokKind::LeftArrow, "<-")) continue;
    if (match2('=', '=', PslTokKind::Eq, "==")) continue;
    if (match2('/', '=', PslTokKind::Neq, "/=")) continue;
    if (match2('<', '=', PslTokKind::Le, "<=")) continue;
    if (match2('>', '=', PslTokKind::Ge, ">=")) continue;
    if (match2('&', '&', PslTokKind::AndAnd, "&&")) continue;
    if (match2('|', '|', PslTokKind::OrOr, "||")) continue;
    if (match2('<', '>', PslTokKind::Append, "<>")) continue;

    // Single-char punctuation/operators
    PslTokKind sk = PslTokKind::Eof;
    switch (c) {
      case '=': sk = PslTokKind::EqualSign; break;
      case '|': sk = PslTokKind::Pipe; break;
      case '$': sk = PslTokKind::Dollar; break;
      case '.': sk = PslTokKind::Dot; break;
      case ',': sk = PslTokKind::Comma; break;
      case '<': sk = PslTokKind::Lt; break;
      case '>': sk = PslTokKind::Gt; break;
      case '+': sk = PslTokKind::Plus; break;
      case '-': sk = PslTokKind::Minus; break;
      case '*': sk = PslTokKind::Star; break;
      case '/': sk = PslTokKind::Slash; break;
      case '(': sk = PslTokKind::LParen; break;
      case ')': sk = PslTokKind::RParen; break;
      case '[': sk = PslTokKind::LBracket; break;
      case ']': sk = PslTokKind::RBracket; break;
      case '{': sk = PslTokKind::LBrace; break;
      case '}': sk = PslTokKind::RBrace; break;
      case ';': sk = PslTokKind::Semicolon; break;
      default: break;
    }

    if (sk != PslTokKind::Eof) {
      PslToken tok;
      tok.kind = sk;
      tok.text = std::string(1, static_cast<char>(c));
      advance();
      tok.span = make_span(sl, sc, so);
      out.push_back(std::move(tok));
      continue;
    }

    diags.error("E0100",
                std::string("unexpected character in `.psem` file: `") +
                    static_cast<char>(c) + "`",
                make_span(sl, sc, so));
    advance();
  }

  PslToken eof_tok;
  eof_tok.kind = PslTokKind::Eof;
  eof_tok.span = make_span(line, col, pos);
  out.push_back(std::move(eof_tok));
  return out;
}

// ---------------------------------------------------------------------------
// Pragma parser helper
// ---------------------------------------------------------------------------

inline void parse_pragma_into(const std::string& raw_body,
                              const Span& sp,
                              PslProvenance& prov,
                              std::string& lang_version,
                              DiagnosticBag& diags) {
  if (!prov.raw_pragmas.empty()) {
    prov.raw_pragmas += "; ";
  }
  prov.raw_pragmas += raw_body;
  prov.span = prov.span.is_valid() ? Span::merge(prov.span, sp) : sp;

  // Split by ';'
  std::size_t start = 0;
  while (start <= raw_body.size()) {
    std::size_t semi = raw_body.find(';', start);
    std::string chunk = (semi == std::string::npos)
                            ? trim_ascii(std::string_view(raw_body).substr(start))
                            : trim_ascii(std::string_view(raw_body).substr(start, semi - start));
    if (!chunk.empty()) {
      // Tokenize chunk simply (respecting quotes)
      std::vector<std::string> parts;
      std::size_t i = 0;
      while (i < chunk.size()) {
        while (i < chunk.size() && std::isspace(static_cast<unsigned char>(chunk[i]))) {
          ++i;
        }
        if (i >= chunk.size()) break;
        if (chunk[i] == '"') {
          ++i;
          std::string s;
          while (i < chunk.size() && chunk[i] != '"') {
            if (chunk[i] == '\\' && i + 1 < chunk.size()) {
              ++i;
            }
            s.push_back(chunk[i++]);
          }
          if (i < chunk.size() && chunk[i] == '"') ++i;
          parts.push_back(std::move(s));
        } else {
          std::size_t b = i;
          while (i < chunk.size() && !std::isspace(static_cast<unsigned char>(chunk[i]))) {
            ++i;
          }
          parts.push_back(chunk.substr(b, i - b));
        }
      }

      if (!parts.empty()) {
        const std::string& head = parts[0];
        if (head == "LANGUAGE") {
          if (parts.size() >= 2) {
            lang_version = parts[1];
            if (lang_version != "Sem1" && lang_version != "Sem0") {
              diags.error("PSL-LANG",
                          "unsupported PSL language version `" + lang_version +
                              "` (expected `Sem1` or `Sem0`)",
                          sp);
            }
          }
        } else if (head == "SOURCE") {
          if (parts.size() >= 2) prov.source_name = parts[1];
          if (parts.size() >= 3) prov.source_ref = parts[2];
          if (parts.size() >= 4) prov.source_hash_or_date = parts[3];
        } else if (head == "STATUS") {
          if (parts.size() >= 2) {
            if (auto st = parse_status_name(parts[1])) {
              prov.status = *st;
              prov.has_status = true;
            } else {
              diags.error("PSL-STATUS",
                          "invalid STATUS `" + parts[1] +
                              "` (expected draft, reviewed, or verified)",
                          sp);
            }
          }
        } else if (head == "CONFIDENCE") {
          if (parts.size() >= 2) {
            try {
              double c = std::stod(parts[1]);
              if (c < 0.0 || c > 1.0) {
                diags.warning("PSL-CONFIDENCE",
                              "CONFIDENCE should be between 0.0 and 1.0", sp);
              }
              prov.confidence = c;
              prov.has_confidence = true;
            } catch (...) {
              diags.error("PSL-CONFIDENCE",
                          "invalid CONFIDENCE float `" + parts[1] + "`", sp);
            }
          }
        } else if (head == "LOCKED") {
          prov.locked = true;
        } else if (head == "SUPERSEDES") {
          if (parts.size() >= 2) prov.supersedes_old = parts[1];
          if (parts.size() >= 4 && parts[2] == "BY") {
            std::string reason;
            for (std::size_t k = 3; k < parts.size(); ++k) {
              if (!reason.empty()) reason.push_back(' ');
              reason += parts[k];
            }
            prov.supersedes_reason = std::move(reason);
          }
        }
      }
    }
    if (semi == std::string::npos) break;
    start = semi + 1;
  }
}

// ---------------------------------------------------------------------------
// .psem Recursive-Descent Parser
// ---------------------------------------------------------------------------

class PslParser {
public:
  PslParser(const std::vector<PslToken>& tokens,
            std::string_view filename,
            DiagnosticBag& diags)
      : toks_(tokens), filename_(filename), diags_(diags) {}

  PslModule parse_module() {
    PslModule mod;
    mod.span.file = filename_;

    PslProvenance pending;
    consume_pragmas(pending, mod.language_version);

    if (check_ident("semantics")) {
      Span hsp = peek().span;
      advance();
      if (peek().kind == PslTokKind::UpperIdent || peek().kind == PslTokKind::Ident) {
        mod.module_name = peek().text;
        advance();
        while (peek().kind == PslTokKind::Dot) {
          advance();
          if (peek().kind != PslTokKind::UpperIdent && peek().kind != PslTokKind::Ident) {
            error_here("expected component after dot in semantic module name");
            break;
          }
          mod.module_name += "." + peek().text;
          advance();
        }
      } else {
        error_here("expected module name after `semantics`");
      }
      // Optional parenthesized exports
      if (peek().kind == PslTokKind::LParen) {
        int depth = 0;
        do {
          if (peek().kind == PslTokKind::LParen) ++depth;
          else if (peek().kind == PslTokKind::RParen) --depth;
          advance();
        } while (!at_eof() && depth > 0);
      }
      expect_ident("where", "expected `where` after module name");
      mod.module_provenance = pending;
      mod.span = hsp;
    } else {
      error_here("expected `semantics <Module> where` header");
      mod.module_name = "Unnamed";
    }

    // Determine top-level column from first non-pragma token
    consume_pragmas(pending, mod.language_version);
    int top_col = at_eof() ? 1 : peek().span.start_col;
    if (top_col < 1) top_col = 1;

    while (!at_eof()) {
      PslProvenance decl_prov;
      if (pending.has_status || pending.has_confidence || !pending.source_name.empty() ||
          pending.locked || !pending.supersedes_old.empty() || !pending.raw_pragmas.empty()) {
        decl_prov = pending;
        pending = PslProvenance{};
      }
      consume_pragmas(decl_prov, mod.language_version);
      if (at_eof()) break;

      if (peek().span.start_col > top_col) {
        top_col = std::min(top_col, peek().span.start_col);
      }

      if (check_ident("import")) {
        mod.imports.push_back(parse_import());
      } else if (check_ident("source")) {
        auto s = parse_source(decl_prov);
        mod.sources.push_back(std::move(s));
      } else if (check_ident("type")) {
        mod.types.push_back(parse_type_decl(decl_prov, top_col));
      } else if (check_ident("tycon")) {
        mod.tycons.push_back(parse_tycon_decl(decl_prov));
      } else if (check_ident("kind")) {
        mod.kinds.push_back(parse_kind_decl(decl_prov, mod.language_version));
      } else if (check_ident("builtin")) {
        mod.builtins.push_back(parse_builtin_decl(decl_prov, mod.language_version));
      } else if (check_ident("diag")) {
        mod.diags.push_back(parse_diag_decl(decl_prov, mod.language_version));
      } else if (check_ident("coerce")) {
        mod.coercions.push_back(parse_coerce_decl(decl_prov));
      } else if (check_ident("overload")) {
        mod.overloads.push_back(parse_overload_decl(decl_prov, top_col));
      } else if (check_ident("node")) {
        mod.node_rules.push_back(parse_node_rule(decl_prov, top_col, mod.language_version));
      } else if (check_ident("flow")) {
        mod.flow_rules.push_back(parse_flow_rule(decl_prov));
      } else if (check_ident("analysis")) {
        mod.analyses.push_back(parse_analysis_rule(decl_prov, top_col, mod.language_version));
      } else if (check_ident("lint")) {
        mod.lints.push_back(parse_lint_rule(decl_prov, top_col, mod.language_version));
      } else if (check_ident("example")) {
        mod.examples.push_back(parse_example_block(top_col));
      } else if (check_ident("profile")) {
        mod.profiles.push_back(parse_profile_decl(decl_prov));
      } else if (check_ident("project")) {
        mod.project = parse_project_decl(decl_prov, top_col);
      } else {
        error_here("unexpected top-level token `" + peek().text + "` in `.psem` module");
        const int err_line = peek().span.start_line;
        advance();
        while (!at_eof() && peek().span.start_line == err_line) {
          advance();
        }
      }
    }

    return mod;
  }

private:
  const std::vector<PslToken>& toks_;
  std::string filename_;
  DiagnosticBag& diags_;
  std::size_t idx_ = 0;

  bool at_eof() const noexcept {
    return idx_ >= toks_.size() || toks_[idx_].kind == PslTokKind::Eof;
  }

  const PslToken& peek(std::size_t offset = 0) const noexcept {
    if (idx_ + offset >= toks_.size()) {
      return toks_.back();
    }
    return toks_[idx_ + offset];
  }

  const PslToken& advance() noexcept {
    const PslToken& t = peek();
    if (!at_eof()) ++idx_;
    return t;
  }

  void advance(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n && !at_eof(); ++i) {
      ++idx_;
    }
  }

  bool check_ident(std::string_view name) const noexcept {
    return (peek().kind == PslTokKind::Ident || peek().kind == PslTokKind::UpperIdent) &&
           peek().text == name;
  }

  bool expect_ident(std::string_view name, const std::string& msg) {
    if (check_ident(name)) {
      advance();
      return true;
    }
    error_here(msg);
    return false;
  }

  void error_here(const std::string& msg) {
    diags_.error("E0200", msg, peek().span);
  }

  void consume_pragmas(PslProvenance& prov, std::string& lang_version) {
    while (!at_eof() && peek().kind == PslTokKind::Pragma) {
      parse_pragma_into(peek().text, peek().span, prov, lang_version, diags_);
      advance();
    }
  }

  void consume_trailing_pragmas(int decl_line, PslProvenance& prov, std::string& lang_version) {
    while (!at_eof() && peek().kind == PslTokKind::Pragma &&
           peek().span.start_line == decl_line) {
      parse_pragma_into(peek().text, peek().span, prov, lang_version, diags_);
      advance();
    }
  }

  bool at_top_level_boundary(int top_col) const noexcept {
    if (at_eof()) return true;
    if (peek().kind == PslTokKind::Pragma && peek().span.start_col <= top_col) return true;
    if (peek().span.start_col > top_col) return false;
    return check_ident("import") || check_ident("source") || check_ident("type") ||
           check_ident("tycon") || check_ident("kind") || check_ident("builtin") ||
           check_ident("diag") || check_ident("coerce") || check_ident("overload") ||
           check_ident("node") || check_ident("flow") || check_ident("analysis") ||
           check_ident("lint") || check_ident("example") || check_ident("profile") ||
           check_ident("project");
  }

  PslImportDecl parse_import() {
    PslImportDecl imp;
    imp.span = peek().span;
    advance(); // import
    if (check_ident("qualified")) {
      imp.is_qualified = true;
      advance();
    }
    std::string mod_name;
    while (!at_eof() &&
           (peek().kind == PslTokKind::UpperIdent || peek().kind == PslTokKind::Ident)) {
      if (!mod_name.empty()) mod_name.push_back('.');
      mod_name += peek().text;
      advance();
      if (peek().kind == PslTokKind::Dot) {
        advance();
      } else {
        break;
      }
    }
    imp.module_name = std::move(mod_name);
    if (check_ident("as")) {
      advance();
      if (!at_eof()) {
        imp.alias = peek().text;
        advance();
      }
    }
    if (check_ident("hiding")) {
      imp.is_hiding = true;
      advance();
    }
    if (peek().kind == PslTokKind::LParen) {
      advance();
      while (!at_eof() && peek().kind != PslTokKind::RParen) {
        if (peek().kind != PslTokKind::Comma) {
          imp.items.push_back(peek().text);
        }
        advance();
      }
      if (peek().kind == PslTokKind::RParen) advance();
    }
    return imp;
  }

  PslSourceDecl parse_source(PslProvenance prov) {
    PslSourceDecl decl;
    decl.span = peek().span;
    decl.provenance = std::move(prov);
    const int line = peek().span.start_line;
    advance(); // source
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::EqualSign) {
      advance();
    }
    if (!at_eof() && peek().span.start_line == line && peek().kind == PslTokKind::Ident) {
      decl.origin_kind = peek().text;
      advance();
    }
    if (!at_eof() && peek().span.start_line == line && peek().kind == PslTokKind::String) {
      decl.title = peek().text;
      advance();
    }
    while (!at_eof() && peek().span.start_line == line && peek().kind != PslTokKind::Pragma) {
      if (check_ident("rev") || check_ident("version")) {
        advance();
        if (!at_eof() && peek().span.start_line == line) {
          decl.revision = peek().text;
          advance();
          continue;
        }
      }
      if (!decl.raw_text.empty()) decl.raw_text.push_back(' ');
      decl.raw_text += peek().text;
      advance();
    }
    return decl;
  }

  PslTypeExprPtr parse_type_expr(int min_line = 0) {
    auto parse_atom = [&](auto& self) -> PslTypeExprPtr {
      auto t = std::make_shared<PslTypeExpr>();
      if (at_eof()) return t;
      t->span = peek().span;
      if (peek().kind == PslTokKind::LParen) {
        advance();
        if (peek().kind == PslTokKind::RParen) {
          t->kind = PslTypeExpr::Kind::Named;
          t->name = "()";
          advance();
          return t;
        }
        auto inner = parse_type_expr(min_line);
        if (peek().kind == PslTokKind::Comma) {
          t->kind = PslTypeExpr::Kind::Tuple;
          t->args.push_back(inner);
          while (peek().kind == PslTokKind::Comma) {
            advance();
            t->args.push_back(parse_type_expr(min_line));
          }
          if (peek().kind == PslTokKind::RParen) advance();
          return t;
        }
        if (peek().kind == PslTokKind::RParen) advance();
        return inner;
      }
      if (peek().kind == PslTokKind::LBracket) {
        advance();
        t->kind = PslTypeExpr::Kind::List;
        t->left = parse_type_expr(min_line);
        if (peek().kind == PslTokKind::RBracket) advance();
        return t;
      }
      if (peek().kind == PslTokKind::UpperIdent) {
        t->kind = PslTypeExpr::Kind::Named;
        t->name = peek().text;
        advance();
        return t;
      }
      if (peek().kind == PslTokKind::Ident) {
        t->kind = PslTypeExpr::Kind::Var;
        t->name = peek().text;
        advance();
        return t;
      }
      (void)self;
      return t;
    };

    if (check_ident("forall")) {
      auto ft = std::make_shared<PslTypeExpr>();
      ft->kind = PslTypeExpr::Kind::Forall;
      ft->span = peek().span;
      advance();
      while (!at_eof() && peek().kind == PslTokKind::Ident) {
        ft->forall_vars.push_back(peek().text);
        advance();
      }
      if (peek().kind == PslTokKind::Dot) advance();
      ft->left = parse_type_expr(min_line);
      return ft;
    }

    auto parse_app = [&]() -> PslTypeExprPtr {
      auto head = parse_atom(parse_atom);
      std::vector<PslTypeExprPtr> args;
      while (!at_eof() &&
             (min_line == 0 || peek().span.start_line == min_line) &&
             (peek().kind == PslTokKind::UpperIdent ||
              peek().kind == PslTokKind::Ident ||
              peek().kind == PslTokKind::LParen ||
              peek().kind == PslTokKind::LBracket) &&
             !check_ident("where")) {
        args.push_back(parse_atom(parse_atom));
      }
      if (args.empty()) return head;
      auto app = std::make_shared<PslTypeExpr>();
      app->kind = PslTypeExpr::Kind::App;
      app->name = head->name;
      app->left = head;
      app->args = std::move(args);
      app->span = head->span;
      return app;
    };

    auto lhs = parse_app();
    if (!at_eof() && peek().kind == PslTokKind::Arrow &&
        (min_line == 0 || peek().span.start_line == min_line)) {
      advance();
      auto arr = std::make_shared<PslTypeExpr>();
      arr->kind = PslTypeExpr::Kind::Arrow;
      arr->name = "->";
      arr->left = lhs;
      arr->right = parse_type_expr(min_line);
      arr->span = lhs->span;
      return arr;
    }
    return lhs;
  }

  PslTypeDecl parse_type_decl(PslProvenance prov, int top_col) {
    PslTypeDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // type
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    while (!at_eof() && peek().kind == PslTokKind::Ident &&
           peek().kind != PslTokKind::EqualSign) {
      decl.params.push_back(peek().text);
      advance();
    }
    if (peek().kind == PslTokKind::EqualSign) {
      advance();
      // Check if it is a sum-type declaration `Ty = Num | Bool | Err`
      if (peek().kind == PslTokKind::UpperIdent &&
          peek(1).kind == PslTokKind::Pipe) {
        while (!at_eof()) {
          if (peek().kind == PslTokKind::UpperIdent) {
            decl.union_ctors.push_back(peek().text);
            advance();
            // Skip any inline type args on same line before '|' or next decl
            while (!at_eof() && peek().span.start_line == decl_line &&
                   peek().kind != PslTokKind::Pipe && peek().kind != PslTokKind::Pragma) {
              advance();
            }
          }
          if (peek().kind == PslTokKind::Pipe) {
            advance();
          } else {
            break;
          }
        }
      } else {
        decl.target = parse_type_expr(decl_line);
      }
    }
    while (!at_eof() && peek().span.start_line > decl_line && !at_top_level_boundary(top_col)) {
      advance();
    }
    return decl;
  }

  PslTyconDecl parse_tycon_decl(PslProvenance prov) {
    PslTyconDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // tycon
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    while (!at_eof() && peek().span.start_line == decl_line &&
           peek().kind == PslTokKind::Ident) {
      decl.params.push_back(peek().text);
      advance();
    }
    return decl;
  }

  PslKindDecl parse_kind_decl(PslProvenance prov, std::string& lang_version) {
    PslKindDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // kind
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    if (check_ident("where")) {
      advance();
      while (!at_eof() && (peek().span.start_line == decl_line || peek().span.start_col > 1)) {
        if (peek().kind == PslTokKind::Pragma) {
          consume_trailing_pragmas(peek().span.start_line, decl.provenance, lang_version);
          continue;
        }
        if (check_ident("shadowing")) {
          advance();
          if (peek().kind == PslTokKind::EqualSign) advance();
          if (check_ident("Warn")) decl.shadowing = ShadowingPolicy::Warn;
          else if (check_ident("Error")) decl.shadowing = ShadowingPolicy::Error;
          else decl.shadowing = ShadowingPolicy::Allow;
          if (!at_eof()) advance();
        } else {
          break;
        }
      }
    }
    return decl;
  }

  PslBuiltinDecl parse_builtin_decl(PslProvenance prov, std::string& lang_version) {
    PslBuiltinDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // builtin
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::ColonColon) advance();
    if (peek().kind == PslTokKind::UpperIdent &&
        (peek(1).kind == PslTokKind::LParen || peek(1).kind == PslTokKind::UpperIdent)) {
      decl.symbol_kind = peek().text;
      advance();
      decl.value_type = parse_type_expr(decl_line);
    } else if (check_ident("forall")) {
      decl.value_type = parse_type_expr(decl_line);
    } else {
      decl.value_type = parse_type_expr(decl_line);
    }
    consume_trailing_pragmas(decl_line, decl.provenance, lang_version);
    return decl;
  }

  PslSeverity parse_severity_tok() {
    if (check_ident("Error")) {
      advance();
      return PslSeverity::Error;
    }
    if (check_ident("Warning")) {
      advance();
      return PslSeverity::Warning;
    }
    if (check_ident("Note") || check_ident("Info")) {
      advance();
      return PslSeverity::Note;
    }
    return PslSeverity::Error;
  }

  PslDiagDecl parse_diag_decl(PslProvenance prov, std::string& lang_version) {
    PslDiagDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // diag
    if (!at_eof()) {
      decl.code = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::ColonColon) advance();
    decl.severity = parse_severity_tok();
    if (peek().kind == PslTokKind::String) {
      decl.template_text = peek().text;
      advance();
    }
    consume_trailing_pragmas(decl_line, decl.provenance, lang_version);
    return decl;
  }

  PslCoerceDecl parse_coerce_decl(PslProvenance prov) {
    PslCoerceDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // coerce
    decl.from_type = parse_type_expr(decl_line);
    if (peek().kind == PslTokKind::Arrow) advance();
    decl.to_type = parse_type_expr(decl_line);
    while (!at_eof() && peek().span.start_line == decl_line) advance();
    return decl;
  }

  PslOverloadDecl parse_overload_decl(PslProvenance prov, int top_col) {
    PslOverloadDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    advance(); // overload
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::ColonColon) advance();
    while (!at_eof() && !at_top_level_boundary(top_col)) {
      if (peek().kind == PslTokKind::Pipe) advance();
      auto t = parse_type_expr(peek().span.start_line);
      if (t) decl.signatures.push_back(std::move(t));
      else break;
    }
    return decl;
  }

  PslPatternPtr parse_pattern(bool allow_app = true) {
    auto p = std::make_shared<PslPattern>();
    if (at_eof()) return p;
    p->span = peek().span;
    if (peek().kind == PslTokKind::LParen) {
      advance();
      if (peek().kind == PslTokKind::Ident && peek(1).kind == PslTokKind::ColonColon) {
        p->kind = PslPattern::Kind::TypedVar;
        p->name = peek().text;
        advance(2);
        if (!at_eof()) {
          p->type_name = peek().text;
          advance();
        }
        if (peek().kind == PslTokKind::RParen) advance();
        return p;
      }
      auto inner = parse_pattern(true);
      if (peek().kind == PslTokKind::RParen) advance();
      return inner;
    }
    if (peek().kind == PslTokKind::LBracket) {
      p->kind = PslPattern::Kind::List;
      advance();
      while (!at_eof() && peek().kind != PslTokKind::RBracket) {
        if (peek().kind == PslTokKind::Comma) {
          advance();
          continue;
        }
        p->args.push_back(parse_pattern(true));
      }
      if (peek().kind == PslTokKind::RBracket) advance();
      return p;
    }
    if (peek().kind == PslTokKind::Number) {
      p->kind = PslPattern::Kind::Number;
      p->number_value = peek().number_value;
      p->name = peek().text;
      advance();
      return p;
    }
    if (peek().kind == PslTokKind::String) {
      p->kind = PslPattern::Kind::String;
      p->name = peek().text;
      advance();
      return p;
    }
    if (peek().kind == PslTokKind::UpperIdent) {
      p->kind = PslPattern::Kind::Ctor;
      p->name = peek().text;
      advance();
      if (allow_app) {
        while (!at_eof() && !check_ident("where") &&
               peek().kind != PslTokKind::EqualSign &&
               peek().kind != PslTokKind::RParen &&
               peek().kind != PslTokKind::RBracket &&
               peek().kind != PslTokKind::Comma &&
               peek().kind != PslTokKind::Arrow &&
               (peek().kind == PslTokKind::Ident ||
                peek().kind == PslTokKind::UpperIdent ||
                peek().kind == PslTokKind::Number ||
                peek().kind == PslTokKind::String ||
                peek().kind == PslTokKind::LParen ||
                peek().kind == PslTokKind::LBracket)) {
          p->args.push_back(parse_pattern(false));
        }
      }
      return p;
    }
    if (peek().kind == PslTokKind::Ident) {
      if (peek().text == "_") {
        p->kind = PslPattern::Kind::Wildcard;
        p->name = "_";
      } else {
        p->kind = PslPattern::Kind::Var;
        p->name = peek().text;
      }
      advance();
      return p;
    }
    return p;
  }

  // Expression parser with precedence: || -> && -> comparisons -> +,- -> *,/ -> <> -> app/atom
  PslExprPtr parse_expr(int min_line = 0, int min_col = 1) {
    return parse_or_expr(min_line, min_col);
  }

  bool can_continue_expr(int min_line, int min_col) const noexcept {
    if (at_eof()) return false;
    if (min_line > 0 && peek().span.start_line > min_line &&
        peek().span.start_col <= min_col) {
      return false;
    }
    if (peek().kind == PslTokKind::Pragma || peek().kind == PslTokKind::Pipe ||
        peek().kind == PslTokKind::RParen || peek().kind == PslTokKind::RBracket ||
        peek().kind == PslTokKind::RBrace || peek().kind == PslTokKind::Comma ||
        peek().kind == PslTokKind::Semicolon || peek().kind == PslTokKind::EqualSign ||
        peek().kind == PslTokKind::LeftArrow) {
      return false;
    }
    if (check_ident("then") || check_ident("else") || check_ident("where") ||
        check_ident("match") || check_ident("when") || check_ident("say") ||
        check_ident("fix") || check_ident("gen") || check_ident("kill") ||
        check_ident("entry") || check_ident("fires") || check_ident("silent")) {
      return false;
    }
    return true;
  }

  PslExprPtr parse_or_expr(int min_line, int min_col) {
    auto lhs = parse_and_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col) && peek().kind == PslTokKind::OrOr) {
      Span sp = peek().span;
      advance();
      auto rhs = parse_and_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = "||";
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_and_expr(int min_line, int min_col) {
    auto lhs = parse_cmp_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col) && peek().kind == PslTokKind::AndAnd) {
      Span sp = peek().span;
      advance();
      auto rhs = parse_cmp_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = "&&";
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_cmp_expr(int min_line, int min_col) {
    auto lhs = parse_append_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col)) {
      std::string op;
      if (peek().kind == PslTokKind::Eq) op = "==";
      else if (peek().kind == PslTokKind::Neq) op = "/=";
      else if (peek().kind == PslTokKind::Lt) op = "<";
      else if (peek().kind == PslTokKind::Le) op = "<=";
      else if (peek().kind == PslTokKind::Gt) op = ">";
      else if (peek().kind == PslTokKind::Ge) op = ">=";
      else if (check_ident("in")) op = "in";
      else if (peek().kind == PslTokKind::BacktickOp && peek().text != "orFail") {
        op = peek().text;
      } else {
        break;
      }
      Span sp = peek().span;
      advance();
      auto rhs = parse_append_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = std::move(op);
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_append_expr(int min_line, int min_col) {
    auto lhs = parse_add_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col) && peek().kind == PslTokKind::Append) {
      Span sp = peek().span;
      advance();
      auto rhs = parse_add_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = "<>";
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_add_expr(int min_line, int min_col) {
    auto lhs = parse_mul_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col) &&
           (peek().kind == PslTokKind::Plus || peek().kind == PslTokKind::Minus)) {
      std::string op = peek().text;
      Span sp = peek().span;
      advance();
      auto rhs = parse_mul_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = std::move(op);
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_mul_expr(int min_line, int min_col) {
    auto lhs = parse_app_expr(min_line, min_col);
    while (can_continue_expr(min_line, min_col) &&
           (peek().kind == PslTokKind::Star || peek().kind == PslTokKind::Slash)) {
      std::string op = peek().text;
      Span sp = peek().span;
      advance();
      auto rhs = parse_app_expr(min_line, min_col);
      auto bin = std::make_shared<PslExpr>();
      bin->kind = PslExpr::Kind::BinOp;
      bin->name = std::move(op);
      bin->args = {lhs, rhs};
      bin->span = sp;
      lhs = bin;
    }
    return lhs;
  }

  PslExprPtr parse_app_expr(int min_line, int min_col) {
    if (!can_continue_expr(min_line, min_col)) {
      return std::make_shared<PslExpr>();
    }
    if (check_ident("if")) {
      Span sp = peek().span;
      advance();
      auto cond = parse_expr(min_line, min_col);
      expect_ident("then", "expected `then` in `if` expression");
      auto t_br = parse_expr(min_line, min_col);
      expect_ident("else", "expected `else` in `if` expression");
      auto f_br = parse_expr(min_line, min_col);
      auto e = std::make_shared<PslExpr>();
      e->kind = PslExpr::Kind::IfThenElse;
      e->args = {cond, t_br, f_br};
      e->span = sp;
      return e;
    }
    if (peek().kind == PslTokKind::Minus) {
      Span sp = peek().span;
      advance();
      auto inner = parse_app_expr(min_line, min_col);
      auto u = std::make_shared<PslExpr>();
      u->kind = PslExpr::Kind::UnaryOp;
      u->name = "-";
      u->args = {inner};
      u->span = sp;
      return u;
    }

    auto head = parse_postfix_expr(min_line, min_col);
    std::vector<PslExprPtr> call_args;
    while (can_continue_expr(min_line, min_col) &&
           !check_ident("in") &&
           (peek().kind == PslTokKind::Ident ||
            peek().kind == PslTokKind::UpperIdent ||
            peek().kind == PslTokKind::Number ||
            peek().kind == PslTokKind::String ||
            peek().kind == PslTokKind::LParen ||
            peek().kind == PslTokKind::LBracket)) {
      if (min_line > 0 && peek().span.start_line > min_line) {
        break;
      }
      call_args.push_back(parse_postfix_expr(min_line, min_col));
    }
    if (call_args.empty()) return head;

    if (head->kind == PslExpr::Kind::Var && head->name == "not" && call_args.size() == 1) {
      auto u = std::make_shared<PslExpr>();
      u->kind = PslExpr::Kind::UnaryOp;
      u->name = "not";
      u->args = {call_args[0]};
      u->span = head->span;
      return u;
    }

    auto call = std::make_shared<PslExpr>();
    if (head->kind == PslExpr::Kind::FieldAccess) {
      call->kind = PslExpr::Kind::FieldAccess;
      call->name = head->name;
      call->args = head->args;
      for (auto& ca : call_args) call->args.push_back(std::move(ca));
      call->span = head->span;
      return call;
    }
    call->kind = (head->kind == PslExpr::Kind::Ctor) ? PslExpr::Kind::Ctor : PslExpr::Kind::Call;
    call->name = head->name;
    call->args = std::move(call_args);
    call->span = head->span;
    return call;
  }

  PslExprPtr parse_postfix_expr(int min_line, int min_col) {
    auto atom = parse_atom_expr();
    while (!at_eof() && peek().kind == PslTokKind::Dot) {
      Span sp = peek().span;
      advance();
      auto fld = std::make_shared<PslExpr>();
      fld->kind = PslExpr::Kind::FieldAccess;
      fld->args = {atom};
      if (!at_eof()) {
        fld->name = peek().text;
        advance();
      }
      fld->span = sp;
      atom = fld;
    }
    (void)min_line;
    (void)min_col;
    return atom;
  }

  PslExprPtr parse_atom_expr() {
    auto e = std::make_shared<PslExpr>();
    if (at_eof()) return e;
    e->span = peek().span;

    if (peek().kind == PslTokKind::LParen) {
      advance();
      auto inner = parse_expr();
      if (peek().kind == PslTokKind::RParen) advance();
      return inner;
    }
    if (peek().kind == PslTokKind::LBracket) {
      advance();
      if (peek().kind == PslTokKind::RBracket) {
        advance();
        e->kind = PslExpr::Kind::List;
        return e;
      }
      auto first = parse_expr();
      if (peek().kind == PslTokKind::Pipe) {
        // List comprehension: [expr | var <- src (, filter)]
        advance();
        e->kind = PslExpr::Kind::ListComp;
        e->args.push_back(first);
        if (!at_eof()) {
          e->comp_var = peek().text;
          advance();
        }
        if (peek().kind == PslTokKind::LeftArrow) advance();
        e->comp_source = parse_expr();
        if (peek().kind == PslTokKind::Comma) {
          advance();
          e->comp_filter = parse_expr();
        }
        if (peek().kind == PslTokKind::RBracket) advance();
        return e;
      }
      e->kind = PslExpr::Kind::List;
      e->args.push_back(first);
      while (peek().kind == PslTokKind::Comma) {
        advance();
        e->args.push_back(parse_expr());
      }
      if (peek().kind == PslTokKind::RBracket) advance();
      return e;
    }
    if (peek().kind == PslTokKind::Number) {
      e->kind = PslExpr::Kind::Number;
      e->number_value = peek().number_value;
      e->name = peek().text;
      advance();
      return e;
    }
    if (peek().kind == PslTokKind::String) {
      e->kind = PslExpr::Kind::String;
      e->name = peek().text;
      advance();
      return e;
    }
    if (peek().kind == PslTokKind::UpperIdent) {
      if (peek().text == "True" || peek().text == "False") {
        e->kind = PslExpr::Kind::Bool;
        e->bool_value = (peek().text == "True");
        e->name = peek().text;
        advance();
        return e;
      }
      e->kind = PslExpr::Kind::Ctor;
      e->name = peek().text;
      advance();
      return e;
    }
    if (peek().kind == PslTokKind::Ident) {
      if (peek().text == "true" || peek().text == "false") {
        e->kind = PslExpr::Kind::Bool;
        e->bool_value = (peek().text == "true");
        e->name = peek().text;
        advance();
        return e;
      }
      e->kind = PslExpr::Kind::Var;
      e->name = peek().text;
      advance();
      return e;
    }
    return e;
  }

  PslOrFail parse_optional_or_fail(int block_col) {
    PslOrFail of;
    if (peek().kind == PslTokKind::BacktickOp && peek().text == "orFail" &&
        peek().span.start_col > block_col) {
      of.present = true;
      of.span = peek().span;
      advance();
      if (!at_eof()) {
        of.diag_code = peek().text;
        advance();
      }
      if (peek().kind == PslTokKind::LBrace) {
        advance();
        while (!at_eof() && peek().kind != PslTokKind::RBrace) {
          if (peek().kind == PslTokKind::Comma) {
            advance();
            continue;
          }
          std::string key = peek().text;
          advance();
          if (peek().kind == PslTokKind::EqualSign) advance();
          auto val = parse_expr();
          of.args.emplace_back(std::move(key), std::move(val));
        }
        if (peek().kind == PslTokKind::RBrace) advance();
      }
    }
    return of;
  }

  PslNodeStmt parse_node_stmt(int block_col) {
    PslNodeStmt st;
    st.span = peek().span;
    const int stmt_line = peek().span.start_line;

    if (check_ident("scope")) {
      st.kind = PslNodeStmt::Kind::Scope;
      advance();
      if (!at_eof()) {
        st.target_var = peek().text;
        advance();
      }
      if (peek().kind == PslTokKind::Dollar) {
        advance();
        st.nested.push_back(std::make_shared<PslNodeStmt>(parse_node_stmt(block_col)));
      }
      return st;
    }
    if (check_ident("declare")) {
      st.kind = PslNodeStmt::Kind::Declare;
      advance();
      if (!at_eof()) {
        st.sym_kind = peek().text;
        advance();
      }
      st.expr = parse_postfix_expr(stmt_line, block_col);
      if (peek().kind == PslTokKind::LParen || peek().kind == PslTokKind::Ident ||
          peek().kind == PslTokKind::UpperIdent) {
        st.expr2 = parse_expr(stmt_line, block_col);
      }
      return st;
    }
    if (check_ident("require")) {
      st.kind = PslNodeStmt::Kind::Require;
      advance();
      st.expr = parse_expr(stmt_line, block_col);
      st.or_fail = parse_optional_or_fail(block_col);
      return st;
    }
    if (check_ident("unify")) {
      st.kind = PslNodeStmt::Kind::Unify;
      advance();
      st.expr = parse_postfix_expr(stmt_line, block_col);
      st.expr2 = parse_postfix_expr(stmt_line, block_col);
      st.or_fail = parse_optional_or_fail(block_col);
      return st;
    }

    if (check_ident("pure")) {
      st.kind = PslNodeStmt::Kind::AssignAttr;
      st.target_var = "_pure";
      advance();
      st.expr = parse_expr(stmt_line, block_col);
      return st;
    }

    // var <- ... or attr = ...
    if (peek().kind == PslTokKind::Ident && peek(1).kind == PslTokKind::LeftArrow) {
      std::string var = peek().text;
      advance(2);
      if (check_ident("fresh")) {
        st.kind = PslNodeStmt::Kind::FreshBind;
        st.target_var = std::move(var);
        advance();
        return st;
      }
      if (check_ident("lookup")) {
        st.kind = PslNodeStmt::Kind::LookupBind;
        st.target_var = std::move(var);
        advance();
        if (!at_eof()) {
          st.sym_kind = peek().text;
          advance();
        }
        st.expr = parse_expr(stmt_line, block_col);
        st.or_fail = parse_optional_or_fail(block_col);
        return st;
      }
      st.kind = PslNodeStmt::Kind::AssignAttr;
      st.target_var = std::move(var);
      st.expr = parse_expr(stmt_line, block_col);
      st.or_fail = parse_optional_or_fail(block_col);
      return st;
    }

    if (peek().kind == PslTokKind::Ident && peek(1).kind == PslTokKind::EqualSign) {
      st.kind = PslNodeStmt::Kind::AssignAttr;
      st.target_var = peek().text;
      advance(2);
      st.expr = parse_expr(stmt_line, block_col);
      return st;
    }

    // Fallback skip line
    while (!at_eof() && peek().span.start_line == stmt_line) advance();
    return st;
  }

  PslNodeRule parse_node_rule(PslProvenance prov, int top_col, std::string& lang_version) {
    PslNodeRule rule;
    rule.provenance = std::move(prov);
    rule.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // node
    rule.pattern = parse_pattern(true);
    if (check_ident("where")) {
      advance();
    }
    consume_trailing_pragmas(decl_line, rule.provenance, lang_version);
    const int block_col = at_eof() ? (top_col + 1) : peek().span.start_col;
    while (!at_eof() && !at_top_level_boundary(top_col) &&
           peek().span.start_col > top_col) {
      if (peek().kind == PslTokKind::Pragma) {
        consume_pragmas(rule.provenance, lang_version);
        continue;
      }
      rule.stmts.push_back(parse_node_stmt(std::min(block_col, top_col + 1)));
    }
    return rule;
  }

  PslFlowRule parse_flow_rule(PslProvenance prov) {
    PslFlowRule rule;
    rule.provenance = std::move(prov);
    rule.span = peek().span;
    const int line = peek().span.start_line;
    advance(); // flow
    rule.pattern = parse_pattern(true);
    if (peek().kind == PslTokKind::EqualSign) advance();
    PslFlowExpr fe;
    fe.span = peek().span;
    if (check_ident("chain")) {
      fe.kind = PslFlowExpr::Kind::Chain;
      advance();
      if (!at_eof() && peek().span.start_line == line) {
        fe.arg_name = peek().text;
        advance();
      }
    } else if (check_ident("branch")) {
      fe.kind = PslFlowExpr::Kind::Branch;
      advance();
      if (!at_eof() && peek().span.start_line == line) {
        fe.arg_name = peek().text;
        advance();
      }
      if (peek().kind == PslTokKind::LBracket) {
        advance();
        while (!at_eof() && peek().kind != PslTokKind::RBracket) {
          if (peek().kind != PslTokKind::Comma) fe.list_args.push_back(peek().text);
          advance();
        }
        if (peek().kind == PslTokKind::RBracket) advance();
      }
    } else if (check_ident("loop")) {
      fe.kind = PslFlowExpr::Kind::Loop;
      advance();
      while (!at_eof() && peek().span.start_line == line && peek().kind != PslTokKind::Pragma) {
        fe.list_args.push_back(peek().text);
        advance();
      }
    } else if (check_ident("jump")) {
      fe.kind = PslFlowExpr::Kind::Jump;
      advance();
      if (!at_eof() && peek().span.start_line == line) {
        fe.arg_name = peek().text;
        advance();
      }
      if (!at_eof() && peek().span.start_line == line) {
        if (peek().text == "Exit") fe.target_exit = true;
        else fe.target_ctor = peek().text;
        advance();
      }
    } else if (check_ident("jumpOut") || check_ident("jumpTo")) {
      fe.kind = check_ident("jumpOut") ? PslFlowExpr::Kind::JumpOut : PslFlowExpr::Kind::JumpTo;
      advance();
      while (!at_eof() && peek().span.start_line == line && peek().kind != PslTokKind::Pragma) {
        if (peek().kind == PslTokKind::UpperIdent) {
          fe.target_ctor = peek().text;
        }
        advance();
      }
    } else if (check_ident("empty")) {
      fe.kind = PslFlowExpr::Kind::Empty;
      advance();
    }
    while (!at_eof() && peek().span.start_line == line && peek().kind != PslTokKind::Pragma) {
      advance();
    }
    rule.expr = std::move(fe);
    return rule;
  }

  PslAnalysisRule parse_analysis_rule(PslProvenance prov, int top_col, std::string& lang_version) {
    PslAnalysisRule rule;
    rule.provenance = std::move(prov);
    rule.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // analysis
    if (!at_eof()) {
      rule.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::ColonColon) advance();
    if (check_ident("Forward")) {
      rule.direction = AnalysisDir::Forward;
      advance();
    } else if (check_ident("Backward")) {
      rule.direction = AnalysisDir::Backward;
      advance();
    }
    if (peek().kind == PslTokKind::Comma) advance();
    if (check_ident("Must")) {
      rule.meet = AnalysisMeet::Must;
      advance();
    } else if (check_ident("May")) {
      rule.meet = AnalysisMeet::May;
      advance();
    }
    // Domain type on same line
    std::string dom;
    while (!at_eof() && peek().span.start_line == decl_line && peek().kind != PslTokKind::Pragma) {
      if (!dom.empty()) dom.push_back(' ');
      dom += peek().text;
      advance();
    }
    rule.domain_type = std::move(dom);
    consume_trailing_pragmas(decl_line, rule.provenance, lang_version);

    while (!at_eof() && !at_top_level_boundary(top_col) && peek().span.start_col > top_col) {
      if (peek().kind == PslTokKind::Pragma) {
        consume_pragmas(rule.provenance, lang_version);
        continue;
      }
      if (check_ident("entry")) {
        advance();
        if (peek().kind == PslTokKind::EqualSign) advance();
        rule.entry_expr = parse_expr(peek().span.start_line, top_col);
      } else if (check_ident("gen") || check_ident("kill")) {
        bool is_gen = check_ident("gen");
        Span csp = peek().span;
        int cline = csp.start_line;
        advance();
        PslTransferClause cl;
        cl.span = csp;
        cl.pattern = parse_pattern(true);
        if (peek().kind == PslTokKind::EqualSign) advance();
        cl.expr = parse_expr(cline, top_col);
        if (is_gen) rule.gen_clauses.push_back(std::move(cl));
        else rule.kill_clauses.push_back(std::move(cl));
      } else {
        advance();
      }
    }
    return rule;
  }

  PslLintCategory parse_lint_category() {
    if (check_ident("Style")) { advance(); return PslLintCategory::Style; }
    if (check_ident("Correctness")) { advance(); return PslLintCategory::Correctness; }
    if (check_ident("Performance")) { advance(); return PslLintCategory::Performance; }
    if (check_ident("Complexity")) { advance(); return PslLintCategory::Complexity; }
    if (check_ident("Security")) { advance(); return PslLintCategory::Security; }
    if (peek().kind == PslTokKind::UpperIdent) advance();
    return PslLintCategory::General;
  }

  PslLintRule parse_lint_rule(PslProvenance prov, int top_col, std::string& lang_version) {
    PslLintRule rule;
    rule.provenance = std::move(prov);
    rule.span = peek().span;
    const int decl_line = peek().span.start_line;
    advance(); // lint
    if (!at_eof()) {
      rule.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::ColonColon) advance();
    rule.severity = parse_severity_tok();
    if (peek().kind == PslTokKind::Comma) {
      advance();
      rule.category = parse_lint_category();
    }
    consume_trailing_pragmas(decl_line, rule.provenance, lang_version);

    while (!at_eof() && !at_top_level_boundary(top_col) && peek().span.start_col > top_col) {
      if (peek().kind == PslTokKind::Pragma) {
        consume_pragmas(rule.provenance, lang_version);
        continue;
      }
      if (check_ident("match")) {
        advance();
        rule.match_pattern = parse_pattern(true);
      } else if (check_ident("when")) {
        int wline = peek().span.start_line;
        advance();
        rule.when_expr = parse_expr(wline, top_col);
      } else if (check_ident("say")) {
        advance();
        if (peek().kind == PslTokKind::String) {
          rule.say_template = peek().text;
          advance();
        }
      } else if (check_ident("fix")) {
        rule.fix.present = true;
        rule.fix.span = peek().span;
        int fline = peek().span.start_line;
        advance();
        if (peek().kind == PslTokKind::String) {
          rule.fix.title = peek().text;
          advance();
        }
        if (check_ident("Safe")) {
          rule.fix.is_safe = true;
          advance();
        } else if (check_ident("Unsafe")) {
          rule.fix.is_safe = false;
          advance();
        }
        auto fexpr = parse_expr(fline, top_col);
        if (fexpr && fexpr->kind == PslExpr::Kind::Call && fexpr->args.size() >= 2) {
          rule.fix.target_span_expr = fexpr->args[0];
          rule.fix.replacement_expr = fexpr->args[1];
        } else {
          rule.fix.replacement_expr = fexpr;
        }
      } else {
        advance();
      }
    }
    return rule;
  }

  PslExampleBlock parse_example_block(int top_col) {
    PslExampleBlock blk;
    blk.span = peek().span;
    advance(); // example
    if (!at_eof()) {
      blk.target_name = peek().text;
      advance();
    }
    while (!at_eof() && !at_top_level_boundary(top_col) && peek().span.start_col > top_col) {
      if (check_ident("fires") || check_ident("silent")) {
        PslExampleCase c;
        c.expect_fire = check_ident("fires");
        c.span = peek().span;
        advance();
        if (peek().kind == PslTokKind::String) {
          c.input = peek().text;
          advance();
        } else {
          error_here("expected string snippet after `fires`/`silent`");
        }
        blk.cases.push_back(std::move(c));
      } else {
        advance();
      }
    }
    return blk;
  }

  PslProfileDecl parse_profile_decl(PslProvenance prov) {
    PslProfileDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    advance(); // profile
    if (!at_eof()) {
      decl.name = peek().text;
      advance();
    }
    if (peek().kind == PslTokKind::EqualSign) advance();
    if (peek().kind == PslTokKind::LBracket) {
      advance();
      while (!at_eof() && peek().kind != PslTokKind::RBracket) {
        if (peek().kind != PslTokKind::Comma) {
          decl.lint_names.push_back(peek().text);
        }
        advance();
      }
      if (peek().kind == PslTokKind::RBracket) advance();
    }
    return decl;
  }

  PslProjectDecl parse_project_decl(PslProvenance prov, int top_col) {
    PslProjectDecl decl;
    decl.provenance = std::move(prov);
    decl.span = peek().span;
    advance(); // project
    if (check_ident("where")) advance();
    while (!at_eof() && !at_top_level_boundary(top_col) && peek().span.start_col > top_col) {
      int cline = peek().span.start_line;
      if (check_ident("unit")) {
        advance();
        if (!at_eof() && peek().span.start_line == cline) {
          decl.unit_ctor = peek().text;
          advance();
        }
      } else if (check_ident("imports")) {
        advance();
        auto pat = parse_pattern(true);
        if (pat) decl.import_ctor = pat->name;
        while (!at_eof() && peek().span.start_line == cline) advance();
      } else if (check_ident("exports")) {
        advance();
        auto pat = parse_pattern(true);
        if (pat) decl.export_ctor = pat->name;
        while (!at_eof() && peek().span.start_line == cline) advance();
      } else if (check_ident("prelude")) {
        advance();
        if (peek().kind == PslTokKind::EqualSign) advance();
        if (peek().kind == PslTokKind::String) {
          decl.prelude = peek().text;
          advance();
        }
      } else if (check_ident("search")) {
        advance();
        if (peek().kind == PslTokKind::EqualSign) advance();
        if (peek().kind == PslTokKind::LBracket) {
          advance();
          while (!at_eof() && peek().kind != PslTokKind::RBracket) {
            if (peek().kind == PslTokKind::String) decl.search_paths.push_back(peek().text);
            advance();
          }
          if (peek().kind == PslTokKind::RBracket) advance();
        }
      } else if (check_ident("cycles")) {
        advance();
        if (peek().kind == PslTokKind::EqualSign) advance();
        if (check_ident("Allowed")) decl.cycles_allowed = true;
        else decl.cycles_allowed = false;
        if (!at_eof()) advance();
      } else {
        advance();
      }
    }
    return decl;
  }
};

// ---------------------------------------------------------------------------
// Static Checker helpers
// ---------------------------------------------------------------------------

struct GrammarCatalog {
  std::unordered_set<std::string> type_names;
  std::unordered_map<std::string, std::size_t> ctor_arities;
};

inline GrammarCatalog build_grammar_catalog(const ir::Grammar& g) {
  GrammarCatalog cat;
  for (const auto& td : g.gTypes) {
    cat.type_names.insert(td.name);
    for (const auto& c : td.constructors) {
      std::size_t arity = c.is_record ? c.record_fields.size() : c.positional_fields.size();
      cat.ctor_arities[c.name] = arity;
    }
  }
  // Builtin boolean constructors
  cat.ctor_arities.try_emplace("True", 0);
  cat.ctor_arities.try_emplace("False", 0);
  cat.ctor_arities.try_emplace("Just", 1);
  cat.ctor_arities.try_emplace("Nothing", 0);
  return cat;
}

inline void check_pattern_against_grammar(const PslPatternPtr& pat,
                                          const GrammarCatalog& cat,
                                          DiagnosticBag& diags) {
  if (!pat) return;
  if (pat->kind == PslPattern::Kind::TypedVar) {
    if (!pat->type_name.empty() && !cat.type_names.contains(pat->type_name)) {
      diags.error("PSL-AST-MISMATCH",
                  "unknown AST type `" + pat->type_name + "` in pattern",
                  pat->span);
    }
    return;
  }
  if (pat->kind == PslPattern::Kind::Ctor) {
    auto it = cat.ctor_arities.find(pat->name);
    if (it == cat.ctor_arities.end()) {
      // Allow operator/tag constants inside nested patterns if they match any known ctor;
      // otherwise report mismatch.
      diags.error("PSL-AST-MISMATCH",
                  "unknown AST constructor `" + pat->name + "` in `.psem` pattern",
                  pat->span);
    } else if (pat->args.size() != it->second) {
      diags.error("PSL-AST-MISMATCH",
                  "constructor `" + pat->name + "` expects " + std::to_string(it->second) +
                      " field(s), got " + std::to_string(pat->args.size()),
                  pat->span);
    }
    for (const auto& arg : pat->args) {
      check_pattern_against_grammar(arg, cat, diags);
    }
    return;
  }
  if (pat->kind == PslPattern::Kind::List) {
    for (const auto& arg : pat->args) {
      check_pattern_against_grammar(arg, cat, diags);
    }
  }
}

inline void collect_expr_attr_deps(const PslExprPtr& e,
                                   const std::unordered_set<std::string>& local_vars,
                                   std::vector<std::string>& deps_out) {
  if (!e) return;
  if (e->kind == PslExpr::Kind::Var) {
    if (!local_vars.contains(e->name)) {
      deps_out.push_back(e->name);
    }
    return;
  }
  if (e->kind == PslExpr::Kind::Call && !e->args.empty()) {
    // e.g. typeOf child vs typeOf self
    if (e->name == "typeOf" && e->args[0]->kind == PslExpr::Kind::Var &&
        e->args[0]->name == "self") {
      deps_out.push_back("type");
    }
  }
  for (const auto& a : e->args) collect_expr_attr_deps(a, local_vars, deps_out);
  for (const auto& kv : e->fields) collect_expr_attr_deps(kv.second, local_vars, deps_out);
  if (e->comp_source) collect_expr_attr_deps(e->comp_source, local_vars, deps_out);
  if (e->comp_filter) collect_expr_attr_deps(e->comp_filter, local_vars, deps_out);
}

inline void collect_pattern_vars(const PslPatternPtr& p, std::unordered_set<std::string>& out) {
  if (!p) return;
  if (p->kind == PslPattern::Kind::Var || p->kind == PslPattern::Kind::TypedVar) {
    if (!p->name.empty() && p->name != "_") out.insert(p->name);
  }
  for (const auto& a : p->args) collect_pattern_vars(a, out);
}

inline bool detect_attr_cycles(const PslModule& mod,
                               DiagnosticBag& diags,
                               std::vector<std::string>& topo_order) {
  bool has_cycle = false;
  for (const auto& nr : mod.node_rules) {
    std::unordered_set<std::string> pat_vars;
    collect_pattern_vars(nr.pattern, pat_vars);

    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_map<std::string, Span> attr_spans;
    for (const auto& st : nr.stmts) {
      if (st.kind == PslNodeStmt::Kind::AssignAttr) {
        attr_spans[st.target_var] = st.span;
        std::vector<std::string> deps;
        collect_expr_attr_deps(st.expr, pat_vars, deps);
        for (const auto& d : deps) {
          if (d != st.target_var) {
            adj[st.target_var].push_back(d);
          } else {
            // Self-referential equation e.g. `type = type`
            diags.error("PSL-CYCLE",
                        "cyclic attribute definition for `" + st.target_var + "` on node `" +
                            (nr.pattern ? nr.pattern->name : "<node>") + "`",
                        st.span);
            has_cycle = true;
          }
        }
      }
    }

    // 3-color DFS on intra-node attributes
    std::unordered_map<std::string, int> color; // 0=unvisited, 1=visiting, 2=done
    auto dfs = [&](auto& self, const std::string& u, std::vector<std::string>& stack) -> void {
      color[u] = 1;
      stack.push_back(u);
      auto it = adj.find(u);
      if (it != adj.end()) {
        for (const auto& v : it->second) {
          if (!attr_spans.contains(v)) continue;
          if (color[v] == 1) {
            std::string cycle_str = v;
            for (auto sit = stack.rbegin(); sit != stack.rend(); ++sit) {
              cycle_str = *sit + " -> " + cycle_str;
              if (*sit == v) break;
            }
            diags.error("PSL-CYCLE",
                        "cyclic attribute dependency in `node " +
                            (nr.pattern ? nr.pattern->name : "") + "`: " + cycle_str,
                        attr_spans[u]);
            has_cycle = true;
          } else if (color[v] == 0) {
            self(self, v, stack);
          }
        }
      }
      stack.pop_back();
      color[u] = 2;
      topo_order.push_back(u);
    };

    for (const auto& [attr, _sp] : attr_spans) {
      if (color[attr] == 0) {
        std::vector<std::string> stk;
        dfs(dfs, attr, stk);
      }
    }
  }
  return !has_cycle;
}

// ---------------------------------------------------------------------------
// 5-Pass Interpreter Runtime
// ---------------------------------------------------------------------------

struct AstNodeInfo {
  int id = -1;
  int parent_id = -1;
  std::string ctor_name;
  const eval::EvalValue* val = nullptr;
  std::vector<int> children;
  std::vector<std::string> child_labels;
};

struct RuntimeSymbol {
  std::string name;
  std::string kind;
  int type_var = -1;
  int decl_node_id = -1;
  int use_count = 0;
  int arity = 0;
};

class TypeStore {
public:
  struct TypeNode {
    enum class Kind { Var, Con, App, Arrow, Error };
    Kind kind = Kind::Var;
    int parent = -1;
    int level = 0;
    std::string name;
    int lhs = -1;
    int rhs = -1;
    std::vector<int> args;
  };

  int fresh_var(int level = 0) {
    int id = static_cast<int>(nodes_.size());
    TypeNode n;
    n.kind = TypeNode::Kind::Var;
    n.parent = id;
    n.level = level;
    nodes_.push_back(std::move(n));
    return id;
  }

  int make_con(std::string name) {
    int id = static_cast<int>(nodes_.size());
    TypeNode n;
    n.kind = TypeNode::Kind::Con;
    n.parent = id;
    n.name = std::move(name);
    nodes_.push_back(std::move(n));
    return id;
  }

  int make_arrow(int l, int r) {
    int id = static_cast<int>(nodes_.size());
    TypeNode n;
    n.kind = TypeNode::Kind::Arrow;
    n.parent = id;
    n.name = "->";
    n.lhs = l;
    n.rhs = r;
    nodes_.push_back(std::move(n));
    return id;
  }

  int make_app(std::string name, std::vector<int> args) {
    int id = static_cast<int>(nodes_.size());
    TypeNode n;
    n.kind = TypeNode::Kind::App;
    n.parent = id;
    n.name = std::move(name);
    n.args = std::move(args);
    nodes_.push_back(std::move(n));
    return id;
  }

  int make_error() {
    int id = static_cast<int>(nodes_.size());
    TypeNode n;
    n.kind = TypeNode::Kind::Error;
    n.parent = id;
    n.name = "Err";
    nodes_.push_back(std::move(n));
    return id;
  }

  int find(int id) {
    if (id < 0 || id >= static_cast<int>(nodes_.size())) return -1;
    int cur = id;
    while (nodes_[cur].parent != cur) {
      cur = nodes_[cur].parent;
    }
    int root = cur;
    cur = id;
    while (nodes_[cur].parent != cur) {
      int nxt = nodes_[cur].parent;
      nodes_[cur].parent = root;
      cur = nxt;
    }
    return root;
  }

  bool occurs(int var_id, int ty_id) {
    ty_id = find(ty_id);
    if (ty_id < 0) return false;
    if (ty_id == var_id) return true;
    const TypeNode& n = nodes_[ty_id];
    if (n.kind == TypeNode::Kind::Arrow) {
      return occurs(var_id, n.lhs) || occurs(var_id, n.rhs);
    }
    if (n.kind == TypeNode::Kind::App) {
      for (int a : n.args) {
        if (occurs(var_id, a)) return true;
      }
    }
    return false;
  }

  bool unify(int a, int b) {
    a = find(a);
    b = find(b);
    if (a < 0 || b < 0) return false;
    if (a == b) return true;
    if (nodes_[a].kind == TypeNode::Kind::Error || nodes_[b].kind == TypeNode::Kind::Error) {
      return true;
    }
    if (nodes_[a].kind == TypeNode::Kind::Var) {
      if (occurs(a, b)) return false;
      nodes_[a].parent = b;
      return true;
    }
    if (nodes_[b].kind == TypeNode::Kind::Var) {
      if (occurs(b, a)) return false;
      nodes_[b].parent = a;
      return true;
    }
    if (nodes_[a].kind == TypeNode::Kind::Con && nodes_[b].kind == TypeNode::Kind::Con) {
      return nodes_[a].name == nodes_[b].name;
    }
    if (nodes_[a].kind == TypeNode::Kind::Arrow && nodes_[b].kind == TypeNode::Kind::Arrow) {
      return unify(nodes_[a].lhs, nodes_[b].lhs) && unify(nodes_[a].rhs, nodes_[b].rhs);
    }
    if (nodes_[a].kind == TypeNode::Kind::App && nodes_[b].kind == TypeNode::Kind::App) {
      if (nodes_[a].name != nodes_[b].name ||
          nodes_[a].args.size() != nodes_[b].args.size()) {
        return false;
      }
      for (std::size_t i = 0; i < nodes_[a].args.size(); ++i) {
        if (!unify(nodes_[a].args[i], nodes_[b].args[i])) return false;
      }
      return true;
    }
    return false;
  }

  std::string format(int id, int depth = 0) {
    id = find(id);
    if (id < 0 || depth > 32) return "?";
    const TypeNode& n = nodes_[id];
    switch (n.kind) {
      case TypeNode::Kind::Var:
        return "t" + std::to_string(id);
      case TypeNode::Kind::Con:
      case TypeNode::Kind::Error:
        return n.name;
      case TypeNode::Kind::Arrow:
        return format(n.lhs, depth + 1) + " -> " + format(n.rhs, depth + 1);
      case TypeNode::Kind::App: {
        std::string s = n.name;
        for (int a : n.args) s += " " + format(a, depth + 1);
        return s;
      }
    }
    return "?";
  }

  int from_psl_type(const PslTypeExprPtr& te,
                    std::unordered_map<std::string, int>& tvars,
                    int level) {
    if (!te) return fresh_var(level);
    switch (te->kind) {
      case PslTypeExpr::Kind::Named:
        return make_con(te->name);
      case PslTypeExpr::Kind::Var: {
        auto it = tvars.find(te->name);
        if (it != tvars.end()) return it->second;
        int v = fresh_var(level);
        tvars[te->name] = v;
        return v;
      }
      case PslTypeExpr::Kind::Arrow:
        return make_arrow(from_psl_type(te->left, tvars, level),
                          from_psl_type(te->right, tvars, level));
      case PslTypeExpr::Kind::App: {
        std::vector<int> args;
        for (const auto& a : te->args) args.push_back(from_psl_type(a, tvars, level));
        return make_app(te->name, std::move(args));
      }
      case PslTypeExpr::Kind::List:
        return make_app("List", {from_psl_type(te->left, tvars, level)});
      case PslTypeExpr::Kind::Tuple: {
        std::vector<int> args;
        for (const auto& a : te->args) args.push_back(from_psl_type(a, tvars, level));
        return make_app("Tuple", std::move(args));
      }
      case PslTypeExpr::Kind::Forall: {
        auto local = tvars;
        for (const auto& fv : te->forall_vars) {
          local[fv] = fresh_var(level);
        }
        return from_psl_type(te->left, local, level);
      }
    }
    return fresh_var(level);
  }

  int arity_of(int id) {
    id = find(id);
    if (id < 0) return 0;
    const TypeNode& n = nodes_[id];
    if (n.kind == TypeNode::Kind::Arrow) {
      return 1 + arity_of(n.rhs);
    }
    return 0;
  }

private:
  std::vector<TypeNode> nodes_;
};

// Pattern matcher against EvalValue tree + NodeId map
struct PatBinding {
  enum class Kind { Node, String, Number, Bool, List };
  Kind kind = Kind::Node;
  int node_id = -1;
  std::string str_val;
  double num_val = 0.0;
  bool bool_val = false;
  std::vector<int> list_nodes;
};

inline bool match_psl_pattern(const PslPatternPtr& pat,
                              const eval::EvalValue& val,
                              int val_node_id,
                              const std::unordered_map<const eval::EvalValue*, int>& val_to_id,
                              const ir::Grammar* grammar,
                              std::unordered_map<std::string, PatBinding>& bindings) {
  if (!pat) return true;
  switch (pat->kind) {
    case PslPattern::Kind::Wildcard:
      return true;

    case PslPattern::Kind::Var: {
      PatBinding b;
      if (val.kind == eval::EvalValue::Kind::String) {
        b.kind = PatBinding::Kind::String;
        b.str_val = val.string_value;
        b.node_id = val_node_id;
      } else if (val.kind == eval::EvalValue::Kind::Number) {
        b.kind = PatBinding::Kind::Number;
        b.num_val = val.number_value;
        b.node_id = val_node_id;
      } else if (val.kind == eval::EvalValue::Kind::Bool) {
        b.kind = PatBinding::Kind::Bool;
        b.bool_val = val.bool_value;
        b.node_id = val_node_id;
      } else if (val.kind == eval::EvalValue::Kind::List) {
        b.kind = PatBinding::Kind::List;
        b.node_id = val_node_id;
        for (const auto& el : val.elements) {
          auto it = val_to_id.find(&el);
          b.list_nodes.push_back(it != val_to_id.end() ? it->second : -1);
        }
      } else {
        b.kind = PatBinding::Kind::Node;
        b.node_id = val_node_id;
      }
      bindings[pat->name] = std::move(b);
      return true;
    }

    case PslPattern::Kind::TypedVar: {
      if (val.kind != eval::EvalValue::Kind::Ctor) return false;
      if (grammar && !pat->type_name.empty()) {
        bool found_type = false;
        for (const auto& td : grammar->gTypes) {
          if (td.name == pat->type_name) {
            for (const auto& c : td.constructors) {
              if (c.name == val.ctor_name) {
                found_type = true;
                break;
              }
            }
          }
        }
        if (!found_type) return false;
      }
      PatBinding b;
      b.kind = PatBinding::Kind::Node;
      b.node_id = val_node_id;
      bindings[pat->name] = std::move(b);
      return true;
    }

    case PslPattern::Kind::Number:
      return val.kind == eval::EvalValue::Kind::Number &&
             std::abs(val.number_value - pat->number_value) < 1e-9;

    case PslPattern::Kind::String:
      return val.kind == eval::EvalValue::Kind::String && val.string_value == pat->name;

    case PslPattern::Kind::List:
      if (val.kind != eval::EvalValue::Kind::List ||
          val.elements.size() != pat->args.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pat->args.size(); ++i) {
        auto it = val_to_id.find(&val.elements[i]);
        int cid = (it != val_to_id.end()) ? it->second : -1;
        if (!match_psl_pattern(pat->args[i], val.elements[i], cid, val_to_id, grammar, bindings)) {
          return false;
        }
      }
      return true;

    case PslPattern::Kind::Ctor:
      if (pat->args.empty() && (pat->name == "True" || pat->name == "False") &&
          val.kind == eval::EvalValue::Kind::Bool) {
        return val.bool_value == (pat->name == "True");
      }
      if (val.kind != eval::EvalValue::Kind::Ctor || val.ctor_name != pat->name ||
          val.elements.size() != pat->args.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pat->args.size(); ++i) {
        auto it = val_to_id.find(&val.elements[i]);
        int cid = (it != val_to_id.end()) ? it->second : -1;
        if (!match_psl_pattern(pat->args[i], val.elements[i], cid, val_to_id, grammar, bindings)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

// Runtime Value for evaluating PSL expressions inside `node` and `lint`
struct RVal {
  enum class Kind { Unit, Bool, Number, String, TypeId, SymId, NodeId, List, Maybe };
  Kind kind = Kind::Unit;
  bool b = false;
  double num = 0.0;
  std::string s;
  int id = -1;
  std::vector<RVal> list;
  std::shared_ptr<RVal> maybe_inner;
};

class PslRuntime {
public:
  PslRuntime(const PslModule& mod,
             const ir::Grammar* grammar,
             const eval::EvalValue& root,
             std::string_view active_profile)
      : mod_(mod), grammar_(grammar), active_profile_(active_profile) {
    // Index AST nodes
    flatten_ast(root, -1);
    // Index diagnostics
    for (const auto& d : mod_.diags) {
      diag_defs_[d.code] = &d;
    }
    // Index type aliases
    for (const auto& td : mod_.types) {
      if (td.target) {
        std::unordered_map<std::string, int> tv;
        type_aliases_[td.name] = types_.from_psl_type(td.target, tv, 0);
      } else {
        type_aliases_[td.name] = types_.make_con(td.name);
      }
      for (const auto& uc : td.union_ctors) {
        type_aliases_[uc] = types_.make_con(uc);
      }
    }
    for (const auto& tc : mod_.tycons) {
      type_aliases_.try_emplace(tc.name, types_.make_con(tc.name));
    }
    // Init root scope + builtins
    scopes_.emplace_back();
    for (const auto& bi : mod_.builtins) {
      std::unordered_map<std::string, int> tv;
      int tid = types_.from_psl_type(bi.value_type, tv, 0);
      RuntimeSymbol sym;
      sym.name = bi.name;
      sym.kind = bi.symbol_kind.empty() ? "Builtin" : bi.symbol_kind;
      sym.type_var = tid;
      sym.arity = types_.arity_of(tid);
      int sid = static_cast<int>(symbols_.size());
      symbols_.push_back(std::move(sym));
      scopes_.back()[bi.name] = sid;
    }
  }

  PslAnalysisResult execute() {
    PslAnalysisResult res;
    if (nodes_.empty()) return res;

    // Passes 1-3: Scope/Declare, Lookup, TypeInference/Require
    run_node_passes(0, 0);

    // Pass 4: Flow CFG + Bitvector Dataflow Analyses
    run_dataflow_pass(res);

    // Pass 5: Lint Matcher + Autofix + Profile filter
    run_lint_pass(res);

    res.nodes_visited = nodes_.size();
    res.symbols_declared = symbols_.size();
    res.type_unifications = unifications_;
    for (const auto& [nid, tid] : node_types_) {
      res.inferred_types[nid] = types_.format(tid);
    }
    res.diagnostics = std::move(diags_);
    res.ok = true;
    for (const auto& d : res.diagnostics) {
      if (d.severity == PslSeverity::Error) {
        res.ok = false;
        break;
      }
    }
    return res;
  }

private:
  const PslModule& mod_;
  const ir::Grammar* grammar_;
  std::string active_profile_;
  std::vector<AstNodeInfo> nodes_;
  std::unordered_map<const eval::EvalValue*, int> val_to_id_;
  std::unordered_map<std::string, const PslDiagDecl*> diag_defs_;
  std::unordered_map<std::string, int> type_aliases_;
  TypeStore types_;
  std::vector<RuntimeSymbol> symbols_;
  std::vector<std::unordered_map<std::string, int>> scopes_;
  std::unordered_map<int, int> node_types_;
  std::unordered_map<int, int> node_resolved_sym_;
  std::unordered_map<int, int> node_declared_sym_;
  std::unordered_map<int, std::unordered_map<std::string, RVal>> node_attrs_;
  std::vector<PslDiagnostic> diags_;
  std::size_t unifications_ = 0;

  // Dataflow state
  std::unordered_map<int, bool> reachable_;
  // analysis_name -> node_id -> set of symbol names
  std::unordered_map<std::string, std::unordered_map<int, std::set<std::string>>> df_before_;
  std::unordered_map<std::string, std::unordered_map<int, std::set<std::string>>> df_after_;

  int flatten_ast(const eval::EvalValue& v, int parent_id) {
    int id = static_cast<int>(nodes_.size());
    AstNodeInfo info;
    info.id = id;
    info.parent_id = parent_id;
    info.val = &v;
    if (v.kind == eval::EvalValue::Kind::Ctor) {
      info.ctor_name = v.ctor_name;
    }
    nodes_.push_back(std::move(info));
    val_to_id_[&v] = id;

    std::vector<int> cids;
    for (const auto& el : v.elements) {
      cids.push_back(flatten_ast(el, id));
    }
    nodes_[id].children = std::move(cids);
    return id;
  }

  int lookup_type_name(const std::string& name, int level) {
    auto it = type_aliases_.find(name);
    if (it != type_aliases_.end()) {
      return it->second;
    }
    int con = types_.make_con(name);
    (void)level;
    return con;
  }

  std::optional<double> eval_const_fold(int nid, int depth = 0) {
    if (nid < 0 || nid >= static_cast<int>(nodes_.size()) || depth > 64) {
      return std::nullopt;
    }
    const eval::EvalValue* v = nodes_[nid].val;
    if (!v) return std::nullopt;
    if (v->kind == eval::EvalValue::Kind::Number) {
      return v->number_value;
    }
    if (v->kind != eval::EvalValue::Kind::Ctor) return std::nullopt;

    if (v->elements.size() == 1 &&
        v->elements[0].kind == eval::EvalValue::Kind::Number &&
        (v->ctor_name == "Num" || v->ctor_name == "Lit" || v->ctor_name == "PyNum" ||
         v->ctor_name == "IntLit" || v->ctor_name == "FloatLit" ||
         v->ctor_name == "HsNum" || v->ctor_name == "KtNum" ||
         v->ctor_name == "SwNum" || v->ctor_name == "SwiftInt" || v->ctor_name == "SwiftFloat")) {
      return v->elements[0].number_value;
    }
    if ((v->ctor_name == "Neg" || v->ctor_name == "Unary" || v->ctor_name == "SwUnary" ||
         v->ctor_name == "PyUnary") && !nodes_[nid].children.empty()) {
      if (v->elements.size() >= 2 && v->elements[0].kind == eval::EvalValue::Kind::Ctor &&
          v->elements[0].ctor_name != "Neg" && v->elements[0].ctor_name != "SwNeg" &&
          v->elements[0].ctor_name != "PyNeg") {
        return std::nullopt;
      }
      if (auto sub = eval_const_fold(nodes_[nid].children.back(), depth + 1)) {
        return -(*sub);
      }
    }
    if (v->ctor_name == "Paren" && nodes_[nid].children.size() == 1) {
      return eval_const_fold(nodes_[nid].children[0], depth + 1);
    }
    if ((v->ctor_name == "Bin" || v->ctor_name == "PyBin" || v->ctor_name == "SwBin" ||
         v->ctor_name == "BinOp") && v->elements.size() == 3) {
      std::string op_name;
      if (v->elements[0].kind == eval::EvalValue::Kind::Ctor) op_name = v->elements[0].ctor_name;
      else if (v->elements[0].kind == eval::EvalValue::Kind::String) op_name = v->elements[0].string_value;
      auto lv = eval_const_fold(nodes_[nid].children[1], depth + 1);
      auto rv = eval_const_fold(nodes_[nid].children[2], depth + 1);
      if (lv && rv) {
        if (op_name == "Add" || op_name == "PyAdd" || op_name == "SwAdd" || op_name == "+") return *lv + *rv;
        if (op_name == "Sub" || op_name == "PySub" || op_name == "SwSub" || op_name == "-") return *lv - *rv;
        if (op_name == "Mul" || op_name == "PyMul" || op_name == "SwMul" || op_name == "*") return *lv * *rv;
        if ((op_name == "Div" || op_name == "PyDiv" || op_name == "SwDiv" || op_name == "/") && std::abs(*rv) > 1e-12) {
          return *lv / *rv;
        }
      }
    }
    return std::nullopt;
  }

  RVal eval_expr(const PslExprPtr& e,
                 const std::unordered_map<std::string, PatBinding>& bindings,
                 const std::unordered_map<std::string, RVal>& locals,
                 int cur_node_id,
                 int level,
                 int depth = 0) {
    if (!e || depth > 64) return {};
    switch (e->kind) {
      case PslExpr::Kind::Number: {
        RVal r;
        r.kind = RVal::Kind::Number;
        r.num = e->number_value;
        return r;
      }
      case PslExpr::Kind::String: {
        RVal r;
        r.kind = RVal::Kind::String;
        r.s = e->name;
        return r;
      }
      case PslExpr::Kind::Bool: {
        RVal r;
        r.kind = RVal::Kind::Bool;
        r.b = e->bool_value;
        return r;
      }
      case PslExpr::Kind::Var: {
        if (auto lit = locals.find(e->name); lit != locals.end()) {
          return lit->second;
        }
        if (e->name == "node" || e->name == "self") {
          RVal r;
          r.kind = RVal::Kind::NodeId;
          r.id = cur_node_id;
          return r;
        }
        if (auto bit = bindings.find(e->name); bit != bindings.end()) {
          const PatBinding& b = bit->second;
          RVal r;
          switch (b.kind) {
            case PatBinding::Kind::String:
              r.kind = RVal::Kind::String;
              r.s = b.str_val;
              r.id = b.node_id;
              return r;
            case PatBinding::Kind::Number:
              r.kind = RVal::Kind::Number;
              r.num = b.num_val;
              return r;
            case PatBinding::Kind::Bool:
              r.kind = RVal::Kind::Bool;
              r.b = b.bool_val;
              return r;
            case PatBinding::Kind::List:
              r.kind = RVal::Kind::List;
              r.id = b.node_id;
              for (int cid : b.list_nodes) {
                RVal el;
                el.kind = RVal::Kind::NodeId;
                el.id = cid;
                r.list.push_back(std::move(el));
              }
              return r;
            case PatBinding::Kind::Node:
              r.kind = RVal::Kind::NodeId;
              r.id = b.node_id;
              return r;
          }
        }
        if (type_aliases_.contains(e->name)) {
          RVal r;
          r.kind = RVal::Kind::TypeId;
          r.id = type_aliases_[e->name];
          return r;
        }
        RVal r;
        r.kind = RVal::Kind::String;
        r.s = e->name;
        return r;
      }
      case PslExpr::Kind::Ctor: {
        if (e->name == "Nothing") {
          RVal r;
          r.kind = RVal::Kind::Maybe;
          return r;
        }
        if (e->name == "Just" && !e->args.empty()) {
          RVal r;
          r.kind = RVal::Kind::Maybe;
          r.maybe_inner = std::make_shared<RVal>(
              eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1));
          return r;
        }
        if (type_aliases_.contains(e->name) || e->args.empty()) {
          RVal r;
          r.kind = RVal::Kind::TypeId;
          if (e->args.empty()) {
            r.id = lookup_type_name(e->name, level);
          } else {
            std::vector<int> tids;
            for (const auto& a : e->args) {
              RVal av = eval_expr(a, bindings, locals, cur_node_id, level, depth + 1);
              tids.push_back(av.kind == RVal::Kind::TypeId ? av.id : types_.fresh_var(level));
            }
            if (e->name == "Fn" && tids.size() == 2) {
              r.id = types_.make_arrow(tids[0], tids[1]);
            } else {
              r.id = types_.make_app(e->name, std::move(tids));
            }
          }
          return r;
        }
        return {};
      }
      case PslExpr::Kind::List: {
        RVal r;
        r.kind = RVal::Kind::List;
        for (const auto& a : e->args) {
          r.list.push_back(eval_expr(a, bindings, locals, cur_node_id, level, depth + 1));
        }
        return r;
      }
      case PslExpr::Kind::ListComp: {
        RVal src = eval_expr(e->comp_source, bindings, locals, cur_node_id, level, depth + 1);
        RVal out;
        out.kind = RVal::Kind::List;
        if (src.kind == RVal::Kind::List && !e->args.empty()) {
          for (const auto& elem : src.list) {
            auto sub_locals = locals;
            sub_locals[e->comp_var] = elem;
            if (e->comp_filter) {
              RVal fv = eval_expr(e->comp_filter, bindings, sub_locals, cur_node_id, level, depth + 1);
              if (!fv.b) continue;
            }
            out.list.push_back(
                eval_expr(e->args[0], bindings, sub_locals, cur_node_id, level, depth + 1));
          }
        }
        return out;
      }
      case PslExpr::Kind::UnaryOp: {
        RVal av = eval_expr(e->args.empty() ? nullptr : e->args[0],
                            bindings, locals, cur_node_id, level, depth + 1);
        if (e->name == "not") {
          RVal r;
          r.kind = RVal::Kind::Bool;
          r.b = !av.b;
          return r;
        }
        if (e->name == "-") {
          RVal r;
          r.kind = RVal::Kind::Number;
          r.num = -av.num;
          return r;
        }
        return av;
      }
      case PslExpr::Kind::IfThenElse: {
        if (e->args.size() >= 3) {
          RVal c = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          return eval_expr(c.b ? e->args[1] : e->args[2],
                           bindings, locals, cur_node_id, level, depth + 1);
        }
        return {};
      }
      case PslExpr::Kind::FieldAccess: {
        if (!e->args.empty() &&
            (e->args[0]->kind == PslExpr::Kind::Var ||
             e->args[0]->kind == PslExpr::Kind::Ctor)) {
          const std::string& aname = e->args[0]->name;
          if (e->name == "before" || e->name == "after") {
            int target_nid = cur_node_id;
            if (e->args.size() >= 2) {
              RVal tv = eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1);
              if (tv.id >= 0) target_nid = tv.id;
            }
            RVal r;
            r.kind = RVal::Kind::List;
            const auto& dfmap = (e->name == "before") ? df_before_ : df_after_;
            auto ait = dfmap.find(aname);
            if (ait != dfmap.end()) {
              auto nit = ait->second.find(target_nid);
              if (nit != ait->second.end()) {
                for (const auto& s : nit->second) {
                  RVal el;
                  el.kind = RVal::Kind::String;
                  el.s = s;
                  r.list.push_back(std::move(el));
                }
              }
            }
            return r;
          }
        }
        return {};
      }
      case PslExpr::Kind::BinOp: {
        if (e->args.size() < 2) return {};
        if (e->name == "&&") {
          RVal l = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          if (!l.b) { RVal r; r.kind = RVal::Kind::Bool; r.b = false; return r; }
          RVal rv = eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1);
          RVal r; r.kind = RVal::Kind::Bool; r.b = rv.b; return r;
        }
        if (e->name == "||") {
          RVal l = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          if (l.b) { RVal r; r.kind = RVal::Kind::Bool; r.b = true; return r; }
          RVal rv = eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1);
          RVal r; r.kind = RVal::Kind::Bool; r.b = rv.b; return r;
        }
        RVal l = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
        RVal rv = eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1);

        auto equal_vals = [&](const RVal& a, const RVal& b) -> bool {
          if (a.kind == RVal::Kind::NodeId && b.kind == RVal::Kind::NodeId) {
            if (a.id == b.id) return true;
            if (a.id >= 0 && b.id >= 0 && nodes_[a.id].val && nodes_[b.id].val) {
              return *nodes_[a.id].val == *nodes_[b.id].val;
            }
            return false;
          }
          if (a.kind == RVal::Kind::NodeId && b.kind == RVal::Kind::TypeId &&
              a.id >= 0 && nodes_[a.id].val &&
              nodes_[a.id].val->kind == eval::EvalValue::Kind::Ctor &&
              nodes_[a.id].val->elements.empty()) {
            return nodes_[a.id].val->ctor_name == types_.format(b.id);
          }
          if (b.kind == RVal::Kind::NodeId && a.kind == RVal::Kind::TypeId &&
              b.id >= 0 && nodes_[b.id].val &&
              nodes_[b.id].val->kind == eval::EvalValue::Kind::Ctor &&
              nodes_[b.id].val->elements.empty()) {
            return nodes_[b.id].val->ctor_name == types_.format(a.id);
          }
          if (a.kind == RVal::Kind::Maybe && b.kind == RVal::Kind::Maybe) {
            if (!a.maybe_inner && !b.maybe_inner) return true;
            if (!a.maybe_inner || !b.maybe_inner) return false;
            return a.maybe_inner->num == b.maybe_inner->num &&
                   a.maybe_inner->s == b.maybe_inner->s;
          }
          if (a.kind == RVal::Kind::TypeId && b.kind == RVal::Kind::TypeId) {
            return types_.format(a.id) == types_.format(b.id);
          }
          if (a.kind == RVal::Kind::Number && b.kind == RVal::Kind::Number) {
            return std::abs(a.num - b.num) < 1e-9;
          }
          if (a.kind == RVal::Kind::String && b.kind == RVal::Kind::String) {
            return a.s == b.s;
          }
          if (a.kind == RVal::Kind::Bool && b.kind == RVal::Kind::Bool) {
            return a.b == b.b;
          }
          return a.kind == b.kind && a.id == b.id && a.s == b.s;
        };

        if (e->name == "==") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = equal_vals(l, rv); return r;
        }
        if (e->name == "/=") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = !equal_vals(l, rv); return r;
        }
        if (e->name == "<") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = l.num < rv.num; return r;
        }
        if (e->name == "<=") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = l.num <= rv.num; return r;
        }
        if (e->name == ">") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = l.num > rv.num; return r;
        }
        if (e->name == ">=") {
          RVal r; r.kind = RVal::Kind::Bool; r.b = l.num >= rv.num; return r;
        }
        if (e->name == "+") { RVal r; r.kind = RVal::Kind::Number; r.num = l.num + rv.num; return r; }
        if (e->name == "-") { RVal r; r.kind = RVal::Kind::Number; r.num = l.num - rv.num; return r; }
        if (e->name == "*") { RVal r; r.kind = RVal::Kind::Number; r.num = l.num * rv.num; return r; }
        if (e->name == "/") {
          RVal r; r.kind = RVal::Kind::Number;
          r.num = (std::abs(rv.num) > 1e-12) ? (l.num / rv.num) : 0.0;
          return r;
        }
        if (e->name == "<>") {
          RVal r; r.kind = RVal::Kind::String; r.s = l.s + rv.s; return r;
        }
        if (e->name == "isPrefixOf") {
          RVal r;
          r.kind = RVal::Kind::Bool;
          r.b = rv.s.size() >= l.s.size() && rv.s.compare(0, l.s.size(), l.s) == 0;
          return r;
        }
        if (e->name == "in" || e->name == "member" || e->name == "notMember") {
          bool found = false;
          for (const auto& el : rv.list) {
            if (el.s == l.s && !l.s.empty()) {
              found = true;
              break;
            }
          }
          RVal r;
          r.kind = RVal::Kind::Bool;
          r.b = (e->name == "notMember") ? !found : found;
          return r;
        }
        return {};
      }
      case PslExpr::Kind::Call: {
        if (e->name == "typeOf" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          RVal r;
          r.kind = RVal::Kind::TypeId;
          if (av.kind == RVal::Kind::SymId && av.id >= 0 &&
              av.id < static_cast<int>(symbols_.size())) {
            r.id = symbols_[av.id].type_var;
          } else if (av.kind == RVal::Kind::NodeId && node_types_.contains(av.id)) {
            r.id = node_types_[av.id];
          } else {
            r.id = types_.fresh_var(level);
          }
          return r;
        }
        if (e->name == "arity" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          RVal r;
          r.kind = RVal::Kind::Number;
          if (av.kind == RVal::Kind::SymId && av.id >= 0 &&
              av.id < static_cast<int>(symbols_.size())) {
            r.num = static_cast<double>(symbols_[av.id].arity);
          }
          return r;
        }
        if (e->name == "length" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          RVal r;
          r.kind = RVal::Kind::Number;
          r.num = static_cast<double>(av.list.size());
          return r;
        }
        if ((e->name == "and" || e->name == "all") && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          bool res = true;
          for (const auto& el : av.list) {
            if (!el.b) { res = false; break; }
          }
          RVal r; r.kind = RVal::Kind::Bool; r.b = res; return r;
        }
        if (e->name == "any" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          bool res = false;
          for (const auto& el : av.list) {
            if (el.b) { res = true; break; }
          }
          RVal r; r.kind = RVal::Kind::Bool; r.b = res; return r;
        }
        if (e->name == "count" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          int cnt = 0;
          for (const auto& el : av.list) {
            if (el.b) ++cnt;
          }
          RVal r; r.kind = RVal::Kind::Number; r.num = static_cast<double>(cnt); return r;
        }
        if (e->name == "useCount" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          int cnt = 0;
          if (node_declared_sym_.contains(cur_node_id)) {
            cnt = symbols_[node_declared_sym_[cur_node_id]].use_count;
          } else if (av.kind == RVal::Kind::String) {
            for (const auto& sym : symbols_) {
              if (sym.name == av.s) cnt += sym.use_count;
            }
          }
          RVal r; r.kind = RVal::Kind::Number; r.num = static_cast<double>(cnt); return r;
        }
        if (e->name == "constFold" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          RVal r;
          r.kind = RVal::Kind::Maybe;
          if (auto folded = eval_const_fold(av.id)) {
            RVal inner;
            inner.kind = RVal::Kind::Number;
            inner.num = *folded;
            r.maybe_inner = std::make_shared<RVal>(std::move(inner));
          }
          return r;
        }
        if (e->name == "reachable" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          RVal r;
          r.kind = RVal::Kind::Bool;
          auto it = reachable_.find(av.id >= 0 ? av.id : cur_node_id);
          r.b = (it == reachable_.end()) ? true : it->second;
          return r;
        }
        if ((e->name == "before" || e->name == "after") && !e->args.empty()) {
          std::string aname = e->args[0]->name;
          int nid = (e->args.size() >= 2)
                        ? eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1).id
                        : cur_node_id;
          RVal r;
          r.kind = RVal::Kind::List;
          const auto& dfmap = (e->name == "before") ? df_before_ : df_after_;
          auto ait = dfmap.find(aname);
          if (ait != dfmap.end()) {
            auto nit = ait->second.find(nid);
            if (nit != ait->second.end()) {
              for (const auto& s : nit->second) {
                RVal el; el.kind = RVal::Kind::String; el.s = s;
                r.list.push_back(std::move(el));
              }
            }
          }
          return r;
        }
        if (e->name == "mono" && !e->args.empty()) {
          return eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
        }
        if (e->name == "generalize" && !e->args.empty()) {
          return eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
        }
        if (e->name == "isImmutable" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          bool immut = false;
          int sid = lookup_symbol_in_scopes(av.s, "Variable");
          if (sid < 0) {
            for (int si = static_cast<int>(symbols_.size()) - 1; si >= 0; --si) {
              if (symbols_[si].name == av.s) {
                sid = si;
                break;
              }
            }
          }
          if (sid >= 0 && symbols_[sid].decl_node_id >= 0) {
            const eval::EvalValue* dv = nodes_[symbols_[sid].decl_node_id].val;
            if (dv && dv->kind == eval::EvalValue::Kind::Ctor &&
                (dv->ctor_name == "ValStmt" || dv->ctor_name == "PropDecl") &&
                !dv->elements.empty() && dv->elements[0].kind == eval::EvalValue::Kind::Bool) {
              immut = !dv->elements[0].bool_value;
            }
          }
          RVal r;
          r.kind = RVal::Kind::Bool;
          r.b = immut;
          return r;
        }
        if (e->name == "hasWhenElse" && !e->args.empty()) {
          RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
          bool has_else = false;
          for (const auto& el : av.list) {
            if (el.id >= 0 && nodes_[el.id].val &&
                nodes_[el.id].val->kind == eval::EvalValue::Kind::Ctor &&
                nodes_[el.id].val->ctor_name == "WhenElse") {
              has_else = true;
              break;
            }
          }
          RVal r;
          r.kind = RVal::Kind::Bool;
          r.b = has_else;
          return r;
        }
        if (e->name == "nameSpan" || e->name == "nodeSpan" || e->name == "replace" ||
            e->name == "textOf") {
          RVal r;
          r.kind = RVal::Kind::String;
          if (e->name == "replace" && e->args.size() >= 2) {
            r = eval_expr(e->args[1], bindings, locals, cur_node_id, level, depth + 1);
          } else if (e->name == "textOf" && !e->args.empty()) {
            RVal av = eval_expr(e->args[0], bindings, locals, cur_node_id, level, depth + 1);
            if (av.id >= 0 && nodes_[av.id].val) {
              r.s = eval::format_value(*nodes_[av.id].val);
            }
          }
          return r;
        }
        return {};
      }
      case PslExpr::Kind::RecordLit:
        return {};
    }
    return {};
  }

  std::string format_diag_message(std::string_view tpl,
                                  const std::vector<std::pair<std::string, PslExprPtr>>& args,
                                  const std::unordered_map<std::string, PatBinding>& bindings,
                                  const std::unordered_map<std::string, RVal>& locals,
                                  int cur_node_id,
                                  int level) {
    std::string out(tpl);
    for (const auto& [k, expr] : args) {
      RVal v = eval_expr(expr, bindings, locals, cur_node_id, level);
      std::string rep;
      if (v.kind == RVal::Kind::String) rep = v.s;
      else if (v.kind == RVal::Kind::Number) {
        std::ostringstream ss;
        ss << v.num;
        rep = ss.str();
      } else if (v.kind == RVal::Kind::TypeId) rep = types_.format(v.id);
      std::string ph = "{" + k + "}";
      std::size_t pos = 0;
      while ((pos = out.find(ph, pos)) != std::string::npos) {
        out.replace(pos, ph.size(), rep);
        pos += rep.size();
      }
    }
    // Also substitute directly from bindings `{x}` for lint `say` messages
    for (const auto& [bk, b] : bindings) {
      std::string ph = "{" + bk + "}";
      std::string rep = (b.kind == PatBinding::Kind::String) ? b.str_val : bk;
      std::size_t pos = 0;
      while ((pos = out.find(ph, pos)) != std::string::npos) {
        out.replace(pos, ph.size(), rep);
        pos += rep.size();
      }
    }
    return out;
  }

  void emit_or_fail(const PslOrFail& of,
                    const std::unordered_map<std::string, PatBinding>& bindings,
                    const std::unordered_map<std::string, RVal>& locals,
                    int cur_node_id,
                    int level) {
    if (!of.present) return;
    PslDiagnostic d;
    d.code = of.diag_code;
    auto it = diag_defs_.find(of.diag_code);
    if (it != diag_defs_.end()) {
      d.severity = it->second->severity;
      d.message = format_diag_message(
          it->second->template_text, of.args, bindings, locals, cur_node_id, level);
    } else {
      d.severity = PslSeverity::Error;
      d.message = "semantic check failed (" + of.diag_code + ")";
    }
    diags_.push_back(std::move(d));
  }

  int lookup_symbol_in_scopes(const std::string& name, const std::string& kind) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto fit = it->find(name);
      if (fit != it->end()) {
        if (kind.empty() || symbols_[fit->second].kind == kind ||
            symbols_[fit->second].kind == "Builtin") {
          return fit->second;
        }
      }
    }
    return -1;
  }

  void exec_node_stmt(const PslNodeStmt& st,
                      const std::unordered_map<std::string, PatBinding>& bindings,
                      std::unordered_map<std::string, RVal>& locals,
                      int nid,
                      int level) {
    switch (st.kind) {
      case PslNodeStmt::Kind::FreshBind: {
        RVal r;
        r.kind = RVal::Kind::TypeId;
        r.id = types_.fresh_var(level);
        locals[st.target_var] = r;
        break;
      }
      case PslNodeStmt::Kind::LookupBind: {
        RVal kv = eval_expr(st.expr, bindings, locals, nid, level);
        int sid = lookup_symbol_in_scopes(kv.s, st.sym_kind);
        if (sid >= 0) {
          ++symbols_[sid].use_count;
          node_resolved_sym_[nid] = sid;
          RVal r;
          r.kind = RVal::Kind::SymId;
          r.id = sid;
          locals[st.target_var] = r;
        } else {
          emit_or_fail(st.or_fail, bindings, locals, nid, level);
          RVal r;
          r.kind = RVal::Kind::TypeId;
          r.id = types_.make_error();
          locals[st.target_var] = r;
        }
        break;
      }
      case PslNodeStmt::Kind::Declare: {
        RVal nv = eval_expr(st.expr, bindings, locals, nid, level);
        RVal tv = eval_expr(st.expr2, bindings, locals, nid, level);
        RuntimeSymbol sym;
        sym.name = nv.s;
        sym.kind = st.sym_kind;
        sym.type_var = (tv.kind == RVal::Kind::TypeId) ? tv.id : types_.fresh_var(level);
        sym.decl_node_id = nid;
        sym.arity = types_.arity_of(sym.type_var);
        int sid = static_cast<int>(symbols_.size());
        symbols_.push_back(std::move(sym));
        scopes_.back()[nv.s] = sid;
        node_declared_sym_[nid] = sid;
        break;
      }
      case PslNodeStmt::Kind::Scope: {
        auto bit = bindings.find(st.target_var);
        if (bit != bindings.end() && bit->second.node_id >= 0) {
          scopes_.emplace_back();
          for (const auto& sub : st.nested) {
            exec_node_stmt(*sub, bindings, locals, nid, level + 1);
          }
          run_node_passes(bit->second.node_id, level + 1);
          scopes_.pop_back();
        }
        break;
      }
      case PslNodeStmt::Kind::Unify: {
        RVal t1 = eval_expr(st.expr, bindings, locals, nid, level);
        RVal t2 = eval_expr(st.expr2, bindings, locals, nid, level);
        ++unifications_;
        if (!types_.unify(t1.id, t2.id)) {
          emit_or_fail(st.or_fail, bindings, locals, nid, level);
        }
        break;
      }
      case PslNodeStmt::Kind::Require: {
        RVal cv = eval_expr(st.expr, bindings, locals, nid, level);
        if (!cv.b) {
          emit_or_fail(st.or_fail, bindings, locals, nid, level);
        }
        break;
      }
      case PslNodeStmt::Kind::AssignAttr: {
        RVal av = eval_expr(st.expr, bindings, locals, nid, level);
        if (st.target_var == "type") {
          node_types_[nid] = (av.kind == RVal::Kind::TypeId) ? av.id : types_.fresh_var(level);
        } else {
          node_attrs_[nid][st.target_var] = av;
        }
        break;
      }
    }
  }

  std::unordered_set<int> scoped_children_;

  void run_node_passes(int nid, int level) {
    if (nid < 0 || nid >= static_cast<int>(nodes_.size())) return;
    if (scoped_children_.contains(nid)) return;

    const eval::EvalValue* val = nodes_[nid].val;
    if (!val) return;

    // Match first applicable `node` rule
    const PslNodeRule* matched_rule = nullptr;
    std::unordered_map<std::string, PatBinding> bindings;
    for (const auto& nr : mod_.node_rules) {
      std::unordered_map<std::string, PatBinding> cand;
      if (match_psl_pattern(nr.pattern, *val, nid, val_to_id_, grammar_, cand)) {
        matched_rule = &nr;
        bindings = std::move(cand);
        break;
      }
    }

    if (matched_rule) {
      // Find any child handled via `scope child $ ...`
      std::unordered_set<int> scoped_ids;
      for (const auto& st : matched_rule->stmts) {
        if (st.kind == PslNodeStmt::Kind::Scope) {
          auto bit = bindings.find(st.target_var);
          if (bit != bindings.end() && bit->second.node_id >= 0) {
            scoped_ids.insert(bit->second.node_id);
          }
        }
      }
      // First evaluate non-scoped children (e.g. `e` in `Let x e body`)
      for (int cid : nodes_[nid].children) {
        if (!scoped_ids.contains(cid)) {
          run_node_passes(cid, level);
        }
      }
      std::unordered_map<std::string, RVal> locals;
      for (const auto& st : matched_rule->stmts) {
        if (st.kind == PslNodeStmt::Kind::Scope) {
          auto bit = bindings.find(st.target_var);
          if (bit != bindings.end() && bit->second.node_id >= 0) {
            int scid = bit->second.node_id;
            scopes_.emplace_back();
            for (const auto& sub : st.nested) {
              exec_node_stmt(*sub, bindings, locals, nid, level + 1);
            }
            run_node_passes(scid, level + 1);
            scopes_.pop_back();
            scoped_children_.insert(scid);
          }
        } else {
          exec_node_stmt(st, bindings, locals, nid, level);
        }
      }
    } else {
      for (int cid : nodes_[nid].children) {
        run_node_passes(cid, level);
      }
    }
  }

  void run_dataflow_pass(PslAnalysisResult& res) {
    // Build CFG over AST node IDs using `flow` rules (or default linear traversal)
    const int n = static_cast<int>(nodes_.size());
    std::vector<std::vector<int>> succ(static_cast<std::size_t>(n));
    std::vector<std::vector<int>> pred(static_cast<std::size_t>(n));
    auto add_edge = [&](int u, int v) {
      if (u >= 0 && v >= 0 && u < n && v < n && u != v) {
        succ[u].push_back(v);
        pred[v].push_back(u);
      }
    };

    for (int i = 0; i < n; ++i) {
      reachable_[i] = false;
      const auto& children = nodes_[i].children;
      if (!children.empty()) {
        add_edge(i, children[0]);
        for (std::size_t k = 0; k + 1 < children.size(); ++k) {
          // If child k is a non-empty list, connect its last element into child k+1 (e.g. HsLet binds -> body)
          if (nodes_[children[k]].val &&
              nodes_[children[k]].val->kind == eval::EvalValue::Kind::List &&
              !nodes_[children[k]].children.empty()) {
            add_edge(nodes_[children[k]].children.back(), children[k + 1]);
          } else {
            add_edge(children[k], children[k + 1]);
          }
        }
      }
    }

    // BFS reachability from root 0 (respecting Return/Jump to Exit pruning siblings)
    std::vector<bool> kills_seq(static_cast<std::size_t>(n), false);
    for (int i = 0; i < n; ++i) {
      for (const auto& fr : mod_.flow_rules) {
        std::unordered_map<std::string, PatBinding> b;
        if (nodes_[i].val &&
            match_psl_pattern(fr.pattern, *nodes_[i].val, i, val_to_id_, grammar_, b)) {
          if (fr.expr.kind == PslFlowExpr::Kind::Jump && fr.expr.target_exit) {
            kills_seq[i] = true;
          }
          break;
        }
      }
    }

    std::vector<int> q;
    q.push_back(0);
    reachable_[0] = true;
    for (std::size_t qi = 0; qi < q.size(); ++qi) {
      int u = q[qi];
      for (int v : succ[u]) {
        if (kills_seq[u] && nodes_[u].parent_id == nodes_[v].parent_id) {
          continue;
        }
        if (!reachable_[v]) {
          reachable_[v] = true;
          q.push_back(v);
        }
      }
    }
    res.cfg_blocks = q.size();

    // Solve each analysis via iterative fixed-point over bitvector/set domain
    for (const auto& an : mod_.analyses) {
      std::vector<std::set<std::string>> gen_set(static_cast<std::size_t>(n));
      std::vector<std::set<std::string>> kill_set(static_cast<std::size_t>(n));
      std::set<std::string> universe;

      for (int i = 0; i < n; ++i) {
        if (!nodes_[i].val) continue;
        for (const auto& gc : an.gen_clauses) {
          std::unordered_map<std::string, PatBinding> b;
          if (match_psl_pattern(gc.pattern, *nodes_[i].val, i, val_to_id_, grammar_, b)) {
            RVal gv = eval_expr(gc.expr, b, {}, i, 0);
            for (const auto& el : gv.list) {
              if (!el.s.empty()) {
                gen_set[i].insert(el.s);
                universe.insert(el.s);
              }
            }
          }
        }
        for (const auto& kc : an.kill_clauses) {
          std::unordered_map<std::string, PatBinding> b;
          if (match_psl_pattern(kc.pattern, *nodes_[i].val, i, val_to_id_, grammar_, b)) {
            RVal kv = eval_expr(kc.expr, b, {}, i, 0);
            for (const auto& el : kv.list) {
              if (!el.s.empty()) {
                kill_set[i].insert(el.s);
                universe.insert(el.s);
              }
            }
          }
        }
      }

      std::vector<std::set<std::string>> in_s(static_cast<std::size_t>(n));
      std::vector<std::set<std::string>> out_s(static_cast<std::size_t>(n));
      if (an.meet == AnalysisMeet::Must) {
        for (int i = 1; i < n; ++i) {
          in_s[i] = universe;
          out_s[i] = universe;
        }
      }

      bool changed = true;
      int iter = 0;
      while (changed && iter < 128) {
        changed = false;
        ++iter;
        for (int i = 0; i < n; ++i) {
          if (an.direction == AnalysisDir::Forward) {
            std::set<std::string> next_in;
            if (!pred[i].empty()) {
              next_in = out_s[pred[i][0]];
              for (std::size_t pi = 1; pi < pred[i].size(); ++pi) {
                const auto& pset = out_s[pred[i][pi]];
                if (an.meet == AnalysisMeet::May) {
                  next_in.insert(pset.begin(), pset.end());
                } else {
                  std::set<std::string> inter;
                  for (const auto& x : next_in) {
                    if (pset.contains(x)) inter.insert(x);
                  }
                  next_in = std::move(inter);
                }
              }
            }
            std::set<std::string> next_out = next_in;
            for (const auto& k : kill_set[i]) next_out.erase(k);
            next_out.insert(gen_set[i].begin(), gen_set[i].end());
            if (next_in != in_s[i] || next_out != out_s[i]) {
              in_s[i] = std::move(next_in);
              out_s[i] = std::move(next_out);
              changed = true;
            }
          } else {
            std::set<std::string> next_out;
            if (!succ[i].empty()) {
              next_out = in_s[succ[i][0]];
              for (std::size_t si = 1; si < succ[i].size(); ++si) {
                const auto& sset = in_s[succ[i][si]];
                if (an.meet == AnalysisMeet::May) {
                  next_out.insert(sset.begin(), sset.end());
                } else {
                  std::set<std::string> inter;
                  for (const auto& x : next_out) {
                    if (sset.contains(x)) inter.insert(x);
                  }
                  next_out = std::move(inter);
                }
              }
            }
            std::set<std::string> next_in = next_out;
            for (const auto& k : kill_set[i]) next_in.erase(k);
            next_in.insert(gen_set[i].begin(), gen_set[i].end());
            if (next_in != in_s[i] || next_out != out_s[i]) {
              in_s[i] = std::move(next_in);
              out_s[i] = std::move(next_out);
              changed = true;
            }
          }
        }
      }

      for (int i = 0; i < n; ++i) {
        df_before_[an.name][i] = std::move(in_s[i]);
        df_after_[an.name][i] = std::move(out_s[i]);
      }
    }
  }

  void run_lint_pass(PslAnalysisResult& res) {
    std::unordered_set<std::string> allowed_lints;
    bool filter_profile = false;
    if (!active_profile_.empty()) {
      for (const auto& pf : mod_.profiles) {
        if (pf.name == active_profile_) {
          filter_profile = true;
          for (const auto& ln : pf.lint_names) allowed_lints.insert(ln);
          break;
        }
      }
    }

    for (const auto& lint : mod_.lints) {
      if (filter_profile && !allowed_lints.contains(lint.name)) continue;
      ++res.lints_evaluated;

      PslSeverity eff_sev = lint.severity;
      // Spec Section 9: draft rules are capped at Warning
      if ((!lint.provenance.has_status || lint.provenance.status == PslStatus::Draft) &&
          eff_sev == PslSeverity::Error) {
        eff_sev = PslSeverity::Warning;
      }

      for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        if (!nodes_[i].val) continue;
        std::unordered_map<std::string, PatBinding> bindings;
        if (!match_psl_pattern(lint.match_pattern, *nodes_[i].val, i, val_to_id_, grammar_,
                               bindings)) {
          continue;
        }
        bool fires = true;
        if (lint.when_expr) {
          RVal cond = eval_expr(lint.when_expr, bindings, {}, i, 0);
          fires = cond.b;
        }
        if (fires) {
          PslDiagnostic d;
          d.code = "PSL-" + lint.name;
          d.lint_name = lint.name;
          d.severity = eff_sev;
          d.message = format_diag_message(lint.say_template, {}, bindings, {}, i, 0);
          if (lint.fix.present) {
            d.has_fix = true;
            d.fix_title = lint.fix.title;
            d.fix_safe = lint.fix.is_safe;
            if (lint.fix.replacement_expr) {
              d.fix_replacement =
                  eval_expr(lint.fix.replacement_expr, bindings, {}, i, 0).s;
            }
          }
          diags_.push_back(std::move(d));
        }
      }
    }
  }
};

// ---------------------------------------------------------------------------
// SHA-256 deterministic lock hashing (pure ISO C++, zero external deps)
// ---------------------------------------------------------------------------

inline std::string sha256_hex(std::string_view input) {
  static constexpr std::array<uint32_t, 64> K = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
      0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
      0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
      0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
      0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
      0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
      0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

  auto rotr = [](uint32_t x, uint32_t n) noexcept -> uint32_t {
    return (x >> n) | (x << (32u - n));
  };

  std::vector<uint8_t> msg(input.begin(), input.end());
  const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8u;
  msg.push_back(0x80u);
  while ((msg.size() % 64u) != 56u) {
    msg.push_back(0x00u);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    msg.push_back(static_cast<uint8_t>((bit_len >> shift) & 0xffu));
  }

  std::array<uint32_t, 8> H = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  for (std::size_t off = 0; off < msg.size(); off += 64u) {
    std::array<uint32_t, 64> W{};
    for (std::size_t t = 0; t < 16u; ++t) {
      W[t] = (static_cast<uint32_t>(msg[off + t * 4u]) << 24u) |
             (static_cast<uint32_t>(msg[off + t * 4u + 1u]) << 16u) |
             (static_cast<uint32_t>(msg[off + t * 4u + 2u]) << 8u) |
             static_cast<uint32_t>(msg[off + t * 4u + 3u]);
    }
    for (std::size_t t = 16u; t < 64u; ++t) {
      uint32_t s0 = rotr(W[t - 15u], 7u) ^ rotr(W[t - 15u], 18u) ^ (W[t - 15u] >> 3u);
      uint32_t s1 = rotr(W[t - 2u], 17u) ^ rotr(W[t - 2u], 19u) ^ (W[t - 2u] >> 10u);
      W[t] = W[t - 16u] + s0 + W[t - 7u] + s1;
    }

    uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
    uint32_t e = H[4], f = H[5], g = H[6], h = H[7];
    for (std::size_t t = 0; t < 64u; ++t) {
      uint32_t S1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + S1 + ch + K[t] + W[t];
      uint32_t S0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;
  }

  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (uint32_t w : H) {
    ss << std::setw(8) << w;
  }
  return ss.str();
}

} // namespace parsley::psl::detail

export namespace parsley::psl {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

inline PslModule parse_psl_module(std::string_view source,
                                  std::string_view filename,
                                  DiagnosticBag& diags) {
  if(source.starts_with("\xef\xbb\xbf"))source.remove_prefix(3);
  const auto tokens = detail::lex_psem(source, filename, diags);
  detail::PslParser parser(tokens, filename, diags);
  return parser.parse_module();
}

inline void check_psl_module(PslModule& mod,
                             const ir::Grammar* grammar,
                             DiagnosticBag& diags,
                             const PslCheckOptions& opts = {}) {
  // 1. Validate --min-status threshold (`PSL-STATUS`)
  auto check_status = [&](const PslProvenance& p, std::string_view what, const Span& sp) {
    if (static_cast<int>(p.status) < static_cast<int>(opts.min_status)) {
      diags.error("PSL-STATUS",
                  std::string(what) + " has status `" +
                      std::string(status_to_string(p.status)) +
                      "` which is below required `--min-status " +
                      std::string(status_to_string(opts.min_status)) + "`",
                  sp);
    }
  };

  if (opts.min_status != PslStatus::Draft) {
    for (const auto& bi : mod.builtins) {
      check_status(bi.provenance, "builtin `" + bi.name + "`", bi.span);
    }
    for (const auto& nr : mod.node_rules) {
      check_status(nr.provenance,
                   "node `" + (nr.pattern ? nr.pattern->name : "<node>") + "`",
                   nr.span);
    }
    for (const auto& an : mod.analyses) {
      check_status(an.provenance, "analysis `" + an.name + "`", an.span);
    }
    for (const auto& lint : mod.lints) {
      check_status(lint.provenance, "lint `" + lint.name + "`", lint.span);
    }
  }

  // 2. Cross-check AST patterns against imported Grammar IR (`PSL-AST-MISMATCH`)
  if (opts.cross_check_grammar && grammar != nullptr) {
    const detail::GrammarCatalog cat = detail::build_grammar_catalog(*grammar);
    for (const auto& nr : mod.node_rules) {
      detail::check_pattern_against_grammar(nr.pattern, cat, diags);
    }
    for (const auto& fr : mod.flow_rules) {
      detail::check_pattern_against_grammar(fr.pattern, cat, diags);
    }
    for (const auto& an : mod.analyses) {
      for (const auto& gc : an.gen_clauses) {
        detail::check_pattern_against_grammar(gc.pattern, cat, diags);
      }
      for (const auto& kc : an.kill_clauses) {
        detail::check_pattern_against_grammar(kc.pattern, cat, diags);
      }
    }
    for (const auto& lint : mod.lints) {
      detail::check_pattern_against_grammar(lint.match_pattern, cat, diags);
    }
  }

  // 3. Validate diagnostic references in `orFail`
  std::unordered_set<std::string> known_diags;
  for (const auto& d : mod.diags) {
    known_diags.insert(d.code);
  }
  auto check_or_fail = [&](const PslOrFail& of) {
    if (of.present && !known_diags.contains(of.diag_code)) {
      diags.warning("PSL-DIAG-UNKNOWN",
                    "diagnostic `" + of.diag_code + "` is not declared with `diag`",
                    of.span);
    }
  };
  for (const auto& nr : mod.node_rules) {
    for (const auto& st : nr.stmts) {
      check_or_fail(st.or_fail);
      for (const auto& sub : st.nested) {
        check_or_fail(sub->or_fail);
      }
    }
  }

  // 4. Validate profile references
  std::unordered_set<std::string> lint_names;
  for (const auto& lint : mod.lints) {
    lint_names.insert(lint.name);
  }
  for (const auto& pf : mod.profiles) {
    for (const auto& ln : pf.lint_names) {
      if (!lint_names.contains(ln)) {
        diags.error("PSL-PROFILE",
                    "profile `" + pf.name + "` references unknown lint `" + ln + "`",
                    pf.span);
      }
    }
  }

  // 5. Validate mandatory examples (`PSL-EXAMPLE-MISSING`): every lint needs fires + silent
  if (opts.require_examples) {
    std::unordered_map<std::string, std::pair<bool, bool>> ex_cov;
    for (const auto& blk : mod.examples) {
      auto& cov = ex_cov[blk.target_name];
      for (const auto& c : blk.cases) {
        if (c.expect_fire) cov.first = true;
        else cov.second = true;
      }
    }
    for (const auto& lint : mod.lints) {
      const auto it = ex_cov.find(lint.name);
      if (it == ex_cov.end()) {
        diags.error("PSL-EXAMPLE-MISSING",
                    "lint `" + lint.name +
                        "` must have an `example " + lint.name +
                        "` block with both `fires` and `silent` cases",
                    lint.span);
      } else if (!it->second.first || !it->second.second) {
        diags.error("PSL-EXAMPLE-MISSING",
                    "lint `" + lint.name + "` requires both at least one `fires` and one `silent` example",
                    lint.span);
      }
    }
  }

  // 6. Attribute cycle detection (`PSL-CYCLE`) + deterministic 5-pass execution plan
  std::vector<std::string> attr_order;
  const bool acyclic = detail::detect_attr_cycles(mod, diags, attr_order);

  PslExecutionPlan plan;
  plan.verified_acyclic = acyclic;
  plan.attribute_eval_order = std::move(attr_order);

  plan.passes[0] = {PslPassKind::ScopeAndDeclare, "Pass 1: Scope stack and symbol declarations", {}};
  plan.passes[1] = {PslPassKind::NameResolution, "Pass 2: Lexical & module symbol lookup", {}};
  plan.passes[2] = {PslPassKind::TypeInference, "Pass 3: Hindley-Milner unification & require checks", {}};
  plan.passes[3] = {PslPassKind::Dataflow, "Pass 4: CFG construction & bitvector dataflow solver", {}};
  plan.passes[4] = {PslPassKind::LintAndFix, "Pass 5: Lint matchers, autofixes & profile filter", {}};

  for (const auto& nr : mod.node_rules) {
    const std::string cname = nr.pattern ? nr.pattern->name : "<node>";
    bool p1 = false, p2 = false, p3 = false;
    for (const auto& st : nr.stmts) {
      if (st.kind == PslNodeStmt::Kind::Scope || st.kind == PslNodeStmt::Kind::Declare) p1 = true;
      if (st.kind == PslNodeStmt::Kind::LookupBind) p2 = true;
      if (st.kind == PslNodeStmt::Kind::FreshBind || st.kind == PslNodeStmt::Kind::Unify ||
          st.kind == PslNodeStmt::Kind::Require || st.kind == PslNodeStmt::Kind::AssignAttr) {
        p3 = true;
      }
      for (const auto& sub : st.nested) {
        if (sub->kind == PslNodeStmt::Kind::Declare) p1 = true;
      }
    }
    if (p1) plan.passes[0].active_ctors.push_back(cname);
    if (p2) plan.passes[1].active_ctors.push_back(cname);
    if (p3) plan.passes[2].active_ctors.push_back(cname);
  }
  for (const auto& fr : mod.flow_rules) {
    if (fr.pattern) plan.passes[3].active_ctors.push_back(fr.pattern->name);
  }
  for (const auto& lr : mod.lints) {
    if (lr.match_pattern) plan.passes[4].active_ctors.push_back(lr.match_pattern->name);
  }
  mod.pass_plan = std::move(plan);
}

inline PslAnalysisResult analyze_ast_with_psl(const PslModule& mod,
                                              const ir::Grammar* grammar,
                                              const eval::EvalValue& root,
                                              std::string_view active_profile = "") {
  detail::PslRuntime rt(mod, grammar, root, active_profile);
  return rt.execute();
}

inline PslExampleSummary run_psl_examples(const PslModule& mod,
                                          const ir::Grammar& grammar,
                                          DiagnosticBag& diags) {
  PslExampleSummary summary;
  for (const auto& blk : mod.examples) {
    for (const auto& ex : blk.cases) {
      ++summary.total;
      PslExampleResult er;
      er.lint_name = blk.target_name;
      er.expect_fire = ex.expect_fire;
      er.input = ex.input;

      auto parsed = eval::parse_input(grammar, ex.input);
      if (!parsed.ok) {
        er.passed = false;
        er.message = "grammar parse failed on PSL example `" + ex.input + "`: " +
                     (parsed.error ? parsed.error->message : "parse error");
        diags.error("PSL-EXAMPLE-FAIL", er.message, ex.span);
        ++summary.failed;
        summary.cases.push_back(std::move(er));
        continue;
      }

      auto analysis = analyze_ast_with_psl(mod, &grammar, parsed.value);
      bool matched_lint = false;
      std::string matched_msg;
      for (const auto& d : analysis.diagnostics) {
        if (d.lint_name == blk.target_name) {
          matched_lint = true;
          matched_msg = d.message;
          break;
        }
      }

      if (ex.expect_fire) {
        if (matched_lint) {
          er.passed = true;
          er.message = "fired as expected: " + matched_msg;
          ++summary.passed;
        } else {
          er.passed = false;
          er.message = "expected lint `" + blk.target_name + "` to fire on `" + ex.input +
                       "`, but it stayed silent";
          diags.error("PSL-EXAMPLE-FAIL", er.message, ex.span);
          ++summary.failed;
        }
      } else {
        if (!matched_lint) {
          er.passed = true;
          er.message = "stayed silent as expected";
          ++summary.passed;
        } else {
          er.passed = false;
          er.message = "expected lint `" + blk.target_name + "` to stay silent on `" + ex.input +
                       "`, but it fired: " + matched_msg;
          diags.error("PSL-EXAMPLE-FAIL", er.message, ex.span);
          ++summary.failed;
        }
      }
      summary.cases.push_back(std::move(er));
    }
  }
  return summary;
}

// ---------------------------------------------------------------------------
// Lockfile hashing (`<Mod>.psem.lock`)
// ---------------------------------------------------------------------------

inline PslLockData compute_psl_lock_data(const ir::Grammar& grammar,
                                         std::string_view lang_version = "Sem1") {
  PslLockData lock;
  lock.module_name = grammar.module_name;
  lock.language_version = std::string(lang_version);

  auto sorted_types = grammar.gTypes;
  std::sort(sorted_types.begin(), sorted_types.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });

  std::ostringstream canonical;
  canonical << "module=" << grammar.module_name << ";lang=" << lang_version << "\n";
  for (const auto& td : sorted_types) {
    std::ostringstream ts;
    ts << "data " << td.name;
    for (const auto& p : td.type_params) ts << " " << p;
    ts << " = ";
    for (std::size_t ci = 0; ci < td.constructors.size(); ++ci) {
      if (ci > 0) ts << " | ";
      const auto& c = td.constructors[ci];
      ts << c.name;
      if (c.is_record) {
        ts << " {";
        for (std::size_t fi = 0; fi < c.record_fields.size(); ++fi) {
          if (fi > 0) ts << ", ";
          ts << c.record_fields[fi].name << "::" << ir::type_to_string(c.record_fields[fi].type);
        }
        ts << "}";
      } else {
        for (const auto& f : c.positional_fields) {
          ts << " (" << ir::type_to_string(f) << ")";
        }
      }
    }
    const std::string tstr = ts.str();
    lock.type_hashes[td.name] = detail::sha256_hex(tstr);
    canonical << tstr << "\n";
  }
  lock.ast_types_hash = detail::sha256_hex(canonical.str());
  return lock;
}

inline std::string serialize_psl_lock(const PslLockData& lock) {
  std::ostringstream out;
  out << "# Parsley Semantic Lockfile (do not edit manually)\n";
  out << "module = " << lock.module_name << "\n";
  out << "language = " << lock.language_version << "\n";
  out << "ast_types_sha256 = " << lock.ast_types_hash << "\n";
  for (const auto& [name, hash] : lock.type_hashes) {
    out << "type." << name << " = " << hash << "\n";
  }
  return out.str();
}

inline std::optional<PslLockData> parse_psl_lock(std::string_view text) {
  PslLockData lock;
  std::size_t pos = 0;
  bool seen_hash = false;
  while (pos < text.size()) {
    std::size_t nl = text.find('\n', pos);
    std::string line = (nl == std::string_view::npos)
                           ? detail::trim_ascii(text.substr(pos))
                           : detail::trim_ascii(text.substr(pos, nl - pos));
    if (!line.empty() && line[0] != '#') {
      auto eq = line.find('=');
      if (eq != std::string::npos) {
        std::string k = detail::trim_ascii(std::string_view(line).substr(0, eq));
        std::string v = detail::trim_ascii(std::string_view(line).substr(eq + 1));
        if (k == "module") lock.module_name = v;
        else if (k == "language") lock.language_version = v;
        else if (k == "ast_types_sha256") {
          lock.ast_types_hash = v;
          seen_hash = true;
        } else if (k.rfind("type.", 0) == 0) {
          lock.type_hashes[k.substr(5)] = v;
        }
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  if (!seen_hash) return std::nullopt;
  return lock;
}

inline PslLockDiff verify_psl_lock(const PslLockData& expected, const PslLockData& current) {
  PslLockDiff diff;
  if (expected.ast_types_hash == current.ast_types_hash) {
    diff.matches = true;
    return diff;
  }
  diff.matches = false;
  for (const auto& [tname, hash] : current.type_hashes) {
    auto it = expected.type_hashes.find(tname);
    if (it == expected.type_hashes.end()) {
      diff.added_types.push_back(tname);
    } else if (it->second != hash) {
      diff.changed_types.push_back(tname);
    }
  }
  for (const auto& [tname, _hash] : expected.type_hashes) {
    if (!current.type_hashes.contains(tname)) {
      diff.removed_types.push_back(tname);
    }
  }
  return diff;
}

// ---------------------------------------------------------------------------
// ISO C23 PSL Codegen Implementation (@sema-ir)
// ---------------------------------------------------------------------------

namespace detail::c23 {

inline std::string to_upper_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

inline std::string sanitize_ident(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    out.push_back((std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_');
  }
  if (out.empty()) out = "anon";
  if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
  return out;
}

inline std::string c_str_lit(std::string_view text) { return c23_text::literal(text); }

struct CtorInfo {
  std::string ctor_name;
  std::string type_name;
  std::string enum_name;
};

struct CtorCatalog {
  std::vector<CtorInfo> ctors;
  std::unordered_map<std::string, std::size_t> by_name;
};

inline void add_ctor(CtorCatalog& cat,
                     const std::string& mod_up,
                     const std::string& ctor,
                     const std::string& type_name) {
  if (ctor.empty() || ctor == "True" || ctor == "False") return;
  auto it = cat.by_name.find(ctor);
  if (it != cat.by_name.end()) {
    if (cat.ctors[it->second].type_name.empty() && !type_name.empty()) {
      cat.ctors[it->second].type_name = type_name;
    }
    return;
  }
  std::string base = mod_up + "_NODE_" + to_upper_ascii(sanitize_ident(ctor));
  std::string uniq = base;
  int suffix = 1;
  bool dup = true;
  while (dup) {
    dup = false;
    for (const auto& c : cat.ctors) {
      if (c.enum_name == uniq) {
        dup = true;
        uniq = base + "_" + std::to_string(++suffix);
        break;
      }
    }
  }
  cat.by_name[ctor] = cat.ctors.size();
  cat.ctors.push_back({ctor, type_name, uniq});
}

inline void collect_pat_ctors(const PslPatternPtr& p, CtorCatalog& cat, const std::string& mod_up) {
  if (!p) return;
  if (p->kind == PslPattern::Kind::Ctor) {
    add_ctor(cat, mod_up, p->name, "");
  }
  for (const auto& a : p->args) collect_pat_ctors(a, cat, mod_up);
}

inline CtorCatalog build_ctor_catalog(const PslModule& psem,
                                      const ir::Grammar& grammar,
                                      const std::string& mod_up) {
  CtorCatalog cat;
  for (const auto& td : grammar.gTypes) {
    for (const auto& c : td.constructors) {
      add_ctor(cat, mod_up, c.name, td.name);
    }
  }
  for (const auto& nr : psem.node_rules) collect_pat_ctors(nr.pattern, cat, mod_up);
  for (const auto& fr : psem.flow_rules) collect_pat_ctors(fr.pattern, cat, mod_up);
  for (const auto& an : psem.analyses) {
    for (const auto& g : an.gen_clauses) collect_pat_ctors(g.pattern, cat, mod_up);
    for (const auto& k : an.kill_clauses) collect_pat_ctors(k.pattern, cat, mod_up);
  }
  for (const auto& l : psem.lints) collect_pat_ctors(l.match_pattern, cat, mod_up);
  return cat;
}

inline std::string enum_for_ctor(const CtorCatalog& cat,
                                 const std::string& mod_up,
                                 std::string_view ctor) {
  auto it = cat.by_name.find(std::string(ctor));
  if (it != cat.by_name.end()) return cat.ctors[it->second].enum_name;
  return mod_up + "_NODE_UNKNOWN";
}

inline std::string type_for_ctor(const CtorCatalog& cat, std::string_view ctor) {
  auto it = cat.by_name.find(std::string(ctor));
  if (it != cat.by_name.end()) return cat.ctors[it->second].type_name;
  return "";
}

inline void collect_pat_vars(const PslPatternPtr& p, std::vector<std::string>& out) {
  if (!p) return;
  if ((p->kind == PslPattern::Kind::Var || p->kind == PslPattern::Kind::TypedVar) &&
      !p->name.empty() && p->name != "_") {
    if (std::find(out.begin(), out.end(), p->name) == out.end()) {
      out.push_back(p->name);
    }
  }
  for (const auto& a : p->args) collect_pat_vars(a, out);
}

inline std::string emit_ast_header(const std::string& mod_low,
                                   const std::string& mod_up,
                                   const CtorCatalog& cat) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#ifndef " << mod_up << "_AST_H\n";
  o << "#define " << mod_up << "_AST_H\n\n";
  o << "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n\n";
  o << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

  o << "#ifndef PARSLEY_API\n"
       "#  if defined(PARSLEY_STATIC)\n"
       "#    define PARSLEY_API\n"
       "#  elif defined(_WIN32)\n"
       "#    ifdef PARSLEY_BUILDING_DLL\n"
       "#      define PARSLEY_API __declspec(dllexport)\n"
       "#    else\n"
       "#      define PARSLEY_API __declspec(dllimport)\n"
       "#    endif\n"
       "#  elif defined(__GNUC__) || defined(__clang__)\n"
       "#    define PARSLEY_API __attribute__((visibility(\"default\")))\n"
       "#  else\n"
       "#    define PARSLEY_API\n"
       "#  endif\n"
       "#endif\n\n";

  o << "#ifndef PSL_RUNTIME_TYPES_DEFINED\n#define PSL_RUNTIME_TYPES_DEFINED\n\n";
  o << "typedef struct psl_str {\n  const char *ptr;\n  size_t len;\n} psl_str;\n\n";
  o << "typedef struct psl_span {\n  uint32_t offset;\n  uint32_t len;\n  uint32_t line;\n  uint32_t col;\n} psl_span;\n\n";
  o << "typedef struct psl_arena {\n  uint8_t *base;\n  size_t cap;\n  size_t used;\n  int error; /* 1 exhausted, 2 invalid size/alignment */\n} psl_arena;\n\n";

  o << "static inline void psl_arena_init(psl_arena *a, void *buf, size_t cap) {\n";
  o << "  if (!a) return;\n  a->base = (uint8_t *)buf;\n  a->cap = cap;\n  a->used = 0;\n  a->error = 0;\n}\n\n";

  o << "static inline void *psl_arena_alloc(psl_arena *a, size_t bytes, size_t align) {\n";
  o << "  if (!a) return NULL;\n  if (!a->base) { a->error=1; return NULL; }\n";
  o << "  if (align == 0) align = 8;\n";
  o << "  if ((align & (align-1u)) || a->used > a->cap || (uintptr_t)a->base > UINTPTR_MAX-a->used || (uintptr_t)a->base+a->used > UINTPTR_MAX-(align-1u)) {a->error=2;return NULL;}\n";
  o << "  uintptr_t address = ((uintptr_t)a->base+a->used+(align-1u)) & ~(uintptr_t)(align-1u);\n";
  o << "  size_t aligned = (size_t)(address-(uintptr_t)a->base);\n";
  o << "  if (aligned > a->cap || bytes > a->cap - aligned) {a->error=1;return NULL;}\n";
  o << "  void *p = a->base + aligned;\n";
  o << "  if (bytes > 0) memset(p, 0, bytes);\n";
  o << "  a->used = aligned + bytes;\n";
  o << "  return p;\n";
  o << "}\n\n";

  o << "static inline psl_str psl_str_make(const char *p, size_t len) {\n";
  o << "  psl_str s;\n  s.ptr = p ? p : \"\";\n  s.len = len;\n  return s;\n}\n\n";

  o << "#define PSL_STR_INIT(LIT) { (LIT), sizeof(LIT) - 1u }\n";
  o << "#ifdef __cplusplus\n#define psl_str_lit(LIT) (psl_str{ (LIT), sizeof(LIT) - 1u })\n#else\n#define psl_str_lit(LIT) ((psl_str){ (LIT), sizeof(LIT) - 1u })\n#endif\n\n";

  o << "static inline int psl_str_eq(psl_str a, psl_str b) {\n";
  o << "  if (a.len != b.len) return 0;\n";
  o << "  if (a.len == 0) return 1;\n";
  o << "  return memcmp(a.ptr, b.ptr, a.len) == 0;\n";
  o << "}\n\n";

  o << "static inline int psl_str_starts_with(psl_str s, psl_str prefix) {\n";
  o << "  if (prefix.len > s.len) return 0;\n";
  o << "  if (prefix.len == 0) return 1;\n";
  o << "  return memcmp(s.ptr, prefix.ptr, prefix.len) == 0;\n";
  o << "}\n\n";

  o << "static inline psl_str psl_str_concat(psl_arena *a, psl_str x, psl_str y) {\n";
  o << "  if (y.len == SIZE_MAX || x.len > SIZE_MAX-y.len-1u) {if(a)a->error=2;psl_str invalid={NULL,0};return invalid;}\n  size_t total = x.len + y.len;\n";
  o << "  char *buf = (char *)psl_arena_alloc(a, total + 1u, 1u);\n";
  o << "  if (!buf) {psl_str invalid={NULL,0};return invalid;}\n";
  o << "  if (x.len > 0) memcpy(buf, x.ptr, x.len);\n";
  o << "  if (y.len > 0) memcpy(buf + x.len, y.ptr, y.len);\n";
  o << "  buf[total] = '\\0';\n";
  o << "  return psl_str_make(buf, total);\n";
  o << "}\n\n";
  o << "#endif /* PSL_RUNTIME_TYPES_DEFINED */\n\n";

  o << "typedef enum " << mod_low << "_node_tag {\n";
  o << "  " << mod_up << "_NODE_UNKNOWN = 0,\n";
  o << "  " << mod_up << "_NODE_LIST,\n";
  o << "  " << mod_up << "_NODE_NUMBER,\n";
  o << "  " << mod_up << "_NODE_STRING,\n";
  o << "  " << mod_up << "_NODE_BOOL";
  for (const auto& c : cat.ctors) {
    o << ",\n  " << c.enum_name;
  }
  o << "\n} " << mod_low << "_node_tag;\n\n";

  o << "typedef struct " << mod_low << "_node " << mod_low << "_node;\n";
  o << "struct " << mod_low << "_node {\n";
  o << "  size_t size;\n";
  o << "  " << mod_low << "_node_tag tag;\n";
  o << "  psl_str ctor_name;\n";
  o << "  psl_str type_name;\n";
  o << "  psl_span span;\n";
  o << "  double num_val;\n";
  o << "  int bool_val;\n";
  o << "  psl_str text_val;\n";
  o << "  const " << mod_low << "_node **children;\n";
  o << "  size_t child_count;\n";
  o << "  uint32_t node_id;\n";
  o << "};\n\n";

  o << "static inline const char *" << mod_low << "_node_tag_name(" << mod_low << "_node_tag tag) {\n";
  o << "  switch (tag) {\n";
  o << "    case " << mod_up << "_NODE_LIST: return \"List\";\n";
  o << "    case " << mod_up << "_NODE_NUMBER: return \"Number\";\n";
  o << "    case " << mod_up << "_NODE_STRING: return \"String\";\n";
  o << "    case " << mod_up << "_NODE_BOOL: return \"Bool\";\n";
  for (const auto& c : cat.ctors) {
    o << "    case " << c.enum_name << ": return " << c_str_lit(c.ctor_name) << ";\n";
  }
  o << "    default: return \"Unknown\";\n";
  o << "  }\n}\n\n";

  o << "#ifdef __cplusplus\n}\n#endif\n\n";
  o << "#endif /* " << mod_up << "_AST_H */\n";
  return o.str();
}

inline std::string emit_diag_header(const PslModule& psem,
                                    const std::string& mod_low,
                                    const std::string& mod_up) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#ifndef " << mod_up << "_DIAG_H\n";
  o << "#define " << mod_up << "_DIAG_H\n\n";
  o << "#include \"" << mod_low << "_ast.h\"\n\n";
  o << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  o << "#define " << mod_up << "_PSL_SCHEMA_VERSION 1u\n\n";

  o << "#ifndef PSL_DIAG_TYPES_DEFINED\n#define PSL_DIAG_TYPES_DEFINED\n\n";
  o << "typedef enum psl_severity {\n  PSL_SEV_NOTE = 0,\n  PSL_SEV_WARNING = 1,\n  PSL_SEV_ERROR = 2\n} psl_severity;\n\n";
  o << "typedef enum psl_rule_status {\n  PSL_STATUS_DRAFT = 0,\n  PSL_STATUS_REVIEWED = 1,\n  PSL_STATUS_VERIFIED = 2\n} psl_rule_status;\n\n";
  o << "typedef enum psl_fix_safety {\n  PSL_FIX_UNSAFE = 0,\n  PSL_FIX_SAFE = 1\n} psl_fix_safety;\n\n";
  o << "typedef struct psl_diag_def {\n";
  o << "  size_t size;\n";
  o << "  psl_str code;\n";
  o << "  psl_str rule_name;\n";
  o << "  psl_severity default_sev;\n";
  o << "  psl_rule_status status;\n";
  o << "  double confidence;\n";
  o << "  psl_str template_str;\n";
  o << "  psl_str source_ref;\n";
  o << "} psl_diag_def;\n\n";

  o << "typedef struct psl_diag {\n";
  o << "  size_t size;\n";
  o << "  const psl_diag_def *def;\n";
  o << "  psl_severity severity;\n";
  o << "  psl_span span;\n";
  o << "  psl_str message;\n";
  o << "  psl_span fix_span;\n";
  o << "  psl_str fix_text;\n";
  o << "  psl_fix_safety fix_safety;\n";
  o << "  int has_fix;\n";
  o << "} psl_diag;\n\n";

  o << "typedef void (*psl_sink)(void *user, const psl_diag *d);\n\n";
  o << "#endif /* PSL_DIAG_TYPES_DEFINED */\n\n";

  o << "#define " << mod_up << "_PROFILE_ALL UINT64_C(0)\n";
  std::unordered_map<std::string, std::size_t> lint_idx;
  for (std::size_t i = 0; i < psem.lints.size() && i < 63; ++i) {
    lint_idx[psem.lints[i].name] = i;
  }
  for (const auto& pf : psem.profiles) {
    uint64_t mask = 0;
    for (const auto& ln : pf.lint_names) {
      auto it = lint_idx.find(ln);
      if (it != lint_idx.end()) mask |= (uint64_t{1} << it->second);
    }
    o << "#define " << mod_up << "_PROFILE_" << to_upper_ascii(sanitize_ident(pf.name))
      << " UINT64_C(0x" << std::hex << mask << std::dec << ")\n";
  }
  o << "\n";

  for (const auto& d : psem.diags) {
    o << "PARSLEY_API extern const psl_diag_def " << mod_up << "_DIAG_" << sanitize_ident(d.code) << ";\n";
  }
  for (const auto& l : psem.lints) {
    o << "PARSLEY_API extern const psl_diag_def " << mod_up << "_LINT_" << sanitize_ident(l.name) << ";\n";
  }

  o << "\n#ifdef __cplusplus\n}\n#endif\n\n";
  o << "#endif /* " << mod_up << "_DIAG_H */\n";
  return o.str();
}

inline std::string emit_parse_header(const std::string& mod_low, const std::string& mod_up) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#ifndef " << mod_up << "_PARSE_H\n";
  o << "#define " << mod_up << "_PARSE_H\n\n";
  o << "#include \"" << mod_low << "_diag.h\"\n\n";
  o << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_node(psl_arena *arena, " << mod_low
    << "_node_tag tag, psl_str ctor_name, psl_str type_name, psl_span span, size_t child_count);\n";
  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_str_node(psl_arena *arena, psl_str text, psl_span span);\n";
  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_num_node(psl_arena *arena, double val, psl_span span);\n";
  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_bool_node(psl_arena *arena, int val, psl_span span);\n";
  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_list_node(psl_arena *arena, size_t count, psl_span span);\n";
  o << "PARSLEY_API int " << mod_low << "_parse(psl_arena *arena, psl_str src, const " << mod_low
    << "_node **out_root, psl_diag *err_out);\n\n";
  o << "#ifdef __cplusplus\n}\n#endif\n\n";
  o << "#endif /* " << mod_up << "_PARSE_H */\n";
  return o.str();
}

inline std::string emit_parse_source(const std::string& mod_low,
                                     const std::string& mod_up, const CtorCatalog& cat) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#if !defined(PARSLEY_STATIC) && !defined(PARSLEY_BUILDING_DLL)\n";
  o << "#define PARSLEY_BUILDING_DLL 1\n";
  o << "#endif\n";
  o << "#include \"" << mod_low << "_parse.h\"\n\n";

  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_node(psl_arena *arena, " << mod_low
    << "_node_tag tag, psl_str ctor_name, psl_str type_name, psl_span span, size_t child_count) {\n";
  o << "  " << mod_low << "_node *n = (" << mod_low << "_node *)psl_arena_alloc(arena, sizeof(" << mod_low << "_node), sizeof(void *));\n";
  o << "  if (!n) return NULL;\n";
  o << "  n->size = sizeof(" << mod_low << "_node);\n";
  o << "  n->tag = tag;\n  n->ctor_name = ctor_name;\n  n->type_name = type_name;\n";
  o << "  n->span = span;\n  n->child_count = child_count;\n";
  o << "  if(child_count>SIZE_MAX/sizeof(void*)){arena->error=2;return NULL;}\n  if (child_count > 0) {\n";
  o << "    n->children = (const " << mod_low << "_node **)psl_arena_alloc(arena, child_count * sizeof(const " << mod_low << "_node *), sizeof(void *));\n";
  o << "    if (!n->children) return NULL;\n";
  o << "  }\n  return n;\n}\n\n";

  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_str_node(psl_arena *arena, psl_str text, psl_span span) {\n";
  o << "  " << mod_low << "_node *n = " << mod_low << "_new_node(arena, " << mod_up
    << "_NODE_STRING, psl_str_lit(\"String\"), psl_str_lit(\"Text\"), span, 0);\n";
  o << "  if (n) {\n    if(text.len==SIZE_MAX){arena->error=2;return NULL;}\n    char *copy=(char*)psl_arena_alloc(arena,text.len+1u,1u);if(!copy)return NULL;\n    if(text.len)memcpy(copy,text.ptr,text.len);copy[text.len]=0;n->text_val=psl_str_make(copy,text.len);\n  }\n  return n;\n}\n\n";

  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_num_node(psl_arena *arena, double val, psl_span span) {\n";
  o << "  " << mod_low << "_node *n = " << mod_low << "_new_node(arena, " << mod_up
    << "_NODE_NUMBER, psl_str_lit(\"Number\"), psl_str_lit(\"Double\"), span, 0);\n";
  o << "  if (n) n->num_val = val;\n  return n;\n}\n\n";

  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_bool_node(psl_arena *arena, int val, psl_span span) {\n";
  o << "  " << mod_low << "_node *n = " << mod_low << "_new_node(arena, " << mod_up
    << "_NODE_BOOL, psl_str_lit(\"Bool\"), psl_str_lit(\"Bool\"), span, 0);\n";
  o << "  if (n) n->bool_val = val ? 1 : 0;\n  return n;\n}\n\n";

  o << "PARSLEY_API " << mod_low << "_node *" << mod_low << "_new_list_node(psl_arena *arena, size_t count, psl_span span) {\n";
  o << "  return " << mod_low << "_new_node(arena, " << mod_up
    << "_NODE_LIST, psl_str_lit(\"List\"), psl_str_lit(\"List\"), span, count);\n}\n\n";

  o << "#define PARSLEY_INTERNAL 1\n#include \"include/" << mod_low << "_parser.h\"\n\n";
  o << "static " << mod_low << "_node *convert_parser_value(psl_arena *arena,const ParsleyValue *value,size_t depth) {\n";
  o << "  if(!value || depth>128u){arena->error=2;return NULL;}\n  psl_span span={0u,0u,1u,1u};\n";
  o << "  switch(value->kind){\n";
  o << "    case PARSLEY_VAL_STRING:return " << mod_low << "_new_str_node(arena,psl_str_make(value->string_val,value->string_len),span);\n";
  o << "    case PARSLEY_VAL_NUMBER:return " << mod_low << "_new_num_node(arena,value->is_int?(double)value->integer_val:value->number_val,span);\n";
  o << "    case PARSLEY_VAL_BOOL:return " << mod_low << "_new_bool_node(arena,value->bool_val,span);\n";
  o << "    default:break;\n  }\n";
  o << "  " << mod_low << "_node_tag tag=" << mod_up << "_NODE_UNKNOWN;\n";
  o << "  psl_str ctor=psl_str_lit(\"Unit\"),type=psl_str_lit(\"Unit\");\n";
  o << "  if(value->kind==PARSLEY_VAL_LIST || value->kind==PARSLEY_VAL_TUPLE){tag=" << mod_up << "_NODE_LIST;ctor=psl_str_lit(\"List\");type=psl_str_lit(\"List\");}\n";
  for(const auto& c:cat.ctors) {
    o << "  if(value->kind==PARSLEY_VAL_CTOR && value->ctor_name && strcmp(value->ctor_name," << c_str_lit(c.ctor_name) << ")==0){tag=" << c.enum_name << ";ctor=psl_str_lit(" << c_str_lit(c.ctor_name) << ");type=psl_str_lit(" << c_str_lit(c.type_name) << ");}\n";
  }
  o << "  " << mod_low << "_node *node=" << mod_low << "_new_node(arena,tag,ctor,type,span,value->item_count);\n";
  o << "  if(!node)return NULL;\n  for(size_t i=0;i<value->item_count;++i){node->children[i]=convert_parser_value(arena,value->items[i],depth+1u);if(!node->children[i])return NULL;}\n  return node;\n}\n\n";
  o << "static const psl_diag_def parse_error={sizeof(psl_diag_def),PSL_STR_INIT(\"PSL-PARSE\"),PSL_STR_INIT(\"parse\"),PSL_SEV_ERROR,PSL_STATUS_VERIFIED,1.0,PSL_STR_INIT(\"Syntax error\"),PSL_STR_INIT(\"parser\")};\n";
  o << "PARSLEY_API int " << mod_low << "_parse(psl_arena *arena,psl_str source,const " << mod_low << "_node **root,psl_diag *error){\n";
  o << "  if(!arena || !root || (!source.ptr && source.len))return -1;\n  *root=NULL;\n  if(error)memset(error,0,sizeof(*error));\n";
  o << "  ParsleyResult parsed=parsley_parse_n(source.ptr,source.len);\n";
  o << "  if(!parsed.ok || parsed.consumed!=source.len){\n    int code=parsed.error.code;\n";
  o << "    if(error){error->size=sizeof(*error);error->def=&parse_error;error->severity=PSL_SEV_ERROR;error->span=(psl_span){(uint32_t)parsed.consumed,0u,(uint32_t)parsed.error.line,(uint32_t)parsed.error.col};error->message=psl_str_lit(\"Invalid or incomplete input\");\n      if(parsed.error.message && arena->used<=arena->cap){\n        size_t len=strlen(parsed.error.message);\n        if(len<arena->cap-arena->used){\n          char *message=psl_arena_alloc(arena,len+1u,1u);\n          if(message){memcpy(message,parsed.error.message,len+1u);error->message=psl_str_make(message,len);}\n        }\n      }\n    }\n";
  o << "    parsley_result_free(&parsed);return code==1||code==2?-2:-1;\n  }\n";
  o << "  *root=convert_parser_value(arena,parsed.value,0u);parsley_result_free(&parsed);\n  return *root && !arena->error ? 0 : -2;\n}\n";  return o.str();
}

inline std::string emit_sem_header(const std::string& mod_low, const std::string& mod_up) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#ifndef " << mod_up << "_SEM_H\n";
  o << "#define " << mod_up << "_SEM_H\n\n";
  o << "#include \"" << mod_low << "_diag.h\"\n\n";
  o << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  o << "typedef struct " << mod_low << "_ctx " << mod_low << "_ctx;\n";
  o << "typedef struct " << mod_low << "_options {\n";
  o << "  size_t size;\n";
  o << "  uint32_t schema_version;\n";
  o << "  int min_status;     /* 0: draft, 1: reviewed, 2: verified */\n";
  o << "  uint64_t profile;   /* Bitmask of active lint rules (0 = all) */\n";
  o << "} " << mod_low << "_options;\n\n";
  o << "PARSLEY_API " << mod_low << "_ctx *" << mod_low << "_ctx_new(psl_arena *arena, const " << mod_low << "_options *opt);\n";
  o << "PARSLEY_API int " << mod_low << "_analyze(" << mod_low << "_ctx *cx, const " << mod_low
    << "_node *root, psl_sink sink, void *user);\n";
  o << "PARSLEY_API int " << mod_low << "_psl_selftest(void);\n\n";
  o << "PARSLEY_API size_t " << mod_low << "_diag_count(const " << mod_low << "_ctx *cx);\n";
  o << "PARSLEY_API size_t " << mod_low << "_nodes_visited(const " << mod_low << "_ctx *cx);\n";
  o << "PARSLEY_API size_t " << mod_low << "_symbols_declared(const " << mod_low << "_ctx *cx);\n";
  o << "PARSLEY_API size_t " << mod_low << "_type_unifications(const " << mod_low << "_ctx *cx);\n";
  o << "PARSLEY_API size_t " << mod_low << "_cfg_blocks(const " << mod_low << "_ctx *cx);\n";
  o << "PARSLEY_API size_t " << mod_low << "_lints_evaluated(const " << mod_low << "_ctx *cx);\n\n";
  o << "#ifdef __cplusplus\n}\n#endif\n\n";
  o << "#endif /* " << mod_up << "_SEM_H */\n";
  return o.str();
}

inline std::string emit_type_expr_builder(const PslTypeExprPtr& te) {
  if (!te) return "ty_fresh(cx, 0)";
  switch (te->kind) {
    case PslTypeExpr::Kind::Named:
      return "ty_con(cx, psl_str_lit(" + c_str_lit(te->name) + "))";
    case PslTypeExpr::Kind::Var:
      return "ty_fresh(cx, 0)";
    case PslTypeExpr::Kind::Forall:
      return emit_type_expr_builder(te->right);
    case PslTypeExpr::Kind::Arrow:
      return "ty_app2(cx, psl_str_lit(\"Fn\"), " + emit_type_expr_builder(te->left) + ", " +
             emit_type_expr_builder(te->right) + ")";
    case PslTypeExpr::Kind::List:
      return "ty_app1(cx, psl_str_lit(\"List\"), " + emit_type_expr_builder(te->left) + ")";
    case PslTypeExpr::Kind::App: {
      if (te->args.size() >= 2) {
        return "ty_app2(cx, psl_str_lit(" + c_str_lit(te->name) + "), " +
               emit_type_expr_builder(te->args[0]) + ", " + emit_type_expr_builder(te->args[1]) + ")";
      }
      if (te->args.size() == 1) {
        return "ty_app1(cx, psl_str_lit(" + c_str_lit(te->name) + "), " +
               emit_type_expr_builder(te->args[0]) + ")";
      }
      return "ty_con(cx, psl_str_lit(" + c_str_lit(te->name) + "))";
    }
    case PslTypeExpr::Kind::Tuple:
      return "ty_con(cx, psl_str_lit(\"Tuple\"))";
  }
  return "ty_fresh(cx, 0)";
}

inline void emit_pattern_matcher(std::ostringstream& out,
                                 const std::string& fn_name,
                                 const std::string& mod_low,
                                 const std::string& mod_up,
                                 const CtorCatalog& cat,
                                 const PslPatternPtr& pat,
                                 const std::vector<std::string>& vars) {
  out << "typedef struct pat_bind_" << fn_name << " {\n";
  if (vars.empty()) {
    out << "  int dummy_;\n";
  } else {
    for (const auto& v : vars) {
      out << "  int32_t nid_" << sanitize_ident(v) << ";\n";
      out << "  psl_str str_" << sanitize_ident(v) << ";\n";
    }
  }
  out << "} pat_bind_" << fn_name << ";\n\n";

  out << "static inline int match_pat_" << fn_name << "(const " << mod_low << "_ctx *cx, int32_t nid, pat_bind_"
      << fn_name << " *b) {\n";
  out << "  (void)b;\n";
  out << "  if (nid < 0 || (size_t)nid >= cx->node_count) return 0;\n";
  out << "  const " << mod_low << "_node *n0 = cx->nodes[nid].node;\n";
  out << "  if (!n0) return 0;\n";

  int node_seq = 0;
  auto emit_rec = [&](auto& self, const PslPatternPtr& p, const std::string& nvar, const std::string& idvar) -> void {
    if (!p) return;
    switch (p->kind) {
      case PslPattern::Kind::Wildcard:
        return;
      case PslPattern::Kind::Var:
        if (!p->name.empty() && p->name != "_") {
          const std::string sv = sanitize_ident(p->name);
          out << "  b->nid_" << sv << " = " << idvar << ";\n";
          out << "  b->str_" << sv << " = " << nvar << "->text_val;\n";
        }
        return;
      case PslPattern::Kind::TypedVar:
        out << "  if (" << nvar << "->tag <= " << mod_up << "_NODE_BOOL) return 0;\n";
        if (!p->type_name.empty()) {
          out << "  if (!psl_str_eq(" << nvar << "->type_name, psl_str_lit(" << c_str_lit(p->type_name)
              << "))) return 0;\n";
        }
        if (!p->name.empty() && p->name != "_") {
          const std::string sv = sanitize_ident(p->name);
          out << "  b->nid_" << sv << " = " << idvar << ";\n";
          out << "  b->str_" << sv << " = " << nvar << "->text_val;\n";
        }
        return;
      case PslPattern::Kind::Number:
        out << "  if (" << nvar << "->tag != " << mod_up << "_NODE_NUMBER || (" << nvar
            << "->num_val - " << p->number_value << ") > 1e-9 || (" << p->number_value
            << " - " << nvar << "->num_val) > 1e-9) return 0;\n";
        return;
      case PslPattern::Kind::String:
        out << "  if (" << nvar << "->tag != " << mod_up << "_NODE_STRING || !psl_str_eq("
            << nvar << "->text_val, psl_str_lit(" << c_str_lit(p->name) << "))) return 0;\n";
        return;
      case PslPattern::Kind::List:
        out << "  if (" << nvar << "->tag != " << mod_up << "_NODE_LIST || " << nvar
            << "->child_count != " << p->args.size() << "u) return 0;\n";
        for (std::size_t i = 0; i < p->args.size(); ++i) {
          int cur_seq = ++node_seq;
          std::string cid = "id_" + std::to_string(cur_seq);
          std::string cn = "n_" + std::to_string(cur_seq);
          out << "  int32_t " << cid << " = cx->nodes[" << idvar << "].children[" << i << "];\n";
          out << "  const " << mod_low << "_node *" << cn << " = (" << cid << " >= 0) ? cx->nodes[" << cid << "].node : NULL;\n";
          out << "  if (!" << cn << ") return 0;\n";
          self(self, p->args[i], cn, cid);
        }
        return;
      case PslPattern::Kind::Ctor:
        if (p->args.empty() && (p->name == "True" || p->name == "False")) {
          out << "  if (" << nvar << "->tag != " << mod_up << "_NODE_BOOL || " << nvar
              << "->bool_val != " << (p->name == "True" ? "1" : "0") << ") return 0;\n";
          return;
        }
        out << "  if ((" << nvar << "->tag != " << enum_for_ctor(cat, mod_up, p->name)
            << " && !psl_str_eq(" << nvar << "->ctor_name, psl_str_lit(" << c_str_lit(p->name)
            << "))) || " << nvar << "->child_count != " << p->args.size() << "u) return 0;\n";
        for (std::size_t i = 0; i < p->args.size(); ++i) {
          int cur_seq = ++node_seq;
          std::string cid = "id_" + std::to_string(cur_seq);
          std::string cn = "n_" + std::to_string(cur_seq);
          out << "  int32_t " << cid << " = cx->nodes[" << idvar << "].children[" << i << "];\n";
          out << "  const " << mod_low << "_node *" << cn << " = (" << cid << " >= 0) ? cx->nodes[" << cid << "].node : NULL;\n";
          out << "  if (!" << cn << ") return 0;\n";
          self(self, p->args[i], cn, cid);
        }
        return;
    }
  };
  emit_rec(emit_rec, pat, "n0", "nid");
  out << "  return 1;\n}\n\n";
}

struct ExprEnv {
  std::string mod_low;
  std::string pat_struct;
  std::vector<std::string> pat_vars;
  std::unordered_map<std::string, std::string> locals;
  std::unordered_set<std::string> type_names;
  std::unordered_map<std::string, std::size_t> analysis_idx;
  std::ostringstream* helpers = nullptr;
  int next_helper_id = 0;
};

inline std::string emit_expr(const PslExprPtr& e, ExprEnv& env, int depth = 0) {
  if (!e || depth > 48) return "rv_none()";
  switch (e->kind) {
    case PslExpr::Kind::Number: {
      std::ostringstream s;
      s << "rv_num(" << std::setprecision(15) << e->number_value << ")";
      return s.str();
    }
    case PslExpr::Kind::String:
      return "rv_str(psl_str_lit(" + c_str_lit(e->name) + "))";
    case PslExpr::Kind::Bool:
      return e->bool_value ? "rv_bool(1)" : "rv_bool(0)";
    case PslExpr::Kind::Var: {
      if (e->name == "node" || e->name == "self") return "rv_node(nid)";
      auto lit = env.locals.find(e->name);
      if (lit != env.locals.end()) return lit->second;
      if (std::find(env.pat_vars.begin(), env.pat_vars.end(), e->name) != env.pat_vars.end()) {
        return "rv_from_node(cx, b->nid_" + sanitize_ident(e->name) + ")";
      }
      if (env.type_names.contains(e->name)) {
        return "rv_type(ty_con(cx, psl_str_lit(" + c_str_lit(e->name) + ")))";
      }
      return "rv_str(psl_str_lit(" + c_str_lit(e->name) + "))";
    }
    case PslExpr::Kind::Ctor: {
      if (e->name == "Nothing") return "rv_maybe_none()";
      if (e->name == "Just" && !e->args.empty()) {
        return "rv_maybe_from(" + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->args.empty()) {
        return "rv_type(ty_con(cx, psl_str_lit(" + c_str_lit(e->name) + ")))";
      }
      if (e->args.size() == 1) {
        return "rv_type(ty_app1(cx, psl_str_lit(" + c_str_lit(e->name) + "), rv_as_type(cx, " +
               emit_expr(e->args[0], env, depth + 1) + ", level)))";
      }
      return "rv_type(ty_app2(cx, psl_str_lit(" + c_str_lit(e->name) + "), rv_as_type(cx, " +
             emit_expr(e->args[0], env, depth + 1) + ", level), rv_as_type(cx, " +
             emit_expr(e->args[1], env, depth + 1) + ", level)))";
    }
    case PslExpr::Kind::List: {
      if (e->args.empty()) return "rv_empty_list()";
      int hid = ++env.next_helper_id;
      std::string fname = "eval_list_" + std::to_string(hid);
      (*env.helpers) << "static psl_rval " << fname << "(" << env.mod_low
                     << "_ctx *cx, int32_t nid, int level, const " << env.pat_struct << " *b";
      for (const auto& [k, _v] : env.locals) {
        (*env.helpers) << ", psl_rval loc_" << sanitize_ident(k);
      }
      (*env.helpers) << ") {\n  (void)cx; (void)nid; (void)level; (void)b;\n";
      for (const auto& [k, _v] : env.locals) {
        (*env.helpers) << "  (void)loc_" << sanitize_ident(k) << ";\n";
      }
      (*env.helpers) << "  psl_rval *arr = (psl_rval *)psl_arena_alloc(cx->arena, " << e->args.size()
                     << "u * sizeof(psl_rval), sizeof(void *));\n";
      (*env.helpers) << "  if (!arr) return rv_empty_list();\n";
      ExprEnv sub = env;
      for (const auto& [k, _v] : env.locals) sub.locals[k] = "loc_" + sanitize_ident(k);
      for (std::size_t i = 0; i < e->args.size(); ++i) {
        (*env.helpers) << "  arr[" << i << "] = " << emit_expr(e->args[i], sub, depth + 1) << ";\n";
      }
      (*env.helpers) << "  return rv_list_make(arr, " << e->args.size() << "u);\n}\n\n";
      std::string call = fname + "(cx, nid, level, b";
      for (const auto& [_k, v] : env.locals) call += ", " + v;
      call += ")";
      return call;
    }
    case PslExpr::Kind::ListComp: {
      int hid = ++env.next_helper_id;
      std::string fname = "eval_comp_" + std::to_string(hid);
      (*env.helpers) << "static psl_rval " << fname << "(" << env.mod_low
                     << "_ctx *cx, int32_t nid, int level, const " << env.pat_struct << " *b";
      for (const auto& [k, _v] : env.locals) {
        (*env.helpers) << ", psl_rval loc_" << sanitize_ident(k);
      }
      (*env.helpers) << ") {\n  (void)nid; (void)level; (void)b;\n";
      for (const auto& [k, _v] : env.locals) {
        (*env.helpers) << "  (void)loc_" << sanitize_ident(k) << ";\n";
      }
      ExprEnv sub = env;
      for (const auto& [k, _v] : env.locals) sub.locals[k] = "loc_" + sanitize_ident(k);
      std::string src_code = emit_expr(e->comp_source, sub, depth + 1);
      (*env.helpers) << "  psl_rval src = " << src_code << ";\n";
      (*env.helpers) << "  if (src.kind != RV_LIST || src.count == 0) return rv_empty_list();\n";
      (*env.helpers) << "  psl_rval *arr = (psl_rval *)psl_arena_alloc(cx->arena, src.count * sizeof(psl_rval), sizeof(void *));\n";
      (*env.helpers) << "  if (!arr) return rv_empty_list();\n";
      (*env.helpers) << "  size_t out_n = 0;\n";
      (*env.helpers) << "  for (size_t i = 0; i < src.count; ++i) {\n";
      std::string cv = sanitize_ident(e->comp_var.empty() ? "el" : e->comp_var);
      (*env.helpers) << "    psl_rval loc_" << cv << " = src.items[i];\n";
      sub.locals[e->comp_var] = "loc_" + cv;
      if (e->comp_filter) {
        (*env.helpers) << "    if (!(" << emit_expr(e->comp_filter, sub, depth + 1) << ").b) continue;\n";
      }
      if (!e->args.empty()) {
        (*env.helpers) << "    arr[out_n++] = " << emit_expr(e->args[0], sub, depth + 1) << ";\n";
      }
      (*env.helpers) << "  }\n  return rv_list_make(arr, out_n);\n}\n\n";
      std::string call = fname + "(cx, nid, level, b";
      for (const auto& [_k, v] : env.locals) call += ", " + v;
      call += ")";
      return call;
    }
    case PslExpr::Kind::UnaryOp: {
      std::string a = e->args.empty() ? "rv_none()" : emit_expr(e->args[0], env, depth + 1);
      if (e->name == "not") return "rv_bool(!(" + a + ").b)";
      if (e->name == "-") return "rv_num(-(" + a + ").num)";
      return a;
    }
    case PslExpr::Kind::IfThenElse:
      if (e->args.size() >= 3) {
        return "((" + emit_expr(e->args[0], env, depth + 1) + ").b ? (" +
               emit_expr(e->args[1], env, depth + 1) + ") : (" +
               emit_expr(e->args[2], env, depth + 1) + "))";
      }
      return "rv_none()";
    case PslExpr::Kind::FieldAccess: {
      if (!e->args.empty() && (e->name == "before" || e->name == "after")) {
        auto it = env.analysis_idx.find(e->args[0]->name);
        std::size_t aidx = (it != env.analysis_idx.end()) ? it->second : 0;
        int is_after = (e->name == "after") ? 1 : 0;
        std::string target = (e->args.size() >= 2) ? emit_expr(e->args[1], env, depth + 1) : "rv_node(nid)";
        return "rv_df_list(cx, " + std::to_string(aidx) + "u, " + std::to_string(is_after) + ", " +
               target + ", nid)";
      }
      return "rv_none()";
    }
    case PslExpr::Kind::BinOp: {
      if (e->args.size() < 2) return "rv_none()";
      std::string l = emit_expr(e->args[0], env, depth + 1);
      std::string r = emit_expr(e->args[1], env, depth + 1);
      if (e->name == "&&") return "rv_bool((" + l + ").b && (" + r + ").b)";
      if (e->name == "||") return "rv_bool((" + l + ").b || (" + r + ").b)";
      if (e->name == "==") return "rv_bool(rv_eq(cx, " + l + ", " + r + "))";
      if (e->name == "/=") return "rv_bool(!rv_eq(cx, " + l + ", " + r + "))";
      if (e->name == "<")  return "rv_bool((" + l + ").num < (" + r + ").num)";
      if (e->name == "<=") return "rv_bool((" + l + ").num <= (" + r + ").num)";
      if (e->name == ">")  return "rv_bool((" + l + ").num > (" + r + ").num)";
      if (e->name == ">=") return "rv_bool((" + l + ").num >= (" + r + ").num)";
      if (e->name == "+")  return "rv_num((" + l + ").num + (" + r + ").num)";
      if (e->name == "-")  return "rv_num((" + l + ").num - (" + r + ").num)";
      if (e->name == "*")  return "rv_num((" + l + ").num * (" + r + ").num)";
      if (e->name == "<>") return "rv_concat(cx, " + l + ", " + r + ")";
      if (e->name == "isPrefixOf") return "rv_bool(psl_str_starts_with((" + r + ").s, (" + l + ").s))";
      if (e->name == "in" || e->name == "member") return "rv_bool(rv_list_contains(" + r + ", " + l + "))";
      if (e->name == "notMember") return "rv_bool(!rv_list_contains(" + r + ", " + l + "))";
      return "rv_none()";
    }
    case PslExpr::Kind::Call: {
      if (e->name == "typeOf" && !e->args.empty()) {
        return "rv_type_of(cx, " + emit_expr(e->args[0], env, depth + 1) + ", level)";
      }
      if (e->name == "arity" && !e->args.empty()) {
        return "rv_arity(cx, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "length" && !e->args.empty()) {
        return "rv_length(" + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "and" && !e->args.empty()) {
        return "rv_and_list(" + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "any" && !e->args.empty()) {
        return "rv_any_list(" + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "count" && !e->args.empty()) {
        return "rv_count_list(" + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "useCount" && !e->args.empty()) {
        return "rv_use_count(cx, nid, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "constFold" && !e->args.empty()) {
        return "rv_const_fold(cx, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "reachable" && !e->args.empty()) {
        return "rv_reachable(cx, nid, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "isImmutable" && !e->args.empty()) {
        return "rv_is_immutable(cx, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "hasWhenElse" && !e->args.empty()) {
        return "rv_has_when_else(cx, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if ((e->name == "before" || e->name == "after") && !e->args.empty()) {
        auto it = env.analysis_idx.find(e->args[0]->name);
        std::size_t aidx = (it != env.analysis_idx.end()) ? it->second : 0;
        int is_after = (e->name == "after") ? 1 : 0;
        std::string target = (e->args.size() >= 2) ? emit_expr(e->args[1], env, depth + 1) : "rv_node(nid)";
        return "rv_df_list(cx, " + std::to_string(aidx) + "u, " + std::to_string(is_after) + ", " +
               target + ", nid)";
      }
      if (e->name == "mono" && !e->args.empty()) {
        return emit_expr(e->args[0], env, depth + 1);
      }
      if (e->name == "generalize" && !e->args.empty()) {
        return emit_expr(e->args[0], env, depth + 1);
      }
      if (e->name == "isPrefixOf" && e->args.size() >= 2) {
        return "rv_bool(psl_str_starts_with((" + emit_expr(e->args[1], env, depth + 1) + ").s, (" +
               emit_expr(e->args[0], env, depth + 1) + ").s))";
      }
      if ((e->name == "in" || e->name == "member" || e->name == "notMember") && e->args.size() >= 2) {
        std::string l = emit_expr(e->args[0], env, depth + 1);
        std::string r = emit_expr(e->args[1], env, depth + 1);
        return (e->name == "notMember") ? ("rv_bool(!rv_list_contains(" + r + ", " + l + "))")
                                        : ("rv_bool(rv_list_contains(" + r + ", " + l + "))");
      }
      if (e->name == "replace" && e->args.size() >= 2) {
        return emit_expr(e->args[1], env, depth + 1);
      }
      if (e->name == "textOf" && !e->args.empty()) {
        return "rv_text_of(cx, " + emit_expr(e->args[0], env, depth + 1) + ")";
      }
      if (e->name == "nameSpan" || e->name == "nodeSpan") {
        return "rv_str(psl_str_lit(\"\"))";
      }
      return "rv_none()";
    }
    case PslExpr::Kind::RecordLit:
      return "rv_none()";
  }
  return "rv_none()";
}

inline void emit_selftest_ast_builder(std::ostringstream& out,
                                      std::size_t idx,
                                      const eval::EvalValue& val,
                                      const std::string& mod_low,
                                      const std::string& mod_up,
                                      const CtorCatalog& cat) {
  out << "static const " << mod_low << "_node *selftest_ast_" << idx << "(psl_arena *arena) {\n";
  int seq = 0;
  auto emit_node = [&](auto& self, const eval::EvalValue& v) -> std::string {
    std::string var = "st_" + std::to_string(seq++);
    switch (v.kind) {
      case eval::EvalValue::Kind::Number:
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_num_node(arena, " << std::setprecision(15) << v.number_value
            << ", (psl_span){0u, 0u, 1u, 1u});\n";
        out << "  if (!" << var << ") return NULL;\n";
        return var;
      case eval::EvalValue::Kind::Bool:
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_bool_node(arena, " << (v.bool_value ? 1 : 0)
            << ", (psl_span){0u, 0u, 1u, 1u});\n";
        out << "  if (!" << var << ") return NULL;\n";
        return var;
      case eval::EvalValue::Kind::String:
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_str_node(arena, psl_str_lit(" << c_str_lit(v.string_value)
            << "), (psl_span){0u, 0u, 1u, 1u});\n";
        out << "  if (!" << var << ") return NULL;\n";
        return var;
      case eval::EvalValue::Kind::Unit:
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_str_node(arena, psl_str_lit(\"()\"), (psl_span){0u, 0u, 1u, 1u});\n";
        out << "  if (!" << var << ") return NULL;\n";
        return var;
      case eval::EvalValue::Kind::List:
      case eval::EvalValue::Kind::Tuple: {
        std::vector<std::string> child_vars;
        child_vars.reserve(v.elements.size());
        for (const auto& el : v.elements) child_vars.push_back(self(self, el));
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_list_node(arena, " << child_vars.size() << "u, (psl_span){0u, 0u, 1u, 1u});\n";
        out << "  if (!" << var << ") return NULL;\n";
        for (std::size_t i = 0; i < child_vars.size(); ++i) {
          out << "  " << var << "->children[" << i << "] = " << child_vars[i] << ";\n";
        }
        return var;
      }
      case eval::EvalValue::Kind::Ctor: {
        std::vector<std::string> child_vars;
        child_vars.reserve(v.elements.size());
        for (const auto& el : v.elements) child_vars.push_back(self(self, el));
        const std::string tag_enum = enum_for_ctor(cat, mod_up, v.ctor_name);
        const std::string tname = type_for_ctor(cat, v.ctor_name);
        out << "  " << mod_low << "_node *" << var << " = " << mod_low
            << "_new_node(arena, " << tag_enum << ", psl_str_lit(" << c_str_lit(v.ctor_name)
            << "), psl_str_lit(" << c_str_lit(tname) << "), (psl_span){0u, 0u, 1u, 1u}, "
            << child_vars.size() << "u);\n";
        out << "  if (!" << var << ") return NULL;\n";
        for (std::size_t i = 0; i < child_vars.size(); ++i) {
          out << "  " << var << "->children[" << i << "] = " << child_vars[i] << ";\n";
        }
        return var;
      }
    }
    return "NULL";
  };
  std::string root_var = emit_node(emit_node, val);
  out << "  return " << root_var << ";\n}\n\n";
}

inline std::string emit_sem_source(const PslModule& psem,
                                   const ir::Grammar& grammar,
                                   const PslCodegenOptions& opts,
                                   const std::string& mod_low,
                                   const std::string& mod_up,
                                   const CtorCatalog& cat) {
  std::ostringstream o;
  o << "/* Generated by Parsley PSL C23 backend; do not edit manually. */\n";
  o << "#if !defined(PARSLEY_STATIC) && !defined(PARSLEY_BUILDING_DLL)\n";
  o << "#define PARSLEY_BUILDING_DLL 1\n";
  o << "#endif\n";
  o << "#include \"" << mod_low << "_sem.h\"\n";
  o << "#include \"" << mod_low << "_parse.h\"\n\n";

  auto sev_enum = [](PslSeverity s) {
    switch (s) {
      case PslSeverity::Error: return "PSL_SEV_ERROR";
      case PslSeverity::Warning: return "PSL_SEV_WARNING";
      case PslSeverity::Note: return "PSL_SEV_NOTE";
    }
    return "PSL_SEV_ERROR";
  };
  auto status_enum = [](PslStatus s) {
    switch (s) {
      case PslStatus::Draft: return "PSL_STATUS_DRAFT";
      case PslStatus::Reviewed: return "PSL_STATUS_REVIEWED";
      case PslStatus::Verified: return "PSL_STATUS_VERIFIED";
    }
    return "PSL_STATUS_DRAFT";
  };

  for (const auto& d : psem.diags) {
    o << "PARSLEY_API const psl_diag_def " << mod_up << "_DIAG_" << sanitize_ident(d.code) << " = {\n";
    o << "  sizeof(psl_diag_def), PSL_STR_INIT(" << c_str_lit(d.code) << "), PSL_STR_INIT("
      << c_str_lit(d.code) << "), " << sev_enum(d.severity) << ", "
      << status_enum(d.provenance.status) << ", " << std::setprecision(6) << d.provenance.confidence
      << ", PSL_STR_INIT(" << c_str_lit(d.template_text) << "), PSL_STR_INIT("
      << c_str_lit(d.provenance.source_ref) << ")\n};\n\n";
  }
  for (const auto& l : psem.lints) {
    o << "PARSLEY_API const psl_diag_def " << mod_up << "_LINT_" << sanitize_ident(l.name) << " = {\n";
    o << "  sizeof(psl_diag_def), PSL_STR_INIT(" << c_str_lit("PSL-" + l.name) << "), PSL_STR_INIT("
      << c_str_lit(l.name) << "), " << sev_enum(l.severity) << ", "
      << status_enum(l.provenance.status) << ", " << std::setprecision(6) << l.provenance.confidence
      << ", PSL_STR_INIT(" << c_str_lit(l.say_template) << "), PSL_STR_INIT("
      << c_str_lit(l.provenance.source_ref) << ")\n};\n\n";
  }

  o << "_Static_assert(sizeof(psl_str) > 0, \"psl_str size invariant\");\n";
  o << "_Static_assert(" << mod_up << "_PSL_SCHEMA_VERSION == 1u, \"schema version invariant\");\n";
  o << "_Static_assert((int)PSL_SEV_ERROR == 2, \"severity enum invariant\");\n\n";

  // Runtime types and arena helpers
  o << "enum {\n";
  o << "  RV_NONE = 0, RV_BOOL = 1, RV_NUM = 2, RV_STR = 3, RV_TYPE = 4,\n";
  o << "  RV_NODE = 5, RV_LIST = 6, RV_MAYBE_NONE = 7, RV_MAYBE_VAL = 8, RV_SYM = 9\n";
  o << "};\n\n";

  o << "typedef struct psl_rval psl_rval;\n";
  o << "struct psl_rval {\n";
  o << "  int kind;\n  int b;\n  double num;\n  psl_str s;\n  int32_t id;\n";
  o << "  const psl_rval *items;\n  size_t count;\n";
  o << "};\n\n";

  o << "typedef struct psl_kv {\n  psl_str key;\n  psl_rval val;\n} psl_kv;\n\n";

  o << "enum { TY_VAR = 0, TY_CON = 1, TY_APP = 2, TY_ERR = 3 };\n";
  o << "typedef struct psl_type_node {\n";
  o << "  int kind;\n  int32_t parent;\n  int level;\n  psl_str name;\n";
  o << "  int32_t args[2];\n  size_t arity;\n";
  o << "} psl_type_node;\n\n";

  o << "typedef struct psl_sym {\n";
  o << "  psl_str name;\n  psl_str kind;\n  int32_t type_id;\n";
  o << "  uint32_t use_count;\n  int32_t decl_node;\n  int scope_depth;\n  uint32_t scope_gen;\n";
  o << "} psl_sym;\n\n";

  o << "typedef struct psl_node_info {\n";
  o << "  const " << mod_low << "_node *node;\n";
  o << "  int32_t parent_id;\n  int32_t *children;\n  size_t child_count;\n";
  o << "  int32_t type_id;\n  int32_t decl_sym;\n  int32_t resolved_sym;\n";
  o << "  uint8_t reachable;\n  uint8_t scoped_done;\n";
  o << "} psl_node_info;\n\n";

  o << "#define PSL_MAX_SCOPES 64\n\n";
  o << "struct " << mod_low << "_ctx {\n";
  o << "  psl_arena *arena;\n  " << mod_low << "_options opt;\n";
  o << "  psl_sink sink;\n  void *user;\n";
  o << "  psl_node_info *nodes;\n  size_t node_count;\n";
  o << "  psl_type_node *types;\n  size_t type_count;\n  size_t type_cap;\n";
  o << "  psl_sym *syms;\n  size_t sym_count;\n  size_t sym_cap;\n";
  o << "  int32_t *hash_slots;\n  size_t hash_cap;\n";
  o << "  int scope_depth;\n  uint32_t active_gen[PSL_MAX_SCOPES];\n  uint32_t next_gen;\n";
  o << "  uint64_t **df_before;\n  uint64_t **df_after;\n";
  o << "  psl_str *df_univ;\n  size_t df_univ_count;\n  size_t df_words;\n";
  o << "  size_t diag_count;\n  size_t type_unifications;\n  size_t cfg_blocks;\n  size_t lints_evaluated;\n";
  o << "};\n\n";

  o << "static inline psl_rval rv_none(void) { psl_rval r; memset(&r, 0, sizeof(r)); r.id = -1; return r; }\n";
  o << "static inline psl_rval rv_bool(int b) { psl_rval r = rv_none(); r.kind = RV_BOOL; r.b = b ? 1 : 0; return r; }\n";
  o << "static inline psl_rval rv_num(double v) { psl_rval r = rv_none(); r.kind = RV_NUM; r.num = v; return r; }\n";
  o << "static inline psl_rval rv_str(psl_str s) { psl_rval r = rv_none(); r.kind = RV_STR; r.s = s; return r; }\n";
  o << "static inline psl_rval rv_type(int32_t tid) { psl_rval r = rv_none(); r.kind = RV_TYPE; r.id = tid; return r; }\n";
  o << "static inline psl_rval rv_node(int32_t nid) { psl_rval r = rv_none(); r.kind = RV_NODE; r.id = nid; return r; }\n";
  o << "static inline psl_rval rv_sym(int32_t sid) { psl_rval r = rv_none(); r.kind = RV_SYM; r.id = sid; return r; }\n";
  o << "static inline psl_rval rv_maybe_none(void) { psl_rval r = rv_none(); r.kind = RV_MAYBE_NONE; return r; }\n";
  o << "static inline psl_rval rv_maybe_from(psl_rval v) { psl_rval r = v; r.kind = RV_MAYBE_VAL; return r; }\n";
  o << "static inline psl_rval rv_empty_list(void) { psl_rval r = rv_none(); r.kind = RV_LIST; return r; }\n";
  o << "static inline psl_rval rv_list_make(const psl_rval *items, size_t n) { psl_rval r = rv_none(); r.kind = RV_LIST; r.items = items; r.count = n; return r; }\n\n";

  // Type arena & iterative HM unification
  o << "static inline int32_t ty_alloc(" << mod_low << "_ctx *cx, int kind, int level, psl_str name) {\n";
  o << "  if (cx->type_count >= cx->type_cap) return 0;\n";
  o << "  int32_t id = (int32_t)cx->type_count++;\n";
  o << "  psl_type_node *t = &cx->types[id];\n";
  o << "  t->kind = kind;\n  t->parent = id;\n  t->level = level;\n";
  o << "  t->name = name;\n  t->args[0] = -1;\n  t->args[1] = -1;\n  t->arity = 0;\n";
  o << "  return id;\n}\n\n";

  o << "static inline int32_t ty_fresh(" << mod_low << "_ctx *cx, int level) { return ty_alloc(cx, TY_VAR, level, psl_str_lit(\"\")); }\n";
  o << "static inline int32_t ty_err(" << mod_low << "_ctx *cx) { return ty_alloc(cx, TY_ERR, 0, psl_str_lit(\"<error>\")); }\n";
  o << "static inline int32_t ty_con(" << mod_low << "_ctx *cx, psl_str name) {\n";
  o << "  for (size_t i = 0; i < cx->type_count; ++i) {\n";
  o << "    if (cx->types[i].kind == TY_CON && psl_str_eq(cx->types[i].name, name)) return (int32_t)i;\n";
  o << "  }\n";
  o << "  return ty_alloc(cx, TY_CON, 0, name);\n}\n\n";

  o << "static inline int32_t ty_app1(" << mod_low << "_ctx *cx, psl_str name, int32_t a0) {\n";
  o << "  int32_t id = ty_alloc(cx, TY_APP, 0, name);\n";
  o << "  cx->types[id].args[0] = a0;\n  cx->types[id].arity = 1;\n  return id;\n}\n\n";

  o << "static inline int32_t ty_app2(" << mod_low << "_ctx *cx, psl_str name, int32_t a0, int32_t a1) {\n";
  o << "  int32_t id = ty_alloc(cx, TY_APP, 0, name);\n";
  o << "  cx->types[id].args[0] = a0;\n  cx->types[id].args[1] = a1;\n  cx->types[id].arity = 2;\n  return id;\n}\n\n";

  o << "static inline int32_t ty_find(const " << mod_low << "_ctx *cx, int32_t id) {\n";
  o << "  size_t steps = 0;\n";
  o << "  while (id >= 0 && (size_t)id < cx->type_count && cx->types[id].kind == TY_VAR && cx->types[id].parent != id && steps < 256u) {\n";
  o << "    id = cx->types[id].parent;\n    ++steps;\n";
  o << "  }\n  return id;\n}\n\n";

  o << "static inline int ty_occurs(const " << mod_low << "_ctx *cx, int32_t vid, int32_t target) {\n";
  o << "  int32_t stack[64];\n  int sp = 0;\n  stack[sp++] = target;\n";
  o << "  while (sp > 0) {\n";
  o << "    int32_t cur = ty_find(cx, stack[--sp]);\n";
  o << "    if (cur == vid) return 1;\n";
  o << "    if (cur >= 0 && (size_t)cur < cx->type_count && cx->types[cur].kind == TY_APP) {\n";
  o << "      for (size_t i = 0; i < cx->types[cur].arity && sp < 64; ++i) {\n";
  o << "        stack[sp++] = cx->types[cur].args[i];\n";
  o << "      }\n    }\n  }\n  return 0;\n}\n\n";

  o << "static inline int ty_unify(" << mod_low << "_ctx *cx, int32_t a0, int32_t b0) {\n";
  o << "  int32_t wa[64];\n  int32_t wb[64];\n  int sp = 0;\n";
  o << "  wa[sp] = a0; wb[sp] = b0; ++sp;\n";
  o << "  ++cx->type_unifications;\n";
  o << "  while (sp > 0) {\n";
  o << "    --sp;\n";
  o << "    int32_t a = ty_find(cx, wa[sp]);\n";
  o << "    int32_t b = ty_find(cx, wb[sp]);\n";
  o << "    if (a == b) continue;\n";
  o << "    if (a < 0 || b < 0 || (size_t)a >= cx->type_count || (size_t)b >= cx->type_count) return 0;\n";
  o << "    if (cx->types[a].kind == TY_ERR || cx->types[b].kind == TY_ERR) continue;\n";
  o << "    if (cx->types[a].kind == TY_VAR) {\n";
  o << "      if (ty_occurs(cx, a, b)) { cx->types[a].kind = TY_ERR; return 0; }\n";
  o << "      cx->types[a].parent = b;\n      continue;\n";
  o << "    }\n";
  o << "    if (cx->types[b].kind == TY_VAR) {\n";
  o << "      if (ty_occurs(cx, b, a)) { cx->types[b].kind = TY_ERR; return 0; }\n";
  o << "      cx->types[b].parent = a;\n      continue;\n";
  o << "    }\n";
  o << "    if (cx->types[a].kind == TY_CON && cx->types[b].kind == TY_CON) {\n";
  o << "      if (psl_str_eq(cx->types[a].name, cx->types[b].name)) continue;\n";
  o << "      if (psl_str_eq(cx->types[a].name, psl_str_lit(\"PyAnyTy\")) || psl_str_eq(cx->types[b].name, psl_str_lit(\"PyAnyTy\"))) continue;\n";
  o << "      return 0;\n";
  o << "    }\n";
  o << "    if (cx->types[a].kind == TY_APP && cx->types[b].kind == TY_APP &&\n";
  o << "        psl_str_eq(cx->types[a].name, cx->types[b].name) && cx->types[a].arity == cx->types[b].arity) {\n";
  o << "      for (size_t i = 0; i < cx->types[a].arity && sp < 64; ++i) {\n";
  o << "        wa[sp] = cx->types[a].args[i];\n        wb[sp] = cx->types[b].args[i];\n        ++sp;\n";
  o << "      }\n      continue;\n";
  o << "    }\n";
  o << "    return 0;\n";
  o << "  }\n  return 1;\n}\n\n";

  o << "static inline psl_str ty_format(const " << mod_low << "_ctx *cx, int32_t id, int depth) {\n";
  o << "  if (depth > 8) return psl_str_lit(\"?\");\n";
  o << "  id = ty_find(cx, id);\n";
  o << "  if (id < 0 || (size_t)id >= cx->type_count) return psl_str_lit(\"?\");\n";
  o << "  const psl_type_node *t = &cx->types[id];\n";
  o << "  if (t->kind == TY_CON) return t->name;\n";
  o << "  if (t->kind == TY_ERR) return psl_str_lit(\"<error>\");\n";
  o << "  if (t->kind == TY_VAR) return psl_str_lit(\"'a\");\n";
  o << "  return t->name;\n}\n\n";

  // Open-addressing scope hash table
  o << "static inline uint32_t fnv1a_sym(psl_str name, psl_str kind, uint32_t gen) {\n";
  o << "  uint32_t h = 2166136261u ^ gen;\n";
  o << "  for (size_t i = 0; i < name.len; ++i) { h ^= (uint8_t)name.ptr[i]; h *= 16777619u; }\n";
  o << "  h ^= 0xffu;\n";
  o << "  for (size_t i = 0; i < kind.len; ++i) { h ^= (uint8_t)kind.ptr[i]; h *= 16777619u; }\n";
  o << "  return h;\n}\n\n";

  o << "static inline void scope_push(" << mod_low << "_ctx *cx) {\n";
  o << "  if (cx->scope_depth + 1 < PSL_MAX_SCOPES) {\n";
  o << "    ++cx->scope_depth;\n    cx->active_gen[cx->scope_depth] = ++cx->next_gen;\n";
  o << "  }\n}\n\n";

  o << "static inline void scope_pop(" << mod_low << "_ctx *cx) {\n";
  o << "  if (cx->scope_depth > 0) --cx->scope_depth;\n}\n\n";

  o << "static inline int32_t sym_declare(" << mod_low << "_ctx *cx, psl_str name, psl_str kind, int32_t type_id, int32_t decl_node) {\n";
  o << "  if (cx->sym_count >= cx->sym_cap || cx->hash_cap == 0) return -1;\n";
  o << "  uint32_t gen = cx->active_gen[cx->scope_depth];\n";
  o << "  size_t mask = cx->hash_cap - 1u;\n";
  o << "  size_t pos = (size_t)fnv1a_sym(name, kind, gen) & mask;\n";
  o << "  for (size_t step = 0; step < cx->hash_cap; ++step) {\n";
  o << "    size_t idx = (pos + step) & mask;\n";
  o << "    int32_t sid = cx->hash_slots[idx];\n";
  o << "    if (sid < 0) {\n";
  o << "      sid = (int32_t)cx->sym_count++;\n";
  o << "      cx->syms[sid].name = name;\n      cx->syms[sid].kind = kind;\n";
  o << "      cx->syms[sid].type_id = type_id;\n      cx->syms[sid].use_count = 0;\n";
  o << "      cx->syms[sid].decl_node = decl_node;\n      cx->syms[sid].scope_depth = cx->scope_depth;\n";
  o << "      cx->syms[sid].scope_gen = gen;\n      cx->hash_slots[idx] = sid;\n";
  o << "      if (decl_node >= 0 && (size_t)decl_node < cx->node_count) cx->nodes[decl_node].decl_sym = sid;\n";
  o << "      return sid;\n";
  o << "    }\n";
  o << "    if (cx->syms[sid].scope_gen == gen && psl_str_eq(cx->syms[sid].name, name) && psl_str_eq(cx->syms[sid].kind, kind)) {\n";
  o << "      cx->syms[sid].type_id = type_id;\n      cx->syms[sid].decl_node = decl_node;\n";
  o << "      if (decl_node >= 0 && (size_t)decl_node < cx->node_count) cx->nodes[decl_node].decl_sym = sid;\n";
  o << "      return sid;\n";
  o << "    }\n";
  o << "  }\n  return -1;\n}\n\n";

  o << "static inline int32_t sym_lookup(const " << mod_low << "_ctx *cx, psl_str name, psl_str kind) {\n";
  o << "  if (cx->hash_cap == 0) return -1;\n";
  o << "  size_t mask = cx->hash_cap - 1u;\n";
  o << "  for (int d = cx->scope_depth; d >= 0; --d) {\n";
  o << "    uint32_t gen = cx->active_gen[d];\n";
  o << "    size_t pos = (size_t)fnv1a_sym(name, kind, gen) & mask;\n";
  o << "    for (size_t step = 0; step < cx->hash_cap; ++step) {\n";
  o << "      size_t idx = (pos + step) & mask;\n";
  o << "      int32_t sid = cx->hash_slots[idx];\n";
  o << "      if (sid < 0) break;\n";
  o << "      if (cx->syms[sid].scope_gen == gen && psl_str_eq(cx->syms[sid].name, name) && psl_str_eq(cx->syms[sid].kind, kind)) {\n";
  o << "        return sid;\n";
  o << "      }\n    }\n  }\n  return -1;\n}\n\n";

  // Helpers: ast_equal, const_fold, rv_*
  o << "static inline int ast_equal(const " << mod_low << "_ctx *cx, int32_t aid, int32_t bid, int depth) {\n";
  o << "  if (depth > 32) return 0;\n";
  o << "  if (aid == bid) return 1;\n";
  o << "  if (aid < 0 || bid < 0 || (size_t)aid >= cx->node_count || (size_t)bid >= cx->node_count) return 0;\n";
  o << "  const " << mod_low << "_node *a = cx->nodes[aid].node;\n";
  o << "  const " << mod_low << "_node *b = cx->nodes[bid].node;\n";
  o << "  if (!a || !b || a->tag != b->tag || a->child_count != b->child_count) return 0;\n";
  o << "  if (a->tag == " << mod_up << "_NODE_NUMBER) return (a->num_val - b->num_val) < 1e-9 && (b->num_val - a->num_val) < 1e-9;\n";
  o << "  if (a->tag == " << mod_up << "_NODE_BOOL) return a->bool_val == b->bool_val;\n";
  o << "  if (a->tag == " << mod_up << "_NODE_STRING) return psl_str_eq(a->text_val, b->text_val);\n";
  o << "  if (!psl_str_eq(a->ctor_name, b->ctor_name)) return 0;\n";
  o << "  for (size_t i = 0; i < a->child_count; ++i) {\n";
  o << "    if (!ast_equal(cx, cx->nodes[aid].children[i], cx->nodes[bid].children[i], depth + 1)) return 0;\n";
  o << "  }\n  return 1;\n}\n\n";

  o << "static inline int is_str_one_of(psl_str s, const char *a, const char *b, const char *c, const char *d) {\n";
  o << "  if (a && psl_str_eq(s, psl_str_make(a, strlen(a)))) return 1;\n";
  o << "  if (b && psl_str_eq(s, psl_str_make(b, strlen(b)))) return 1;\n";
  o << "  if (c && psl_str_eq(s, psl_str_make(c, strlen(c)))) return 1;\n";
  o << "  if (d && psl_str_eq(s, psl_str_make(d, strlen(d)))) return 1;\n";
  o << "  return 0;\n}\n\n";

  o << "static inline int eval_const_fold(const " << mod_low << "_ctx *cx, int32_t nid, double *out, int depth) {\n";
  o << "  if (depth > 32 || nid < 0 || (size_t)nid >= cx->node_count) return 0;\n";
  o << "  const " << mod_low << "_node *n = cx->nodes[nid].node;\n";
  o << "  if (!n) return 0;\n";
  o << "  if (n->tag == " << mod_up << "_NODE_NUMBER) { *out = n->num_val; return 1; }\n";
  o << "  if (n->child_count == 1 && cx->nodes[cx->nodes[nid].children[0]].node &&\n";
  o << "      cx->nodes[cx->nodes[nid].children[0]].node->tag == " << mod_up << "_NODE_NUMBER &&\n";
  o << "      (is_str_one_of(n->ctor_name, \"Num\", \"Lit\", \"PyNum\", \"SwNum\") ||\n";
  o << "       is_str_one_of(n->ctor_name, \"IntLit\", \"FloatLit\", \"HsNum\", \"KtNum\"))) {\n";
  o << "    *out = cx->nodes[cx->nodes[nid].children[0]].node->num_val;\n    return 1;\n";
  o << "  }\n";
  o << "  if ((is_str_one_of(n->ctor_name, \"Neg\", \"Unary\", \"PyUnary\", \"SwUnary\")) && n->child_count >= 1) {\n";
  o << "    double sub = 0.0;\n";
  o << "    if (eval_const_fold(cx, cx->nodes[nid].children[n->child_count - 1u], &sub, depth + 1)) {\n";
  o << "      *out = -sub;\n      return 1;\n";
  o << "    }\n  }\n";
  o << "  if (psl_str_eq(n->ctor_name, psl_str_lit(\"Paren\")) && n->child_count == 1) {\n";
  o << "    return eval_const_fold(cx, cx->nodes[nid].children[0], out, depth + 1);\n";
  o << "  }\n";
  o << "  if (is_str_one_of(n->ctor_name, \"Bin\", \"PyBin\", \"SwBin\", \"BinOp\") && n->child_count == 3) {\n";
  o << "    const " << mod_low << "_node *opn = cx->nodes[cx->nodes[nid].children[0]].node;\n";
  o << "    if (!opn) return 0;\n";
  o << "    psl_str op = (opn->tag == " << mod_up << "_NODE_STRING) ? opn->text_val : opn->ctor_name;\n";
  o << "    double lv = 0.0, rv = 0.0;\n";
  o << "    if (!eval_const_fold(cx, cx->nodes[nid].children[1], &lv, depth + 1)) return 0;\n";
  o << "    if (!eval_const_fold(cx, cx->nodes[nid].children[2], &rv, depth + 1)) return 0;\n";
  o << "    if (is_str_one_of(op, \"Add\", \"PyAdd\", \"SwAdd\", \"+\")) { *out = lv + rv; return 1; }\n";
  o << "    if (is_str_one_of(op, \"Sub\", \"PySub\", \"SwSub\", \"-\")) { *out = lv - rv; return 1; }\n";
  o << "    if (is_str_one_of(op, \"Mul\", \"PyMul\", \"SwMul\", \"*\")) { *out = lv * rv; return 1; }\n";
  o << "    if (is_str_one_of(op, \"Div\", \"PyDiv\", \"SwDiv\", \"/\") && (rv > 1e-12 || rv < -1e-12)) {\n";
  o << "      *out = lv / rv;\n      return 1;\n";
  o << "    }\n  }\n  return 0;\n}\n\n";

  o << "static inline psl_rval rv_from_node(const " << mod_low << "_ctx *cx, int32_t nid) {\n";
  o << "  if (nid < 0 || (size_t)nid >= cx->node_count || !cx->nodes[nid].node) return rv_none();\n";
  o << "  const " << mod_low << "_node *n = cx->nodes[nid].node;\n";
  o << "  if (n->tag == " << mod_up << "_NODE_STRING) { psl_rval r = rv_str(n->text_val); r.id = nid; return r; }\n";
  o << "  if (n->tag == " << mod_up << "_NODE_NUMBER) { psl_rval r = rv_num(n->num_val); r.id = nid; return r; }\n";
  o << "  if (n->tag == " << mod_up << "_NODE_BOOL) { psl_rval r = rv_bool(n->bool_val); r.id = nid; return r; }\n";
  o << "  if (n->tag == " << mod_up << "_NODE_LIST) {\n";
  o << "    psl_rval r = rv_none(); r.kind = RV_LIST; r.id = nid;\n";
  o << "    if (n->child_count > 0) {\n";
  o << "      psl_rval *arr = (psl_rval *)psl_arena_alloc(cx->arena, n->child_count * sizeof(psl_rval), sizeof(void *));\n";
  o << "      if (arr) {\n";
  o << "        for (size_t i = 0; i < n->child_count; ++i) arr[i] = rv_node(cx->nodes[nid].children[i]);\n";
  o << "        r.items = arr; r.count = n->child_count;\n";
  o << "      }\n    }\n    return r;\n  }\n";
  o << "  return rv_node(nid);\n}\n\n";

  o << "static inline int rv_eq(const " << mod_low << "_ctx *cx, psl_rval a, psl_rval b) {\n";
  o << "  if (a.kind == RV_NODE && b.kind == RV_NODE) return ast_equal(cx, a.id, b.id, 0);\n";
  o << "  if (a.kind == RV_NODE && b.kind == RV_TYPE && a.id >= 0 && (size_t)a.id < cx->node_count && cx->nodes[a.id].node && cx->nodes[a.id].node->child_count == 0) {\n";
  o << "    return psl_str_eq(cx->nodes[a.id].node->ctor_name, ty_format(cx, b.id, 0));\n";
  o << "  }\n";
  o << "  if (b.kind == RV_NODE && a.kind == RV_TYPE && b.id >= 0 && (size_t)b.id < cx->node_count && cx->nodes[b.id].node && cx->nodes[b.id].node->child_count == 0) {\n";
  o << "    return psl_str_eq(cx->nodes[b.id].node->ctor_name, ty_format(cx, a.id, 0));\n";
  o << "  }\n";
  o << "  if (a.kind == RV_MAYBE_NONE && b.kind == RV_MAYBE_NONE) return 1;\n";
  o << "  if (a.kind == RV_MAYBE_VAL && b.kind == RV_MAYBE_VAL) return (a.num - b.num) < 1e-9 && (b.num - a.num) < 1e-9 && psl_str_eq(a.s, b.s);\n";
  o << "  if (a.kind == RV_MAYBE_NONE || b.kind == RV_MAYBE_NONE || a.kind == RV_MAYBE_VAL || b.kind == RV_MAYBE_VAL) return 0;\n";
  o << "  if (a.kind == RV_TYPE && b.kind == RV_TYPE) return psl_str_eq(ty_format(cx, a.id, 0), ty_format(cx, b.id, 0));\n";
  o << "  if (a.kind == RV_NUM && b.kind == RV_NUM) return (a.num - b.num) < 1e-9 && (b.num - a.num) < 1e-9;\n";
  o << "  if (a.kind == RV_STR && b.kind == RV_STR) return psl_str_eq(a.s, b.s);\n";
  o << "  if (a.kind == RV_BOOL && b.kind == RV_BOOL) return a.b == b.b;\n";
  o << "  return a.kind == b.kind && a.id == b.id && psl_str_eq(a.s, b.s);\n}\n\n";

  o << "static inline int32_t rv_as_type(" << mod_low << "_ctx *cx, psl_rval v, int level) {\n";
  o << "  if (v.kind == RV_TYPE && v.id >= 0) return v.id;\n";
  o << "  return ty_fresh(cx, level);\n}\n\n";

  o << "static inline psl_rval rv_type_of(" << mod_low << "_ctx *cx, psl_rval v, int level) {\n";
  o << "  if (v.kind == RV_SYM && v.id >= 0 && (size_t)v.id < cx->sym_count) return rv_type(cx->syms[v.id].type_id);\n";
  o << "  if ((v.kind == RV_NODE || v.id >= 0) && v.id >= 0 && (size_t)v.id < cx->node_count) {\n";
  o << "    if (cx->nodes[v.id].type_id >= 0) return rv_type(cx->nodes[v.id].type_id);\n";
  o << "    return rv_type(ty_fresh(cx, level));\n";
  o << "  }\n  return rv_type(ty_fresh(cx, level));\n}\n\n";

  o << "static inline psl_rval rv_arity(const " << mod_low << "_ctx *cx, psl_rval v) {\n";
  o << "  if (v.kind == RV_SYM && v.id >= 0 && (size_t)v.id < cx->sym_count) {\n";
  o << "    int32_t tid = ty_find(cx, cx->syms[v.id].type_id);\n";
  o << "    size_t cnt = 0;\n";
  o << "    while (tid >= 0 && (size_t)tid < cx->type_count && cx->types[tid].kind == TY_APP && psl_str_eq(cx->types[tid].name, psl_str_lit(\"Fn\")) && cnt < 16u) {\n";
  o << "      ++cnt;\n      tid = ty_find(cx, cx->types[tid].args[1]);\n";
  o << "    }\n    return rv_num((double)cnt);\n";
  o << "  }\n  return rv_num(0.0);\n}\n\n";

  o << "static inline psl_rval rv_length(psl_rval v) { return rv_num((v.kind == RV_LIST) ? (double)v.count : (double)v.s.len); }\n";
  o << "static inline psl_rval rv_and_list(psl_rval v) {\n";
  o << "  for (size_t i = 0; i < v.count; ++i) { if (!v.items[i].b) return rv_bool(0); }\n  return rv_bool(1);\n}\n";
  o << "static inline psl_rval rv_any_list(psl_rval v) {\n";
  o << "  for (size_t i = 0; i < v.count; ++i) { if (v.items[i].b) return rv_bool(1); }\n  return rv_bool(0);\n}\n";
  o << "static inline psl_rval rv_count_list(psl_rval v) {\n";
  o << "  size_t c = 0;\n  for (size_t i = 0; i < v.count; ++i) { if (v.items[i].b) ++c; }\n  return rv_num((double)c);\n}\n";
  o << "static inline psl_rval rv_concat(" << mod_low << "_ctx *cx, psl_rval a, psl_rval b) {\n";
  o << "  return rv_str(psl_str_concat(cx->arena, a.s, b.s));\n}\n\n";

  o << "static inline psl_rval rv_const_fold(const " << mod_low << "_ctx *cx, psl_rval v) {\n";
  o << "  double out = 0.0;\n";
  o << "  if (eval_const_fold(cx, v.id, &out, 0)) return rv_maybe_from(rv_num(out));\n";
  o << "  return rv_maybe_none();\n}\n\n";

  o << "static inline psl_rval rv_use_count(const " << mod_low << "_ctx *cx, int32_t cur_nid, psl_rval v) {\n";
  o << "  if (cur_nid >= 0 && (size_t)cur_nid < cx->node_count && cx->nodes[cur_nid].decl_sym >= 0) {\n";
  o << "    return rv_num((double)cx->syms[cx->nodes[cur_nid].decl_sym].use_count);\n";
  o << "  }\n";
  o << "  uint32_t sum = 0;\n";
  o << "  if (v.kind == RV_STR) {\n";
  o << "    for (size_t i = 0; i < cx->sym_count; ++i) {\n";
  o << "      if (psl_str_eq(cx->syms[i].name, v.s)) sum += cx->syms[i].use_count;\n";
  o << "    }\n  }\n  return rv_num((double)sum);\n}\n\n";

  o << "static inline psl_rval rv_reachable(const " << mod_low << "_ctx *cx, int32_t cur_nid, psl_rval v) {\n";
  o << "  int32_t target = (v.id >= 0) ? v.id : cur_nid;\n";
  o << "  if (target < 0 || (size_t)target >= cx->node_count) return rv_bool(1);\n";
  o << "  return rv_bool(cx->nodes[target].reachable ? 1 : 0);\n}\n\n";

  o << "static inline psl_rval rv_is_immutable(const " << mod_low << "_ctx *cx, psl_rval v) {\n";
  o << "  int32_t sid = sym_lookup(cx, v.s, psl_str_lit(\"Variable\"));\n";
  o << "  if (sid < 0) {\n";
  o << "    for (size_t i = cx->sym_count; i > 0; --i) {\n";
  o << "      if (psl_str_eq(cx->syms[i - 1u].name, v.s)) { sid = (int32_t)(i - 1u); break; }\n";
  o << "    }\n  }\n";
  o << "  if (sid >= 0 && cx->syms[sid].decl_node >= 0) {\n";
  o << "    int32_t dnid = cx->syms[sid].decl_node;\n";
  o << "    const " << mod_low << "_node *dn = cx->nodes[dnid].node;\n";
  o << "    if (dn && (psl_str_eq(dn->ctor_name, psl_str_lit(\"ValStmt\")) || psl_str_eq(dn->ctor_name, psl_str_lit(\"PropDecl\"))) &&\n";
  o << "        dn->child_count > 0 && cx->nodes[cx->nodes[dnid].children[0]].node &&\n";
  o << "        cx->nodes[cx->nodes[dnid].children[0]].node->tag == " << mod_up << "_NODE_BOOL) {\n";
  o << "      return rv_bool(!cx->nodes[cx->nodes[dnid].children[0]].node->bool_val);\n";
  o << "    }\n";
  o << "    if (dn && psl_str_eq(dn->ctor_name, psl_str_lit(\"StmtLet\"))) return rv_bool(1);\n";
  o << "  }\n  return rv_bool(0);\n}\n\n";

  o << "static inline psl_rval rv_has_when_else(const " << mod_low << "_ctx *cx, psl_rval v) {\n";
  o << "  for (size_t i = 0; i < v.count; ++i) {\n";
  o << "    int32_t cid = v.items[i].id;\n";
  o << "    if (cid >= 0 && (size_t)cid < cx->node_count && cx->nodes[cid].node &&\n";
  o << "        psl_str_eq(cx->nodes[cid].node->ctor_name, psl_str_lit(\"WhenElse\"))) {\n";
  o << "      return rv_bool(1);\n";
  o << "    }\n  }\n  return rv_bool(0);\n}\n\n";

  o << "static inline psl_rval rv_df_list(const " << mod_low << "_ctx *cx, size_t an_idx, int is_after, psl_rval target, int32_t cur_nid) {\n";
  o << "  int32_t nid = (target.id >= 0) ? target.id : cur_nid;\n";
  o << "  if (nid < 0 || (size_t)nid >= cx->node_count || cx->df_words == 0 || cx->df_univ_count == 0) return rv_empty_list();\n";
  o << "  const uint64_t *vec = (is_after ? cx->df_after[an_idx] : cx->df_before[an_idx]) + (size_t)nid * cx->df_words;\n";
  o << "  psl_rval *arr = (psl_rval *)psl_arena_alloc(cx->arena, cx->df_univ_count * sizeof(psl_rval), sizeof(void *));\n";
  o << "  if (!arr) return rv_empty_list();\n";
  o << "  size_t cnt = 0;\n";
  o << "  for (size_t u = 0; u < cx->df_univ_count; ++u) {\n";
  o << "    if ((vec[u >> 6u] & (UINT64_C(1) << (u & 63u))) != 0u) arr[cnt++] = rv_str(cx->df_univ[u]);\n";
  o << "  }\n  return rv_list_make(arr, cnt);\n}\n\n";

  o << "static inline int rv_list_contains(psl_rval lst, psl_rval item) {\n";
  o << "  for (size_t i = 0; i < lst.count; ++i) {\n";
  o << "    if (psl_str_eq(lst.items[i].s, item.s)) return 1;\n";
  o << "  }\n  return 0;\n}\n\n";

  o << "static inline psl_rval rv_text_of(const " << mod_low << "_ctx *cx, psl_rval v) {\n";
  o << "  if (v.id >= 0 && (size_t)v.id < cx->node_count && cx->nodes[v.id].node) return rv_str(cx->nodes[v.id].node->ctor_name);\n";
  o << "  return rv_str(v.s);\n}\n\n";

  o << "static inline psl_str format_diag_tpl(" << mod_low << "_ctx *cx, psl_str tpl, const psl_kv *kvs, size_t kvn) {\n";
  o << R"C23(
  psl_str invalid = {NULL,0};
  if (tpl.len > (SIZE_MAX / sizeof(psl_str))-1u) {cx->arena->error=2;return invalid;}
  psl_str* parts=(psl_str*)psl_arena_alloc(cx->arena,(tpl.len+1u)*sizeof(psl_str),sizeof(void*));
  if(!parts)return invalid;
  size_t i=0,count=0,total=0;
  while(i<tpl.len) {
    psl_str value; size_t start=i;
    if(tpl.ptr[i]=='{') {
      size_t end=i+1;while(end<tpl.len&&tpl.ptr[end]!='}')++end;
      if(end<tpl.len) {
        psl_str key=psl_str_make(tpl.ptr+i+1,end-i-1);value=psl_str_lit("?");
        for(size_t k=0;k<kvn;++k)if(psl_str_eq(kvs[k].key,key)){value=kvs[k].val.kind==RV_TYPE?ty_format(cx,kvs[k].val.id,0):kvs[k].val.s;break;}
        i=end+1;
      } else {++i;value=psl_str_make(tpl.ptr+start,1);}
    } else {while(i<tpl.len&&tpl.ptr[i]!='{')++i;value=psl_str_make(tpl.ptr+start,i-start);}
    if(value.len>SIZE_MAX-total-1u){cx->arena->error=2;return invalid;}
    parts[count++]=value;total+=value.len;
  }
  char* text=(char*)psl_arena_alloc(cx->arena,total+1u,1u);if(!text)return invalid;
  size_t pos=0;for(size_t k=0;k<count;++k){if(parts[k].len)memcpy(text+pos,parts[k].ptr,parts[k].len);pos+=parts[k].len;}
  text[pos]='\0';return psl_str_make(text,pos);
}
)C23";
  o << "static inline void emit_diag_msg(" << mod_low << "_ctx *cx, const psl_diag_def *def, psl_severity sev, psl_span sp, psl_str msg, int has_fix, psl_span fsp, psl_str ftxt, psl_fix_safety fsafe) {\n";
  o << "  ++cx->diag_count;\n";
  o << "  if (!cx->sink) return;\n";
  o << "  psl_diag d;\n  memset(&d, 0, sizeof(d));\n";
  o << "  d.size = sizeof(psl_diag);\n  d.def = def;\n  d.severity = sev;\n  d.span = sp;\n";
  o << "  d.message = msg;\n  d.has_fix = has_fix;\n  d.fix_span = fsp;\n  d.fix_text = ftxt;\n  d.fix_safety = fsafe;\n";
  o << "  cx->sink(cx->user, &d);\n}\n\n";

  std::unordered_map<std::string, std::size_t> analysis_idx;
  for (std::size_t i = 0; i < psem.analyses.size(); ++i) analysis_idx[psem.analyses[i].name] = i;

  std::unordered_set<std::string> type_names;
  for (const auto& t : psem.types) type_names.insert(t.name);
  for (const auto& tc : psem.tycons) type_names.insert(tc.name);

  std::ostringstream matchers;
  std::ostringstream helpers;
  std::ostringstream bodies;
  int next_helper_id = 0;

  // Emit node rule matchers and functions (Passes 1-3)
  std::vector<std::vector<std::string>> node_vars(psem.node_rules.size());
  for (std::size_t ri = 0; ri < psem.node_rules.size(); ++ri) {
    collect_pat_vars(psem.node_rules[ri].pattern, node_vars[ri]);
    emit_pattern_matcher(matchers, "node_" + std::to_string(ri), mod_low, mod_up, cat,
                         psem.node_rules[ri].pattern, node_vars[ri]);
  }

  bodies << "static void run_node_passes(" << mod_low << "_ctx *cx, int32_t nid, int level);\n\n";
  for (std::size_t ri = 0; ri < psem.node_rules.size(); ++ri) {
    const auto& nr = psem.node_rules[ri];
    std::string pstruct = "pat_bind_node_" + std::to_string(ri);
    bodies << "static void exec_node_rule_" << ri << "(" << mod_low << "_ctx *cx, int32_t nid, int level, const "
           << pstruct << " *b) {\n";
    bodies << "  (void)cx; (void)nid; (void)level; (void)b;\n";
    ExprEnv env;
    env.mod_low = mod_low;
    env.pat_struct = pstruct;
    env.pat_vars = node_vars[ri];
    env.type_names = type_names;
    env.analysis_idx = analysis_idx;
    env.helpers = &helpers;
    env.next_helper_id = next_helper_id;

    // Collect scoped children and run non-scoped children first
    bodies << "  int32_t scoped_ids[16];\n  size_t scoped_cnt = 0;\n";
    for (const auto& st : nr.stmts) {
      if (st.kind == PslNodeStmt::Kind::Scope &&
          std::find(env.pat_vars.begin(), env.pat_vars.end(), st.target_var) != env.pat_vars.end()) {
        bodies << "  if (b->nid_" << sanitize_ident(st.target_var)
               << " >= 0 && scoped_cnt < 16u) scoped_ids[scoped_cnt++] = b->nid_"
               << sanitize_ident(st.target_var) << ";\n";
      }
    }
    bodies << "  for (size_t ci = 0; ci < cx->nodes[nid].child_count; ++ci) {\n";
    bodies << "    int32_t cid = cx->nodes[nid].children[ci];\n";
    bodies << "    int is_scoped = 0;\n";
    bodies << "    for (size_t si = 0; si < scoped_cnt; ++si) { if (scoped_ids[si] == cid) { is_scoped = 1; break; } }\n";
    bodies << "    if (!is_scoped) run_node_passes(cx, cid, level);\n";
    bodies << "  }\n";

    auto emit_stmt = [&](auto& self, const PslNodeStmt& st, int slevel_delta) -> void {
      std::string lvl = slevel_delta ? ("(level + " + std::to_string(slevel_delta) + ")") : "level";
      switch (st.kind) {
        case PslNodeStmt::Kind::FreshBind: {
          std::string lv = "loc_" + sanitize_ident(st.target_var);
          bodies << "  psl_rval " << lv << " = rv_type(ty_fresh(cx, " << lvl << "));\n";
          bodies << "  (void)" << lv << ";\n";
          env.locals[st.target_var] = lv;
          break;
        }
        case PslNodeStmt::Kind::LookupBind: {
          std::string lv = "loc_" + sanitize_ident(st.target_var);
          std::string kv = emit_expr(st.expr, env);
          bodies << "  psl_rval " << lv << ";\n";
          bodies << "  {\n    psl_rval keyv = " << kv << ";\n";
          bodies << "    int32_t sid = sym_lookup(cx, keyv.s, psl_str_lit(" << c_str_lit(st.sym_kind) << "));\n";
          bodies << "    if (sid >= 0) {\n";
          bodies << "      ++cx->syms[sid].use_count;\n";
          bodies << "      cx->nodes[nid].resolved_sym = sid;\n";
          bodies << "      " << lv << " = rv_sym(sid);\n";
          bodies << "    } else {\n";
          if (st.or_fail.present) {
            std::string dname = mod_up + "_DIAG_" + sanitize_ident(st.or_fail.diag_code);
            bodies << "      psl_kv okvs[" << (st.or_fail.args.size() + 1) << "];\n";
            for (std::size_t ai = 0; ai < st.or_fail.args.size(); ++ai) {
              bodies << "      okvs[" << ai << "].key = psl_str_lit(" << c_str_lit(st.or_fail.args[ai].first) << ");\n";
              bodies << "      okvs[" << ai << "].val = " << emit_expr(st.or_fail.args[ai].second, env) << ";\n";
            }
            bodies << "      psl_str msg = format_diag_tpl(cx, " << dname << ".template_str, okvs, "
                   << st.or_fail.args.size() << "u);\n";
            bodies << "      emit_diag_msg(cx, &" << dname << ", " << dname << ".default_sev, cx->nodes[nid].node->span, msg, 0, cx->nodes[nid].node->span, psl_str_lit(\"\"), PSL_FIX_SAFE);\n";
          }
          bodies << "      " << lv << " = rv_type(ty_err(cx));\n";
          bodies << "    }\n  }\n  (void)" << lv << ";\n";
          env.locals[st.target_var] = lv;
          break;
        }
        case PslNodeStmt::Kind::Declare: {
          std::string nv = emit_expr(st.expr, env);
          std::string tv = emit_expr(st.expr2, env);
          bodies << "  {\n    psl_rval dn = " << nv << ";\n    psl_rval dt = " << tv << ";\n";
          bodies << "    sym_declare(cx, dn.s, psl_str_lit(" << c_str_lit(st.sym_kind) << "), rv_as_type(cx, dt, " << lvl << "), nid);\n";
          bodies << "  }\n";
          break;
        }
        case PslNodeStmt::Kind::Scope: {
          if (std::find(env.pat_vars.begin(), env.pat_vars.end(), st.target_var) != env.pat_vars.end()) {
            std::string scid = "b->nid_" + sanitize_ident(st.target_var);
            bodies << "  if (" << scid << " >= 0) {\n";
            bodies << "    scope_push(cx);\n";
            for (const auto& sub : st.nested) self(self, *sub, slevel_delta + 1);
            bodies << "    run_node_passes(cx, " << scid << ", " << lvl << " + 1);\n";
            bodies << "    scope_pop(cx);\n";
            bodies << "    cx->nodes[" << scid << "].scoped_done = 1;\n";
            bodies << "  }\n";
          }
          break;
        }
        case PslNodeStmt::Kind::Unify: {
          std::string t1 = emit_expr(st.expr, env);
          std::string t2 = emit_expr(st.expr2, env);
          bodies << "  if (!ty_unify(cx, rv_as_type(cx, " << t1 << ", " << lvl << "), rv_as_type(cx, " << t2 << ", " << lvl << "))) {\n";
          if (st.or_fail.present) {
            std::string dname = mod_up + "_DIAG_" + sanitize_ident(st.or_fail.diag_code);
            bodies << "    psl_kv okvs[" << (st.or_fail.args.size() + 1) << "];\n";
            for (std::size_t ai = 0; ai < st.or_fail.args.size(); ++ai) {
              bodies << "    okvs[" << ai << "].key = psl_str_lit(" << c_str_lit(st.or_fail.args[ai].first) << ");\n";
              bodies << "    okvs[" << ai << "].val = " << emit_expr(st.or_fail.args[ai].second, env) << ";\n";
            }
            bodies << "    psl_str msg = format_diag_tpl(cx, " << dname << ".template_str, okvs, "
                   << st.or_fail.args.size() << "u);\n";
            bodies << "    emit_diag_msg(cx, &" << dname << ", " << dname << ".default_sev, cx->nodes[nid].node->span, msg, 0, cx->nodes[nid].node->span, psl_str_lit(\"\"), PSL_FIX_SAFE);\n";
          }
          bodies << "  }\n";
          break;
        }
        case PslNodeStmt::Kind::Require: {
          std::string cv = emit_expr(st.expr, env);
          bodies << "  if (!(" << cv << ").b) {\n";
          if (st.or_fail.present) {
            std::string dname = mod_up + "_DIAG_" + sanitize_ident(st.or_fail.diag_code);
            bodies << "    psl_kv okvs[" << (st.or_fail.args.size() + 1) << "];\n";
            for (std::size_t ai = 0; ai < st.or_fail.args.size(); ++ai) {
              bodies << "    okvs[" << ai << "].key = psl_str_lit(" << c_str_lit(st.or_fail.args[ai].first) << ");\n";
              bodies << "    okvs[" << ai << "].val = " << emit_expr(st.or_fail.args[ai].second, env) << ";\n";
            }
            bodies << "    psl_str msg = format_diag_tpl(cx, " << dname << ".template_str, okvs, "
                   << st.or_fail.args.size() << "u);\n";
            bodies << "    emit_diag_msg(cx, &" << dname << ", " << dname << ".default_sev, cx->nodes[nid].node->span, msg, 0, cx->nodes[nid].node->span, psl_str_lit(\"\"), PSL_FIX_SAFE);\n";
          }
          bodies << "  }\n";
          break;
        }
        case PslNodeStmt::Kind::AssignAttr:
          if (st.target_var == "type") {
            std::string av = emit_expr(st.expr, env);
            bodies << "  cx->nodes[nid].type_id = rv_as_type(cx, " << av << ", " << lvl << ");\n";
          }
          break;
      }
    };
    for (const auto& st : nr.stmts) emit_stmt(emit_stmt, st, 0);
    next_helper_id = env.next_helper_id;
    bodies << "}\n\n";
  }

  bodies << "static void run_node_passes(" << mod_low << "_ctx *cx, int32_t nid, int level) {\n";
  bodies << "  if (level > 128 || nid < 0 || (size_t)nid >= cx->node_count) return;\n";
  bodies << "  if (cx->nodes[nid].scoped_done) return;\n";
  bodies << "  const " << mod_low << "_node *n = cx->nodes[nid].node;\n";
  bodies << "  if (!n) return;\n";
  for (std::size_t ri = 0; ri < psem.node_rules.size(); ++ri) {
    bodies << "  {\n    pat_bind_node_" << ri << " b;\n    memset(&b, 0, sizeof(b));\n";
    bodies << "    if (match_pat_node_" << ri << "(cx, nid, &b)) {\n";
    bodies << "      exec_node_rule_" << ri << "(cx, nid, level, &b);\n      return;\n";
    bodies << "    }\n  }\n";
  }
  bodies << "  for (size_t ci = 0; ci < cx->nodes[nid].child_count; ++ci) {\n";
  bodies << "    run_node_passes(cx, cx->nodes[nid].children[ci], level);\n";
  bodies << "  }\n}\n\n";

  // Pass 4: CFG & Bitvector Dataflow Solver
  for (std::size_t fi = 0; fi < psem.flow_rules.size(); ++fi) {
    std::vector<std::string> fv;
    collect_pat_vars(psem.flow_rules[fi].pattern, fv);
    emit_pattern_matcher(matchers, "flow_" + std::to_string(fi), mod_low, mod_up, cat,
                         psem.flow_rules[fi].pattern, fv);
  }
  for (std::size_t ai = 0; ai < psem.analyses.size(); ++ai) {
    const auto& an = psem.analyses[ai];
    for (std::size_t gi = 0; gi < an.gen_clauses.size(); ++gi) {
      std::vector<std::string> gv;
      collect_pat_vars(an.gen_clauses[gi].pattern, gv);
      std::string pid = "df_g_" + std::to_string(ai) + "_" + std::to_string(gi);
      emit_pattern_matcher(matchers, pid, mod_low, mod_up, cat, an.gen_clauses[gi].pattern, gv);
      ExprEnv env;
      env.mod_low = mod_low;
      env.pat_struct = "pat_bind_" + pid;
      env.pat_vars = gv;
      env.type_names = type_names;
      env.analysis_idx = analysis_idx;
      env.helpers = &helpers;
      env.next_helper_id = next_helper_id;
      bodies << "static psl_rval eval_" << pid << "(" << mod_low << "_ctx *cx, int32_t nid, int level, const pat_bind_"
             << pid << " *b) {\n  (void)cx; (void)nid; (void)level; (void)b;\n  return "
             << emit_expr(an.gen_clauses[gi].expr, env) << ";\n}\n\n";
      next_helper_id = env.next_helper_id;
    }
    for (std::size_t ki = 0; ki < an.kill_clauses.size(); ++ki) {
      std::vector<std::string> kv;
      collect_pat_vars(an.kill_clauses[ki].pattern, kv);
      std::string pid = "df_k_" + std::to_string(ai) + "_" + std::to_string(ki);
      emit_pattern_matcher(matchers, pid, mod_low, mod_up, cat, an.kill_clauses[ki].pattern, kv);
      ExprEnv env;
      env.mod_low = mod_low;
      env.pat_struct = "pat_bind_" + pid;
      env.pat_vars = kv;
      env.type_names = type_names;
      env.analysis_idx = analysis_idx;
      env.helpers = &helpers;
      env.next_helper_id = next_helper_id;
      bodies << "static psl_rval eval_" << pid << "(" << mod_low << "_ctx *cx, int32_t nid, int level, const pat_bind_"
             << pid << " *b) {\n  (void)cx; (void)nid; (void)level; (void)b;\n  return "
             << emit_expr(an.kill_clauses[ki].expr, env) << ";\n}\n\n";
      next_helper_id = env.next_helper_id;
    }
  }

  bodies << "static inline size_t df_intern_sym(" << mod_low << "_ctx *cx, psl_str s, size_t max_u) {\n";
  bodies << "  for (size_t i = 0; i < cx->df_univ_count; ++i) {\n";
  bodies << "    if (psl_str_eq(cx->df_univ[i], s)) return i;\n";
  o << "";
  bodies << "  }\n";
  bodies << "  if (cx->df_univ_count < max_u) {\n";
  bodies << "    cx->df_univ[cx->df_univ_count] = s;\n";
  bodies << "    return cx->df_univ_count++;\n";
  bodies << "  }\n  return 0;\n}\n\n";

  bodies << "static void run_dataflow_pass(" << mod_low << "_ctx *cx) {\n";
  bodies << "  size_t n = cx->node_count;\n";
  bodies << "  if (n == 0) return;\n";
  bodies << "  int32_t *succ_off = (int32_t *)psl_arena_alloc(cx->arena, (n + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  int32_t *pred_off = (int32_t *)psl_arena_alloc(cx->arena, (n + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  int32_t *succ_adj = (int32_t *)psl_arena_alloc(cx->arena, (n * 3u + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  int32_t *pred_adj = (int32_t *)psl_arena_alloc(cx->arena, (n * 3u + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  uint8_t *kills_seq = (uint8_t *)psl_arena_alloc(cx->arena, n, 1u);\n";
  bodies << "  int32_t *bfs_q = (int32_t *)psl_arena_alloc(cx->arena, n * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  if (!succ_off || !pred_off || !succ_adj || !pred_adj || !kills_seq || !bfs_q) return;\n";
  bodies << "  int32_t *eu = (int32_t *)psl_arena_alloc(cx->arena, (n * 2u + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  int32_t *ev = (int32_t *)psl_arena_alloc(cx->arena, (n * 2u + 1u) * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  if (!eu || !ev) return;\n";
  bodies << "  size_t ecnt = 0;\n";
  bodies << "  for (size_t i = 0; i < n; ++i) {\n";
  bodies << "    cx->nodes[i].reachable = 0;\n";
  bodies << "    size_t cc = cx->nodes[i].child_count;\n";
  bodies << "    if (cc > 0 && ecnt < n * 2u) {\n";
  bodies << "      eu[ecnt] = (int32_t)i; ev[ecnt] = cx->nodes[i].children[0]; ++ecnt;\n";
  bodies << "      for (size_t k = 0; k + 1u < cc && ecnt < n * 2u; ++k) {\n";
  bodies << "        int32_t c0 = cx->nodes[i].children[k];\n";
  bodies << "        int32_t c1 = cx->nodes[i].children[k + 1u];\n";
  bodies << "        if (cx->nodes[c0].node && cx->nodes[c0].node->tag == " << mod_up
         << "_NODE_LIST && cx->nodes[c0].child_count > 0) {\n";
  bodies << "          eu[ecnt] = cx->nodes[c0].children[cx->nodes[c0].child_count - 1u];\n";
  bodies << "        } else {\n";
  bodies << "          eu[ecnt] = c0;\n";
  bodies << "        }\n";
  bodies << "        ev[ecnt] = c1; ++ecnt;\n";
  bodies << "      }\n    }\n  }\n";
  bodies << "  for (size_t e = 0; e < ecnt; ++e) {\n";
  bodies << "    ++succ_off[eu[e] + 1];\n    ++pred_off[ev[e] + 1];\n";
  bodies << "  }\n";
  bodies << "  for (size_t i = 1; i <= n; ++i) { succ_off[i] += succ_off[i - 1u]; pred_off[i] += pred_off[i - 1u]; }\n";
  bodies << "  int32_t *scur = (int32_t *)psl_arena_alloc(cx->arena, n * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  int32_t *pcur = (int32_t *)psl_arena_alloc(cx->arena, n * sizeof(int32_t), sizeof(int32_t));\n";
  bodies << "  if (!scur || !pcur) return;\n";
  bodies << "  for (size_t i = 0; i < n; ++i) { scur[i] = succ_off[i]; pcur[i] = pred_off[i]; }\n";
  bodies << "  for (size_t e = 0; e < ecnt; ++e) {\n";
  bodies << "    succ_adj[scur[eu[e]]++] = ev[e];\n";
  bodies << "    pred_adj[pcur[ev[e]]++] = eu[e];\n";
  bodies << "  }\n";
  for (std::size_t fi = 0; fi < psem.flow_rules.size(); ++fi) {
    bodies << "  (void)match_pat_flow_" << fi << ";\n";
    if (psem.flow_rules[fi].expr.kind == PslFlowExpr::Kind::Jump &&
        psem.flow_rules[fi].expr.target_exit) {
      bodies << "  for (size_t i = 0; i < n; ++i) {\n";
      bodies << "    pat_bind_flow_" << fi << " fb;\n";
      bodies << "    if (match_pat_flow_" << fi << "(cx, (int32_t)i, &fb)) kills_seq[i] = 1;\n";
      bodies << "  }\n";
    }
  }
  bodies << "  size_t qh = 0, qt = 0;\n";
  bodies << "  bfs_q[qt++] = 0;\n  cx->nodes[0].reachable = 1;\n";
  bodies << "  while (qh < qt) {\n";
  bodies << "    int32_t u = bfs_q[qh++];\n";
  bodies << "    for (int32_t ei = succ_off[u]; ei < succ_off[u + 1]; ++ei) {\n";
  bodies << "      int32_t v = succ_adj[ei];\n";
  bodies << "      if (kills_seq[u] && cx->nodes[u].parent_id == cx->nodes[v].parent_id) continue;\n";
  bodies << "      if (!cx->nodes[v].reachable) { cx->nodes[v].reachable = 1; bfs_q[qt++] = v; }\n";
  bodies << "    }\n  }\n";
  bodies << "  cx->cfg_blocks = qt;\n";

  if (!psem.analyses.empty()) {
    bodies << "  size_t max_u = (n * 2u > 64u) ? n * 2u : 64u;\n";
    bodies << "  cx->df_univ = (psl_str *)psl_arena_alloc(cx->arena, max_u * sizeof(psl_str), sizeof(void *));\n";
    bodies << "  cx->df_univ_count = 0;\n";
    bodies << "  cx->df_words = (max_u + 63u) / 64u;\n";
    bodies << "  cx->df_before = (uint64_t **)psl_arena_alloc(cx->arena, " << psem.analyses.size() << "u * sizeof(uint64_t *), sizeof(void *));\n";
    bodies << "  cx->df_after = (uint64_t **)psl_arena_alloc(cx->arena, " << psem.analyses.size() << "u * sizeof(uint64_t *), sizeof(void *));\n";
    bodies << "  if (!cx->df_univ || !cx->df_before || !cx->df_after) return;\n";
    bodies << "  size_t bv_bytes = n * cx->df_words * sizeof(uint64_t);\n";
    bodies << "  uint64_t *gen_b = (uint64_t *)psl_arena_alloc(cx->arena, bv_bytes, 8u);\n";
    bodies << "  uint64_t *kill_b = (uint64_t *)psl_arena_alloc(cx->arena, bv_bytes, 8u);\n";
    bodies << "  uint64_t *tmp_v = (uint64_t *)psl_arena_alloc(cx->arena, cx->df_words * sizeof(uint64_t), 8u);\n";
    bodies << "  uint64_t *tmp_o = (uint64_t *)psl_arena_alloc(cx->arena, cx->df_words * sizeof(uint64_t), 8u);\n";
    bodies << "  if (!gen_b || !kill_b || !tmp_v || !tmp_o) return;\n";

    for (std::size_t ai = 0; ai < psem.analyses.size(); ++ai) {
      const auto& an = psem.analyses[ai];
      bodies << "  {\n";
      bodies << "    cx->df_before[" << ai << "] = (uint64_t *)psl_arena_alloc(cx->arena, bv_bytes, 8u);\n";
      bodies << "    cx->df_after[" << ai << "] = (uint64_t *)psl_arena_alloc(cx->arena, bv_bytes, 8u);\n";
      bodies << "    if (!cx->df_before[" << ai << "] || !cx->df_after[" << ai << "]) return;\n";
      bodies << "    memset(gen_b, 0, bv_bytes); memset(kill_b, 0, bv_bytes);\n";
      bodies << "    for (size_t i = 0; i < n; ++i) {\n";
      for (std::size_t gi = 0; gi < an.gen_clauses.size(); ++gi) {
        std::string pid = "df_g_" + std::to_string(ai) + "_" + std::to_string(gi);
        bodies << "      {\n        pat_bind_" << pid << " gb;\n        memset(&gb, 0, sizeof(gb));\n";
        bodies << "        if (match_pat_" << pid << "(cx, (int32_t)i, &gb)) {\n";
        bodies << "          psl_rval gv = eval_" << pid << "(cx, (int32_t)i, 0, &gb);\n";
        bodies << "          for (size_t k = 0; k < gv.count; ++k) {\n";
        bodies << "            if (gv.items[k].s.len > 0) {\n";
        bodies << "              size_t uid = df_intern_sym(cx, gv.items[k].s, max_u);\n";
        bodies << "              gen_b[i * cx->df_words + (uid >> 6u)] |= (UINT64_C(1) << (uid & 63u));\n";
        bodies << "            }\n          }\n        }\n      }\n";
      }
      for (std::size_t ki = 0; ki < an.kill_clauses.size(); ++ki) {
        std::string pid = "df_k_" + std::to_string(ai) + "_" + std::to_string(ki);
        bodies << "      {\n        pat_bind_" << pid << " kb;\n        memset(&kb, 0, sizeof(kb));\n";
        bodies << "        if (match_pat_" << pid << "(cx, (int32_t)i, &kb)) {\n";
        bodies << "          psl_rval kv = eval_" << pid << "(cx, (int32_t)i, 0, &kb);\n";
        bodies << "          for (size_t k = 0; k < kv.count; ++k) {\n";
        bodies << "            if (kv.items[k].s.len > 0) {\n";
        bodies << "              size_t uid = df_intern_sym(cx, kv.items[k].s, max_u);\n";
        bodies << "              kill_b[i * cx->df_words + (uid >> 6u)] |= (UINT64_C(1) << (uid & 63u));\n";
        bodies << "            }\n          }\n        }\n      }\n";
      }
      bodies << "    }\n";
      if (an.meet == AnalysisMeet::Must) {
        bodies << "    for (size_t i = 1; i < n; ++i) {\n";
        bodies << "      for (size_t w = 0; w < cx->df_words; ++w) {\n";
        bodies << "        cx->df_before[" << ai << "][i * cx->df_words + w] = ~UINT64_C(0);\n";
        bodies << "        cx->df_after[" << ai << "][i * cx->df_words + w] = ~UINT64_C(0);\n";
        bodies << "      }\n    }\n";
      }
      bodies << "    int changed = 1;\n    int iter = 0;\n";
      bodies << "    while (changed && iter < 128) {\n      changed = 0;\n      ++iter;\n";
      bodies << "      for (size_t i = 0; i < n; ++i) {\n";
      bodies << "        memset(tmp_v, 0, cx->df_words * sizeof(uint64_t));\n";
      if (an.direction == AnalysisDir::Forward) {
        bodies << "        if (pred_off[i] < pred_off[i + 1]) {\n";
        bodies << "          int32_t p0 = pred_adj[pred_off[i]];\n";
        bodies << "          for (size_t w = 0; w < cx->df_words; ++w) tmp_v[w] = cx->df_after[" << ai << "][(size_t)p0 * cx->df_words + w];\n";
        bodies << "          for (int32_t pe = pred_off[i] + 1; pe < pred_off[i + 1]; ++pe) {\n";
        bodies << "            int32_t px = pred_adj[pe];\n";
        bodies << "            for (size_t w = 0; w < cx->df_words; ++w) tmp_v[w] "
               << (an.meet == AnalysisMeet::May ? "|=" : "&=") << " cx->df_after[" << ai << "][(size_t)px * cx->df_words + w];\n";
        bodies << "          }\n        }\n";
        bodies << "        for (size_t w = 0; w < cx->df_words; ++w) {\n";
        bodies << "          tmp_o[w] = (tmp_v[w] & ~kill_b[i * cx->df_words + w]) | gen_b[i * cx->df_words + w];\n";
        bodies << "          if (tmp_v[w] != cx->df_before[" << ai << "][i * cx->df_words + w] || tmp_o[w] != cx->df_after[" << ai << "][i * cx->df_words + w]) {\n";
        bodies << "            cx->df_before[" << ai << "][i * cx->df_words + w] = tmp_v[w];\n";
        bodies << "            cx->df_after[" << ai << "][i * cx->df_words + w] = tmp_o[w];\n";
        bodies << "            changed = 1;\n";
        bodies << "          }\n        }\n";
      } else {
        bodies << "        if (succ_off[i] < succ_off[i + 1]) {\n";
        bodies << "          int32_t s0 = succ_adj[succ_off[i]];\n";
        bodies << "          for (size_t w = 0; w < cx->df_words; ++w) tmp_v[w] = cx->df_before[" << ai << "][(size_t)s0 * cx->df_words + w];\n";
        bodies << "          for (int32_t se = succ_off[i] + 1; se < succ_off[i + 1]; ++se) {\n";
        bodies << "            int32_t sx = succ_adj[se];\n";
        bodies << "            for (size_t w = 0; w < cx->df_words; ++w) tmp_v[w] "
               << (an.meet == AnalysisMeet::May ? "|=" : "&=") << " cx->df_before[" << ai << "][(size_t)sx * cx->df_words + w];\n";
        bodies << "          }\n        }\n";
        bodies << "        for (size_t w = 0; w < cx->df_words; ++w) {\n";
        bodies << "          tmp_o[w] = (tmp_v[w] & ~kill_b[i * cx->df_words + w]) | gen_b[i * cx->df_words + w];\n";
        bodies << "          if (tmp_o[w] != cx->df_before[" << ai << "][i * cx->df_words + w] || tmp_v[w] != cx->df_after[" << ai << "][i * cx->df_words + w]) {\n";
        bodies << "            cx->df_before[" << ai << "][i * cx->df_words + w] = tmp_o[w];\n";
        bodies << "            cx->df_after[" << ai << "][i * cx->df_words + w] = tmp_v[w];\n";
        bodies << "            changed = 1;\n";
        bodies << "          }\n        }\n";
      }
      bodies << "      }\n    }\n  }\n";
    }
  }
  bodies << "}\n\n";

  // Pass 5: Lints
  for (std::size_t li = 0; li < psem.lints.size(); ++li) {
    const auto& l = psem.lints[li];
    std::vector<std::string> lv;
    collect_pat_vars(l.match_pattern, lv);
    std::string pid = "lint_" + std::to_string(li);
    emit_pattern_matcher(matchers, pid, mod_low, mod_up, cat, l.match_pattern, lv);

    ExprEnv env;
    env.mod_low = mod_low;
    env.pat_struct = "pat_bind_" + pid;
    env.pat_vars = lv;
    env.type_names = type_names;
    env.analysis_idx = analysis_idx;
    env.helpers = &helpers;
    env.next_helper_id = next_helper_id;

    bodies << "/* generated from lint " << l.name << "; do not edit */\n";
    bodies << "static void lint_" << sanitize_ident(l.name) << "(" << mod_low << "_ctx *cx, const "
           << mod_low << "_node *n, int32_t nid) {\n";
    bodies << "  int level = 0; (void)level; (void)n;\n";
    bodies << "  if (cx->opt.profile != UINT64_C(0) && (cx->opt.profile & (UINT64_C(1) << " << li << "u)) == 0u) return;\n";
    bodies << "  if ((int)" << status_enum(l.provenance.status) << " < cx->opt.min_status) return;\n";
    bodies << "  pat_bind_" << pid << " b_inst;\n";
    bodies << "  pat_bind_" << pid << " *b = &b_inst;\n";
    bodies << "  memset(b, 0, sizeof(*b));\n";
    bodies << "  if (!match_pat_" << pid << "(cx, nid, b)) return;\n";
    if (l.when_expr) {
      bodies << "  if (!(" << emit_expr(l.when_expr, env) << ").b) return;\n";
    }
    bodies << "  psl_severity eff_sev = " << sev_enum(l.severity) << ";\n";
    if (!l.provenance.has_status || l.provenance.status == PslStatus::Draft) {
      bodies << "  if (eff_sev == PSL_SEV_ERROR) eff_sev = PSL_SEV_WARNING;\n";
    }
    bodies << "  psl_kv kvs[" << (lv.size() + 1) << "];\n";
    for (std::size_t vi = 0; vi < lv.size(); ++vi) {
      bodies << "  kvs[" << vi << "].key = psl_str_lit(" << c_str_lit(lv[vi]) << ");\n";
      bodies << "  kvs[" << vi << "].val = rv_from_node(cx, b->nid_" << sanitize_ident(lv[vi]) << ");\n";
    }
    bodies << "  psl_str msg = format_diag_tpl(cx, " << mod_up << "_LINT_" << sanitize_ident(l.name)
           << ".template_str, kvs, " << lv.size() << "u);\n";
    if (l.fix.present && l.fix.replacement_expr) {
      std::string fexpr = emit_expr(l.fix.replacement_expr, env);
      bodies << "  psl_str frep = (" << fexpr << ").s;\n";
      bodies << "  emit_diag_msg(cx, &" << mod_up << "_LINT_" << sanitize_ident(l.name)
             << ", eff_sev, n->span, msg, 1, n->span, frep, "
             << (l.fix.is_safe ? "PSL_FIX_SAFE" : "PSL_FIX_UNSAFE") << ");\n";
    } else {
      bodies << "  emit_diag_msg(cx, &" << mod_up << "_LINT_" << sanitize_ident(l.name)
             << ", eff_sev, n->span, msg, 0, n->span, psl_str_lit(\"\"), PSL_FIX_SAFE);\n";
    }
    next_helper_id = env.next_helper_id;
    bodies << "}\n\n";
  }

  bodies << "static void run_lints(" << mod_low << "_ctx *cx, const " << mod_low << "_node *n, int32_t nid) {\n";
  bodies << "  (void)cx; (void)nid;\n  if (!n) return;\n";
  bodies << "  switch (n->tag) {\n";
  std::map<std::string, std::vector<std::size_t>> by_ctor;
  std::vector<std::size_t> wildcard_lints;
  for (std::size_t li = 0; li < psem.lints.size(); ++li) {
    const auto& pat = psem.lints[li].match_pattern;
    if (pat && pat->kind == PslPattern::Kind::Ctor) {
      by_ctor[pat->name].push_back(li);
    } else {
      wildcard_lints.push_back(li);
    }
  }
  for (const auto& [cname, indices] : by_ctor) {
    bodies << "    case " << enum_for_ctor(cat, mod_up, cname) << ":\n";
    for (std::size_t li : indices) {
      bodies << "      lint_" << sanitize_ident(psem.lints[li].name) << "(cx, n, nid);\n";
    }
    bodies << "      break;\n";
  }
  bodies << "    default: break;\n  }\n";
  for (std::size_t li : wildcard_lints) {
    bodies << "  lint_" << sanitize_ident(psem.lints[li].name) << "(cx, n, nid);\n";
  }
  bodies << "}\n\n";

  // Emit pattern matchers and deferred expression helpers before bodies
  o << matchers.str() << helpers.str() << bodies.str();

  // Context creation & analyze entrypoint
  o << "PARSLEY_API " << mod_low << "_ctx *" << mod_low << "_ctx_new(psl_arena *arena, const " << mod_low << "_options *opt) {\n";
  o << "  if (!arena) return NULL;\n";
  o << "  " << mod_low << "_ctx *cx = (" << mod_low << "_ctx *)psl_arena_alloc(arena, sizeof(" << mod_low << "_ctx), sizeof(void *));\n";
  o << "  if (!cx) return NULL;\n";
  o << "  cx->arena = arena;\n";
  o << "  if (opt) {\n    cx->opt = *opt;\n  } else {\n";
  o << "    cx->opt.size = sizeof(" << mod_low << "_options);\n";
  o << "    cx->opt.schema_version = " << mod_up << "_PSL_SCHEMA_VERSION;\n";
  o << "  }\n  return cx;\n}\n\n";

  o << "static size_t count_ast_nodes(const " << mod_low << "_node *n, int depth) {\n";
  o << "  if (!n) return 0;\n  if (depth > 128 || n->child_count > 1000000u || (n->child_count && !n->children)) return SIZE_MAX;\n";
  o << "  size_t c = 1u;\n";
  o << "  for (size_t i = 0; i < n->child_count; ++i) {size_t child=count_ast_nodes(n->children[i],depth+1);if(child>1000000u || c>1000000u-child)return SIZE_MAX;c+=child;}\n";
  o << "  return c;\n}\n\n";

  o << "static int32_t index_ast_nodes(" << mod_low << "_ctx *cx, const " << mod_low << "_node *n, int32_t parent_id, size_t *cursor, int depth) {\n";
  o << "  if (!n || depth > 256 || *cursor >= cx->node_count) return -1;\n";
  o << "  int32_t id = (int32_t)(*cursor)++;\n";
  o << "  cx->nodes[id].node = n;\n";
  o << "  cx->nodes[id].parent_id = parent_id;\n";
  o << "  cx->nodes[id].type_id = -1;\n";
  o << "  cx->nodes[id].decl_sym = -1;\n";
  o << "  cx->nodes[id].resolved_sym = -1;\n";
  o << "  cx->nodes[id].reachable = 1;\n";
  o << "  cx->nodes[id].child_count = n->child_count;\n";
  o << "  if (n->child_count > 0) {\n";
  o << "    cx->nodes[id].children = (int32_t *)psl_arena_alloc(cx->arena, n->child_count * sizeof(int32_t), sizeof(int32_t));\n";
  o << "    if (cx->nodes[id].children) {\n";
  o << "      for (size_t i = 0; i < n->child_count; ++i) {\n";
  o << "        cx->nodes[id].children[i] = index_ast_nodes(cx, n->children[i], id, cursor, depth + 1);\n";
  o << "      }\n    }\n  }\n  return id;\n}\n\n";

  o << "PARSLEY_API int " << mod_low << "_analyze(" << mod_low << "_ctx *cx, const " << mod_low
    << "_node *root, psl_sink sink, void *user) {\n";
  o << "  (void)rv_none; (void)rv_bool; (void)rv_num; (void)rv_str; (void)rv_type;\n";
  o << "  (void)rv_node; (void)rv_sym; (void)rv_maybe_none; (void)rv_maybe_from;\n";
  o << "  (void)rv_empty_list; (void)rv_list_make; (void)ty_alloc; (void)ty_fresh;\n";
  o << "  (void)ty_err; (void)ty_con; (void)ty_app1; (void)ty_app2; (void)ty_find;\n";
  o << "  (void)ty_occurs; (void)ty_unify; (void)ty_format; (void)fnv1a_sym;\n";
  o << "  (void)scope_push; (void)scope_pop; (void)sym_declare; (void)sym_lookup;\n";
  o << "  (void)ast_equal; (void)is_str_one_of; (void)eval_const_fold; (void)rv_from_node;\n";
  o << "  (void)rv_eq; (void)rv_as_type; (void)rv_type_of; (void)rv_arity; (void)rv_length;\n";
  o << "  (void)rv_and_list; (void)rv_any_list; (void)rv_count_list; (void)rv_concat;\n";
  o << "  (void)rv_const_fold; (void)rv_use_count; (void)rv_reachable; (void)rv_is_immutable;\n";
  o << "  (void)rv_has_when_else; (void)rv_df_list; (void)rv_list_contains; (void)rv_text_of;\n";
  o << "  (void)format_diag_tpl; (void)emit_diag_msg; (void)df_intern_sym;\n";
  o << "  if (!cx || !root) return -1;\n";
  o << "  cx->sink = sink;\n  cx->user = user;\n";
  o << "  cx->diag_count = 0;\n  cx->type_unifications = 0;\n  cx->cfg_blocks = 0;\n  cx->lints_evaluated = "
    << psem.lints.size() << "u;\n";
  o << "  size_t total = count_ast_nodes(root, 0);\n";
  o << "  if (total == 0) return -1;\n  if (total > 1000000u || cx->arena->error) return -2;\n";
  o << "  cx->nodes = (psl_node_info *)psl_arena_alloc(cx->arena, total * sizeof(psl_node_info), sizeof(void *));\n";
  o << "  if (!cx->nodes) return -1;\n";
  o << "  cx->node_count = total;\n";
  o << "  size_t cursor = 0;\n  index_ast_nodes(cx, root, -1, &cursor, 0);\n  if(cx->arena->error)return -2;\n";
  o << "  cx->type_cap = total * 4u + 64u;\n";
  o << "  cx->types = (psl_type_node *)psl_arena_alloc(cx->arena, cx->type_cap * sizeof(psl_type_node), sizeof(void *));\n";
  o << "  cx->type_count = 0;\n";
  o << "  cx->sym_cap = total * 2u + " << (psem.builtins.size() + 32u) << "u;\n";
  o << "  cx->syms = (psl_sym *)psl_arena_alloc(cx->arena, cx->sym_cap * sizeof(psl_sym), sizeof(void *));\n";
  o << "  cx->sym_count = 0;\n";
  o << "  size_t hcap = 64u;\n";
  o << "  while (hcap < cx->sym_cap * 2u) hcap <<= 1u;\n";
  o << "  cx->hash_cap = hcap;\n";
  o << "  cx->hash_slots = (int32_t *)psl_arena_alloc(cx->arena, hcap * sizeof(int32_t), sizeof(int32_t));\n";
  o << "  if (!cx->types || !cx->syms || !cx->hash_slots) return -1;\n";
  o << "  for (size_t i = 0; i < hcap; ++i) cx->hash_slots[i] = -1;\n";
  o << "  cx->scope_depth = 0;\n  cx->next_gen = 1;\n  cx->active_gen[0] = 1;\n";
  for (const auto& bi : psem.builtins) {
    o << "  sym_declare(cx, psl_str_lit(" << c_str_lit(bi.name) << "), psl_str_lit("
      << c_str_lit(bi.symbol_kind) << "), " << emit_type_expr_builder(bi.value_type) << ", -1);\n";
  }
  o << "  run_node_passes(cx, 0, 0);\n  if(cx->arena->error)return -2;\n";
  o << "  run_dataflow_pass(cx);\n  if(cx->arena->error)return -2;\n";
  o << "  for (size_t i = 0; i < cx->node_count; ++i) {\n";
  o << "    run_lints(cx, cx->nodes[i].node, (int32_t)i);\n    if(cx->arena->error)return -2;\n";
  o << "  }\n";
  o << "  return 0;\n}\n\n";

  o << "PARSLEY_API size_t " << mod_low << "_diag_count(const " << mod_low << "_ctx *cx) { return cx ? cx->diag_count : 0u; }\n";
  o << "PARSLEY_API size_t " << mod_low << "_nodes_visited(const " << mod_low << "_ctx *cx) { return cx ? cx->node_count : 0u; }\n";
  o << "PARSLEY_API size_t " << mod_low << "_symbols_declared(const " << mod_low << "_ctx *cx) { return cx ? cx->sym_count : 0u; }\n";
  o << "PARSLEY_API size_t " << mod_low << "_type_unifications(const " << mod_low << "_ctx *cx) { return cx ? cx->type_unifications : 0u; }\n";
  o << "PARSLEY_API size_t " << mod_low << "_cfg_blocks(const " << mod_low << "_ctx *cx) { return cx ? cx->cfg_blocks : 0u; }\n";
  o << "PARSLEY_API size_t " << mod_low << "_lints_evaluated(const " << mod_low << "_ctx *cx) { return cx ? cx->lints_evaluated : 0u; }\n\n";

  // Selftest harness
  struct SelftestCase {
    std::size_t idx;
    std::string lint_name;
    bool expect_fire;
  };
  std::vector<SelftestCase> selftests;
  if (opts.emit_tests && !grammar.gRules.empty()) {
    std::size_t st_idx = 0;
    for (const auto& blk : psem.examples) {
      for (const auto& ex : blk.cases) {
        auto parsed = eval::parse_input(grammar, ex.input);
        if (parsed.ok) {
          emit_selftest_ast_builder(o, st_idx, parsed.value, mod_low, mod_up, cat);
          selftests.push_back({st_idx, blk.target_name, ex.expect_fire});
          ++st_idx;
        }
      }
    }
  }

  o << "typedef struct selftest_cap {\n  psl_str target;\n  int matched;\n} selftest_cap;\n\n";
  o << "static void selftest_sink(void *user, const psl_diag *d) {\n";
  o << "  selftest_cap *cap = (selftest_cap *)user;\n";
  o << "  if (cap && d && d->def && psl_str_eq(d->def->rule_name, cap->target)) cap->matched = 1;\n";
  o << "}\n\n";

  o << "PARSLEY_API int " << mod_low << "_psl_selftest(void) {\n  (void)selftest_sink;\n";
  if (selftests.empty()) {
    o << "  uint8_t buf[1024];\n  psl_arena arena;\n  psl_arena_init(&arena, buf, sizeof(buf));\n";
    o << "  " << mod_low << "_ctx *cx = " << mod_low << "_ctx_new(&arena, NULL);\n";
    o << "  return cx ? 0 : -1;\n";
  } else {
    o << "  uint8_t buf[65536];\n";
    o << "  psl_arena arena;\n";
    for (const auto& tc : selftests) {
      o << "  {\n";
      o << "    psl_arena_init(&arena, buf, sizeof(buf));\n";
      o << "    const " << mod_low << "_node *root = selftest_ast_" << tc.idx << "(&arena);\n";
      o << "    " << mod_low << "_ctx *cx = " << mod_low << "_ctx_new(&arena, NULL);\n";
      o << "    if (!root || !cx) return -" << (tc.idx + 1) << ";\n";
      o << "    selftest_cap cap = { psl_str_lit(" << c_str_lit(tc.lint_name) << "), 0 };\n";
      o << "    if (" << mod_low << "_analyze(cx, root, selftest_sink, &cap) != 0) return -" << (tc.idx + 1) << ";\n";
      o << "    if (cap.matched != " << (tc.expect_fire ? 1 : 0) << ") return -" << (tc.idx + 100) << ";\n";
      o << "  }\n";
    }
    o << "  return 0;\n";
  }
  o << "}\n";
  return o.str();
}

} // namespace detail::c23

// ---------------------------------------------------------------------------
// ISO C23 PSL Codegen Entrypoint (@sema-ir)
// ---------------------------------------------------------------------------

inline std::vector<PslGeneratedFile> emit_psl_c23_files(
    const PslModule& psem,
    const ir::Grammar& grammar,
    const PslCodegenOptions& opts = {}) {
  const std::string mod_name = opts.module_override.empty()
                                   ? (psem.module_name.empty() ? grammar.module_name : psem.module_name)
                                   : opts.module_override;
  const std::string mod_low = c23_text::module_stem(mod_name);
  const std::string mod_up = detail::c23::to_upper_ascii(mod_low);
  const auto cat = detail::c23::build_ctor_catalog(psem, grammar, mod_up);

  std::vector<PslGeneratedFile> files;
  files.push_back({mod_low + "_ast.h", detail::c23::emit_ast_header(mod_low, mod_up, cat)});
  files.push_back({mod_low + "_diag.h", detail::c23::emit_diag_header(psem, mod_low, mod_up)});
  files.push_back({mod_low + "_parse.h", detail::c23::emit_parse_header(mod_low, mod_up)});
  files.push_back({mod_low + "_parse.c", detail::c23::emit_parse_source(mod_low, mod_up, cat)});
  files.push_back({mod_low + "_sem.h", detail::c23::emit_sem_header(mod_low, mod_up)});
  files.push_back({mod_low + "_sem.c",
                   detail::c23::emit_sem_source(psem, grammar, opts, mod_low, mod_up, cat)});
  return files;
}

} // namespace parsley::psl
