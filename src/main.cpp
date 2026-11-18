#define _CRT_SECURE_NO_WARNINGS
#include "process.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

import parsley.version;
import parsley.diagnostic;
import parsley.ast;
import parsley.parser;
import parsley.ir;
import parsley.analysis;
import parsley.eval;
import parsley.psl;
import parsley.codegen;
import parsley.c23_text;
import parsley.tooling;
import parsley.compiler;

namespace {

namespace fs = std::filesystem;
bool verbose_diagnostics = false;

std::string to_lower(std::string_view name) {return parsley::c23_text::module_stem(name);}

std::optional<std::string> read_text_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_text_file(const fs::path& path, std::string_view content) {
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

void print_help() {
  std::cout
      << "Parsley " << parsley::VERSION_STRING
      << " — Haskell-style Grammar DSL, PSL Static Semantics & ISO C23 Generator\n\n"
      << "Usage:\n"
      << "  parsley build <file.parsley|file.psem> [-o <dir>] [--no-compile-shared] [--emit-nextc] [--compile-compiler] [--min-status draft|reviewed|verified] [--emit-agent-docs] [--emit-ir-json] [--emit-ir-sexpr]\n"
      << "  parsley check <file.parsley|file.psem> [--min-status draft|reviewed|verified]\n"
      << "  parsley test <file.parsley|file.psem> [--fuzz <N>] [--seed <N>] [--min-status draft|reviewed|verified]\n"
      << "  parsley explain <CODE>\n"
      << "  parsley fmt <file.parsley|file.psem> [--write|--check]\n"
      << "  parsley graph <file.parsley> [--format dot|svg|modules-dot] [-o <file>]\n"
      << "  parsley agent-docs [--format markdown|json] [-o <dir_or_file>] [<file.parsley>]\n"
      << "  parsley lsp\n"
      << "  parsley pkg <list|show|init> [name]\n"
      << "  parsley --version | --help\n"
      << "  Global: -v|--verbose (include analysis notes), --no-color (plain text)\n";
}

void print_diagnostics(const parsley::DiagnosticBag& diags, std::string_view source,
                       std::string_view psl_source = "") {
  for (const auto& d : diags.items()) {
    if (d.severity == parsley::Severity::Note && !verbose_diagnostics) continue;
    const std::string_view src =
        (d.span.file.ends_with(".psem") && !psl_source.empty()) ? psl_source : source;
    std::cerr << parsley::render_diagnostics(std::vector<parsley::Diagnostic>{d}, src);
  }
}

void print_diagnostics(const parsley::compiler::CompileResult& res) {
  for (const auto& diagnostic : res.diagnostics.items()) {
    if (diagnostic.severity == parsley::Severity::Note && !verbose_diagnostics) continue;
    const auto module = res.module_sources.find(diagnostic.span.file);
    const std::string& source = module != res.module_sources.end() ? module->second
        : (diagnostic.span.file.ends_with(".psem") && !res.psl_source.empty() ? res.psl_source : res.source);
    std::cerr << parsley::render_diagnostic(diagnostic, source);
  }
}

std::vector<std::string> candidate_c_compilers() {
  if (const char* cc = std::getenv("PARSLEY_CC"); cc && *cc) return {cc};
  std::vector<std::string> list;
#ifdef PARSLEY_DEFAULT_CC
  if (fs::exists(PARSLEY_DEFAULT_CC)) list.emplace_back(PARSLEY_DEFAULT_CC);
#endif
  list.emplace_back("clang"); list.emplace_back("cc"); list.emplace_back("gcc"); return list;
}

bool compile_shared_library(const fs::path& out_dir, const std::string& mod_low,
                            const std::vector<std::string>& extra_c_files, fs::path& out_lib_path) {
  const std::string tgt = mod_low + "_parser";
#ifdef _WIN32
  out_lib_path = out_dir / (tgt + ".dll");
#elif defined(__APPLE__)
  out_lib_path = out_dir / ("lib" + tgt + ".dylib");
#else
  out_lib_path = out_dir / ("lib" + tgt + ".so");
#endif
  for (const auto& cc : candidate_c_compilers()) {
    std::vector<std::string> args{cc, "-std=c23", "-O2", "-Wall", "-Wextra", "-pedantic", "-DPARSLEY_BUILDING_DLL"};
#ifdef __APPLE__
    args.push_back("-dynamiclib");
#else
    args.push_back("-shared");
#endif
#ifndef _WIN32
    args.push_back("-fPIC");
#endif
    args.push_back("-I" + (out_dir / "include").string());
    args.push_back("-I" + out_dir.string()); args.push_back((out_dir / "parser.c").string());
    for (const auto& cf : extra_c_files) if (fs::exists(out_dir / cf)) args.push_back((out_dir / cf).string());
#if !defined(_WIN32) && !defined(__APPLE__)
    args.push_back("-lm");
#endif
    args.push_back("-o"); args.push_back(out_lib_path.string());
    if (run_process(args) == 0 && fs::exists(out_lib_path)) return true;
  }
  return false;
}
bool compile_language_compiler(const fs::path& out_dir,const std::string& mod,
                              const std::vector<std::string>& sources,fs::path& output) {
#ifdef _WIN32
  output=out_dir/(mod+"_compiler.exe");
#else
  output=out_dir/(mod+"_compiler");
#endif
  for(const auto& cc:candidate_c_compilers()){
    std::vector<std::string> command{cc,"-std=c23","-O2","-Wall","-Wextra","-pedantic",
      "-DPARSLEY_STATIC","-I"+out_dir.string(),(out_dir/"parser.c").string()};
    for(const auto& source:sources)command.push_back((out_dir/source).string());
    command.push_back((out_dir/"tools"/(mod+"_compiler.c")).string());
#if !defined(_WIN32) && !defined(__APPLE__)
    command.push_back("-lm");
#endif
    command.push_back("-o");command.push_back(output.string());
    if(run_process(command)==0 && fs::exists(output))return true;
  }
  return false;
}
bool parse_min_status_arg(std::string_view val, parsley::psl::PslStatus& out_status) {
  auto parsed = parsley::psl::parse_status_name(val);
  if (!parsed) {
    std::cerr << "error: invalid `--min-status " << val
              << "` (expected `draft`, `reviewed`, or `verified`)\n";
    return false;
  }
  out_status = *parsed;
  return true;
}

int cmd_build(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley build` requires an input <file.parsley>\n";
    return 1;
  }

  std::string input_file;
  std::string out_dir_str;
  std::string backend = "c23";
  bool compile_shared = true;
  bool emit_nextc = false, compile_compiler = false;
  bool emit_ir_json = false;
  bool emit_ir_sexpr = false;
  bool emit_agent_docs = false;
  parsley::psl::PslStatus min_status = parsley::psl::PslStatus::Draft;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-o" || a == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing directory after `" << a << "`\n";
        return 1;
      }
      out_dir_str = args[++i];
    } else if (a == "-b" || a == "--backend") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing backend after `" << a << "`\n";
        return 1;
      }
      backend = args[++i];
    } else if (a == "--min-status") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing status after `--min-status`\n";
        return 1;
      }
      if (!parse_min_status_arg(args[++i], min_status)) {
        return 1;
      }
    } else if (a == "--emit-nextc") {
      emit_nextc = true;
    } else if (a == "--compile-compiler") {
      emit_nextc = compile_compiler = true;
    } else if (a == "--no-compile-shared") {
      compile_shared = false;
    } else if (a == "--emit-agent-docs") {
      emit_agent_docs = true;
    } else if (a == "--emit-ir-json") {
      emit_ir_json = true;
    } else if (a == "--emit-ir-sexpr") {
      emit_ir_sexpr = true;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "error: unknown option `" << a << "` for `build`\n";
      return 1;
    } else {
      input_file = a;
    }
  }

  if (input_file.empty()) {
    std::cerr << "error: missing input `.parsley` file\n";
    return 1;
  }

  parsley::compiler::CompileOptions copts;
  copts.run_examples = false;
  copts.min_status = min_status;
  auto res = parsley::compiler::compile_file_with_options(input_file, copts);
  print_diagnostics(res);
  if (!res.ok) {
    return 1;
  }

  const std::string mod_name = res.grammar.module_name.empty() && res.psl_module
                                   ? res.psl_module->module_name
                                   : res.grammar.module_name;
  const std::string mod_low = to_lower(mod_name);
  fs::path out_dir = out_dir_str.empty() ? fs::path(mod_low + "_parser") : fs::path(out_dir_str);

  if(emit_nextc && (!res.psl_module || res.grammar.gRules.empty() || (backend!="c23" && backend!="c"))){
    std::cerr<<"error: nextc generation requires an adjacent grammar and PSL and the c23 backend\n";return 1;
  }
  if (backend == "ir-json") {
    emit_ir_json = true;
  } else if (backend == "ir-sexpr") {
    emit_ir_sexpr = true;
  } else if (backend != "c23" && backend != "c") {
    std::cerr << "error: unsupported backend `" << backend
              << "` (supported: c23, c, ir-json, ir-sexpr)\n";
    return 1;
  }

  if (backend == "c23" || backend == "c") {
    parsley::codegen::CodegenOptions opts;
    opts.target = parsley::codegen::TargetLang::C23;
    opts.emit_nextc = emit_nextc;
    if (emit_nextc) {
      auto adapter_path = fs::path(input_file).replace_extension(".native.h");
      std::error_code ec;
      const bool exists = fs::exists(adapter_path, ec);
      if (exists) opts.native_adapter = read_text_file(adapter_path.string());
      if (ec || (exists && !opts.native_adapter)) {
        std::cerr << "error: cannot read native adapter " << adapter_path << "\n";
        return 1;
      }
    }
    opts.emit_tests = true;
    opts.min_status = min_status;
    opts.psl_module = res.psl_module ? &*res.psl_module : nullptr;
    const auto files = parsley::codegen::generate_backend(res.grammar, res.analysis, opts);

    std::vector<std::string> extra_c_files;
    for (const auto& f : files) {
      fs::path target = out_dir / f.relative_path;
      if (!write_text_file(target, f.content)) {
        std::cerr << "error: failed to write `" << target.string() << "`\n";
        return 1;
      }
      if (f.relative_path.ends_with(".c") && f.relative_path != "parser.c" &&
          !f.relative_path.starts_with("test_") && !f.relative_path.starts_with("tools/")) {
        extra_c_files.push_back(f.relative_path);
      }
    }

    if (emit_agent_docs) {
      auto bundle = parsley::tooling::generate_agent_docs(
          &res.grammar, res.psl_module ? &*res.psl_module : nullptr);
      write_text_file(out_dir / "AGENTS.md", bundle.agents_md);
      write_text_file(out_dir / "skills" / "parsley" / "SKILL.md", bundle.skill_md);
    }

    std::cout << "Generated ISO C23 parser project in `" << out_dir.string() << "` ("
              << files.size() << " files)\n";

    if (compile_shared) {
      fs::path lib_path;
      if (!compile_shared_library(out_dir, mod_low, extra_c_files, lib_path)) {
        std::cerr << "error: failed to compile C23 shared library in `" << out_dir.string()
                  << "`\n";
        return 1;
      }
      std::cout << "Compiled shared library: `" << lib_path.string() << "`\n";
    }
    if(compile_compiler){
      fs::path executable;
      if(!compile_language_compiler(out_dir,mod_low,extra_c_files,executable)){
        std::cerr<<"error: failed to compile generated language compiler\n";return 1;
      }
      std::cout<<"Compiled language compiler: "<<executable.string()<<"\n";
    }
  }

  if (emit_ir_json) {
    fs::path p = out_dir / (mod_low + ".ir.json");
    if (!write_text_file(p, parsley::ir::to_json(res.grammar))) {
      std::cerr << "error: failed to write `" << p.string() << "`\n";
      return 1;
    }
    std::cout << "Wrote IR JSON: `" << p.string() << "`\n";
  }

  if (emit_ir_sexpr) {
    fs::path p = out_dir / (mod_low + ".ir.sexpr");
    if (!write_text_file(p, parsley::ir::to_sexpr(res.grammar))) {
      std::cerr << "error: failed to write `" << p.string() << "`\n";
      return 1;
    }
    std::cout << "Wrote IR S-expr: `" << p.string() << "`\n";
  }

  return 0;
}

int cmd_check(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley check` requires an input <file.parsley|file.psem>\n";
    return 1;
  }

  std::string input_file;
  parsley::psl::PslStatus min_status = parsley::psl::PslStatus::Draft;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--min-status") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing status after `--min-status`\n";
        return 1;
      }
      if (!parse_min_status_arg(args[++i], min_status)) {
        return 1;
      }
    } else if (!args[i].empty() && args[i][0] == '-') {
      std::cerr << "error: unknown option `" << args[i] << "` for `check`\n";
      return 1;
    } else {
      input_file = args[i];
    }
  }
  if (input_file.empty()) {
    std::cerr << "error: missing input file\n";
    return 1;
  }

  parsley::compiler::CompileOptions copts;
  copts.run_examples = false;
  copts.min_status = min_status;
  auto res = parsley::compiler::compile_file_with_options(input_file, copts);
  print_diagnostics(res);
  if (!res.ok) {
    return 1;
  }

  if (!res.is_psl_only && !res.grammar.module_name.empty()) {
    std::cout << "OK: module `" << res.grammar.module_name << "` checked ("
              << res.grammar.gRules.size() << " rules, analysis mode: "
              << parsley::analysis::strategy_name(res.analysis.selected_strategy) << ")\n";
  }
  if (res.psl_module) {
    std::cout << "OK: PSL semantics `" << res.psl_module->module_name << "` checked ("
              << res.psl_module->node_rules.size() << " node rules, "
              << res.psl_module->lints.size() << " lints, 5-pass plan: "
              << (res.psl_module->pass_plan.verified_acyclic ? "acyclic" : "cyclic") << ")\n";
  }
  return 0;
}

int cmd_test(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley test` requires an input <file.parsley|file.psem>\n";
    return 1;
  }
  std::string input_file;
  std::size_t fuzz_count = 0;
  std::uint64_t fuzz_seed = 42;
  parsley::psl::PslStatus min_status = parsley::psl::PslStatus::Draft;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if(args[i]=="--seed") {
      if(i+1>=args.size()){std::cerr<<"error: missing --seed value\n";return 2;}
      const auto& value=args[++i];auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),fuzz_seed);
      if(value.empty()||ec!=std::errc{}||end!=value.data()+value.size()){std::cerr<<"error: --seed requires an unsigned 64-bit integer\n";return 2;}
    } else if (args[i] == "--fuzz") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing count after `--fuzz`\n";
        return 1;
      }
      const std::string& count = args[++i];
      const auto [end, ec] = std::from_chars(count.data(), count.data() + count.size(), fuzz_count);
      if (count.empty() || ec != std::errc{} || end != count.data() + count.size() || fuzz_count > 1000000) {
        std::cerr << "error: --fuzz requires an integer in [0, 1000000]\n";
        return 2;
      }
    } else if (args[i] == "--min-status") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing status after `--min-status`\n";
        return 1;
      }
      if (!parse_min_status_arg(args[++i], min_status)) {
        return 1;
      }
    } else if (!args[i].empty() && args[i][0] == '-') {
      std::cerr << "error: unknown option `" << args[i] << "` for `test`\n";
      return 1;
    } else {
      input_file = args[i];
    }
  }

  if (input_file.empty()) {
    std::cerr << "error: missing input `.parsley` or `.psem` file\n";
    return 1;
  }

  parsley::compiler::CompileOptions copts;
  copts.run_examples = true;
  copts.min_status = min_status;
  auto res = parsley::compiler::compile_file_with_options(input_file, copts);
  print_diagnostics(res);

  if (res.example_summary) {
    for (const auto& c : res.example_summary->cases) {
      std::cout << (c.passed ? "[PASS] " : "[FAIL] ") << c.name;
      if (!c.message.empty()) {
        std::cout << ": " << c.message;
      }
      std::cout << "\n";
    }
    std::cout << "Examples: " << res.example_summary->passed << "/"
              << res.example_summary->total << " passed\n";
  }

  if (res.psl_example_summary && res.psl_example_summary->total > 0) {
    for (const auto& pc : res.psl_example_summary->cases) {
      std::cout << (pc.passed ? "[PASS] " : "[FAIL] ") << pc.lint_name << " ("
                << (pc.expect_fire ? "fires" : "silent") << " `" << pc.input << "`): "
                << pc.message << "\n";
    }
    std::cout << "PSL Examples: " << res.psl_example_summary->passed << "/"
              << res.psl_example_summary->total << " passed\n";
  }

  bool fuzz_ok = true;
  if (res.ok && fuzz_count > 0 && !res.grammar.gRules.empty()) {
    auto fz = parsley::eval::fuzz_grammar(res.grammar, fuzz_count, fuzz_seed);
    std::cout << "Fuzz: " << fz.passed << "/" << fz.generated << " generated inputs parsed ("
              << fz.attempts << " attempts, seed " << fuzz_seed << ")\n";
    for (const auto& fail : fz.failures) {
      std::cerr << "  [FUZZ FAIL] " << fail << "\n";
    }
    fuzz_ok = (fz.failed == 0);
  }

  return (res.ok && fuzz_ok) ? 0 : 1;
}

std::optional<std::string> explain_psl_code(std::string_view code) {
  if (code == "PSL-STATUS") {
    return "PSL-STATUS — Rule maturity status below --min-status threshold\n\n"
           "A PSL rule has `STATUS draft` (or `reviewed`) while the build requires a higher\n"
           "maturity level via `--min-status reviewed` or `--min-status verified`.\n"
           "Fix: Review and verify the rule against examples and update `{-\x23 STATUS verified \x23-}`.";
  }
  if (code == "PSL-AST-MISMATCH") {
    return "PSL-AST-MISMATCH — Constructor pattern does not match imported grammar AST\n\n"
           "A constructor name or arity in a `node`, `flow`, `analysis`, or `lint` pattern\n"
           "does not match any `data` constructor in the imported `.parsley` grammar.\n"
           "Fix: Ensure constructor names and field counts match the `.parsley` `data` declaration.";
  }
  if (code == "PSL-EXAMPLE-MISSING") {
    return "PSL-EXAMPLE-MISSING — Lint rule missing mandatory fires/silent examples\n\n"
           "Every `lint <name>` declaration in `.psem` must have a matching `example <name>`\n"
           "block with at least one `fires` case and at least one `silent` case.";
  }
  if (code == "PSL-EXAMPLE-FAIL") {
    return "PSL-EXAMPLE-FAIL — PSL self-attestation example did not match expectation\n\n"
           "A `fires` example stayed silent or a `silent` example triggered the lint.";
  }
  if (code == "PSL-CYCLE") {
    return "PSL-CYCLE — Cyclic dependency detected among PSL node attributes\n\n"
           "Node attribute equations must form a directed acyclic graph so the 5-pass\n"
           "evaluator terminates in linear time.";
  }
  if (code == "PSL-LOCK-MISMATCH") {
    return "PSL-LOCK-MISMATCH — Grammar AST types changed since `.psem.lock` was generated\n\n"
           "Update `.psem` patterns to match the new grammar AST and re-run `parsley build`.";
  }
  if (code == "PSL-PROFILE") {
    return "PSL-PROFILE — Profile references an undeclared lint rule\n\n"
           "Ensure every lint listed in `profile <name> = [...]` is declared via `lint`.";
  }
  return std::nullopt;
}

int cmd_explain(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley explain` requires a diagnostic code (e.g. E0214 or PSL-STATUS)\n";
    return 1;
  }
  if (auto psl_exp = explain_psl_code(args[0])) {
    std::cout << *psl_exp << "\n";
    return 0;
  }
  auto found = parsley::find_error_explanation(args[0]);
  if (!found) {
    std::cerr << "Unknown diagnostic code `" << args[0] << "`.\nKnown codes:";
    for (const auto& code : parsley::known_error_codes()) {
      std::cerr << " " << code;
    }
    std::cerr << " PSL-STATUS PSL-AST-MISMATCH PSL-EXAMPLE-MISSING PSL-EXAMPLE-FAIL PSL-CYCLE PSL-LOCK-MISMATCH PSL-PROFILE\n";
    return 1;
  }
  std::cout << parsley::format_error_explanation(args[0]) << "\n";
  return 0;
}

int cmd_fmt(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley fmt` requires an input <file.parsley|file.psem>\n";
    return 1;
  }
  std::string input_file;
  bool mode_write = false;
  bool mode_check = false;

  for (const auto& a : args) {
    if (a == "--write" || a == "-w") {
      mode_write = true;
    } else if (a == "--check" || a == "-c") {
      mode_check = true;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "error: unknown option `" << a << "` for `fmt`\n";
      return 1;
    } else {
      input_file = a;
    }
  }
  if (input_file.empty()) {
    std::cerr << "error: missing input `.parsley` or `.psem` file\n";
    return 1;
  }

  auto src = read_text_file(input_file);
  if (!src) {
    std::cerr << "error: cannot read `" << input_file << "`\n";
    return 1;
  }

  std::string formatted;
  if (input_file.ends_with(".psem")) {
    parsley::DiagnosticBag diags;
    (void)parsley::psl::parse_psl_module(*src, input_file, diags);
    if (diags.has_errors()) {
      print_diagnostics(diags, *src);
      return 1;
    }
    formatted = parsley::tooling::format_psl_module(*src);
  } else {
    parsley::DiagnosticBag diags;
    auto mod = parsley::parser::parse_module(*src, input_file, diags);
    if (diags.has_errors()) {
      print_diagnostics(diags, *src);
      return 1;
    }
    formatted = parsley::tooling::format_module(mod, *src);
  }

  if (mode_write && mode_check) {std::cerr << "error: --write and --check are mutually exclusive\n";return 2;}
  if (mode_check) {
    if (*src == formatted) {
      return 0;
    }
    std::cerr << "File `" << input_file << "` is not formatted.\n";
    return 1;
  }
  if (mode_write) {
    if (!write_text_file(input_file, formatted)) {
      std::cerr << "error: failed to write `" << input_file << "`\n";
      return 1;
    }
    return 0;
  }
  std::cout << formatted;
  return 0;
}

int cmd_graph(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley graph` requires an input <file.parsley>\n";
    return 1;
  }
  std::string input_file;
  std::string format = "dot";
  std::string out_file;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--format" || a == "-f") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing format after `" << a << "`\n";
        return 1;
      }
      format = args[++i];
    } else if (a == "-o" || a == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing file after `" << a << "`\n";
        return 1;
      }
      out_file = args[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "error: unknown option `" << a << "` for `graph`\n";
      return 1;
    } else {
      input_file = a;
    }
  }

  if (input_file.empty()) {
    std::cerr << "error: missing input `.parsley` file\n";
    return 1;
  }

  auto res = parsley::compiler::compile_file(input_file, false);
  print_diagnostics(res);
  if (!res.ok) {
    return 1;
  }

  std::string graph_out;
  if (format == "modules-dot") {
    std::ostringstream dot;
    const auto& entry = res.is_psl_only && res.psl_module ? res.psl_module->module_name : res.ast_module.name;
    dot << "digraph modules {\n  \"" << entry << "\";\n";
    for (const auto& dependency : res.module_dependencies) {
      dot << "  \"" << dependency.importer << "\" -> \"" << dependency.imported << "\";\n";
    }
    dot << "}\n";
    graph_out = dot.str();
  } else if (format == "dot") {
    graph_out = parsley::tooling::generate_dot(res.grammar);
  } else if (format == "svg") {
    graph_out = parsley::tooling::generate_railroad_svg(res.grammar);
  } else {
    std::cerr << "error: unknown graph format `" << format << "` (expected dot, svg or modules-dot)\n";
    return 1;
  }

  if (!out_file.empty()) {
    if (!write_text_file(out_file, graph_out)) {
      std::cerr << "error: failed to write `" << out_file << "`\n";
      return 1;
    }
  } else {
    std::cout << graph_out;
  }
  return 0;
}

int cmd_agent_docs(const std::vector<std::string>& args) {
  std::string input_file;
  std::string format = "markdown";
  std::string out_target;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--format" || a == "-f") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing format after `" << a << "`\n";
        return 1;
      }
      format = args[++i];
    } else if (a == "-o" || a == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing output path after `" << a << "`\n";
        return 1;
      }
      out_target = args[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "error: unknown option `" << a << "` for `agent-docs`\n";
      return 1;
    } else {
      input_file = a;
    }
  }

  std::optional<parsley::compiler::CompileResult> compiled;
  const parsley::ir::Grammar* g_ptr = nullptr;
  const parsley::psl::PslModule* p_ptr = nullptr;
  if (!input_file.empty()) {
    compiled = parsley::compiler::compile_file(input_file, false);
    if (!compiled->ok) {
      print_diagnostics(*compiled);
      return 1;
    }
    if (!compiled->grammar.module_name.empty()) {
      g_ptr = &compiled->grammar;
    }
    if (compiled->psl_module) {
      p_ptr = &*compiled->psl_module;
    }
  }

  auto bundle = parsley::tooling::generate_agent_docs(g_ptr, p_ptr);
  if (format == "json") {
    if (!out_target.empty()) {
      if (!write_text_file(out_target, bundle.schema_json + "\n")) {
        std::cerr << "error: failed to write `" << out_target << "`\n";
        return 1;
      }
    } else {
      std::cout << bundle.schema_json << "\n";
    }
    return 0;
  }

  if (format != "markdown" && format != "md") {
    std::cerr << "error: unknown format `" << format << "` (expected markdown or json)\n";
    return 1;
  }

  if (!out_target.empty()) {
    fs::path p(out_target);
    if (p.extension() == ".md") {
      if (!write_text_file(p, bundle.agents_md)) {
        std::cerr << "error: failed to write `" << p.string() << "`\n";
        return 1;
      }
    } else {
      if (!write_text_file(p / "AGENTS.md", bundle.agents_md) ||
          !write_text_file(p / "skills" / "parsley" / "SKILL.md", bundle.skill_md)) {
        std::cerr << "error: failed to write agent docs into `" << p.string() << "`\n";
        return 1;
      }
      std::cout << "Generated `AGENTS.md` and `skills/parsley/SKILL.md` in `" << p.string()
                << "`\n";
    }
  } else {
    std::cout << bundle.agents_md;
  }
  return 0;
}

int cmd_pkg(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley pkg` requires a subcommand: list | show | init\n";
    return 1;
  }
  const std::string& sub = args[0];
  if (sub == "list") {
    std::cout << parsley::tooling::list_packages();
    return 0;
  }
  if (sub == "show") {
    if (args.size() < 2) {
      std::cerr << "error: `parsley pkg show` requires <name>\n";
      return 1;
    }
    auto info = parsley::tooling::show_package(args[1]);
    if (!info) {
      std::cerr << "error: unknown package `" << args[1] << "`\n";
      return 1;
    }
    std::cout << *info << "\n";
    return 0;
  }
  if (sub == "init") {
    std::string name = args.size() >= 2 ? args[1] : "MyGrammar";
    std::string dir = args.size() >= 3 ? args[2] : ".";
    auto res = parsley::tooling::init_package(name, dir);
    if (!res.ok) {
      std::cerr << "error: " << res.message << "\n";
      return 1;
    }
    std::cout << res.message << "\n";
    for (const auto& f : res.created_files) {
      std::cout << "  created: " << f << "\n";
    }
    return 0;
  }
  std::cerr << "error: unknown `pkg` subcommand `" << sub << "`\n";
  return 1;
}

} // namespace

int parsley_main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 0;
  }

  std::string cmd = argv[1];
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg=argv[i];
    if(arg=="-v"||arg=="--verbose")verbose_diagnostics=true;
    else if(arg=="--no-color") { /* Diagnostics are plain text. */ }
    else args.emplace_back(argv[i]);
  }

  if (cmd == "--version" || cmd == "-V" || cmd == "version") {
    std::cout << "parsley " << parsley::VERSION_STRING << "\n";
    return 0;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    print_help();
    return 0;
  }
  if (cmd == "build") {
    return cmd_build(args);
  }
  if (cmd == "check") {
    return cmd_check(args);
  }
  if (cmd == "test") {
    return cmd_test(args);
  }
  if (cmd == "explain") {
    return cmd_explain(args);
  }
  if (cmd == "fmt") {
    return cmd_fmt(args);
  }
  if (cmd == "graph") {
    return cmd_graph(args);
  }
  if (cmd == "agent-docs") {
    return cmd_agent_docs(args);
  }
  if (cmd == "lsp") {
    return parsley::tooling::run_lsp_server();
  }
  if (cmd == "pkg") {
    return cmd_pkg(args);
  }

  std::cerr << "error: unknown command `" << cmd << "`\n";
  print_help();
  return 1;
}

int main(int argc, char** argv) {
  try { return parsley_main(argc, argv); }
  catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 2; }
  catch (...) { std::cerr << "error: unexpected compiler failure\n"; return 2; }
}
