#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
import parsley.codegen;
import parsley.tooling;
import parsley.compiler;

namespace {

namespace fs = std::filesystem;

std::string to_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? "parser" : out;
}

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
      << " — Haskell-style Grammar DSL & ISO C17 Parser Generator\n\n"
      << "Usage:\n"
      << "  parsley build <file.parsley> [-o <dir>] [--no-compile-shared] [--emit-ir-json] [--emit-ir-sexpr]\n"
      << "  parsley check <file.parsley>\n"
      << "  parsley test <file.parsley> [--fuzz <N>]\n"
      << "  parsley explain <CODE>\n"
      << "  parsley fmt <file.parsley> [--write|--check]\n"
      << "  parsley graph <file.parsley> [--format dot|svg] [-o <file>]\n"
      << "  parsley lsp\n"
      << "  parsley pkg <list|show|init> [name]\n"
      << "  parsley --version | --help\n";
}

void print_diagnostics(const parsley::DiagnosticBag& diags, std::string_view source) {
  if (!diags.items().empty()) {
    std::cerr << parsley::render_diagnostics(diags.items(), source);
  }
}

int run_system_cmd(const std::string& cmd) {
#ifdef _WIN32
  std::string wrapped = "\"" + cmd + "\"";
  return std::system(wrapped.c_str());
#else
  return std::system(cmd.c_str());
#endif
}

std::vector<std::string> candidate_c_compilers() {
  std::vector<std::string> list;
  if (const char* env_cc = std::getenv("PARSLEY_CC"); env_cc && env_cc[0] != '\0') {
    list.emplace_back(env_cc);
  }
  list.emplace_back("clang");
#ifdef _WIN32
  if (fs::exists("D:/clang-23.1.3/bin/clang.exe")) {
    list.emplace_back("D:/clang-23.1.3/bin/clang.exe");
  }
  list.emplace_back("gcc");
#else
  list.emplace_back("cc");
  list.emplace_back("gcc");
#endif
  return list;
}

bool compile_shared_library(const fs::path& out_dir,
                            const std::string& mod_low,
                            fs::path& out_lib_path) {
  const std::string tgt = mod_low + "_parser";
#ifdef _WIN32
  out_lib_path = out_dir / (tgt + ".dll");
  const std::string platform_flags = "-shared";
  const std::string math_flag;
#elif defined(__APPLE__)
  out_lib_path = out_dir / ("lib" + tgt + ".dylib");
  const std::string platform_flags = "-dynamiclib -fPIC";
  const std::string math_flag;
#else
  out_lib_path = out_dir / ("lib" + tgt + ".so");
  const std::string platform_flags = "-shared -fPIC";
  const std::string math_flag = " -lm";
#endif

  const fs::path src = out_dir / "parser.c";
  const fs::path inc1 = out_dir / "include";
  const fs::path inc2 = out_dir;

  for (const auto& cc : candidate_c_compilers()) {
    std::ostringstream cmd;
    cmd << "\"" << cc << "\" -std=c17 -O2 " << platform_flags
        << " -DPARSLEY_BUILDING_DLL"
        << " -I\"" << inc1.string() << "\""
        << " -I\"" << inc2.string() << "\""
        << " \"" << src.string() << "\""
        << math_flag
        << " -o \"" << out_lib_path.string() << "\"";
    if (run_system_cmd(cmd.str()) == 0 && fs::exists(out_lib_path)) {
      return true;
    }
  }
  return false;
}

int cmd_build(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley build` requires an input <file.parsley>\n";
    return 1;
  }

  std::string input_file;
  std::string out_dir_str;
  std::string backend = "c17";
  bool compile_shared = true;
  bool emit_ir_json = false;
  bool emit_ir_sexpr = false;

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
    } else if (a == "--no-compile-shared") {
      compile_shared = false;
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

  auto res = parsley::compiler::compile_file(input_file, false);
  print_diagnostics(res.diagnostics, res.source);
  if (!res.ok) {
    return 1;
  }

  const std::string mod_low = to_lower(res.grammar.module_name);
  fs::path out_dir = out_dir_str.empty() ? fs::path(mod_low + "_parser") : fs::path(out_dir_str);

  if (backend == "ir-json") {
    emit_ir_json = true;
  } else if (backend == "ir-sexpr") {
    emit_ir_sexpr = true;
  } else if (backend != "c17" && backend != "c") {
    std::cerr << "error: unsupported backend `" << backend
              << "` (supported: c17, c, ir-json, ir-sexpr)\n";
    return 1;
  }

  if (backend == "c17" || backend == "c") {
    parsley::codegen::CodegenOptions opts;
    opts.target = parsley::codegen::TargetLang::C17;
    opts.emit_tests = true;
    const auto files = parsley::codegen::generate_backend(res.grammar, res.analysis, opts);
    for (const auto& f : files) {
      fs::path target = out_dir / f.relative_path;
      if (!write_text_file(target, f.content)) {
        std::cerr << "error: failed to write `" << target.string() << "`\n";
        return 1;
      }
    }
    std::cout << "Generated ISO C17 parser project in `" << out_dir.string() << "` ("
              << files.size() << " files)\n";

    if (compile_shared) {
      fs::path lib_path;
      if (!compile_shared_library(out_dir, mod_low, lib_path)) {
        std::cerr << "error: failed to compile C17 shared library in `" << out_dir.string()
                  << "`\n";
        return 1;
      }
      std::cout << "Compiled shared library: `" << lib_path.string() << "`\n";
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
    std::cerr << "error: `parsley check` requires an input <file.parsley>\n";
    return 1;
  }
  auto res = parsley::compiler::compile_file(args[0], false);
  print_diagnostics(res.diagnostics, res.source);
  if (!res.ok) {
    return 1;
  }
  std::cout << "OK: module `" << res.grammar.module_name << "` checked ("
            << res.grammar.gRules.size() << " rules, strategy: "
            << parsley::analysis::strategy_name(res.analysis.selected_strategy) << ")\n";
  return 0;
}

int cmd_test(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley test` requires an input <file.parsley>\n";
    return 1;
  }
  std::string input_file;
  std::size_t fuzz_count = 0;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--fuzz") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing count after `--fuzz`\n";
        return 1;
      }
      fuzz_count = static_cast<std::size_t>(std::stoull(args[++i]));
    } else if (!args[i].empty() && args[i][0] == '-') {
      std::cerr << "error: unknown option `" << args[i] << "` for `test`\n";
      return 1;
    } else {
      input_file = args[i];
    }
  }

  if (input_file.empty()) {
    std::cerr << "error: missing input `.parsley` file\n";
    return 1;
  }

  auto res = parsley::compiler::compile_file(input_file, true);
  print_diagnostics(res.diagnostics, res.source);

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

  bool fuzz_ok = true;
  if (fuzz_count > 0 && !res.grammar.gRules.empty()) {
    auto fz = parsley::eval::fuzz_grammar(res.grammar, fuzz_count);
    std::cout << "Fuzz: " << fz.passed << "/" << fz.generated << " generated inputs parsed ("
              << fz.attempts << " attempts)\n";
    for (const auto& fail : fz.failures) {
      std::cerr << "  [FUZZ FAIL] " << fail << "\n";
    }
    fuzz_ok = (fz.failed == 0);
  }

  return (res.ok && fuzz_ok) ? 0 : 1;
}

int cmd_explain(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley explain` requires a diagnostic code (e.g. E0214)\n";
    return 1;
  }
  auto found = parsley::find_error_explanation(args[0]);
  if (!found) {
    std::cerr << "Unknown diagnostic code `" << args[0] << "`.\nKnown codes:";
    for (const auto& code : parsley::known_error_codes()) {
      std::cerr << " " << code;
    }
    std::cerr << "\n";
    return 1;
  }
  std::cout << parsley::format_error_explanation(args[0]) << "\n";
  return 0;
}

int cmd_fmt(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: `parsley fmt` requires an input <file.parsley>\n";
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
    std::cerr << "error: missing input `.parsley` file\n";
    return 1;
  }

  auto src = read_text_file(input_file);
  if (!src) {
    std::cerr << "error: cannot read `" << input_file << "`\n";
    return 1;
  }

  parsley::DiagnosticBag diags;
  auto mod = parsley::parser::parse_module(*src, input_file, diags);
  if (diags.has_errors()) {
    print_diagnostics(diags, *src);
    return 1;
  }

  std::string formatted = parsley::tooling::format_module(mod, *src);
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
  print_diagnostics(res.diagnostics, res.source);
  if (!res.ok) {
    return 1;
  }

  std::string graph_out;
  if (format == "dot") {
    graph_out = parsley::tooling::generate_dot(res.grammar);
  } else if (format == "svg") {
    graph_out = parsley::tooling::generate_railroad_svg(res.grammar);
  } else {
    std::cerr << "error: unknown graph format `" << format << "` (expected dot or svg)\n";
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

int main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 0;
  }

  std::string cmd = argv[1];
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    args.emplace_back(argv[i]);
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
