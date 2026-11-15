module;

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module parsley.lexer;

import parsley.diagnostic;

export namespace parsley::lexer {

enum class TokenKind {
  // Literals & identifiers
  LowerIdent,
  UpperIdent,
  BacktickIdent,
  StringLit,
  RegexLit,
  IntLit,
  FloatLit,
  Pragma,

  // Keywords
  KwModule,
  KwWhere,
  KwImport,
  KwQualified,
  KwAs,
  KwHiding,
  KwData,
  KwNewtype,
  KwForeign,
  KwToken,
  KwSkip,
  KwKeywords,
  KwMode,
  KwOperators,
  KwInfixl,
  KwInfixr,
  KwInfix,
  KwPrefix,
  KwPostfix,
  KwMixfix,
  KwDo,
  KwPure,
  KwContext,
  KwExample,
  KwParsesTo,
  KwFailsAt,
  KwExpecting,
  KwPush,
  KwPop,

  // Operators
  ColonColon,
  Equals,
  Pipe,
  Arrow,
  LArrow,
  FatArrow,
  OpFmap,
  OpAp,
  OpSeqR,
  OpSeqL,
  OpAlt,
  OpLabel,
  LAngle,
  RAngle,

  // Delimiters
  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  Comma,
  Colon,
  Dot,

  // Virtual layout tokens
  VBraceOpen,
  VSemi,
  VBraceClose,

  // End / Error
  Eof,
  Invalid,
};

struct CommentTrivia {
  Span span;
  std::string text;
  bool is_block = false;
};

struct Token {
  TokenKind kind = TokenKind::Eof;
  std::string lexeme;
  std::string value;
  Span span;
  bool is_virtual = false;
};

struct LexResult {
  std::vector<Token> tokens;
  std::vector<CommentTrivia> comments;
};

inline std::string_view token_kind_name(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::LowerIdent: return "identifier";
    case TokenKind::UpperIdent: return "constructor/type identifier";
    case TokenKind::BacktickIdent: return "backtick operator";
    case TokenKind::StringLit: return "string literal";
    case TokenKind::RegexLit: return "regex literal";
    case TokenKind::IntLit: return "integer literal";
    case TokenKind::FloatLit: return "float literal";
    case TokenKind::Pragma: return "pragma";
    case TokenKind::KwModule: return "`module`";
    case TokenKind::KwWhere: return "`where`";
    case TokenKind::KwImport: return "`import`";
    case TokenKind::KwQualified: return "`qualified`";
    case TokenKind::KwAs: return "`as`";
    case TokenKind::KwHiding: return "`hiding`";
    case TokenKind::KwData: return "`data`";
    case TokenKind::KwNewtype: return "`newtype`";
    case TokenKind::KwForeign: return "`foreign`";
    case TokenKind::KwToken: return "`token`";
    case TokenKind::KwSkip: return "`skip`";
    case TokenKind::KwKeywords: return "`keywords`";
    case TokenKind::KwMode: return "`mode`";
    case TokenKind::KwOperators: return "`operators`";
    case TokenKind::KwInfixl: return "`infixl`";
    case TokenKind::KwInfixr: return "`infixr`";
    case TokenKind::KwInfix: return "`infix`";
    case TokenKind::KwPrefix: return "`prefix`";
    case TokenKind::KwPostfix: return "`postfix`";
    case TokenKind::KwMixfix: return "`mixfix`";
    case TokenKind::KwDo: return "`do`";
    case TokenKind::KwPure: return "`pure`";
    case TokenKind::KwContext: return "`context`";
    case TokenKind::KwExample: return "`example`";
    case TokenKind::KwParsesTo: return "`parses-to`";
    case TokenKind::KwFailsAt: return "`fails-at`";
    case TokenKind::KwExpecting: return "`expecting`";
    case TokenKind::KwPush: return "`push`";
    case TokenKind::KwPop: return "`pop`";
    case TokenKind::ColonColon: return "`::`";
    case TokenKind::Equals: return "`=`";
    case TokenKind::Pipe: return "`|`";
    case TokenKind::Arrow: return "`->`";
    case TokenKind::LArrow: return "`<-`";
    case TokenKind::FatArrow: return "`=>`";
    case TokenKind::OpFmap: return "`<$>`";
    case TokenKind::OpAp: return "`<*>`";
    case TokenKind::OpSeqR: return "`*>`";
    case TokenKind::OpSeqL: return "`<*`";
    case TokenKind::OpAlt: return "`<|>`";
    case TokenKind::OpLabel: return "`<?>`";
    case TokenKind::LAngle: return "`<`";
    case TokenKind::RAngle: return "`>`";
    case TokenKind::LParen: return "`(`";
    case TokenKind::RParen: return "`)`";
    case TokenKind::LBracket: return "`[`";
    case TokenKind::RBracket: return "`]`";
    case TokenKind::LBrace: return "`{`";
    case TokenKind::RBrace: return "`}`";
    case TokenKind::Comma: return "`,`";
    case TokenKind::Colon: return "`:`";
    case TokenKind::Dot: return "`.`";
    case TokenKind::VBraceOpen: return "layout `{`";
    case TokenKind::VSemi: return "layout `;`";
    case TokenKind::VBraceClose: return "layout `}`";
    case TokenKind::Eof: return "end of file";
    case TokenKind::Invalid: return "invalid token";
  }
  return "unknown token";
}

} // namespace parsley::lexer

namespace parsley::lexer::detail {

inline bool is_ident_start_lower(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || c == '_';
}

inline bool is_ident_start_upper(unsigned char c) noexcept {
  return (c >= 'A' && c <= 'Z');
}

inline bool is_ident_cont(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '\'';
}

inline bool is_digit(unsigned char c) noexcept {
  return c >= '0' && c <= '9';
}

inline std::string trim_sv(std::string_view sv) {
  std::size_t b = 0;
  while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
    ++b;
  }
  std::size_t e = sv.size();
  while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
    --e;
  }
  return std::string(sv.substr(b, e - b));
}

inline TokenKind classify_keyword(std::string_view text) noexcept {
  if (text == "module") return TokenKind::KwModule;
  if (text == "where") return TokenKind::KwWhere;
  if (text == "import") return TokenKind::KwImport;
  if (text == "qualified") return TokenKind::KwQualified;
  if (text == "as") return TokenKind::KwAs;
  if (text == "hiding") return TokenKind::KwHiding;
  if (text == "data") return TokenKind::KwData;
  if (text == "newtype") return TokenKind::KwNewtype;
  if (text == "foreign") return TokenKind::KwForeign;
  if (text == "token") return TokenKind::KwToken;
  if (text == "skip") return TokenKind::KwSkip;
  if (text == "keywords") return TokenKind::KwKeywords;
  if (text == "mode") return TokenKind::KwMode;
  if (text == "operators") return TokenKind::KwOperators;
  if (text == "infixl") return TokenKind::KwInfixl;
  if (text == "infixr") return TokenKind::KwInfixr;
  if (text == "infix") return TokenKind::KwInfix;
  if (text == "prefix") return TokenKind::KwPrefix;
  if (text == "postfix") return TokenKind::KwPostfix;
  if (text == "mixfix") return TokenKind::KwMixfix;
  if (text == "do") return TokenKind::KwDo;
  if (text == "pure") return TokenKind::KwPure;
  if (text == "context") return TokenKind::KwContext;
  if (text == "example") return TokenKind::KwExample;
  if (text == "parses-to") return TokenKind::KwParsesTo;
  if (text == "fails-at") return TokenKind::KwFailsAt;
  if (text == "expecting") return TokenKind::KwExpecting;
  if (text == "push") return TokenKind::KwPush;
  if (text == "pop") return TokenKind::KwPop;
  return TokenKind::LowerIdent;
}

class Scanner {
public:
  Scanner(std::string_view source, std::string_view filename, DiagnosticBag& diags)
      : src_(source), filename_(filename), diags_(diags) {}

  LexResult scan_raw() {
    LexResult result;

    while (!at_end()) {
      skip_whitespace();
      if (at_end()) {
        break;
      }

      if (starts_with("{-#")) {
        result.tokens.push_back(scan_pragma());
        continue;
      }
      if (starts_with("{-")) {
        result.comments.push_back(scan_block_comment());
        continue;
      }
      if (starts_with("--")) {
        result.comments.push_back(scan_line_comment());
        continue;
      }

      const std::size_t start_off = pos_;
      const int start_line = line_;
      const int start_col = col_;
      const unsigned char c = static_cast<unsigned char>(peek());

      if (c == '"') {
        result.tokens.push_back(scan_string());
        continue;
      }
      if (c == '/') {
        result.tokens.push_back(scan_regex());
        continue;
      }
      if (c == '`') {
        result.tokens.push_back(scan_backtick());
        continue;
      }
      if (is_digit(c)) {
        result.tokens.push_back(scan_number());
        continue;
      }
      if (is_ident_start_lower(c)) {
        result.tokens.push_back(scan_lower_ident());
        continue;
      }
      if (is_ident_start_upper(c)) {
        result.tokens.push_back(scan_upper_ident());
        continue;
      }

      // Multi-char operators (longest match first)
      if (starts_with("<$>")) {
        advance_n(3);
        result.tokens.push_back(make_token(TokenKind::OpFmap, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("<*>")) {
        advance_n(3);
        result.tokens.push_back(make_token(TokenKind::OpAp, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("<|>")) {
        advance_n(3);
        result.tokens.push_back(make_token(TokenKind::OpAlt, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("<?>")) {
        advance_n(3);
        result.tokens.push_back(make_token(TokenKind::OpLabel, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("::")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::ColonColon, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("->")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::Arrow, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("<-")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::LArrow, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("=>")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::FatArrow, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("*>")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::OpSeqR, start_off, start_line, start_col));
        continue;
      }
      if (starts_with("<*")) {
        advance_n(2);
        result.tokens.push_back(make_token(TokenKind::OpSeqL, start_off, start_line, start_col));
        continue;
      }

      // Single-char operators & delimiters
      advance_char();
      switch (c) {
        case '=':
          result.tokens.push_back(make_token(TokenKind::Equals, start_off, start_line, start_col));
          break;
        case '|':
          result.tokens.push_back(make_token(TokenKind::Pipe, start_off, start_line, start_col));
          break;
        case '<':
          result.tokens.push_back(make_token(TokenKind::LAngle, start_off, start_line, start_col));
          break;
        case '>':
          result.tokens.push_back(make_token(TokenKind::RAngle, start_off, start_line, start_col));
          break;
        case '(':
          result.tokens.push_back(make_token(TokenKind::LParen, start_off, start_line, start_col));
          break;
        case ')':
          result.tokens.push_back(make_token(TokenKind::RParen, start_off, start_line, start_col));
          break;
        case '[':
          result.tokens.push_back(make_token(TokenKind::LBracket, start_off, start_line, start_col));
          break;
        case ']':
          result.tokens.push_back(make_token(TokenKind::RBracket, start_off, start_line, start_col));
          break;
        case '{':
          result.tokens.push_back(make_token(TokenKind::LBrace, start_off, start_line, start_col));
          break;
        case '}':
          result.tokens.push_back(make_token(TokenKind::RBrace, start_off, start_line, start_col));
          break;
        case ',':
          result.tokens.push_back(make_token(TokenKind::Comma, start_off, start_line, start_col));
          break;
        case ':':
          result.tokens.push_back(make_token(TokenKind::Colon, start_off, start_line, start_col));
          break;
        case '.':
          result.tokens.push_back(make_token(TokenKind::Dot, start_off, start_line, start_col));
          break;
        default: {
          Token tok = make_token(TokenKind::Invalid, start_off, start_line, start_col);
          diags_.error("E0001",
                       std::string("unexpected character `") + tok.lexeme + "`",
                       tok.span,
                       "unrecognized character");
          result.tokens.push_back(std::move(tok));
          break;
        }
      }
    }

    Token eof_tok;
    eof_tok.kind = TokenKind::Eof;
    eof_tok.span = Span{
        .file = std::string(filename_),
        .start_line = line_,
        .start_col = col_,
        .end_line = line_,
        .end_col = col_,
        .start_offset = pos_,
        .end_offset = pos_,
    };
    result.tokens.push_back(std::move(eof_tok));
    return result;
  }

private:
  bool at_end() const noexcept { return pos_ >= src_.size(); }

  char peek(std::size_t offset = 0) const noexcept {
    if (pos_ + offset >= src_.size()) {
      return '\0';
    }
    return src_[pos_ + offset];
  }

  bool starts_with(std::string_view prefix) const noexcept {
    return src_.substr(pos_).starts_with(prefix);
  }

  void advance_char() noexcept {
    if (pos_ >= src_.size()) {
      return;
    }
    const unsigned char c = static_cast<unsigned char>(src_[pos_]);
    ++pos_;
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else if (c == '\r') {
      // Ignore CR for column tracking
    } else if ((c & 0xC0) != 0x80) {
      // Advance column once per UTF-8 codepoint
      ++col_;
    }
  }

  void advance_n(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n && !at_end(); ++i) {
      advance_char();
    }
  }

  void skip_whitespace() noexcept {
    while (!at_end()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
        advance_char();
      } else {
        break;
      }
    }
  }

  Span make_span(std::size_t start_off, int start_line, int start_col) const {
    return Span{
        .file = std::string(filename_),
        .start_line = start_line,
        .start_col = start_col,
        .end_line = line_,
        .end_col = col_,
        .start_offset = start_off,
        .end_offset = pos_,
    };
  }

  Token make_token(TokenKind kind,
                   std::size_t start_off,
                   int start_line,
                   int start_col,
                   std::string value = {}) const {
    Token tok;
    tok.kind = kind;
    tok.lexeme = std::string(src_.substr(start_off, pos_ - start_off));
    tok.value = value.empty() ? tok.lexeme : std::move(value);
    tok.span = make_span(start_off, start_line, start_col);
    return tok;
  }

  Token scan_pragma() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    advance_n(3); // {-#
    const std::size_t body_start = pos_;
    while (!at_end() && !starts_with("#-}")) {
      advance_char();
    }
    const std::size_t body_end = pos_;
    if (at_end()) {
      const Span sp = make_span(start_off, start_line, start_col);
      diags_.error("E0001", "unterminated pragma `{-#`", sp, "expected `#-}`");
      Token tok = make_token(TokenKind::Pragma, start_off, start_line, start_col);
      tok.value = trim_sv(src_.substr(body_start, body_end - body_start));
      return tok;
    }
    advance_n(3); // #-}
    Token tok = make_token(TokenKind::Pragma, start_off, start_line, start_col);
    tok.value = trim_sv(src_.substr(body_start, body_end - body_start));
    return tok;
  }

  CommentTrivia scan_block_comment() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    advance_n(2); // {-
    int depth = 1;
    while (!at_end() && depth > 0) {
      if (starts_with("{-")) {
        ++depth;
        advance_n(2);
      } else if (starts_with("-}")) {
        --depth;
        advance_n(2);
      } else {
        advance_char();
      }
    }
    const Span sp = make_span(start_off, start_line, start_col);
    if (depth > 0) {
      diags_.error("E0001", "unterminated block comment `{-`", sp, "expected `-}`");
    }
    return CommentTrivia{
        .span = sp,
        .text = std::string(src_.substr(start_off, pos_ - start_off)),
        .is_block = true,
    };
  }

  CommentTrivia scan_line_comment() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    while (!at_end() && peek() != '\n') {
      advance_char();
    }
    return CommentTrivia{
        .span = make_span(start_off, start_line, start_col),
        .text = std::string(src_.substr(start_off, pos_ - start_off)),
        .is_block = false,
    };
  }

  Token scan_string() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    advance_char(); // opening "
    std::string decoded;
    bool terminated = false;

    while (!at_end()) {
      const char c = peek();
      if (c == '"') {
        advance_char();
        terminated = true;
        break;
      }
      if (c == '\n') {
        break;
      }
      if (c == '\\') {
        advance_char();
        if (at_end() || peek() == '\n') {
          break;
        }
        const char esc = peek();
        advance_char();
        switch (esc) {
          case 'n': decoded.push_back('\n'); break;
          case 'r': decoded.push_back('\r'); break;
          case 't': decoded.push_back('\t'); break;
          case '\\': decoded.push_back('\\'); break;
          case '"': decoded.push_back('"'); break;
          case '\'': decoded.push_back('\''); break;
          case '0': decoded.push_back('\0'); break;
          default: decoded.push_back(esc); break;
        }
      } else {
        decoded.push_back(c);
        advance_char();
      }
    }

    Token tok;
    tok.kind = TokenKind::StringLit;
    tok.lexeme = std::string(src_.substr(start_off, pos_ - start_off));
    tok.value = std::move(decoded);
    tok.span = make_span(start_off, start_line, start_col);
    if (!terminated) {
      diags_.error("E0001", "unterminated string literal", tok.span, "missing closing `\"`");
    }
    return tok;
  }

  Token scan_regex() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    advance_char(); // opening /
    const std::size_t body_start = pos_;
    std::size_t body_end = pos_;
    bool in_class = false;
    bool terminated = false;

    while (!at_end()) {
      const char c = peek();
      if (c == '\n') {
        body_end = pos_;
        break;
      }
      if (c == '\\') {
        advance_char();
        if (!at_end() && peek() != '\n') {
          advance_char();
        }
        continue;
      }
      if (c == '[' && !in_class) {
        in_class = true;
        advance_char();
        continue;
      }
      if (c == ']' && in_class) {
        in_class = false;
        advance_char();
        continue;
      }
      if (c == '/' && !in_class) {
        body_end = pos_;
        advance_char();
        terminated = true;
        break;
      }
      advance_char();
    }

    Token tok;
    tok.kind = TokenKind::RegexLit;
    tok.lexeme = std::string(src_.substr(start_off, pos_ - start_off));
    tok.value = std::string(src_.substr(body_start, body_end - body_start));
    tok.span = make_span(start_off, start_line, start_col);
    if (!terminated) {
      diags_.error("E0001", "unterminated regex literal", tok.span, "missing closing `/`");
    }
    return tok;
  }

  Token scan_backtick() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    advance_char(); // opening `
    const std::size_t body_start = pos_;
    while (!at_end() && peek() != '`' && peek() != '\n') {
      advance_char();
    }
    const std::size_t body_end = pos_;
    bool terminated = false;
    if (!at_end() && peek() == '`') {
      advance_char();
      terminated = true;
    }

    Token tok;
    tok.kind = TokenKind::BacktickIdent;
    tok.lexeme = std::string(src_.substr(start_off, pos_ - start_off));
    tok.value = trim_sv(src_.substr(body_start, body_end - body_start));
    tok.span = make_span(start_off, start_line, start_col);
    if (!terminated || tok.value.empty()) {
      diags_.error("E0001", "invalid backtick infix operator", tok.span, "expected `` `ident` ``");
    }
    return tok;
  }

  Token scan_number() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    while (!at_end() && is_digit(static_cast<unsigned char>(peek()))) {
      advance_char();
    }
    bool is_float = false;
    if (peek() == '.' && is_digit(static_cast<unsigned char>(peek(1)))) {
      is_float = true;
      advance_char(); // '.'
      while (!at_end() && is_digit(static_cast<unsigned char>(peek()))) {
        advance_char();
      }
      if ((peek() == 'e' || peek() == 'E')) {
        std::size_t exp_look = 1;
        if (peek(exp_look) == '+' || peek(exp_look) == '-') {
          ++exp_look;
        }
        if (is_digit(static_cast<unsigned char>(peek(exp_look)))) {
          advance_n(exp_look);
          while (!at_end() && is_digit(static_cast<unsigned char>(peek()))) {
            advance_char();
          }
        }
      }
    }
    return make_token(is_float ? TokenKind::FloatLit : TokenKind::IntLit,
                      start_off,
                      start_line,
                      start_col);
  }

  Token scan_lower_ident() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    while (!at_end() && is_ident_cont(static_cast<unsigned char>(peek()))) {
      advance_char();
    }
    std::string_view word = src_.substr(start_off, pos_ - start_off);
    if ((word == "parses" && starts_with("-to") &&
         !is_ident_cont(static_cast<unsigned char>(peek(3)))) ||
        (word == "fails" && starts_with("-at") &&
         !is_ident_cont(static_cast<unsigned char>(peek(3))))) {
      advance_n(3);
      word = src_.substr(start_off, pos_ - start_off);
    }
    return make_token(classify_keyword(word), start_off, start_line, start_col);
  }

  Token scan_upper_ident() {
    const std::size_t start_off = pos_;
    const int start_line = line_;
    const int start_col = col_;
    while (!at_end() && is_ident_cont(static_cast<unsigned char>(peek()))) {
      advance_char();
    }
    return make_token(TokenKind::UpperIdent, start_off, start_line, start_col);
  }

  std::string_view src_;
  std::string_view filename_;
  DiagnosticBag& diags_;
  std::size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
};

struct LayoutEntry {
  int column = 1;
  bool is_explicit = false;
  int base_paren_depth = 0;
};

inline Token make_virtual(TokenKind kind, const Span& anchor) {
  Token tok;
  tok.kind = kind;
  tok.is_virtual = true;
  tok.span = Span{
      .file = anchor.file,
      .start_line = anchor.start_line,
      .start_col = anchor.start_col,
      .end_line = anchor.start_line,
      .end_col = anchor.start_col,
      .start_offset = anchor.start_offset,
      .end_offset = anchor.start_offset,
  };
  switch (kind) {
    case TokenKind::VBraceOpen: tok.lexeme = "{"; tok.value = "{"; break;
    case TokenKind::VSemi: tok.lexeme = ";"; tok.value = ";"; break;
    case TokenKind::VBraceClose: tok.lexeme = "}"; tok.value = "}"; break;
    default: break;
  }
  return tok;
}

inline std::vector<Token> apply_layout(const std::vector<Token>& raw) {
  std::vector<Token> out;
  out.reserve(raw.size() + raw.size() / 4);

  std::vector<LayoutEntry> stack;
  int paren_depth = 0;
  bool pending_layout = false;
  bool seen_non_pragma = false;
  bool waiting_module_where = false;
  int last_real_line = 0;

  auto emit_real = [&](const Token& t) {
    out.push_back(t);
    last_real_line = t.span.end_line;
  };

  for (std::size_t i = 0; i < raw.size(); ++i) {
    const Token& tok = raw[i];

    if (tok.kind == TokenKind::Eof) {
      if (pending_layout) {
        out.push_back(make_virtual(TokenKind::VBraceOpen, tok.span));
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        pending_layout = false;
      }
      while (!stack.empty()) {
        if (!stack.back().is_explicit) {
          out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        }
        stack.pop_back();
      }
      emit_real(tok);
      break;
    }

    // Track explicit grouping delimiters that close any inner layout blocks
    if (tok.kind == TokenKind::RParen || tok.kind == TokenKind::RBracket) {
      if (pending_layout) {
        out.push_back(make_virtual(TokenKind::VBraceOpen, tok.span));
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        pending_layout = false;
      }
      while (!stack.empty() && !stack.back().is_explicit &&
             stack.back().base_paren_depth >= paren_depth) {
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        stack.pop_back();
      }
      if (paren_depth > 0) {
        --paren_depth;
      }
      emit_real(tok);
      continue;
    }

    // Handle headerless modules: if first non-pragma token is not `module`,
    // open an implicit top-level layout block before it.
    if (!seen_non_pragma && tok.kind != TokenKind::Pragma) {
      seen_non_pragma = true;
      if (tok.kind == TokenKind::KwModule) {
        waiting_module_where = true;
      } else {
        out.push_back(make_virtual(TokenKind::VBraceOpen, tok.span));
        stack.push_back(LayoutEntry{
            .column = tok.span.start_col,
            .is_explicit = false,
            .base_paren_depth = 0,
        });
        emit_real(tok);
        if (tok.kind == TokenKind::LParen || tok.kind == TokenKind::LBracket) {
          ++paren_depth;
        }
        if (tok.kind == TokenKind::KwDo || tok.kind == TokenKind::KwWhere) {
          pending_layout = true;
        }
        continue;
      }
    }

    if (pending_layout) {
      pending_layout = false;
      if (tok.kind == TokenKind::LBrace) {
        stack.push_back(LayoutEntry{
            .column = tok.span.start_col,
            .is_explicit = true,
            .base_paren_depth = paren_depth,
        });
        emit_real(tok);
        continue;
      }
      const int tok_col = tok.span.start_col;
      int enclosing_col = 0;
      for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (!it->is_explicit && it->base_paren_depth == paren_depth) {
          enclosing_col = it->column;
          break;
        }
      }
      if (enclosing_col > 0 && tok_col <= enclosing_col) {
        // Empty layout block
        out.push_back(make_virtual(TokenKind::VBraceOpen, tok.span));
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        // Fall through to handle tok against enclosing layout blocks
      } else {
        out.push_back(make_virtual(TokenKind::VBraceOpen, tok.span));
        stack.push_back(LayoutEntry{
            .column = tok_col,
            .is_explicit = false,
            .base_paren_depth = paren_depth,
        });
        emit_real(tok);
        if (tok.kind == TokenKind::LParen || tok.kind == TokenKind::LBracket) {
          ++paren_depth;
        }
        if (tok.kind == TokenKind::KwWhere || tok.kind == TokenKind::KwDo) {
          pending_layout = true;
        }
        continue;
      }
    }

    if (tok.kind == TokenKind::LBrace) {
      stack.push_back(LayoutEntry{
          .column = tok.span.start_col,
          .is_explicit = true,
          .base_paren_depth = paren_depth,
      });
      emit_real(tok);
      continue;
    }

    if (tok.kind == TokenKind::RBrace) {
      while (!stack.empty() && !stack.back().is_explicit &&
             stack.back().base_paren_depth >= paren_depth) {
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        stack.pop_back();
      }
      if (!stack.empty() && stack.back().is_explicit) {
        stack.pop_back();
      }
      emit_real(tok);
      continue;
    }

    // Apply offside rule on tokens that start at the current paren depth
    const bool new_line = (last_real_line == 0) || (tok.span.start_line > last_real_line);
    if (new_line && (!stack.empty() && stack.back().base_paren_depth == paren_depth)) {
      const int col = tok.span.start_col;
      while (!stack.empty() && !stack.back().is_explicit &&
             stack.back().base_paren_depth == paren_depth &&
             col < stack.back().column) {
        out.push_back(make_virtual(TokenKind::VBraceClose, tok.span));
        stack.pop_back();
      }
      if (!stack.empty() && !stack.back().is_explicit &&
          stack.back().base_paren_depth == paren_depth &&
          col == stack.back().column) {
        if (out.empty() || out.back().kind != TokenKind::VBraceOpen) {
          out.push_back(make_virtual(TokenKind::VSemi, tok.span));
        }
      }
    }

    emit_real(tok);

    if (tok.kind == TokenKind::LParen || tok.kind == TokenKind::LBracket) {
      ++paren_depth;
    } else if (tok.kind == TokenKind::KwWhere) {
      if (waiting_module_where) {
        waiting_module_where = false;
      }
      pending_layout = true;
    } else if (tok.kind == TokenKind::KwDo) {
      pending_layout = true;
    }
  }

  return out;
}

} // namespace parsley::lexer::detail

export namespace parsley::lexer {

inline LexResult tokenize(std::string_view source,
                          std::string_view filename,
                          DiagnosticBag& diags) {
  detail::Scanner scanner(source, filename, diags);
  LexResult raw = scanner.scan_raw();
  LexResult result;
  result.comments = std::move(raw.comments);
  result.tokens = detail::apply_layout(raw.tokens);
  return result;
}

} // namespace parsley::lexer
