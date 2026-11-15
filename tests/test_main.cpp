#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

import parsley.version;
import parsley.diagnostic;
import parsley.lexer;
import parsley.ast;
import parsley.parser;
import parsley.stdlib;
import parsley.resolve;
import parsley.typecheck;
import parsley.ir;
import parsley.desugar;
import parsley.analysis;

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "[FAIL] " << msg << "\n";
    ++g_failures;
  }
}

std::string read_fixture(std::string_view name) {
  std::string path = std::string(PARSLEY_FIXTURES_DIR) + "/" + std::string(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "[FAIL] Cannot open fixture: " << path << "\n";
    ++g_failures;
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void test_version() {
  check(parsley::VERSION_MAJOR == 0, "VERSION_MAJOR == 0");
  check(parsley::VERSION_MINOR == 5, "VERSION_MINOR == 5");
  check(parsley::VERSION_PATCH == 0, "VERSION_PATCH == 0");
  check(parsley::VERSION_STRING == "0.5.0", "VERSION_STRING == 0.5.0");
}

void test_diagnostics() {
  using namespace parsley;

  Span a{"Calc.parsley", 2, 5, 2, 10, 20, 25};
  Span b{"Calc.parsley", 2, 12, 2, 18, 27, 33};
  Span merged = Span::merge(a, b);
  check(merged.is_valid(), "merged span valid");
  check(merged.start_line == 2 && merged.start_col == 5, "merged start");
  check(merged.end_line == 2 && merged.end_col == 18, "merged end");

  DiagnosticBag bag;
  check(!bag.has_errors(), "empty bag has no errors");

  Diagnostic d;
  d.code = "E0301";
  d.severity = Severity::Error;
  d.message = "missing explicit type signature for exported rule `expr`";
  d.span = Span{"Calc.parsley", 2, 1, 2, 5, 10, 14};
  d.label = "exported rule defined without `::`";
  d.notes.push_back("all exported rules must declare a type signature");
  d.helps.push_back("add `expr :: Expr` above the rule");
  bag.push(d);
  bag.warning("W0001", "sample warning");

  check(bag.has_errors(), "bag has errors");
  check(bag.error_count() == 1, "bag error count == 1");
  check(bag.warning_count() == 1, "bag warning count == 1");

  const std::string src = "module Calc (expr) where\nexpr = number\n";
  const std::string rendered = render_diagnostic(bag.items().front(), src);
  check(rendered.find("error[E0301]") != std::string::npos, "rendered contains error[E0301]");
  check(rendered.find("Calc.parsley:2:1") != std::string::npos, "rendered contains location");
  check(rendered.find("expr = number") != std::string::npos, "rendered contains source line");

  const auto codes = known_error_codes();
  check(!codes.empty(), "known_error_codes non-empty");
  check(find_error_explanation("E0214").has_value(), "E0214 explanation exists");
  check(find_error_explanation("E0310").has_value(), "E0310 explanation exists");
  check(format_error_explanation("E0214").find("E0214") != std::string::npos, "format_error_explanation E0214");
}

void test_lexer_and_parser() {
  using namespace parsley;

  // 1. Calc.parsley
  const std::string calc_src = read_fixture("Calc.parsley");
  check(!calc_src.empty(), "Calc.parsley loaded");

  DiagnosticBag lex_diags;
  auto lexed = lexer::tokenize(calc_src, "Calc.parsley", lex_diags);
  check(!lex_diags.has_errors(), "Calc.parsley lexed without errors");
  check(!lexed.tokens.empty(), "Calc.parsley produced tokens");
  check(!lexed.comments.empty(), "Calc.parsley captured comments");

  DiagnosticBag parse_diags;
  auto calc_mod = parser::parse_module(calc_src, "Calc.parsley", parse_diags);
  check(!parse_diags.has_errors(), "Calc.parsley parsed without errors");
  check(calc_mod.name == "Calc", "Calc module name");
  check(calc_mod.has_module_header, "Calc has module header");
  check(calc_mod.exports.size() == 1 && calc_mod.exports[0].name == "program", "Calc exports program");
  check(calc_mod.imports.size() == 1 && calc_mod.imports[0].module_name == "Parsley.Prelude", "Calc imports Parsley.Prelude");
  check(calc_mod.tokens.size() == 2, "Calc has 2 token decls");
  check(calc_mod.skips.size() == 2, "Calc has 2 skip decls");
  check(calc_mod.keywords.size() == 1, "Calc has 1 keywords decl");
  check(calc_mod.data_decls.size() == 2, "Calc has 2 data decls (Op, Expr)");
  check(calc_mod.rule_defs.size() == 5, "Calc has 5 rules (program, expr, atom, letExpr, parens)");
  check(calc_mod.examples.size() == 2, "Calc has 2 examples");

  // 2. LexModes.parsley
  const std::string modes_src = read_fixture("LexModes.parsley");
  DiagnosticBag modes_diags;
  auto modes_mod = parser::parse_module(modes_src, "LexModes.parsley", modes_diags);
  check(!modes_diags.has_errors(), "LexModes.parsley parsed without errors");
  check(modes_mod.name == "LexModes", "LexModes module name");
  check(modes_mod.modes.size() == 1 && modes_mod.modes[0].name == "string", "LexModes has mode string");
  check(modes_mod.modes[0].tokens.size() == 3, "LexModes string mode has 3 tokens");

  // 3. HtmlContext.parsley
  const std::string html_src = read_fixture("HtmlContext.parsley");
  DiagnosticBag html_diags;
  auto html_mod = parser::parse_module(html_src, "HtmlContext.parsley", html_diags);
  check(!html_diags.has_errors(), "HtmlContext.parsley parsed without errors");
  check(html_mod.name == "HtmlContext", "HtmlContext module name");
  bool found_context_do = false;
  for (const auto& r : html_mod.rule_defs) {
    if (r.name == "htmlTag" && r.body && r.body->kind == ast::RuleExpr::Kind::Do && r.body->is_context) {
      found_context_do = true;
    }
  }
  check(found_context_do, "HtmlContext htmlTag uses context do");

  // 4. Error recovery test
  const std::string bad_src =
      "module Bad where\n"
      "data T = \n"
      "good :: Text\n"
      "good = \"ok\"\n";
  DiagnosticBag bad_diags;
  auto bad_mod = parser::parse_module(bad_src, "Bad.parsley", bad_diags);
  check(bad_diags.has_errors(), "Bad.parsley reports syntax error");
  check(!bad_mod.rule_defs.empty(), "Parser recovered to parse subsequent rule `good`");
}

bool has_diag_code(const parsley::DiagnosticBag& diags, std::string_view code) {
  for (const auto& d : diags.items()) {
    if (d.code == code) {
      return true;
    }
  }
  return false;
}

void test_stdlib_resolve_typecheck() {
  using namespace parsley;

  // Stdlib
  check(stdlib::builtin_modules().size() >= 5, "builtin_modules has >= 5 modules");
  check(stdlib::find_builtin_module("Parsley.Prelude").has_value(), "Parsley.Prelude found");
  check(stdlib::find_builtin_module("Json").has_value(), "Json found");
  check(stdlib::find_builtin_module("Sql").has_value(), "Sql found");

  // Calc.parsley resolve + typecheck
  {
    const std::string calc_src = read_fixture("Calc.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(calc_src, "Calc.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    check(!diags.has_errors(), "Calc.parsley resolves & typechecks without errors");
    check(resolved.lookup_value("sepBy").has_value(), "Calc resolved sepBy from Prelude");
    check(resolved.lookup_type("Expr").has_value(), "Calc resolved Expr type");
    check(resolved.lookup_ctor("Bin").has_value(), "Calc resolved Bin ctor");
    auto prog_ty = typed.lookup_rule_result_type_string("program");
    check(prog_ty.has_value() && *prog_ty == "Expr", "Calc program result type is Expr");
  }

  // HtmlContext.parsley resolve + typecheck
  {
    const std::string html_src = read_fixture("HtmlContext.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(html_src, "HtmlContext.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    check(!diags.has_errors(), "HtmlContext.parsley resolves & typechecks without errors");
    check(typed.rule_is_context["htmlTag"], "htmlTag marked as context do");
  }

  // E0200: undefined symbol / bad export
  {
    const std::string src = "module BadExport (missingRule) where\ngood :: Text\ngood = \"a\"\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(src, "BadExport.parsley", diags);
    (void)resolve::resolve_module(mod, diags);
    check(has_diag_code(diags, "E0200"), "E0200 emitted on missing exported symbol");
  }

  // E0301: exported rule without type signature
  {
    const std::string src = "module NoSig (entry) where\ntoken id = /[a-z]+/ :: Text\nentry = id\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(src, "NoSig.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    (void)typecheck::check_module(mod, resolved, diags);
    check(has_diag_code(diags, "E0301"), "E0301 emitted when exported rule lacks type signature");
  }

  // E0310: ApplicativeDo violation (dependent binding without context do)
  {
    const std::string src =
        "module AppDo where\n"
        "import Parsley.Prelude (exactly)\n"
        "token id = /[a-z]+/ :: Text\n"
        "tag :: Text\n"
        "tag = do\n"
        "  n <- id\n"
        "  exactly n\n"
        "  pure n\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(src, "AppDo.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    (void)typecheck::check_module(mod, resolved, diags);
    check(has_diag_code(diags, "E0310"), "E0310 emitted on non-context dependent do");
  }
}

void test_ir_and_desugar() {
  using namespace parsley;

  // 1. Calc.parsley desugaring + IR JSON & S-expr
  {
    const std::string calc_src = read_fixture("Calc.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(calc_src, "Calc.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);

    check(!diags.has_errors(), "Calc.parsley desugars without errors");
    check(grammar.module_name == "Calc", "IR module_name == Calc");
    check(!grammar.gTokens.empty(), "IR gTokens non-empty");
    check(grammar.gRules.find("program") != grammar.gRules.end(), "IR contains program rule");
    check(grammar.gRules.find("expr") != grammar.gRules.end(), "IR contains expr rule");
    check(grammar.gTypes.size() == 2, "IR has 2 ADTs (Op, Expr)");
    check(grammar.gExamples.size() == 2, "IR has 2 examples");

    // Check anonymous literals/keywords have higher priority than regex tokens
    const auto* kw_let = ir::find_token(grammar, "kw_let");
    const auto* tok_ident = ir::find_token(grammar, "ident");
    check(tok_ident != nullptr, "IR has ident token");
    if (kw_let && tok_ident) {
      check(kw_let->priority > tok_ident->priority, "keyword priority > regex ident priority");
    }

    const std::string json = ir::to_json(grammar);
    const std::string sexpr = ir::to_sexpr(grammar);
    check(json.find("\"module\": \"Calc\"") != std::string::npos ||
          json.find("\"module\":\"Calc\"") != std::string::npos, "IR JSON contains Calc module");
    check(json.find("PrecTable") != std::string::npos, "IR JSON contains PrecTable");
    check(sexpr.find("(grammar") != std::string::npos &&
          sexpr.find("(module \"Calc\")") != std::string::npos, "IR S-expr contains Calc grammar");
  }

  // 2. LexModes.parsley desugaring
  {
    const std::string modes_src = read_fixture("LexModes.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(modes_src, "LexModes.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    check(!diags.has_errors(), "LexModes.parsley desugars without errors");
    check(grammar.gModes.size() == 1 && grammar.gModes[0].name == "string", "IR has string lex mode");
  }

  // 3. E0400: infinite parameterized rule expansion guard
  {
    const std::string rec_src =
        "module BadRec (entry) where\n"
        "token a = /a/ :: Text\n"
        "loop :: Rule a -> Rule a\n"
        "loop p = loop (loop p)\n"
        "entry :: Text\n"
        "entry = loop a\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(rec_src, "BadRec.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    (void)desugar::desugar_module(mod, resolved, typed, diags);
    check(has_diag_code(diags, "E0400"), "E0400 emitted on infinite parameterized rule expansion");
  }
}

void test_analysis() {
  using namespace parsley;

  // 1. Calc.parsley: FIRST/FOLLOW, auto left-factoring E0214 Note, LL(1) strategy
  {
    const std::string calc_src = read_fixture("Calc.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(calc_src, "Calc.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    auto res = analysis::analyze_grammar(grammar, diags);

    check(!diags.has_errors(), "Calc.parsley analysis succeeds without errors");
    check(has_diag_code(diags, "E0214"), "Calc.parsley emits E0214 note for auto left-factoring of atom");
    check(!res.factored_rules.empty(), "Calc.parsley records left-factored rule");
    check(res.rules.count("atom") == 1 && !res.rules["atom"].first.empty(), "atom has non-empty FIRST set");
    check(res.rules.count("expr") == 1 && !res.rules["expr"].follow.empty(), "expr has non-empty FOLLOW set");
    check(res.selected_strategy == analysis::Strategy::LL1, "Calc.parsley selects LL1 strategy");
  }

  // 2. NOFACTOR triggers E0214 error with counterexample
  {
    const std::string nofactor_src =
        "{-# NOFACTOR #-}\n"
        "module NoFactor (entry) where\n"
        "token ident = /[a-z]+/ :: Text\n"
        "data Node = Call Text Text | Var Text\n"
        "entry :: Node\n"
        "entry = Call <$> ident <*> (\"(\" *> ident <* \")\") | Var <$> ident\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(nofactor_src, "NoFactor.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    auto res = analysis::analyze_grammar(grammar, diags);

    check(diags.has_errors(), "NOFACTOR emits error on unfactored conflict");
    check(has_diag_code(diags, "E0214"), "NOFACTOR emits E0214");
    check(!res.counterexamples.empty(), "NOFACTOR generates counterexample");
    if (!res.counterexamples.empty()) {
      const std::string formatted = analysis::format_counterexample(res.counterexamples[0]);
      check(!formatted.empty(), "format_counterexample non-empty");
    }
  }

  // 3. HtmlContext selects PEG/GLL due to `context do`
  {
    const std::string html_src = read_fixture("HtmlContext.parsley");
    DiagnosticBag diags;
    auto mod = parser::parse_module(html_src, "HtmlContext.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    auto res = analysis::analyze_grammar(grammar, diags);

    check(!diags.has_errors(), "HtmlContext analysis succeeds");
    check(res.has_context_rules, "HtmlContext detects context rules");
    check(res.selected_strategy == analysis::Strategy::PEG ||
          res.selected_strategy == analysis::Strategy::GLL,
          "HtmlContext selects PEG or GLL strategy");
  }

  // 4. E0510: context rule under LL1 mode
  {
    const std::string bad_ctx_src =
        "{-# MODE LL1 #-}\n"
        "module BadCtx (tag) where\n"
        "import Parsley.Prelude (exactly)\n"
        "token id = /[a-z]+/ :: Text\n"
        "tag :: Text\n"
        "tag = context do\n"
        "  n <- id\n"
        "  exactly n\n"
        "  pure n\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(bad_ctx_src, "BadCtx.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    (void)analysis::analyze_grammar(grammar, diags);
    check(has_diag_code(diags, "E0510"), "E0510 emitted for context rule in LL1 mode");
  }

  // 5. E0500 (left recursion in LL1) & E0520 (dead rule)
  {
    const std::string lr_src =
        "{-# MODE LL1 #-}\n"
        "module LeftRec (expr) where\n"
        "token n = /[0-9]+/ :: Text\n"
        "expr :: Text\n"
        "expr = expr | n\n"
        "dead :: Text\n"
        "dead = n\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(lr_src, "LeftRec.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    (void)analysis::analyze_grammar(grammar, diags);
    check(has_diag_code(diags, "E0500"), "E0500 emitted for left recursion in LL1 mode");
    check(has_diag_code(diags, "E0520"), "E0520 emitted for unreachable dead rule");
  }
}

} // namespace

int main() {
  test_version();
  test_diagnostics();
  test_lexer_and_parser();
  test_stdlib_resolve_typecheck();
  test_ir_and_desugar();
  test_analysis();

  if (g_failures > 0) {
    std::cerr << g_failures << " test check(s) failed.\n";
    return 1;
  }
  std::cout << "All Phase v0.5.0 tests passed.\n";
  return 0;
}
