#include <cstdlib>
#include <filesystem>
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
import parsley.eval;
import parsley.codegen;
import parsley.tooling;
import parsley.compiler;

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
  check(parsley::VERSION_MINOR == 9, "VERSION_MINOR == 9");
  check(parsley::VERSION_PATCH == 0, "VERSION_PATCH == 0");
  check(parsley::VERSION_STRING == "0.9.0", "VERSION_STRING == 0.9.0");
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

void test_eval() {
  using namespace parsley;

  auto run_fixture = [](std::string_view name) -> ir::Grammar {
    const std::string src = read_fixture(name);
    DiagnosticBag diags;
    auto mod = parser::parse_module(src, name, diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    (void)analysis::analyze_grammar(grammar, diags);
    check(!diags.has_errors(), std::string(name) + " compiled without errors for eval");
    return grammar;
  };

  // 1. Calc.parsley examples + ad-hoc inputs + fuzzing
  {
    auto g = run_fixture("Calc.parsley");
    DiagnosticBag ex_diags;
    auto summary = eval::run_examples(g, ex_diags);
    check(!ex_diags.has_errors(), "Calc.parsley examples pass without errors");
    check(summary.total == 2 && summary.passed == 2 && summary.failed == 0,
          "Calc.parsley 2/2 examples passed");

    auto res_ok = eval::parse_input(g, "let x = 2^3^2 in -x + f(1, 2)");
    check(res_ok.ok, "Calc.parsley parses let, right-assoc ^, unary -, and call");

    auto res_err = eval::parse_input(g, "1 +");
    check(!res_err.ok && res_err.error.has_value(), "Calc.parsley rejects '1 +'");
    if (res_err.error) {
      check(res_err.error->line == 1 && res_err.error->col == 4, "Calc error at (1, 4)");
    }

    auto fuzz = eval::fuzz_grammar(g, 25, 42);
    check(fuzz.generated > 0 && fuzz.failed == 0, "Calc.parsley fuzzing (25 cases) 0 failures");
  }

  // 2. LexModes.parsley
  {
    auto g = run_fixture("LexModes.parsley");
    DiagnosticBag ex_diags;
    auto summary = eval::run_examples(g, ex_diags);
    check(!ex_diags.has_errors(), "LexModes.parsley examples pass");
    check(summary.passed == 1 && summary.failed == 0, "LexModes 1/1 example passed");
  }

  // 3. HtmlContext.parsley
  {
    auto g = run_fixture("HtmlContext.parsley");
    DiagnosticBag ex_diags;
    auto summary = eval::run_examples(g, ex_diags);
    check(!ex_diags.has_errors(), "HtmlContext.parsley examples pass");
    check(summary.passed == 1 && summary.failed == 0, "HtmlContext 1/1 example passed");

    auto mismatch = eval::parse_input(g, "<div>hello</span>");
    check(!mismatch.ok, "HtmlContext rejects mismatched closing tag <div>...</span>");
  }

  // 4. E0600 on failing example
  {
    const std::string bad_ex_src =
        "module BadEx (program) where\n"
        "token num = /[0-9]+/ :: Int\n"
        "data Expr = Num Int\n"
        "program :: Expr\n"
        "program = Num <$> num\n"
        "example \"wrong\" : \"123\" parses-to Num 999\n";
    DiagnosticBag diags;
    auto mod = parser::parse_module(bad_ex_src, "BadEx.parsley", diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    (void)analysis::analyze_grammar(grammar, diags);
    DiagnosticBag ex_diags;
    auto summary = eval::run_examples(grammar, ex_diags);
    check(summary.failed == 1, "BadEx fails 1 example");
    check(has_diag_code(ex_diags, "E0600"), "BadEx emits E0600");
  }
}

void write_text_file(const std::filesystem::path& p, std::string_view content) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

int run_shell(const std::string& cmd) {
#if defined(_WIN32)
  const std::string wrapped = "\"" + cmd + "\"";
  return std::system(wrapped.c_str());
#else
  return std::system(cmd.c_str());
#endif
}

void test_codegen_c17() {
  using namespace parsley;
  namespace fs = std::filesystem;

  auto compile_ir = [](std::string_view name) {
    const std::string src = read_fixture(name);
    DiagnosticBag diags;
    auto mod = parser::parse_module(src, name, diags);
    auto resolved = resolve::resolve_module(mod, diags);
    auto typed = typecheck::check_module(mod, resolved, diags);
    auto grammar = desugar::desugar_module(mod, resolved, typed, diags);
    auto an = analysis::analyze_grammar(grammar, diags);
    check(!diags.has_errors(), std::string(name) + " compiled without errors for C17 codegen");
    return std::pair{grammar, an};
  };

  // 1. Calc.parsley full multi-build-system C17 project + CMake/CTest + shared library consumer
  {
    auto [grammar, an] = compile_ir("Calc.parsley");
    auto files = codegen::generate_backend(grammar, an, {});
    check(!files.empty(), "generate_backend returns files");

    const fs::path out_dir = fs::temp_directory_path() / "parsley_c17_calc_test";
    std::error_code ec;
    fs::remove_all(out_dir, ec);
    fs::create_directories(out_dir, ec);

    for (const auto& gf : files) {
      write_text_file(out_dir / gf.relative_path, gf.content);
    }

    const std::string expected_files[] = {
        "parser.h",
        "include/calc_parser.h",
        "parser.c",
        "test_parser.c",
        "CMakeLists.txt",
        "meson.build",
        "Makefile",
        "xmake.lua",
        "BUILD.bazel",
        "MODULE.bazel",
        "calc_parser.pc",
        "README.md",
    };
    for (const auto& rel : expected_files) {
      check(fs::exists(out_dir / rel), std::string("Generated file exists: ") + rel);
    }

    // Configure, build, and run ctest in generated CMake project
    const std::string cc = PARSLEY_C_COMPILER;
    const fs::path cmake_build = out_dir / "cmake_build";
    const std::string cfg_cmd = "cmake -S \"" + out_dir.string() + "\" -B \"" +
                                cmake_build.string() + "\" -G Ninja -DCMAKE_C_COMPILER=\"" + cc + "\"";
    check(run_shell(cfg_cmd) == 0, "Generated C17 CMake project configures");

    const std::string bld_cmd = "cmake --build \"" + cmake_build.string() + "\"";
    check(run_shell(bld_cmd) == 0, "Generated C17 CMake project builds");

    const std::string tst_cmd = "ctest --test-dir \"" + cmake_build.string() + "\" --output-on-failure";
    check(run_shell(tst_cmd) == 0, "Generated C17 CMake project ctest passes");

    // Direct shared library + C17 consumer executable compilation & execution
    const fs::path consumer_c = out_dir / "consumer.c";
    write_text_file(consumer_c, R"C(
#include "parser.h"
#include <stdio.h>
#include <string.h>

int main(void) {
  if (parsley_run_examples() != 0) {
    fprintf(stderr, "parsley_run_examples failed\n");
    return 1;
  }

  ParsleyResult ok = parsley_parse("1 + 2 * 3");
  if (!ok.ok) {
    fprintf(stderr, "expected ok parse for '1 + 2 * 3'\n");
    parsley_result_free(&ok);
    return 2;
  }
  char buf[512];
  int n = parsley_format_ast(&ok, buf, sizeof(buf));
  if (n <= 0 || strcmp(buf, "Bin Add (Num 1) (Bin Mul (Num 2) (Num 3))") != 0) {
    fprintf(stderr, "unexpected AST: '%s'\n", buf);
    parsley_result_free(&ok);
    return 3;
  }
  parsley_result_free(&ok);

  ParsleyResult bad = parsley_parse("1 +");
  if (bad.ok || bad.error.line != 1 || bad.error.col != 4) {
    fprintf(stderr, "expected error at 1:4, got ok=%d (%d:%d)\n",
            (int)bad.ok, bad.error.line, bad.error.col);
    parsley_result_free(&bad);
    return 4;
  }
  parsley_result_free(&bad);
  return 0;
}
)C");

    const fs::path shared_dir = out_dir / "direct_shared";
    fs::create_directories(shared_dir, ec);
#if defined(_WIN32)
    const fs::path dll_path = shared_dir / "calc_parser.dll";
    const fs::path lib_path = shared_dir / "calc_parser.lib";
    const fs::path exe_path = shared_dir / "consumer.exe";
    const std::string build_dll = "\"" + cc + "\" -std=c17 -Wall -Wextra -shared \"" +
                                  (out_dir / "parser.c").string() + "\" -o \"" + dll_path.string() + "\"";
    check(run_shell(build_dll) == 0, "Direct clang -std=c17 -shared builds calc_parser.dll");
    const std::string build_exe = "\"" + cc + "\" -std=c17 -Wall -Wextra -I\"" + out_dir.string() + "\" \"" +
                                  consumer_c.string() + "\" \"" + lib_path.string() + "\" -o \"" +
                                  exe_path.string() + "\"";
    check(run_shell(build_exe) == 0, "Direct clang -std=c17 links consumer.exe against calc_parser.dll");
    check(run_shell("\"" + exe_path.string() + "\"") == 0,
          "C17 consumer executable passes against calc_parser.dll");
#else
#  if defined(__APPLE__)
    const fs::path so_path = shared_dir / "libcalc_parser.dylib";
#  else
    const fs::path so_path = shared_dir / "libcalc_parser.so";
#  endif
    const fs::path exe_path = shared_dir / "consumer";
    const std::string build_so = "\"" + cc + "\" -std=c17 -Wall -Wextra -fPIC -shared \"" +
                                 (out_dir / "parser.c").string() + "\" -o \"" + so_path.string() + "\"";
    check(run_shell(build_so) == 0, "Direct clang -std=c17 -shared builds shared library");
    const std::string build_exe = "\"" + cc + "\" -std=c17 -Wall -Wextra -I\"" + out_dir.string() + "\" \"" +
                                  consumer_c.string() + "\" \"" + so_path.string() + "\" -Wl,-rpath,\"" +
                                  shared_dir.string() + "\" -o \"" + exe_path.string() + "\"";
    check(run_shell(build_exe) == 0, "Direct clang -std=c17 links consumer against shared library");
    check(run_shell("\"" + exe_path.string() + "\"") == 0,
          "C17 consumer executable passes against shared library");
#endif

    fs::remove_all(out_dir, ec);
  }

  // 2. LexModes.parsley & HtmlContext.parsley C17 standalone test_parser.c compilation & execution
  for (std::string_view fixture : {"LexModes.parsley", "HtmlContext.parsley"}) {
    auto [grammar, an] = compile_ir(fixture);
    auto files = codegen::generate_backend(grammar, an, {});
    const fs::path out_dir = fs::temp_directory_path() / (std::string("parsley_c17_") + grammar.module_name);
    std::error_code ec;
    fs::remove_all(out_dir, ec);
    fs::create_directories(out_dir, ec);
    for (const auto& gf : files) {
      write_text_file(out_dir / gf.relative_path, gf.content);
    }
    const std::string cc = PARSLEY_C_COMPILER;
#if defined(_WIN32)
    const fs::path exe_path = out_dir / "test_parser.exe";
#else
    const fs::path exe_path = out_dir / "test_parser";
#endif
    const std::string cmd = "\"" + cc + "\" -std=c17 -DPARSLEY_STATIC -I\"" + out_dir.string() + "\" \"" +
                            (out_dir / "parser.c").string() + "\" \"" +
                            (out_dir / "test_parser.c").string() + "\" -o \"" + exe_path.string() + "\"";
    check(run_shell(cmd) == 0, std::string(fixture) + " C17 test_parser compiles with -std=c17");
    check(run_shell("\"" + exe_path.string() + "\"") == 0,
          std::string(fixture) + " C17 test_parser runs and passes");
    fs::remove_all(out_dir, ec);
  }
}

void test_tooling_compiler_cli() {
  using namespace parsley;
  namespace fs = std::filesystem;

  // 1. compiler::compile_file on Calc.parsley
  const std::string calc_path = std::string(PARSLEY_FIXTURES_DIR) + "/Calc.parsley";
  auto compiled = compiler::compile_file(calc_path, true);
  check(compiled.ok, "compiler::compile_file(Calc.parsley) ok");
  check(compiled.example_summary.has_value() && compiled.example_summary->passed == 2,
        "compiler::compile_file ran 2/2 Calc examples");

  // 2. Formatter idempotency & round-trip parse
  const std::string formatted = tooling::format_module(compiled.ast_module, compiled.source);
  check(formatted.find("module Calc") != std::string::npos, "formatted module contains module Calc");
  auto recompiled = compiler::compile_source(formatted, "Calc_fmt.parsley", true);
  check(recompiled.ok, "re-formatted Calc.parsley compiles and passes examples");

  // 3. Graph DOT & SVG
  const std::string dot = tooling::generate_dot(compiled.grammar);
  const std::string svg = tooling::generate_railroad_svg(compiled.grammar);
  check(dot.find("digraph") != std::string::npos, "generate_dot produces digraph");
  check(svg.find("<svg") != std::string::npos, "generate_railroad_svg produces <svg");

  // 4. LSP server JSON-RPC 2.0 tests
  {
    tooling::LspServer lsp;
    auto init_resp = lsp.process_message(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    check(init_resp.size() == 1 && init_resp[0].find("hoverProvider") != std::string::npos,
          "LSP initialize returns capabilities");

    // Escape calc_src as JSON string minimally by using a simple grammar
    auto open_resp = lsp.process_message(
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///T.parsley","languageId":"parsley","version":1,"text":"module T (expr) where\ntoken n = /[0-9]+/ :: Text\nexpr :: Text\nexpr = n\n"}}})");
    check(open_resp.size() == 1 && open_resp[0].find("publishDiagnostics") != std::string::npos,
          "LSP didOpen publishes diagnostics");

    auto hover_resp = lsp.process_message(
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///T.parsley"},"position":{"line":3,"character":1}}})");
    check(hover_resp.size() == 1 && hover_resp[0].find("FIRST") != std::string::npos,
          "LSP hover returns FIRST/FOLLOW info");

    auto def_resp = lsp.process_message(
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///T.parsley"},"position":{"line":3,"character":7}}})");
    check(def_resp.size() == 1 && def_resp[0].find("file:///T.parsley") != std::string::npos,
          "LSP definition returns target location");

    auto refs_resp = lsp.process_message(
        R"({"jsonrpc":"2.0","id":4,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///T.parsley"},"position":{"line":1,"character":6}}})");
    check(refs_resp.size() == 1 && refs_resp[0].find("file:///T.parsley") != std::string::npos,
          "LSP references returns locations");

    auto shut_resp = lsp.process_message(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
    check(shut_resp.size() == 1, "LSP shutdown responds");
    (void)lsp.process_message(R"({"jsonrpc":"2.0","method":"exit"})");
    check(lsp.should_exit() && lsp.exit_code() == 0, "LSP clean exit returns 0");
  }

  // 5. Package manager helpers
  {
    check(tooling::list_packages().find("Parsley.Prelude") != std::string::npos,
          "list_packages includes Parsley.Prelude");
    auto shown = tooling::show_package("Json");
    check(shown.has_value() && shown->find("JsonValue") != std::string::npos,
          "show_package Json includes JsonValue");

    const fs::path pkg_dir = fs::temp_directory_path() / "parsley_pkg_init_test";
    std::error_code ec;
    fs::remove_all(pkg_dir, ec);
    auto init_res = tooling::init_package("DemoGrammar", pkg_dir.string());
    check(init_res.ok, "init_package succeeds");
    check(fs::exists(pkg_dir / "parsley.toml"), "init_package creates parsley.toml");
    check(fs::exists(pkg_dir / "src" / "DemoGrammar.parsley"), "init_package creates src/DemoGrammar.parsley");
    fs::remove_all(pkg_dir, ec);
  }

  // 6. CLI `parsley build`, `parsley fmt`, `parsley graph` smoke checks
  {
    const std::string cli = PARSLEY_CLI_EXE;
    const fs::path cli_dir = fs::temp_directory_path() / "parsley_cli_build_test";
    std::error_code ec;
    fs::remove_all(cli_dir, ec);
    fs::create_directories(cli_dir, ec);

    const std::string build_cmd = "\"" + cli + "\" build \"" + calc_path + "\" -o \"" +
                                  cli_dir.string() + "\" --emit-ir-json --emit-ir-sexpr";
    check(run_shell(build_cmd) == 0, "parsley build Calc.parsley succeeds");
    check(fs::exists(cli_dir / "parser.h"), "parsley build emitted parser.h");
    check(fs::exists(cli_dir / "parser.c"), "parsley build emitted parser.c");
    check(fs::exists(cli_dir / "CMakeLists.txt"), "parsley build emitted CMakeLists.txt");
    check(fs::exists(cli_dir / "calc.ir.json") || fs::exists(cli_dir / "Calc.ir.json") ||
          fs::exists(cli_dir / "ir.json"), "parsley build emitted IR JSON");
#if defined(_WIN32)
    check(fs::exists(cli_dir / "calc_parser.dll"), "parsley build compiled calc_parser.dll");
#elif defined(__APPLE__)
    check(fs::exists(cli_dir / "libcalc_parser.dylib"), "parsley build compiled libcalc_parser.dylib");
#else
    check(fs::exists(cli_dir / "libcalc_parser.so"), "parsley build compiled libcalc_parser.so");
#endif

    const fs::path dot_out = cli_dir / "calc.dot";
    const fs::path svg_out = cli_dir / "calc.svg";
    check(run_shell("\"" + cli + "\" graph \"" + calc_path + "\" --format dot -o \"" + dot_out.string() + "\"") == 0 &&
          fs::exists(dot_out), "parsley graph --format dot succeeds");
    check(run_shell("\"" + cli + "\" graph \"" + calc_path + "\" --format svg -o \"" + svg_out.string() + "\"") == 0 &&
          fs::exists(svg_out), "parsley graph --format svg succeeds");
    check(run_shell("\"" + cli + "\" fmt \"" + calc_path + "\"") != 2, "parsley fmt runs cleanly");

    fs::remove_all(cli_dir, ec);
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
  test_eval();
  test_codegen_c17();
  test_tooling_compiler_cli();

  if (g_failures > 0) {
    std::cerr << g_failures << " test check(s) failed.\n";
    return 1;
  }
  std::cout << "All Phase v0.8.0 tests passed.\n";
  return 0;
}
