module;

#include <memory>
#include <optional>
#include <string>
#include <vector>

export module parsley.ast;

import parsley.diagnostic;

export namespace parsley::ast {

struct Comment {
  Span span;
  std::string text;
  bool is_block = false;
};

struct TypeExpr {
  enum class Kind {
    Named,
    Var,
    App,
    List,
    Tuple,
    Arrow,
  };

  Kind kind = Kind::Named;
  std::string name;
  std::shared_ptr<TypeExpr> left;
  std::shared_ptr<TypeExpr> right;
  std::vector<std::shared_ptr<TypeExpr>> args;
  Span span;
};

using TypeExprPtr = std::shared_ptr<TypeExpr>;

struct PragmaDecl {
  std::string name;
  std::vector<std::string> args;
  std::string raw_body;
  Span span;
};

struct ImportItem {
  std::string name;
  bool all_ctors = false;
  std::vector<std::string> subitems;
  Span span;
};

struct ImportDecl {
  std::string module_name;
  bool is_qualified = false;
  std::optional<std::string> alias;
  bool is_hiding = false;
  std::vector<ImportItem> items;
  Span span;
};

enum class LexActionKind { None, Push, Pop };

struct LexAction {
  LexActionKind kind = LexActionKind::None;
  std::string target_mode;
  Span span;
};

struct TokenDecl {
  std::string name;
  std::string pattern;
  bool is_regex = true;
  std::optional<TypeExpr> type_ann;
  LexAction action;
  Span span;
};

struct SkipDecl {
  std::string name;
  std::string pattern;
  bool is_regex = true;
  Span span;
};

struct KeywordsDecl {
  std::vector<std::string> words;
  Span span;
};

struct ModeDecl {
  std::string name;
  std::vector<TokenDecl> tokens;
  std::vector<SkipDecl> skips;
  std::vector<KeywordsDecl> keywords;
  Span span;
};

struct FieldDecl {
  std::string name;
  TypeExpr type;
  Span span;
};

struct CtorDecl {
  std::string name;
  std::vector<TypeExpr> positional_fields;
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

struct RuleExpr;
using RuleExprPtr = std::shared_ptr<RuleExpr>;

enum class AssocKind {
  InfixL,
  InfixR,
  Infix,
  Prefix,
  Postfix,
  Mixfix,
};

struct OperatorSpec {
  std::vector<RuleExprPtr> pattern;
  RuleExprPtr action;
  Span span;
};

struct OperatorLevel {
  AssocKind assoc = AssocKind::InfixL;
  int precedence = 0;
  std::vector<OperatorSpec> specs;
  Span span;
};

struct DoStmt {
  enum class Kind {
    Bind,
    Expr,
    Pure,
  };

  Kind kind = Kind::Expr;
  std::string binder;
  RuleExprPtr expr;
  Span span;
};

enum class InfixOpKind {
  Fmap,     // <$>
  Ap,       // <*>
  SeqR,     // *>
  SeqL,     // <*
  Alt,      // <|> or |
  Label,    // <?>
  Backtick, // `ident`
};

struct RuleExpr {
  enum class Kind {
    Lit,
    IntLit,
    FloatLit,
    Var,
    App,
    InfixOp,
    Alt,
    List,
    Tuple,
    Paren,
    Do,
    Operators,
    Pure,
  };

  Kind kind = Kind::Var;
  std::string text;
  double number_value = 0.0;
  InfixOpKind op_kind = InfixOpKind::Ap;
  bool is_context = false;
  bool is_upper = false;
  RuleExprPtr lhs;
  RuleExprPtr rhs;
  std::vector<RuleExprPtr> items;
  std::vector<DoStmt> do_stmts;
  std::vector<OperatorLevel> op_levels;
  Span span;
};

struct RuleSig {
  std::string name;
  TypeExpr type;
  Span span;
};

struct RuleDef {
  std::string name;
  std::vector<std::string> params;
  RuleExprPtr body;
  std::optional<TypeExpr> signature;
  Span span;
};

struct AstPattern {
  enum class Kind {
    Ctor,
    Number,
    String,
    List,
    Wildcard,
    Ident,
  };

  Kind kind = Kind::Ctor;
  std::string name;
  double number_value = 0.0;
  std::string string_value;
  std::vector<AstPattern> args;
  Span span;
};

struct ExampleDecl {
  enum class Kind {
    ParsesTo,
    FailsAt,
  };

  std::string name;
  std::string input;
  Kind kind = Kind::ParsesTo;
  std::optional<AstPattern> expected_ast;
  int fail_line = 1;
  int fail_col = 1;
  std::string expecting;
  Span span;
};

struct ExportItem {
  std::string name;
  bool all_ctors = false;
  Span span;
};

struct Module {
  std::string name = "Main";
  bool has_module_header = false;
  bool has_export_list = false;
  std::vector<ExportItem> exports;
  std::vector<PragmaDecl> pragmas;
  std::vector<ImportDecl> imports;
  std::vector<TokenDecl> tokens;
  std::vector<SkipDecl> skips;
  std::vector<KeywordsDecl> keywords;
  std::vector<ModeDecl> modes;
  std::vector<DataDecl> data_decls;
  std::vector<RuleSig> rule_sigs;
  std::vector<RuleDef> rule_defs;
  std::vector<ExampleDecl> examples;
  std::vector<Comment> comments;
  Span span;
};

} // namespace parsley::ast
