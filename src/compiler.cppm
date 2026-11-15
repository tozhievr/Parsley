module;

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

export module parsley.compiler;

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
import parsley.psl;
import parsley.codegen;
import parsley.tooling;

export namespace parsley::compiler {

struct CompileOptions {
  bool run_examples = true;
  psl::PslStatus min_status = psl::PslStatus::Draft;
  std::string psl_source_override;
  std::string psl_filename_override;
};

struct CompileResult {
  bool ok = false;
  bool is_psl_only = false;
  std::string filename;
  std::string source;
  DiagnosticBag diagnostics;
  ast::Module ast_module;
  resolve::ResolvedModule resolved;
  typecheck::TypedModule typed;
  ir::Grammar grammar;
  analysis::AnalysisResult analysis;
  std::optional<eval::ExampleRunSummary> example_summary;
  std::optional<psl::PslModule> psl_module;
  std::string psl_filename;
  std::string psl_source;
  std::optional<psl::PslExampleSummary> psl_example_summary;
};

namespace detail {

inline std::optional<std::string> read_file_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline bool looks_like_psem(std::string_view src, std::string_view filename) {
  if (filename.ends_with(".psem")) {
    return true;
  }
  std::size_t i = 0;
  while (i < src.size()) {
    while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) {
      ++i;
    }
    if (i + 1 < src.size() && src[i] == '-' && src[i + 1] == '-') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (i + 1 < src.size() && src[i] == '{' && src[i + 1] == '-') {
      auto end = src.find("-}", i + 2);
      if (end == std::string_view::npos) break;
      i = end + 2;
      continue;
    }
    break;
  }
  constexpr std::string_view kw = "semantics";
  if (src.substr(i).starts_with(kw)) {
    std::size_t after = i + kw.size();
    if (after < src.size() && std::isspace(static_cast<unsigned char>(src[after]))) {
      return true;
    }
  }
  return false;
}

inline void verify_lockfile_if_present(const std::filesystem::path& lock_path,
                                       const ir::Grammar& grammar,
                                       const psl::PslModule& psl_mod,
                                       DiagnosticBag& diags) {
  std::error_code ec;
  if (!std::filesystem::exists(lock_path, ec)) {
    return;
  }
  auto lock_txt = read_file_bytes(lock_path);
  if (!lock_txt) {
    return;
  }
  auto expected = psl::parse_psl_lock(*lock_txt);
  if (!expected) {
    return;
  }
  auto current = psl::compute_psl_lock_data(grammar, psl_mod.language_version);
  auto diff = psl::verify_psl_lock(*expected, current);
  if (!diff.matches) {
    std::ostringstream msg;
    msg << "grammar AST signature mismatch against lockfile `" << lock_path.filename().string()
        << "`";
    if (!diff.changed_types.empty()) {
      msg << " (changed:";
      for (const auto& t : diff.changed_types) msg << " " << t;
      msg << ")";
    }
    if (!diff.added_types.empty()) {
      msg << " (added:";
      for (const auto& t : diff.added_types) msg << " " << t;
      msg << ")";
    }
    if (!diff.removed_types.empty()) {
      msg << " (removed:";
      for (const auto& t : diff.removed_types) msg << " " << t;
      msg << ")";
    }
    diags.error("PSL-LOCK-MISMATCH", msg.str(), psl_mod.span);
  }
}

inline void attach_psl_to_result(CompileResult& res,
                                 std::string_view psl_src,
                                 std::string_view psl_fname,
                                 const ir::Grammar* grammar_ptr,
                                 const CompileOptions& opts,
                                 const std::filesystem::path* lock_path = nullptr) {
  res.psl_filename = std::string(psl_fname);
  res.psl_source = std::string(psl_src);
  psl::PslModule pmod = psl::parse_psl_module(res.psl_source, res.psl_filename, res.diagnostics);

  psl::PslCheckOptions chk_opts;
  chk_opts.min_status = opts.min_status;
  chk_opts.require_examples = true;
  chk_opts.cross_check_grammar = (grammar_ptr != nullptr);
  psl::check_psl_module(pmod, grammar_ptr, res.diagnostics, chk_opts);

  if (lock_path && grammar_ptr != nullptr) {
    verify_lockfile_if_present(*lock_path, *grammar_ptr, pmod, res.diagnostics);
  }

  if (opts.run_examples && grammar_ptr != nullptr && !res.diagnostics.has_errors()) {
    res.psl_example_summary = psl::run_psl_examples(pmod, *grammar_ptr, res.diagnostics);
  }

  res.psl_module = std::move(pmod);
}

} // namespace detail

inline CompileResult compile_source_with_options(std::string_view source,
                                                 std::string_view filename,
                                                 const CompileOptions& opts) {
  CompileResult res;
  res.filename = std::string(filename);
  res.source = std::string(source);

  if (detail::looks_like_psem(source, filename)) {
    res.is_psl_only = true;
    detail::attach_psl_to_result(res, source, filename, nullptr, opts, nullptr);
    res.ok = !res.diagnostics.has_errors();
    return res;
  }

  res.ast_module = parser::parse_module(res.source, res.filename, res.diagnostics);
  res.resolved = resolve::resolve_module(res.ast_module, res.diagnostics);
  res.typed = typecheck::check_module(res.ast_module, res.resolved, res.diagnostics);
  res.grammar = desugar::desugar_module(res.ast_module, res.resolved, res.typed, res.diagnostics);
  res.analysis = analysis::analyze_grammar(res.grammar, res.diagnostics);

  if (opts.run_examples && !res.diagnostics.has_errors()) {
    res.example_summary = eval::run_examples(res.grammar, res.diagnostics);
  }

  if (!opts.psl_source_override.empty()) {
    std::string pfname = opts.psl_filename_override.empty()
                             ? (res.grammar.module_name + ".psem")
                             : opts.psl_filename_override;
    detail::attach_psl_to_result(res, opts.psl_source_override, pfname, &res.grammar, opts,
                                 nullptr);
  }

  res.ok = !res.diagnostics.has_errors();
  return res;
}

inline CompileResult compile_source(std::string_view source,
                                    std::string_view filename = "<input>",
                                    bool run_examples_flag = true) {
  CompileOptions opts;
  opts.run_examples = run_examples_flag;
  return compile_source_with_options(source, filename, opts);
}

inline CompileResult compile_file_with_options(const std::string& path,
                                               const CompileOptions& opts) {
  std::filesystem::path in_path(path);
  auto content = detail::read_file_bytes(in_path);
  if (!content) {
    CompileResult res;
    res.filename = path;
    res.diagnostics.error("E0100", "cannot open input file `" + path + "`");
    res.ok = false;
    return res;
  }

  if (in_path.extension() == ".psem" || detail::looks_like_psem(*content, path)) {
    std::filesystem::path sibling_grammar = in_path;
    sibling_grammar.replace_extension(".parsley");
    std::error_code ec;
    if (std::filesystem::exists(sibling_grammar, ec)) {
      auto g_content = detail::read_file_bytes(sibling_grammar);
      if (g_content) {
        CompileOptions g_opts = opts;
        g_opts.psl_source_override.clear();
        CompileResult res =
            compile_source_with_options(*g_content, sibling_grammar.string(), g_opts);
        std::filesystem::path lock_path = in_path.string() + ".lock";
        detail::attach_psl_to_result(res, *content, path, &res.grammar, opts, &lock_path);
        res.ok = !res.diagnostics.has_errors();
        return res;
      }
    }
    return compile_source_with_options(*content, path, opts);
  }

  CompileResult res = compile_source_with_options(*content, path, opts);
  if (!res.psl_module.has_value()) {
    std::filesystem::path sibling_psem = in_path;
    sibling_psem.replace_extension(".psem");
    std::error_code ec;
    if (std::filesystem::exists(sibling_psem, ec)) {
      if (auto psem_txt = detail::read_file_bytes(sibling_psem)) {
        std::filesystem::path lock_path = sibling_psem.string() + ".lock";
        detail::attach_psl_to_result(res, *psem_txt, sibling_psem.string(), &res.grammar, opts,
                                     &lock_path);
      }
    }
  }
  res.ok = !res.diagnostics.has_errors();
  return res;
}

inline CompileResult compile_file(const std::string& path, bool run_examples_flag = true) {
  CompileOptions opts;
  opts.run_examples = run_examples_flag;
  return compile_file_with_options(path, opts);
}

} // namespace parsley::compiler
