module;

#include <cctype>
#include <filesystem>
#include <exception>
#include <fstream>
#include <optional>
#include <iterator>
#include <map>
#include <set>
#include <vector>
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
import parsley.regex;
import parsley.psl;
import parsley.codegen;
import parsley.tooling;

export namespace parsley::compiler {

struct CompileOptions {
  bool run_examples = true;
  bool load_file_modules = false;
  psl::PslStatus min_status = psl::PslStatus::Draft;
  std::string psl_source_override;
  std::string psl_filename_override;
};

struct ModuleDependency {
  std::string importer;
  std::string imported;
};

struct CompileResult {
  bool ok = false;
  bool is_psl_only = false;
  std::string filename;
  std::string source;
  DiagnosticBag diagnostics;
  ast::Module ast_module;
  std::vector<ModuleDependency> module_dependencies;
  std::map<std::string, std::string> module_sources;
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

// File modules are source components of one grammar. Their declarations share
// the lexer and symbol table, so normal duplicate checks run after composition.
class FileModules {
 public:
  explicit FileModules(CompileResult& result)
      : result_(result), root_(std::filesystem::canonical(
            std::filesystem::absolute(result.filename)).parent_path()) {}

  void load(ast::Module& entry) {
    active_.insert(entry.name);
    visit_imports(entry);
    active_.erase(entry.name);
    for (auto& dependency : modules_) {
      append(entry.imports, dependency.imports);
      append(entry.tokens, dependency.tokens);
      append(entry.skips, dependency.skips);
      append(entry.keywords, dependency.keywords);
      append(entry.modes, dependency.modes);
      append(entry.data_decls, dependency.data_decls);
      append(entry.rule_sigs, dependency.rule_sigs);
      append(entry.rule_defs, dependency.rule_defs);
    }
  }

 private:
  template<class T>
  static void append(std::vector<T>& destination, std::vector<T>& source) {
    destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
  }

  void visit_imports(ast::Module& module) {
    std::vector<ast::ImportDecl> builtin_imports;
    for (auto& import : module.imports) {
      result_.module_dependencies.push_back({module.name, import.module_name});
      if (stdlib::find_builtin_module(import.module_name)) {
        builtin_imports.push_back(std::move(import));
        continue;
      }
      visit(import);
    }
    module.imports = std::move(builtin_imports);
  }

  void visit(const ast::ImportDecl& import) {
    const auto& name = import.module_name;
    if (active_.contains(name)) {
      error(import, "module dependency cycle through");
      return;
    }
    if (import.is_hiding || import.is_qualified || import.alias || !import.items.empty()) {
      error(import, "source components require a plain import of");
      return;
    }
    if (loaded_.contains(name)) return;
    if (active_.size() >= 64 || loaded_.size() + active_.size() >= 256) {
      error(import, "module dependency limit reached at");
      return;
    }
    auto relative = name;
    for (auto& c : relative) if (c == '.') c = '/';
    if (relative.empty() || relative.front() == '/' ||
        relative.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/")
            != std::string::npos) {
      error(import, "invalid source module name");
      return;
    }
    std::error_code ec;
    const auto path = std::filesystem::canonical(root_ / (relative + ".parsley"), ec);
    const auto inside = path.lexically_relative(root_);
    if (ec || inside.empty() || *inside.begin() == "..") {
      error(import, "cannot find source module within project directory");
      return;
    }
    const auto filename = path.string();
    auto source = read_file_bytes(path);
    if (!source) {
      error(import, "cannot read source module");
      return;
    }
    result_.module_sources.emplace(filename, *source);
    auto dependency = parser::parse_module(*source, filename, result_.diagnostics);
    if (!dependency.has_module_header || dependency.name != name) {
      error(import, "module header does not match import of");
      return;
    }
    if (dependency.has_export_list || !dependency.pragmas.empty() || !dependency.examples.empty()) {
      error(import, "export lists, pragmas and examples belong to the entry grammar, not component");
      return;
    }
    active_.insert(name);
    visit_imports(dependency);
    active_.erase(name);
    loaded_.insert(name);
    modules_.push_back(std::move(dependency));
  }

  void error(const ast::ImportDecl& import, std::string message) {
    result_.diagnostics.error("E0610", std::move(message) + " " + import.module_name, import.span);
  }

  CompileResult& result_;
  std::filesystem::path root_;
  std::set<std::string> active_;
  std::set<std::string> loaded_;
  std::vector<ast::Module> modules_;
};

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
  const bool exists=std::filesystem::exists(lock_path,ec);
  if(ec){diags.error("PSL-LOCK-READ","cannot inspect lockfile `"+lock_path.string()+"`",psl_mod.span);return;}
  if(!exists)return;
  auto lock_txt = read_file_bytes(lock_path);
  if (!lock_txt) {
    diags.error("PSL-LOCK-READ", "cannot read existing lockfile `" + lock_path.string() + "`", psl_mod.span);
    return;
  }
  auto expected = psl::parse_psl_lock(*lock_txt);
  if (!expected) {
    diags.error("PSL-LOCK-FORMAT", "invalid lockfile `" + lock_path.string() + "`", psl_mod.span);
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

class SemanticComponents {
 public:
  SemanticComponents(CompileResult& result, const ir::Grammar* grammar)
      : result_(result), grammar_(grammar),
        root_(std::filesystem::absolute(result.psl_filename).parent_path()) {}

  void load(psl::PslModule& entry) {
    active_.insert(entry.module_name);
    visit(entry);
    for (auto& component : components_) {
      append(entry.sources, component.sources);
      append(entry.types, component.types);
      append(entry.tycons, component.tycons);
      append(entry.kinds, component.kinds);
      append(entry.builtins, component.builtins);
      append(entry.diags, component.diags);
      append(entry.coercions, component.coercions);
      append(entry.overloads, component.overloads);
      append(entry.node_rules, component.node_rules);
      append(entry.flow_rules, component.flow_rules);
      append(entry.analyses, component.analyses);
      append(entry.lints, component.lints);
      append(entry.examples, component.examples);
      append(entry.profiles, component.profiles);
    }
  }

 private:
  template<class T>
  static void append(std::vector<T>& target, std::vector<T>& source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
  }

  void visit(const psl::PslModule& parent) {
    for (const auto& import : parent.imports) {
      const auto& name = import.module_name;
      if (name == "Parsley.Sem.Prelude" || (grammar_ && name == grammar_->module_name)) continue;
      result_.module_dependencies.push_back({parent.module_name, name});
      if (active_.contains(name)) {
        error(import, "semantic dependency cycle through");
        continue;
      }
      if (import.is_hiding || import.is_qualified || import.alias || !import.items.empty()) {
        error(import, "semantic components require a plain import of");
        continue;
      }
      if (loaded_.contains(name)) continue;
      if (active_.size() >= 64 || loaded_.size() + active_.size() >= 256) {
        error(import, "semantic dependency limit reached at");
        continue;
      }
      auto relative = name;
      for (auto& c : relative) if (c == '.') c = '/';
      if (relative.empty() || relative.front() == '/' ||
          relative.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/")
              != std::string::npos) {
        error(import, "invalid semantic component name");
        continue;
      }
      std::error_code ec;
      const auto root = std::filesystem::canonical(root_, ec);
      if (ec) { error(import, "cannot resolve project root for"); continue; }
      const auto path = std::filesystem::canonical(root / (relative + ".psem"), ec);
      const auto inside = path.lexically_relative(root);
      if (ec || inside.empty() || *inside.begin() == "..") {
        error(import, "cannot find semantic component within project directory");
        continue;
      }
      const auto source = read_file_bytes(path);
      if (!source) { error(import, "cannot read semantic component"); continue; }
      const auto filename = path.string();
      result_.module_sources.emplace(filename, *source);
      auto component = psl::parse_psl_module(*source, filename, result_.diagnostics);
      if (component.module_name != name || component.language_version != parent.language_version ||
          component.project) {
        error(import, "semantic component needs a matching header and language version, without project declaration:");
        continue;
      }
      active_.insert(name);
      visit(component);
      active_.erase(name);
      loaded_.insert(name);
      components_.push_back(std::move(component));
    }
  }

  void error(const psl::PslImportDecl& import, std::string message) {
    result_.diagnostics.error("E0610", std::move(message) + " " + import.module_name, import.span);
  }

  CompileResult& result_;
  const ir::Grammar* grammar_;
  std::filesystem::path root_;
  std::set<std::string> active_;
  std::set<std::string> loaded_;
  std::vector<psl::PslModule> components_;
};

inline void attach_psl_to_result(CompileResult& res,
                                 std::string_view psl_src,
                                 std::string_view psl_fname,
                                 const ir::Grammar* grammar_ptr,
                                 const CompileOptions& opts,
                                 const std::filesystem::path* lock_path = nullptr) {
  res.psl_filename = std::string(psl_fname);
  res.psl_source = std::string(psl_src);
  psl::PslModule pmod = psl::parse_psl_module(res.psl_source, res.psl_filename, res.diagnostics);

  if (opts.load_file_modules && !res.diagnostics.has_errors()) SemanticComponents(res, grammar_ptr).load(pmod);
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
  if (opts.load_file_modules && !res.diagnostics.has_errors()) detail::FileModules(res).load(res.ast_module);
  if (res.ast_module.rule_defs.empty()) res.diagnostics.error("E0600", "grammar must define at least one rule");
  if (res.diagnostics.has_errors()) return res;
  res.resolved = resolve::resolve_module(res.ast_module, res.diagnostics);
  res.typed = typecheck::check_module(res.ast_module, res.resolved, res.diagnostics);
  res.grammar = desugar::desugar_module(res.ast_module, res.resolved, res.typed, res.diagnostics);
  auto validate_tokens = [&](const auto& tokens) {
    for (const auto& token : tokens) if (token.is_regex) {
      try { (void)regex::RegexParser(token.pattern).parse(); }
      catch (const std::exception& e) { res.diagnostics.error("E0600", "invalid regex for `" + token.id + "`: " + e.what()); }
    }
  };
  validate_tokens(res.grammar.gTokens);
  for (const auto& mode : res.grammar.gModes) validate_tokens(mode.tokens);
  if (!res.diagnostics.has_errors()) res.analysis = analysis::analyze_grammar(res.grammar, res.diagnostics);

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
  CompileOptions file_opts = opts;
  file_opts.load_file_modules = true;
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
        CompileOptions g_opts = file_opts;
        g_opts.psl_source_override.clear();
        CompileResult res =
            compile_source_with_options(*g_content, sibling_grammar.string(), g_opts);
        if(!res.ok)return res;
        std::filesystem::path lock_path = in_path.string() + ".lock";
        detail::attach_psl_to_result(res, *content, path, &res.grammar, file_opts, &lock_path);
        res.ok = !res.diagnostics.has_errors();
        return res;
      }
    }
    return compile_source_with_options(*content, path, file_opts);
  }

  CompileResult res = compile_source_with_options(*content, path, file_opts);
  if(!res.ok)return res;
  if (!res.psl_module.has_value()) {
    std::filesystem::path sibling_psem = in_path;
    sibling_psem.replace_extension(".psem");
    std::error_code ec;
    if (std::filesystem::exists(sibling_psem, ec)) {
      if (auto psem_txt = detail::read_file_bytes(sibling_psem)) {
        std::filesystem::path lock_path = sibling_psem.string() + ".lock";
        detail::attach_psl_to_result(res, *psem_txt, sibling_psem.string(), &res.grammar, file_opts,
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
