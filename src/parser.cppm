module;

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module parsley.parser;

import parsley.diagnostic;
import parsley.lexer;
import parsley.ast;

namespace parsley::parser::detail {

using namespace parsley::lexer;
using namespace parsley::ast;

inline std::vector<std::string> split_pragma_args(std::string_view body) {
  std::vector<std::string> args;
  std::size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
      ++i;
    }
    if (i >= body.size()) {
      break;
    }
    if (body[i] == '<' || body[i] == '>') {
      args.emplace_back(1, body[i]);
      ++i;
      continue;
    }
    const std::size_t start = i;
    while (i < body.size() && !std::isspace(static_cast<unsigned char>(body[i])) &&
           body[i] != '<' && body[i] != '>') {
      ++i;
    }
    args.emplace_back(body.substr(start, i - start));
  }
  return args;
}

inline double parse_double(std::string_view text) {
  std::string tmp(text);
  char* end_ptr = nullptr;
  const double val = std::strtod(tmp.c_str(), &end_ptr);
  return val;
}

inline int parse_int(std::string_view text) {
  int val = 0;
  std::from_chars(text.data(), text.data() + text.size(), val);
  return val;
}

class Parser {
public:
  Parser(std::vector<Token> tokens,
         std::vector<CommentTrivia> comments,
         std::string_view filename,
         DiagnosticBag& diags)
      : tokens_(std::move(tokens)),
        comments_(std::move(comments)),
        filename_(filename),
        diags_(diags) {}

  Module parse_module() {
    Module mod;
    for (const auto& c : comments_) {
      mod.comments.push_back(Comment{
          .span = c.span,
          .text = c.text,
          .is_block = c.is_block,
      });
    }

    // Consume any pragmas appearing before `module`
    while (check(TokenKind::Pragma)) {
      mod.pragmas.push_back(parse_pragma());
    }

    if (check(TokenKind::KwModule)) {
      mod.has_module_header = true;
      const Token mod_kw = advance();
      mod.name = parse_qualified_upper("expected module name");

      if (match(TokenKind::LParen)) {
        mod.has_export_list = true;
        if (!check(TokenKind::RParen)) {
          while (true) {
            mod.exports.push_back(parse_export_item());
            if (!match(TokenKind::Comma)) {
              break;
            }
          }
        }
        expect(TokenKind::RParen, "expected `)` to close export list");
      }

      expect(TokenKind::KwWhere, "expected `where` after module header");
      mod.span = mod_kw.span;
    }

    // Top-level declarations (wrapped in VBraceOpen ... VBraceClose or explicit braces)
    const bool has_brace = match(TokenKind::VBraceOpen) || match(TokenKind::LBrace);
    while (!at_end() && !check(TokenKind::VBraceClose) && !check(TokenKind::RBrace)) {
      while (match(TokenKind::VSemi)) {
      }
      if (at_end() || check(TokenKind::VBraceClose) || check(TokenKind::RBrace)) {
        break;
      }

      parse_top_decl(mod);

      if (at_end() || check(TokenKind::VBraceClose) || check(TokenKind::RBrace)) {
        break;
      }
      if (!match(TokenKind::VSemi)) {
        if (!check(TokenKind::VBraceClose) && !check(TokenKind::RBrace) && !at_end()) {
          error_here("unexpected token in declaration; expected new declaration or end of block");
          sync_to_top_decl();
        }
      }
    }

    if (has_brace) {
      if (!match(TokenKind::VBraceClose) && !match(TokenKind::RBrace) && !at_end()) {
        error_here("expected end of module block");
      }
    }

    // Attach RuleSigs to matching RuleDefs
    for (auto& def : mod.rule_defs) {
      if (!def.signature.has_value()) {
        for (const auto& sig : mod.rule_sigs) {
          if (sig.name == def.name) {
            def.signature = sig.type;
            break;
          }
        }
      }
    }

    if (!tokens_.empty()) {
      const Span& first_sp = tokens_.front().span;
      const Span& last_sp = tokens_.back().span;
      mod.span = Span::merge(first_sp, last_sp);
    } else {
      mod.span.file = std::string(filename_);
    }

    return mod;
  }

private:
  const Token& peek(std::size_t offset = 0) const {
    if (pos_ + offset >= tokens_.size()) {
      return tokens_.back();
    }
    return tokens_[pos_ + offset];
  }

  const Token& previous() const {
    if (pos_ == 0) {
      return tokens_.front();
    }
    return tokens_[pos_ - 1];
  }

  bool at_end() const noexcept { return peek().kind == TokenKind::Eof; }

  bool check(TokenKind kind) const noexcept { return peek().kind == kind; }

  const Token& advance() {
    if (!at_end()) {
      ++pos_;
    }
    return previous();
  }

  bool match(TokenKind kind) {
    if (check(kind)) {
      advance();
      return true;
    }
    return false;
  }

  void error_here(const std::string& message, std::string label = {}) {
    const Token& tok = peek();
    if (label.empty()) {
      label = std::string("found ") + std::string(token_kind_name(tok.kind));
    }
    diags_.error("E0100", message, tok.span, std::move(label));
  }

  bool expect(TokenKind kind, const std::string& message) {
    if (check(kind)) {
      advance();
      return true;
    }
    error_here(message, std::string("expected ") + std::string(token_kind_name(kind)));
    return false;
  }

  void sync_to_top_decl() {
    int depth = 0;
    while (!at_end()) {
      if (check(TokenKind::VBraceOpen) || check(TokenKind::LBrace) ||
          check(TokenKind::LParen) || check(TokenKind::LBracket)) {
        ++depth;
      } else if (check(TokenKind::VBraceClose) || check(TokenKind::RBrace) ||
                 check(TokenKind::RParen) || check(TokenKind::RBracket)) {
        if (depth == 0) {
          break;
        }
        --depth;
      } else if (depth == 0 && check(TokenKind::VSemi)) {
        break;
      }
      advance();
    }
  }

  void sync_to_stmt() {
    int depth = 0;
    while (!at_end()) {
      if (check(TokenKind::VBraceOpen) || check(TokenKind::LBrace) ||
          check(TokenKind::LParen) || check(TokenKind::LBracket)) {
        ++depth;
      } else if (check(TokenKind::VBraceClose) || check(TokenKind::RBrace) ||
                 check(TokenKind::RParen) || check(TokenKind::RBracket)) {
        if (depth == 0) {
          break;
        }
        --depth;
      } else if (depth == 0 && check(TokenKind::VSemi)) {
        break;
      }
      advance();
    }
  }

  std::string parse_qualified_upper(const std::string& err_msg) {
    if (!check(TokenKind::UpperIdent)) {
      error_here(err_msg);
      return "<error>";
    }
    std::string name = advance().value;
    while (check(TokenKind::Dot) && peek(1).kind == TokenKind::UpperIdent) {
      advance(); // .
      name.push_back('.');
      name.append(advance().value);
    }
    return name;
  }

  std::string parse_ident_name(const std::string& err_msg) {
    if (check(TokenKind::LowerIdent) || check(TokenKind::UpperIdent)) {
      return advance().value;
    }
    error_here(err_msg);
    return "<error>";
  }

  PragmaDecl parse_pragma() {
    const Token tok = advance();
    PragmaDecl p;
    p.raw_body = tok.value;
    p.args = split_pragma_args(tok.value);
    if (!p.args.empty()) {
      p.name = p.args.front();
      p.args.erase(p.args.begin());
    }
    p.span = tok.span;
    return p;
  }

  ExportItem parse_export_item() {
    ExportItem item;
    const Token first = peek();
    if (check(TokenKind::LowerIdent) || check(TokenKind::UpperIdent)) {
      item.name = advance().value;
      item.span = first.span;
      if (match(TokenKind::LParen)) {
        // Handle Type(..)
        if (check(TokenKind::Dot) && peek(1).kind == TokenKind::Dot) {
          advance();
          advance();
          item.all_ctors = true;
        }
        expect(TokenKind::RParen, "expected `)` after export specifier");
        item.span = Span::merge(first.span, previous().span);
      }
    } else {
      error_here("expected exported rule or type name");
      item.name = "<error>";
      item.span = first.span;
    }
    return item;
  }

  void parse_top_decl(Module& mod) {
    switch (peek().kind) {
      case TokenKind::Pragma:
        mod.pragmas.push_back(parse_pragma());
        return;
      case TokenKind::KwImport:
        mod.imports.push_back(parse_import_decl());
        return;
      case TokenKind::KwToken:
        mod.tokens.push_back(parse_token_decl());
        return;
      case TokenKind::KwSkip:
        mod.skips.push_back(parse_skip_decl());
        return;
      case TokenKind::KwKeywords:
        mod.keywords.push_back(parse_keywords_decl());
        return;
      case TokenKind::KwMode:
        mod.modes.push_back(parse_mode_decl());
        return;
      case TokenKind::KwData:
        mod.data_decls.push_back(parse_data_decl(false, false));
        return;
      case TokenKind::KwNewtype:
        mod.data_decls.push_back(parse_data_decl(true, false));
        return;
      case TokenKind::KwForeign:
        mod.data_decls.push_back(parse_foreign_decl());
        return;
      case TokenKind::KwExample:
        mod.examples.push_back(parse_example_decl());
        return;
      case TokenKind::LowerIdent:
        if (peek(1).kind == TokenKind::ColonColon) {
          mod.rule_sigs.push_back(parse_rule_sig());
        } else {
          mod.rule_defs.push_back(parse_rule_def());
        }
        return;
      default:
        error_here("unexpected token at top level");
        sync_to_top_decl();
        return;
    }
  }

  ImportDecl parse_import_decl() {
    const Token kw = advance(); // import
    ImportDecl imp;
    imp.is_qualified = match(TokenKind::KwQualified);
    imp.module_name = parse_qualified_upper("expected module name after `import`");
    if (match(TokenKind::KwAs)) {
      imp.alias = parse_qualified_upper("expected alias name after `as`");
    }
    imp.is_hiding = match(TokenKind::KwHiding);
    if (match(TokenKind::LParen)) {
      if (!check(TokenKind::RParen)) {
        while (true) {
          ImportItem item;
          const Token start_tok = peek();
          item.name = parse_ident_name("expected imported symbol name");
          item.span = start_tok.span;
          if (match(TokenKind::LParen)) {
            if (check(TokenKind::Dot) && peek(1).kind == TokenKind::Dot) {
              advance();
              advance();
              item.all_ctors = true;
            } else if (!check(TokenKind::RParen)) {
              while (true) {
                item.subitems.push_back(parse_ident_name("expected constructor name"));
                if (!match(TokenKind::Comma)) {
                  break;
                }
              }
            }
            expect(TokenKind::RParen, "expected `)` in import item");
            item.span = Span::merge(start_tok.span, previous().span);
          }
          imp.items.push_back(std::move(item));
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      expect(TokenKind::RParen, "expected `)` to close import list");
    }
    imp.span = Span::merge(kw.span, previous().span);
    return imp;
  }

  TokenDecl parse_token_decl() {
    const Token kw = advance(); // token
    TokenDecl decl;
    decl.name = parse_ident_name("expected token name");
    expect(TokenKind::Equals, "expected `=` after token name");

    if (check(TokenKind::RegexLit)) {
      decl.is_regex = true;
      decl.pattern = advance().value;
    } else if (check(TokenKind::StringLit)) {
      decl.is_regex = false;
      decl.pattern = advance().value;
    } else {
      error_here("expected regex `/.../` or string `\"...\"` pattern for token");
    }

    if (match(TokenKind::ColonColon)) {
      decl.type_ann = parse_type_expr();
    }

    if (match(TokenKind::Arrow)) {
      const Token arrow_tok = previous();
      if (match(TokenKind::KwPush)) {
        decl.action.kind = LexActionKind::Push;
        decl.action.target_mode = parse_ident_name("expected mode name after `push`");
        decl.action.span = Span::merge(arrow_tok.span, previous().span);
      } else if (match(TokenKind::KwPop)) {
        decl.action.kind = LexActionKind::Pop;
        decl.action.span = Span::merge(arrow_tok.span, previous().span);
      } else {
        error_here("expected `push <mode>` or `pop` after `->`");
      }
    }

    decl.span = Span::merge(kw.span, previous().span);
    return decl;
  }

  SkipDecl parse_skip_decl() {
    const Token kw = advance(); // skip
    SkipDecl decl;
    decl.name = parse_ident_name("expected skip token name");
    expect(TokenKind::Equals, "expected `=` after skip name");
    if (check(TokenKind::RegexLit)) {
      decl.is_regex = true;
      decl.pattern = advance().value;
    } else if (check(TokenKind::StringLit)) {
      decl.is_regex = false;
      decl.pattern = advance().value;
    } else {
      error_here("expected regex `/.../` or string `\"...\"` pattern for `skip`");
    }
    decl.span = Span::merge(kw.span, previous().span);
    return decl;
  }

  KeywordsDecl parse_keywords_decl() {
    const Token kw = advance(); // keywords
    KeywordsDecl decl;
    expect(TokenKind::Equals, "expected `=` after `keywords`");
    expect(TokenKind::LBracket, "expected `[` after `keywords =`");
    if (!check(TokenKind::RBracket)) {
      while (true) {
        if (check(TokenKind::StringLit)) {
          decl.words.push_back(advance().value);
        } else {
          error_here("expected keyword string literal");
          if (!check(TokenKind::Comma) && !check(TokenKind::RBracket)) {
            advance();
          }
        }
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
    }
    expect(TokenKind::RBracket, "expected `]` to close keywords list");
    decl.span = Span::merge(kw.span, previous().span);
    return decl;
  }

  ModeDecl parse_mode_decl() {
    const Token kw = advance(); // mode
    ModeDecl mode;
    mode.name = parse_ident_name("expected lexer mode name");
    expect(TokenKind::KwWhere, "expected `where` after mode name");

    const bool has_brace = match(TokenKind::VBraceOpen) || match(TokenKind::LBrace);
    while (!at_end() && !check(TokenKind::VBraceClose) && !check(TokenKind::RBrace)) {
      while (match(TokenKind::VSemi)) {
      }
      if (at_end() || check(TokenKind::VBraceClose) || check(TokenKind::RBrace)) {
        break;
      }
      if (check(TokenKind::KwToken)) {
        mode.tokens.push_back(parse_token_decl());
      } else if (check(TokenKind::KwSkip)) {
        mode.skips.push_back(parse_skip_decl());
      } else if (check(TokenKind::KwKeywords)) {
        mode.keywords.push_back(parse_keywords_decl());
      } else {
        error_here("expected `token`, `skip`, or `keywords` inside `mode` block");
        sync_to_stmt();
      }
      if (!match(TokenKind::VSemi)) {
        if (!check(TokenKind::VBraceClose) && !check(TokenKind::RBrace) && !at_end()) {
          error_here("unexpected token in `mode` block");
          sync_to_stmt();
        }
      }
    }

    if (has_brace) {
      if (!match(TokenKind::VBraceClose) && !match(TokenKind::RBrace) && !at_end()) {
        error_here("expected end of `mode` block");
      }
    }
    mode.span = Span::merge(kw.span, previous().span);
    return mode;
  }

  DataDecl parse_foreign_decl() {
    const Token kw = advance(); // foreign
    expect(TokenKind::KwData, "expected `data` after `foreign`");
    DataDecl decl;
    decl.is_foreign = true;
    if (check(TokenKind::UpperIdent)) {
      decl.name = advance().value;
    } else {
      error_here("expected type name after `foreign data`");
      decl.name = "<error>";
    }
    while (check(TokenKind::LowerIdent)) {
      decl.type_params.push_back(advance().value);
    }
    expect(TokenKind::Equals, "expected `=` in `foreign data` declaration");
    if (check(TokenKind::StringLit)) {
      decl.foreign_target = advance().value;
    } else {
      error_here("expected target type string literal after `=`");
    }
    decl.span = Span::merge(kw.span, previous().span);
    return decl;
  }

  DataDecl parse_data_decl(bool is_newtype, bool is_foreign) {
    const Token kw = advance(); // data or newtype
    DataDecl decl;
    decl.is_newtype = is_newtype;
    decl.is_foreign = is_foreign;

    if (check(TokenKind::UpperIdent)) {
      decl.name = advance().value;
    } else {
      error_here("expected data type name");
      decl.name = "<error>";
    }

    while (check(TokenKind::LowerIdent)) {
      decl.type_params.push_back(advance().value);
    }

    if (match(TokenKind::Equals)) {
      while (true) {
        decl.constructors.push_back(parse_ctor_decl());
        if (is_newtype || !match(TokenKind::Pipe)) {
          break;
        }
      }
    }

    decl.span = Span::merge(kw.span, previous().span);
    return decl;
  }

  CtorDecl parse_ctor_decl() {
    CtorDecl ctor;
    const Token first = peek();
    if (check(TokenKind::UpperIdent)) {
      ctor.name = advance().value;
      ctor.span = first.span;
    } else {
      error_here("expected constructor name");
      ctor.name = "<error>";
      ctor.span = first.span;
      return ctor;
    }

    if (match(TokenKind::LBrace)) {
      ctor.is_record = true;
      if (!check(TokenKind::RBrace)) {
        while (true) {
          const Token fstart = peek();
          FieldDecl field;
          field.name = parse_ident_name("expected record field name");
          expect(TokenKind::ColonColon, "expected `::` after record field name");
          field.type = parse_type_expr();
          field.span = Span::merge(fstart.span, previous().span);
          ctor.record_fields.push_back(std::move(field));
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      expect(TokenKind::RBrace, "expected `}` to close record constructor");
    } else {
      while (can_start_type_atom()) {
        ctor.positional_fields.push_back(parse_type_atom());
      }
    }

    ctor.span = Span::merge(first.span, previous().span);
    return ctor;
  }

  bool can_start_type_atom() const noexcept {
    switch (peek().kind) {
      case TokenKind::UpperIdent:
      case TokenKind::LowerIdent:
      case TokenKind::LBracket:
      case TokenKind::LParen:
        return true;
      default:
        return false;
    }
  }

  TypeExpr parse_type_expr() {
    TypeExpr lhs = parse_type_app();
    if (match(TokenKind::Arrow)) {
      TypeExpr rhs = parse_type_expr();
      TypeExpr arrow;
      arrow.kind = TypeExpr::Kind::Arrow;
      arrow.span = Span::merge(lhs.span, rhs.span);
      arrow.left = std::make_shared<TypeExpr>(std::move(lhs));
      arrow.right = std::make_shared<TypeExpr>(std::move(rhs));
      return arrow;
    }
    return lhs;
  }

  TypeExpr parse_type_app() {
    TypeExpr head = parse_type_atom();
    std::vector<TypeExprPtr> args;
    while (can_start_type_atom()) {
      args.push_back(std::make_shared<TypeExpr>(parse_type_atom()));
    }
    if (args.empty()) {
      return head;
    }
    TypeExpr app;
    app.kind = TypeExpr::Kind::App;
    app.span = Span::merge(head.span, args.back()->span);
    app.left = std::make_shared<TypeExpr>(std::move(head));
    app.args = std::move(args);
    return app;
  }

  TypeExpr parse_type_atom() {
    if (check(TokenKind::UpperIdent)) {
      const Token first = advance();
      std::string name = first.value;
      while (check(TokenKind::Dot) && peek(1).kind == TokenKind::UpperIdent) {
        advance();
        name.push_back('.');
        name.append(advance().value);
      }
      TypeExpr t;
      t.kind = TypeExpr::Kind::Named;
      t.name = std::move(name);
      t.span = Span::merge(first.span, previous().span);
      return t;
    }
    if (check(TokenKind::LowerIdent)) {
      const Token tok = advance();
      TypeExpr t;
      t.kind = TypeExpr::Kind::Var;
      t.name = tok.value;
      t.span = tok.span;
      return t;
    }
    if (match(TokenKind::LBracket)) {
      const Token open_tok = previous();
      TypeExpr elem = parse_type_expr();
      expect(TokenKind::RBracket, "expected `]` in list type");
      TypeExpr t;
      t.kind = TypeExpr::Kind::List;
      t.left = std::make_shared<TypeExpr>(std::move(elem));
      t.span = Span::merge(open_tok.span, previous().span);
      return t;
    }
    if (match(TokenKind::LParen)) {
      const Token open_tok = previous();
      if (match(TokenKind::RParen)) {
        TypeExpr t;
        t.kind = TypeExpr::Kind::Tuple;
        t.name = "Unit";
        t.span = Span::merge(open_tok.span, previous().span);
        return t;
      }
      TypeExpr first = parse_type_expr();
      if (match(TokenKind::Comma)) {
        std::vector<TypeExprPtr> elems;
        elems.push_back(std::make_shared<TypeExpr>(std::move(first)));
        do {
          elems.push_back(std::make_shared<TypeExpr>(parse_type_expr()));
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected `)` in tuple type");
        TypeExpr t;
        t.kind = TypeExpr::Kind::Tuple;
        t.args = std::move(elems);
        t.span = Span::merge(open_tok.span, previous().span);
        return t;
      }
      expect(TokenKind::RParen, "expected `)` in parenthesized type");
      first.span = Span::merge(open_tok.span, previous().span);
      return first;
    }

    error_here("expected type expression");
    TypeExpr err;
    err.kind = TypeExpr::Kind::Named;
    err.name = "<error>";
    err.span = peek().span;
    return err;
  }

  RuleSig parse_rule_sig() {
    const Token name_tok = advance(); // LowerIdent
    expect(TokenKind::ColonColon, "expected `::` in rule signature");
    TypeExpr type = parse_type_expr();
    RuleSig sig;
    sig.name = name_tok.value;
    sig.span = Span::merge(name_tok.span, type.span);
    sig.type = std::move(type);
    return sig;
  }

  RuleDef parse_rule_def() {
    const Token name_tok = advance(); // LowerIdent
    RuleDef def;
    def.name = name_tok.value;
    while (check(TokenKind::LowerIdent)) {
      def.params.push_back(advance().value);
    }
    expect(TokenKind::Equals, "expected `=` in rule definition");
    def.body = parse_rule_expr();
    def.span = Span::merge(name_tok.span, def.body ? def.body->span : previous().span);
    return def;
  }

  RuleExprPtr make_expr(RuleExpr::Kind kind, const Span& sp) {
    auto node = std::make_shared<RuleExpr>();
    node->kind = kind;
    node->span = sp;
    return node;
  }

  RuleExprPtr parse_rule_expr() { return parse_alt_expr(); }

  // `|` and `<|>` (lowest precedence)
  RuleExprPtr parse_alt_expr() {
    RuleExprPtr first = parse_label_expr();
    if (!check(TokenKind::Pipe) && !check(TokenKind::OpAlt)) {
      return first;
    }
    auto alt = make_expr(RuleExpr::Kind::Alt, first->span);
    alt->items.push_back(first);
    while (match(TokenKind::Pipe) || match(TokenKind::OpAlt)) {
      RuleExprPtr next = parse_label_expr();
      alt->span = Span::merge(alt->span, next->span);
      alt->items.push_back(std::move(next));
    }
    return alt;
  }

  // `<?>`
  RuleExprPtr parse_label_expr() {
    RuleExprPtr lhs = parse_backtick_expr();
    while (match(TokenKind::OpLabel)) {
      RuleExprPtr rhs = parse_backtick_expr();
      auto node = make_expr(RuleExpr::Kind::InfixOp, Span::merge(lhs->span, rhs->span));
      node->op_kind = InfixOpKind::Label;
      node->text = "<?>";
      node->lhs = std::move(lhs);
      node->rhs = std::move(rhs);
      lhs = std::move(node);
    }
    return lhs;
  }

  // `` `ident` ``
  RuleExprPtr parse_backtick_expr() {
    RuleExprPtr lhs = parse_seq_expr();
    while (check(TokenKind::BacktickIdent)) {
      const Token op_tok = advance();
      RuleExprPtr rhs = parse_seq_expr();
      auto node = make_expr(RuleExpr::Kind::InfixOp, Span::merge(lhs->span, rhs->span));
      node->op_kind = InfixOpKind::Backtick;
      node->text = op_tok.value;
      node->lhs = std::move(lhs);
      node->rhs = std::move(rhs);
      lhs = std::move(node);
    }
    return lhs;
  }

  // `<*>`, `*>`, `<*`
  RuleExprPtr parse_seq_expr() {
    RuleExprPtr lhs = parse_fmap_expr();
    while (check(TokenKind::OpAp) || check(TokenKind::OpSeqR) || check(TokenKind::OpSeqL)) {
      const Token op_tok = advance();
      RuleExprPtr rhs = parse_fmap_expr();
      auto node = make_expr(RuleExpr::Kind::InfixOp, Span::merge(lhs->span, rhs->span));
      if (op_tok.kind == TokenKind::OpAp) {
        node->op_kind = InfixOpKind::Ap;
        node->text = "<*>";
      } else if (op_tok.kind == TokenKind::OpSeqR) {
        node->op_kind = InfixOpKind::SeqR;
        node->text = "*>";
      } else {
        node->op_kind = InfixOpKind::SeqL;
        node->text = "<*";
      }
      node->lhs = std::move(lhs);
      node->rhs = std::move(rhs);
      lhs = std::move(node);
    }
    return lhs;
  }

  // `<$>`
  RuleExprPtr parse_fmap_expr() {
    RuleExprPtr lhs = parse_app_expr();
    while (match(TokenKind::OpFmap)) {
      RuleExprPtr rhs = parse_app_expr();
      auto node = make_expr(RuleExpr::Kind::InfixOp, Span::merge(lhs->span, rhs->span));
      node->op_kind = InfixOpKind::Fmap;
      node->text = "<$>";
      node->lhs = std::move(lhs);
      node->rhs = std::move(rhs);
      lhs = std::move(node);
    }
    return lhs;
  }

  bool can_start_atom() const noexcept {
    switch (peek().kind) {
      case TokenKind::LowerIdent:
      case TokenKind::UpperIdent:
      case TokenKind::StringLit:
      case TokenKind::IntLit:
      case TokenKind::FloatLit:
      case TokenKind::LParen:
      case TokenKind::LBracket:
      case TokenKind::KwPure:
      case TokenKind::KwDo:
      case TokenKind::KwContext:
      case TokenKind::KwOperators:
        return true;
      default:
        return false;
    }
  }

  RuleExprPtr parse_app_expr() {
    RuleExprPtr func = parse_primary();
    while (can_start_atom()) {
      RuleExprPtr arg = parse_primary();
      auto app = make_expr(RuleExpr::Kind::App, Span::merge(func->span, arg->span));
      app->lhs = std::move(func);
      app->rhs = std::move(arg);
      func = std::move(app);
    }
    return func;
  }

  RuleExprPtr parse_primary() {
    if (check(TokenKind::KwOperators)) {
      return parse_operators_expr();
    }
    if (check(TokenKind::KwContext)) {
      const Token ctx_kw = advance();
      if (check(TokenKind::KwDo)) {
        RuleExprPtr block = parse_do_expr(true);
        block->span = Span::merge(ctx_kw.span, block->span);
        return block;
      }
      error_here("expected `do` after `context`");
      return make_expr(RuleExpr::Kind::Var, ctx_kw.span);
    }
    if (check(TokenKind::KwDo)) {
      return parse_do_expr(false);
    }
    if (check(TokenKind::KwPure)) {
      const Token tok = advance();
      auto node = make_expr(RuleExpr::Kind::Var, tok.span);
      node->text = "pure";
      return node;
    }
    if (check(TokenKind::LowerIdent)) {
      const Token tok = advance();
      auto node = make_expr(RuleExpr::Kind::Var, tok.span);
      node->text = tok.value;
      node->is_upper = false;
      return node;
    }
    if (check(TokenKind::UpperIdent)) {
      const Token first = advance();
      std::string name = first.value;
      bool qualified_lower = false;
      while (check(TokenKind::Dot) &&
             (peek(1).kind == TokenKind::UpperIdent || peek(1).kind == TokenKind::LowerIdent)) {
        advance(); // .
        const Token next = advance();
        name.push_back('.');
        name.append(next.value);
        qualified_lower = (next.kind == TokenKind::LowerIdent);
        if (qualified_lower) {
          break;
        }
      }
      auto node = make_expr(RuleExpr::Kind::Var, Span::merge(first.span, previous().span));
      node->text = std::move(name);
      node->is_upper = !qualified_lower;
      return node;
    }
    if (check(TokenKind::StringLit)) {
      const Token tok = advance();
      auto node = make_expr(RuleExpr::Kind::Lit, tok.span);
      node->text = tok.value;
      return node;
    }
    if (check(TokenKind::IntLit)) {
      const Token tok = advance();
      auto node = make_expr(RuleExpr::Kind::IntLit, tok.span);
      node->text = tok.lexeme;
      node->number_value = parse_double(tok.lexeme);
      return node;
    }
    if (check(TokenKind::FloatLit)) {
      const Token tok = advance();
      auto node = make_expr(RuleExpr::Kind::FloatLit, tok.span);
      node->text = tok.lexeme;
      node->number_value = parse_double(tok.lexeme);
      return node;
    }
    if (match(TokenKind::LParen)) {
      const Token open_tok = previous();
      if (match(TokenKind::RParen)) {
        auto unit = make_expr(RuleExpr::Kind::Tuple, Span::merge(open_tok.span, previous().span));
        return unit;
      }
      RuleExprPtr first = parse_rule_expr();
      if (match(TokenKind::Comma)) {
        auto tup = make_expr(RuleExpr::Kind::Tuple, open_tok.span);
        tup->items.push_back(std::move(first));
        do {
          tup->items.push_back(parse_rule_expr());
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected `)` to close tuple expression");
        tup->span = Span::merge(open_tok.span, previous().span);
        return tup;
      }
      expect(TokenKind::RParen, "expected `)`");
      auto paren = make_expr(RuleExpr::Kind::Paren, Span::merge(open_tok.span, previous().span));
      paren->lhs = std::move(first);
      return paren;
    }
    if (match(TokenKind::LBracket)) {
      const Token open_tok = previous();
      auto list = make_expr(RuleExpr::Kind::List, open_tok.span);
      if (!check(TokenKind::RBracket)) {
        while (true) {
          list->items.push_back(parse_rule_expr());
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      expect(TokenKind::RBracket, "expected `]` to close list expression");
      list->span = Span::merge(open_tok.span, previous().span);
      return list;
    }

    error_here("expected rule expression");
    const Token err_tok = peek();
    if (!at_end() && !check(TokenKind::VSemi) && !check(TokenKind::VBraceClose) &&
        !check(TokenKind::RParen) && !check(TokenKind::RBracket)) {
      advance();
    }
    auto err = make_expr(RuleExpr::Kind::Var, err_tok.span);
    err->text = "<error>";
    return err;
  }

  RuleExprPtr parse_do_expr(bool is_context) {
    const Token do_kw = advance(); // do
    auto node = make_expr(RuleExpr::Kind::Do, do_kw.span);
    node->is_context = is_context;

    const bool has_brace = match(TokenKind::VBraceOpen) || match(TokenKind::LBrace);
    while (!at_end() && !check(TokenKind::VBraceClose) && !check(TokenKind::RBrace)) {
      while (match(TokenKind::VSemi)) {
      }
      if (at_end() || check(TokenKind::VBraceClose) || check(TokenKind::RBrace)) {
        break;
      }

      DoStmt stmt;
      const Token stmt_start = peek();
      if (check(TokenKind::LowerIdent) && peek(1).kind == TokenKind::LArrow) {
        stmt.kind = DoStmt::Kind::Bind;
        stmt.binder = advance().value;
        advance(); // <-
        stmt.expr = parse_rule_expr();
      } else if (match(TokenKind::KwPure)) {
        stmt.kind = DoStmt::Kind::Pure;
        stmt.expr = parse_rule_expr();
      } else {
        stmt.kind = DoStmt::Kind::Expr;
        stmt.expr = parse_rule_expr();
      }
      stmt.span = Span::merge(stmt_start.span, stmt.expr ? stmt.expr->span : previous().span);
      node->do_stmts.push_back(std::move(stmt));

      if (!match(TokenKind::VSemi)) {
        if (!check(TokenKind::VBraceClose) && !check(TokenKind::RBrace) && !at_end()) {
          error_here("unexpected token in `do` block");
          sync_to_stmt();
        }
      }
    }

    if (has_brace) {
      if (!match(TokenKind::VBraceClose) && !match(TokenKind::RBrace) && !at_end()) {
        error_here("expected end of `do` block");
      }
    }

    node->span = Span::merge(do_kw.span, previous().span);
    return node;
  }

  RuleExprPtr parse_operators_expr() {
    const Token op_kw = advance(); // operators
    auto node = make_expr(RuleExpr::Kind::Operators, op_kw.span);
    node->lhs = parse_primary();

    expect(TokenKind::LBracket, "expected `[` with operator precedence levels after base rule");
    if (!check(TokenKind::RBracket)) {
      while (!at_end() && !check(TokenKind::RBracket)) {
        node->op_levels.push_back(parse_operator_level());
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
    }
    expect(TokenKind::RBracket, "expected `]` to close `operators` table");
    node->span = Span::merge(op_kw.span, previous().span);
    return node;
  }

  OperatorLevel parse_operator_level() {
    OperatorLevel lvl;
    const Token assoc_tok = peek();
    lvl.span = assoc_tok.span;

    if (match(TokenKind::KwInfixl)) {
      lvl.assoc = AssocKind::InfixL;
    } else if (match(TokenKind::KwInfixr)) {
      lvl.assoc = AssocKind::InfixR;
    } else if (match(TokenKind::KwInfix)) {
      lvl.assoc = AssocKind::Infix;
    } else if (match(TokenKind::KwPrefix)) {
      lvl.assoc = AssocKind::Prefix;
    } else if (match(TokenKind::KwPostfix)) {
      lvl.assoc = AssocKind::Postfix;
    } else if (match(TokenKind::KwMixfix)) {
      lvl.assoc = AssocKind::Mixfix;
    } else {
      error_here("expected operator associativity (`infixl`, `infixr`, `infix`, `prefix`, `postfix`, `mixfix`)");
      if (!at_end() && !check(TokenKind::RBracket) && !check(TokenKind::Comma)) {
        advance();
      }
    }

    if (check(TokenKind::IntLit)) {
      lvl.precedence = parse_int(advance().lexeme);
    } else {
      error_here("expected integer precedence level");
    }

    expect(TokenKind::LBracket, "expected `[` for operator specs");
    if (!check(TokenKind::RBracket)) {
      while (!at_end() && !check(TokenKind::RBracket)) {
        lvl.specs.push_back(parse_operator_spec());
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
    }
    expect(TokenKind::RBracket, "expected `]` to close operator specs");
    lvl.span = Span::merge(assoc_tok.span, previous().span);
    return lvl;
  }

  OperatorSpec parse_operator_spec() {
    OperatorSpec spec;
    const Token start_tok = peek();
    while (!at_end() && !check(TokenKind::FatArrow) &&
           !check(TokenKind::Comma) && !check(TokenKind::RBracket)) {
      if (!can_start_atom()) {
        error_here("unexpected token in operator pattern");
        advance();
        break;
      }
      spec.pattern.push_back(parse_primary());
    }
    if (match(TokenKind::FatArrow)) {
      spec.action = parse_rule_expr();
    }
    spec.span = Span::merge(start_tok.span, previous().span);
    return spec;
  }

  ExampleDecl parse_example_decl() {
    const Token kw = advance(); // example
    ExampleDecl ex;
    if (check(TokenKind::StringLit)) {
      ex.name = advance().value;
    } else {
      error_here("expected example name string literal");
    }
    expect(TokenKind::Colon, "expected `:` after example name");
    if (check(TokenKind::StringLit)) {
      ex.input = advance().value;
    } else {
      error_here("expected example input string literal");
    }

    if (match(TokenKind::KwParsesTo)) {
      ex.kind = ExampleDecl::Kind::ParsesTo;
      ex.expected_ast = parse_ast_pattern();
    } else if (match(TokenKind::KwFailsAt)) {
      ex.kind = ExampleDecl::Kind::FailsAt;
      expect(TokenKind::LParen, "expected `(` after `fails-at`");
      if (check(TokenKind::IntLit)) {
        ex.fail_line = parse_int(advance().lexeme);
      } else {
        error_here("expected line number in `fails-at`");
      }
      expect(TokenKind::Comma, "expected `,` between line and column in `fails-at`");
      if (check(TokenKind::IntLit)) {
        ex.fail_col = parse_int(advance().lexeme);
      } else {
        error_here("expected column number in `fails-at`");
      }
      expect(TokenKind::RParen, "expected `)` after `(line, col)`");
      if (match(TokenKind::KwExpecting)) {
        if (check(TokenKind::StringLit)) {
          ex.expecting = advance().value;
        } else {
          error_here("expected string literal after `expecting`");
        }
      }
    } else {
      error_here("expected `parses-to` or `fails-at` in `example` declaration");
    }

    ex.span = Span::merge(kw.span, previous().span);
    return ex;
  }

  AstPattern parse_ast_pattern() {
    if (check(TokenKind::UpperIdent)) {
      const Token ctor = advance();
      AstPattern pat;
      pat.kind = AstPattern::Kind::Ctor;
      pat.name = ctor.value;
      pat.span = ctor.span;
      while (can_start_ast_pattern_atom()) {
        pat.args.push_back(parse_ast_pattern_atom());
        pat.span = Span::merge(pat.span, pat.args.back().span);
      }
      return pat;
    }
    return parse_ast_pattern_atom();
  }

  bool can_start_ast_pattern_atom() const noexcept {
    switch (peek().kind) {
      case TokenKind::UpperIdent:
      case TokenKind::LowerIdent:
      case TokenKind::IntLit:
      case TokenKind::FloatLit:
      case TokenKind::StringLit:
      case TokenKind::LParen:
      case TokenKind::LBracket:
        return true;
      default:
        return false;
    }
  }

  AstPattern parse_ast_pattern_atom() {
    if (check(TokenKind::UpperIdent)) {
      const Token tok = advance();
      AstPattern pat;
      pat.kind = AstPattern::Kind::Ctor;
      pat.name = tok.value;
      pat.span = tok.span;
      return pat;
    }
    if (check(TokenKind::LowerIdent)) {
      const Token tok = advance();
      AstPattern pat;
      pat.kind = (tok.value == "_") ? AstPattern::Kind::Wildcard : AstPattern::Kind::Ident;
      pat.name = tok.value;
      pat.span = tok.span;
      return pat;
    }
    if (check(TokenKind::IntLit) || check(TokenKind::FloatLit)) {
      const Token tok = advance();
      AstPattern pat;
      pat.kind = AstPattern::Kind::Number;
      pat.name = tok.lexeme;
      pat.number_value = parse_double(tok.lexeme);
      pat.span = tok.span;
      return pat;
    }
    if (check(TokenKind::StringLit)) {
      const Token tok = advance();
      AstPattern pat;
      pat.kind = AstPattern::Kind::String;
      pat.string_value = tok.value;
      pat.span = tok.span;
      return pat;
    }
    if (match(TokenKind::LBracket)) {
      const Token open_tok = previous();
      AstPattern pat;
      pat.kind = AstPattern::Kind::List;
      if (!check(TokenKind::RBracket)) {
        while (true) {
          pat.args.push_back(parse_ast_pattern());
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      expect(TokenKind::RBracket, "expected `]` in AST list pattern");
      pat.span = Span::merge(open_tok.span, previous().span);
      return pat;
    }
    if (match(TokenKind::LParen)) {
      const Token open_tok = previous();
      AstPattern inner = parse_ast_pattern();
      expect(TokenKind::RParen, "expected `)` in AST pattern");
      inner.span = Span::merge(open_tok.span, previous().span);
      return inner;
    }

    error_here("expected AST pattern");
    AstPattern err;
    err.kind = AstPattern::Kind::Wildcard;
    err.name = "_";
    err.span = peek().span;
    return err;
  }

  std::vector<Token> tokens_;
  std::vector<CommentTrivia> comments_;
  std::string_view filename_;
  DiagnosticBag& diags_;
  std::size_t pos_ = 0;
};

} // namespace parsley::parser::detail

export namespace parsley::parser {

inline ast::Module parse_tokens(const lexer::LexResult& lexed,
                                std::string_view filename,
                                DiagnosticBag& diags) {
  detail::Parser p(lexed.tokens, lexed.comments, filename, diags);
  return p.parse_module();
}

inline ast::Module parse_module(std::string_view source,
                                std::string_view filename,
                                DiagnosticBag& diags) {
  lexer::LexResult lexed = lexer::tokenize(source, filename, diags);
  return parse_tokens(lexed, filename, diags);
}

} // namespace parsley::parser
