module;

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
import parsley.codegen;
import parsley.tooling;

export namespace parsley::compiler {

struct CompileResult {
  bool ok = false;
  std::string filename;
  std::string source;
  DiagnosticBag diagnostics;
  ast::Module ast_module;
  resolve::ResolvedModule resolved;
  typecheck::TypedModule typed;
  ir::Grammar grammar;
  analysis::AnalysisResult analysis;
  std::optional<eval::ExampleRunSummary> example_summary;
};

inline CompileResult compile_source(std::string_view source,
                                    std::string_view filename = "<input>",
                                    bool run_examples_flag = true) {
  CompileResult res;
  res.filename = std::string(filename);
  res.source = std::string(source);

  res.ast_module = parser::parse_module(res.source, res.filename, res.diagnostics);
  res.resolved = resolve::resolve_module(res.ast_module, res.diagnostics);
  res.typed = typecheck::check_module(res.ast_module, res.resolved, res.diagnostics);
  res.grammar = desugar::desugar_module(res.ast_module, res.resolved, res.typed, res.diagnostics);
  res.analysis = analysis::analyze_grammar(res.grammar, res.diagnostics);

  if (run_examples_flag && !res.diagnostics.has_errors()) {
    res.example_summary = eval::run_examples(res.grammar, res.diagnostics);
  }

  res.ok = !res.diagnostics.has_errors();
  return res;
}

inline CompileResult compile_file(const std::string& path, bool run_examples_flag = true) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    CompileResult res;
    res.filename = path;
    res.diagnostics.error("E0100", "cannot open input file `" + path + "`");
    res.ok = false;
    return res;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return compile_source(ss.str(), path, run_examples_flag);
}

} // namespace parsley::compiler
