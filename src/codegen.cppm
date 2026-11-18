module;

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.codegen;

import parsley.ast;
import parsley.diagnostic;
import parsley.ir;
import parsley.regex;
import parsley.c23_runtime;
import parsley.c23_text;
import parsley.analysis;
import parsley.psl;
import parsley.optimizer;

export namespace parsley::codegen {

enum class TargetLang {
  C23,
};

struct CodegenOptions {
  TargetLang target = TargetLang::C23;
  bool emit_tests = true;
  std::string module_override;
  psl::PslStatus min_status = psl::PslStatus::Draft;
  std::string psl_profile;
  const psl::PslModule* psl_module = nullptr;
};

struct GeneratedFile {
  std::string relative_path;
  std::string content;
};

} // namespace parsley::codegen

namespace parsley::codegen::detail {

// ---------------------------------------------------------------------------
// Identifier and string escaping helpers
// ---------------------------------------------------------------------------

inline bool is_c_keyword(std::string_view s) {
  static const std::unordered_set<std::string_view> kws = {
      "auto",       "break",     "case",           "char",          "const",
      "continue",   "default",   "do",             "double",        "else",
      "enum",       "extern",    "float",          "for",           "goto",
      "if",         "inline",    "int",            "long",          "register",
      "restrict",   "return",    "short",          "signed",        "sizeof",
      "static",     "struct",    "switch",         "typedef",       "union",
      "unsigned",   "void",      "volatile",       "while",         "_Alignas",
      "_Alignof",   "_Atomic",   "_Bool",          "_Complex",      "_Generic",
      "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "true",
      "false",      "bool",      "NULL",           "EOF",
      "alignas", "alignof", "constexpr", "nullptr", "static_assert",
      "thread_local", "typeof", "typeof_unqual", "_BitInt",
      "_Decimal32", "_Decimal64", "_Decimal128",
  };
  return kws.contains(s);
}

inline std::string sanitize_c_ident(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '_') {
      out.push_back(static_cast<char>(c));
    } else {
      switch (c) {
        case '+':
          out += "_plus_";
          break;
        case '-':
          out += "_minus_";
          break;
        case '*':
          out += "_star_";
          break;
        case '/':
          out += "_slash_";
          break;
        case '^':
          out += "_caret_";
          break;
        case '=':
          out += "_eq_";
          break;
        case '<':
          out += "_lt_";
          break;
        case '>':
          out += "_gt_";
          break;
        case '.':
          out += "_dot_";
          break;
        default:
          out.push_back('_');
          break;
      }
    }
  }
  if (out.empty()) {
    out = "anon";
  }
  if (std::isdigit(static_cast<unsigned char>(out[0]))) {
    out.insert(out.begin(), 'p');
  }
  if (is_c_keyword(out)) {
    out.push_back('_');
  }
  return out;
}

inline std::string to_lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) {
    out = "parser";
  }
  if (std::isdigit(static_cast<unsigned char>(out[0]))) {
    out.insert(out.begin(), 'p');
  }
  return out;
}

inline std::string to_upper_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::toupper(c)));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) {
    out = "PARSER";
  }
  return out;
}

inline std::string c_string_lit(std::string_view text) { return c23_text::literal(text); }

inline std::string safe_comment(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') {
      out += "* /";
      ++i;
    } else if (s[i] == '\n' || s[i] == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Compile-time regex -> Thompson NFA compiler (produces 256-bit char bitmaps)
// ---------------------------------------------------------------------------

using regex::CharMatcher;
using regex::RegexAst;
using regex::RegexAstPtr;
using regex::RegexParser;

struct CompiledNfaState {
  int kind = 0; // 0 = MATCH, 1 = SPLIT, 2 = CHAR
  int bitmap_idx = -1;
  int out1 = -1;
  int out2 = -1;
};

struct CompiledNfa {
  std::vector<CompiledNfaState> states;
  int start = 0;
};

inline CompiledNfa compile_regex_to_nfa(std::string_view pattern,
                                        std::vector<std::array<std::uint8_t, 32>>& bitmaps) {
  CompiledNfa nfa;
  RegexParser parser(pattern);
  RegexAstPtr ast = parser.parse();

  auto add_bitmap = [&](const CharMatcher& m) -> int {
    auto bm = m.to_bitmap();
    for (std::size_t i = 0; i < bitmaps.size(); ++i) {
      if (bitmaps[i] == bm) {
        return static_cast<int>(i);
      }
    }
    bitmaps.push_back(bm);
    return static_cast<int>(bitmaps.size() - 1);
  };

  auto add_state = [&](CompiledNfaState st) -> int {
    int id = static_cast<int>(nfa.states.size());
    nfa.states.push_back(st);
    return id;
  };

  auto emit = [&](auto& self, const RegexAstPtr& node, int next_state) -> int {
    if (!node) {
      return next_state;
    }
    switch (node->kind) {
      case RegexAst::Kind::Empty:
        return next_state;
      case RegexAst::Kind::Char:
        return add_state(CompiledNfaState{2, add_bitmap(node->matcher), next_state, -1});
      case RegexAst::Kind::Seq: {
        int cur = next_state;
        for (std::size_t i = node->items.size(); i > 0; --i) {
          cur = self(self, node->items[i - 1], cur);
        }
        return cur;
      }
      case RegexAst::Kind::Alt: {
        if (node->items.empty()) {
          return next_state;
        }
        std::vector<int> starts;
        starts.reserve(node->items.size());
        for (const auto& it : node->items) {
          starts.push_back(self(self, it, next_state));
        }
        int head = starts.back();
        for (std::size_t i = starts.size() - 1; i > 0; --i) {
          head = add_state(CompiledNfaState{1, -1, starts[i - 1], head});
        }
        return head;
      }
      case RegexAst::Kind::Repeat: {
        int tail = next_state;
        if (node->max_rep < 0) {
          int split_id = add_state(CompiledNfaState{1, -1, -1, tail});
          int body_start = self(self, node->inner, split_id);
          nfa.states[static_cast<std::size_t>(split_id)].out1 = body_start;
          tail = split_id;
        } else {
          for (int k = node->max_rep; k > node->min_rep; --k) {
            int body_start = self(self, node->inner, tail);
            tail = add_state(CompiledNfaState{1, -1, body_start, tail});
          }
        }
        for (int k = 0; k < node->min_rep; ++k) {
          tail = self(self, node->inner, tail);
        }
        return tail;
      }
    }
    return next_state;
  };

  int match_id = add_state(CompiledNfaState{0, -1, -1, -1});
  nfa.start = emit(emit, ast, match_id);
  return nfa;
}

// ---------------------------------------------------------------------------
// C23 ADT type mapping helpers
// ---------------------------------------------------------------------------

struct CTypeInfo {
  std::unordered_map<std::string, const ir::DataDecl*> data_map;
  std::unordered_set<std::string> enum_only_types;
  std::set<std::string> list_elem_types; // e.g. "Expr", "Text", "Int", "Double", "Bool"
  std::optional<std::string> start_rule;
  std::optional<std::string> start_data_type;
};

inline bool is_enum_only_decl(const ir::DataDecl& dd) {
  if (dd.is_foreign || dd.constructors.empty()) {
    return false;
  }
  for (const auto& c : dd.constructors) {
    if (!c.positional_fields.empty() || !c.record_fields.empty()) {
      return false;
    }
  }
  return true;
}

inline std::string map_ir_type_to_c(const ir::Type& t, const CTypeInfo& info) {
  switch (t.kind) {
    case ir::Type::Kind::Unit:
      return "uint8_t";
    case ir::Type::Kind::Named:
    case ir::Type::Kind::Var: {
      if (t.name == "Text" || t.name == "String") {
        return "char*";
      }
      if (t.name == "Double" || t.name == "Float") {
        return "double";
      }
      if (t.name == "Int") {
        return "int64_t";
      }
      if (t.name == "Bool") {
        return "bool";
      }
      if (t.name == "Unit" || t.name == "()") {
        return "uint8_t";
      }
      if (info.data_map.contains(t.name)) {
        std::string cname = sanitize_c_ident(t.name);
        if (info.enum_only_types.contains(t.name)) {
          return cname;
        }
        return cname + "*";
      }
      return "ParsleyValue*";
    }
    case ir::Type::Kind::List: {
      if (!t.args.empty()) {
        const auto& elem = t.args[0];
        if ((elem.kind == ir::Type::Kind::Named || elem.kind == ir::Type::Kind::Var) &&
            (info.data_map.contains(elem.name) || elem.name == "Text" || elem.name == "String" ||
             elem.name == "Double" || elem.name == "Int" || elem.name == "Bool")) {
          if (elem.name == "Text" || elem.name == "String") {
            return "ParsleyTextList";
          }
          if (elem.name == "Double") {
            return "ParsleyDoubleList";
          }
          if (elem.name == "Int") {
            return "ParsleyIntList";
          }
          if (elem.name == "Bool") {
            return "ParsleyBoolList";
          }
          return sanitize_c_ident(elem.name) + "List";
        }
      }
      return "ParsleyValueList";
    }
    case ir::Type::Kind::App:
    case ir::Type::Kind::Tuple:
    case ir::Type::Kind::Arrow:
      return "ParsleyValue*";
  }
  return "ParsleyValue*";
}

inline void collect_list_types(const ir::Type& t, CTypeInfo& info) {
  if (t.kind == ir::Type::Kind::List && !t.args.empty()) {
    const auto& elem = t.args[0];
    if (elem.kind == ir::Type::Kind::Named || elem.kind == ir::Type::Kind::Var) {
      if (info.data_map.contains(elem.name)) {
        info.list_elem_types.insert(elem.name);
      }
    }
  }
  for (const auto& a : t.args) {
    collect_list_types(a, info);
  }
}

inline CTypeInfo analyze_c_types(const ir::Grammar& g) {
  CTypeInfo info;
  for (const auto& dd : g.gTypes) {
    if (!dd.is_foreign) {
      info.data_map[dd.name] = &dd;
      if (is_enum_only_decl(dd)) {
        info.enum_only_types.insert(dd.name);
      }
    }
  }
  for (const auto& dd : g.gTypes) {
    if (dd.is_foreign) {
      continue;
    }
    for (const auto& c : dd.constructors) {
      for (const auto& pf : c.positional_fields) {
        collect_list_types(pf, info);
      }
      for (const auto& rf : c.record_fields) {
        collect_list_types(rf.type, info);
      }
    }
  }
  if (!g.gStarts.empty()) {
    info.start_rule = g.gStarts[0];
  } else if (g.gRules.contains("program")) {
    info.start_rule = "program";
  } else if (!g.gRules.empty()) {
    info.start_rule = g.gRules.begin()->first;
  }
  if (info.start_rule) {
    auto it = g.gRules.find(*info.start_rule);
    if (it != g.gRules.end()) {
      const auto& rt = it->second.rType;
      if (rt.kind == ir::Type::Kind::Named && info.data_map.contains(rt.name)) {
        info.start_data_type = rt.name;
      }
    }
  }
  return info;
}

// ---------------------------------------------------------------------------
// Header generator (parser.h & include/<mod>_parser.h)
// ---------------------------------------------------------------------------

inline std::string namespace_c_header(std::string header, const ir::Grammar& g, const std::string& prefix) {
  std::set<std::string> names = {"ParsleyValueKind", "ParsleyValue", "ParsleyTextList", "ParsleyDoubleList", "ParsleyIntList", "ParsleyBoolList", "ParsleyValueList", "ParsleyError", "ParsleyResult",
    "PARSLEY_VAL_UNIT", "PARSLEY_VAL_BOOL", "PARSLEY_VAL_NUMBER", "PARSLEY_VAL_STRING", "PARSLEY_VAL_LIST", "PARSLEY_VAL_TUPLE", "PARSLEY_VAL_CTOR"};
  for (const auto& dd : g.gTypes) {
    auto name = sanitize_c_ident(dd.name); names.insert(name); names.insert(name+"Tag"); names.insert(name+"List");
    for (const auto& ctor : dd.constructors) names.insert(to_upper_ascii(dd.name)+"_"+to_upper_ascii(ctor.name));
  }
  const std::vector<std::string> functions = {"parsley_parse", "parsley_parse_n", "parsley_result_free", "parsley_format_ast", "parsley_run_examples"};
  names.insert(functions.begin(),functions.end());
  std::ostringstream aliases, clean;
  aliases << "/* Public ABI is module-scoped; generic aliases support one-parser consumers. */\n";
  for (const auto& name : names) {
    std::string renamed = prefix + (name.starts_with("parsley_") ? "_parser_" + name.substr(8) : "_" + name);
    aliases << "#ifdef " << name << "\n#undef " << name << "\n#endif\n#define " << name << " " << renamed << "\n";
    clean << "#undef " << name << "\n";
  }
  auto after_guard = header.find("\n\n",header.find("#define "));
  header.insert(after_guard+2,aliases.str()+"\n");
  auto final_guard = header.rfind("#endif");
  header.insert(final_guard,"#if defined(PARSLEY_NO_COMPAT) && !defined(PARSLEY_INTERNAL)\n"+clean.str()+"#endif\n\n");
  return header;
}
inline std::string generate_c23_header(const ir::Grammar& g,
                                       const analysis::AnalysisResult& analysis,
                                       const CTypeInfo& info,
                                       const std::string& mod_upper) {
  std::ostringstream out;
  out << "/* Generated by Parsley (ISO C23 backend) for module " << safe_comment(g.module_name)
      << " */\n";
  out << "/* Analysis mode: " << analysis::strategy_name(analysis.selected_strategy) << " — "
      << safe_comment(analysis.strategy_reason) << " */\n\n";
  out << "#ifndef PARSLEY_" << mod_upper << "_PARSER_H\n";
  out << "#define PARSLEY_" << mod_upper << "_PARSER_H\n\n";
  out << "#include <stdbool.h>\n";
  out << "#include <stddef.h>\n";
  out << "#include <stdint.h>\n\n";
  out << "#ifdef __cplusplus\n";
  out << "extern \"C\" {\n";
  out << "#endif\n\n";
  out << "#ifndef PARSLEY_API\n#if defined(PARSLEY_STATIC)\n";
  out << "#  define PARSLEY_API\n";
  out << "#elif defined(_WIN32) || defined(__CYGWIN__)\n";
  out << "#  if defined(PARSLEY_BUILDING_DLL)\n";
  out << "#    define PARSLEY_API __declspec(dllexport)\n";
  out << "#  else\n";
  out << "#    define PARSLEY_API __declspec(dllimport)\n";
  out << "#  endif\n";
  out << "#else\n";
  out << "#  if defined(__GNUC__) || defined(__clang__)\n";
  out << "#    define PARSLEY_API __attribute__((visibility(\"default\")))\n";
  out << "#  else\n";
  out << "#    define PARSLEY_API\n";
  out << "#  endif\n";
  out << "#endif\n#endif /* PARSLEY_API */\n\n";
  out << "#ifdef __cplusplus\n";
  out << "#  define PARSLEY_STATIC_ASSERT(cond, msg) static_assert((cond), msg)\n";
  out << "#else\n";
  out << "#  define PARSLEY_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)\n";
  out << "#endif\n\n";
  out << "PARSLEY_STATIC_ASSERT(sizeof(size_t) >= 4, \"Parsley C23 requires >= 32-bit size_t\");\n\n";

  // Generic ParsleyValue representation
  out << "typedef enum ParsleyValueKind {\n";
  out << "  PARSLEY_VAL_UNIT = 0,\n";
  out << "  PARSLEY_VAL_BOOL,\n";
  out << "  PARSLEY_VAL_NUMBER,\n";
  out << "  PARSLEY_VAL_STRING,\n";
  out << "  PARSLEY_VAL_LIST,\n";
  out << "  PARSLEY_VAL_TUPLE,\n";
  out << "  PARSLEY_VAL_CTOR\n";
  out << "} ParsleyValueKind;\n\n";
  out << "typedef struct ParsleyValue ParsleyValue;\n\n";
  out << "struct ParsleyValue {\n";
  out << "  ParsleyValueKind kind;\n";
  out << "  bool bool_val;\n";
  out << "  double number_val;\n  int64_t integer_val;\n";
  out << "  bool is_int;\n";
  out << "  char* string_val;\n  size_t string_len;\n";
  out << "  char* ctor_name;\n";
  out << "  ParsleyValue** items;\n";
  out << "  size_t item_count;\n  ParsleyValue* list_head;\n  ParsleyValue* list_tail;\n";
  out << "};\n\n";

  out << "typedef struct ParsleyTextList { char** items; size_t len; } ParsleyTextList;\n";
  out << "typedef struct ParsleyDoubleList { double* items; size_t len; } ParsleyDoubleList;\n";
  out << "typedef struct ParsleyIntList { int64_t* items; size_t len; } ParsleyIntList;\n";
  out << "typedef struct ParsleyBoolList { bool* items; size_t len; } ParsleyBoolList;\n";
  out << "typedef struct ParsleyValueList { ParsleyValue** items; size_t len; } ParsleyValueList;\n\n";

  // Forward declarations for all DataDecl ADTs
  for (const auto& dd : g.gTypes) {
    if (dd.is_foreign) {
      continue;
    }
    const std::string cname = sanitize_c_ident(dd.name);
    out << "typedef struct " << cname << " " << cname << ";\n";
  }
  if (!g.gTypes.empty()) {
    out << "\n";
  }

  for (const auto& elem : info.list_elem_types) {
    const std::string cname = sanitize_c_ident(elem);
    if (info.enum_only_types.contains(elem)) {
      out << "typedef struct " << cname << "List { " << cname << "* items; size_t len; } " << cname
          << "List;\n";
    } else {
      out << "typedef struct " << cname << "List { " << cname << "** items; size_t len; } " << cname
          << "List;\n";
    }
  }
  if (!info.list_elem_types.empty()) {
    out << "\n";
  }

  // Emit enum-only ADTs first (so value fields in later ADTs are complete types)
  auto emit_data_decl = [&](const ir::DataDecl& dd) {
    const std::string cname = sanitize_c_ident(dd.name);
    const std::string upper = to_upper_ascii(dd.name);
    out << "typedef enum " << cname << "Tag {\n";
    for (std::size_t ci = 0; ci < dd.constructors.size(); ++ci) {
      out << "  " << upper << "_" << to_upper_ascii(dd.constructors[ci].name);
      if (ci == 0) {
        out << " = 0";
      }
      if (ci + 1 < dd.constructors.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "} " << cname << "Tag;\n\n";

    bool has_any_fields = false;
    for (const auto& c : dd.constructors) {
      if (!c.positional_fields.empty() || !c.record_fields.empty()) {
        has_any_fields = true;
        break;
      }
    }

    out << "struct " << cname << " {\n";
    out << "  " << cname << "Tag tag;\n";
    if (has_any_fields) {
      out << "  union {\n";
      for (const auto& c : dd.constructors) {
        if (c.positional_fields.empty() && c.record_fields.empty()) {
          continue;
        }
        const std::string member = sanitize_c_ident(to_lower_ascii(c.name));
        out << "    struct {\n";
        if (c.is_record) {
          for (const auto& rf : c.record_fields) {
            out << "      " << map_ir_type_to_c(rf.type, info) << " " << sanitize_c_ident(rf.name)
                << ";\n";
          }
        } else {
          for (std::size_t fi = 0; fi < c.positional_fields.size(); ++fi) {
            out << "      " << map_ir_type_to_c(c.positional_fields[fi], info) << " f" << fi
                << ";\n";
          }
        }
        out << "    } " << member << ";\n";
      }
      out << "  } as;\n";
    }
    out << "};\n\n";
  };

  for (const auto& dd : g.gTypes) {
    if (!dd.is_foreign && is_enum_only_decl(dd)) {
      emit_data_decl(dd);
    }
  }
  for (const auto& dd : g.gTypes) {
    if (!dd.is_foreign && !is_enum_only_decl(dd)) {
      emit_data_decl(dd);
    }
  }

  // Error and Result structs
  out << "/* code: 0 syntax, 1 allocation failure, 2 resource limit, 3 invalid number/input */\n";
  out << "typedef struct ParsleyError {\n  int code;\n";
  out << "  int line;\n";
  out << "  int col;\n";
  out << "  char* expected;\n";
  out << "  char* found;\n";
  out << "  char* message;\n";
  out << "} ParsleyError;\n\n";

  out << "typedef struct ParsleyResult {\n";
  out << "  bool ok;\n  size_t consumed; /* input bytes consumed; use eof for a complete parse */\n";
  if (info.start_data_type) {
    out << "  " << sanitize_c_ident(*info.start_data_type) << "* root;\n";
  }
  out << "  ParsleyValue* value;\n";
  out << "  ParsleyError error;\n";
  out << "} ParsleyResult;\n\n";

  // Exported ABI
  out << "PARSLEY_API ParsleyResult parsley_parse(const char* input);\n";
  out << "PARSLEY_API ParsleyResult parsley_parse_n(const char* input, size_t len);\n";
  out << "PARSLEY_API void parsley_result_free(ParsleyResult* result);\n";
  out << "PARSLEY_API int parsley_format_ast(const ParsleyResult* result, char* buf, size_t buf_size);\n";
  out << "PARSLEY_API int parsley_run_examples(void);\n\n";

  out << "#ifdef __cplusplus\n";
  out << "}\n";
  out << "#endif\n\n";
  out << "#endif /* PARSLEY_" << mod_upper << "_PARSER_H */\n";
  return out.str();
}

// ---------------------------------------------------------------------------
// C23 Implementation Generator (parser.c)
// ---------------------------------------------------------------------------

class C23Emitter {
public:
  C23Emitter(const ir::Grammar& g,
             const analysis::AnalysisResult& analysis,
             const CTypeInfo& info,
             bool emit_tests)
      : g_(g), analysis_(analysis), info_(info), emit_tests_(emit_tests) {
    for (const auto& [rid, _] : g_.gRules) {
      rule_c_names_[rid] = "parse_rule_" + sanitize_c_ident(rid);
    }
    collect_prec_tables();
  }

  std::string emit() {
    std::ostringstream out;
    emit_preamble(out);
    emit_nfa_and_lexer_tables(out);
    emit_runtime_helpers(out);
    emit_rule_forward_decls(out);
    emit_prec_table_functions(out);
    emit_rule_functions(out);
    emit_adt_converters(out);
    emit_public_api(out);
    emit_examples_runner(out);
    std::string source = out.str();
    // Reference optional helpers portably so strict ISO C23 warning builds stay clean.
    const auto api = source.find("static ParsleyResult parsley_parse_n_impl(");
    std::set<std::string> helpers;
    std::istringstream lines(source.substr(0, api)); std::string line;
    while (std::getline(lines,line)) if (line.starts_with("static ")) {
      const auto open = line.find('('), equal = line.find('=');
      if (open == std::string::npos || (equal != std::string::npos && equal < open)) continue;
      auto begin = open; while (begin && (std::isalnum(static_cast<unsigned char>(line[begin-1])) || line[begin-1]=='_')) --begin;
      if (begin < open) helpers.insert(line.substr(begin,open-begin));
    }
    std::string references;
    for (const auto& helper : helpers) references += "  (void)" + helper + ";\n";
    source.insert(source.find('{',api)+1,"\n"+references);
    std::istringstream input(source); std::ostringstream clean;
    while (std::getline(input,line)) {
      auto colon = line.find(':');
      if (colon != std::string::npos && colon > 0 && std::all_of(line.begin(),line.begin()+static_cast<std::ptrdiff_t>(colon),[](unsigned char c){return std::isalnum(c)||c=='_';}))
        line += "; if (0) goto " + line.substr(0,colon) + ";";
      clean << line << '\n';
    }
    return clean.str();
  }

private:
  const ir::Grammar& g_;
  const analysis::AnalysisResult& analysis_;
  const CTypeInfo& info_;
  bool emit_tests_;

  std::unordered_map<std::string, std::string> rule_c_names_;
  std::vector<const ir::Prod*> prec_tables_;
  std::unordered_map<const ir::Prod*, std::string> prec_fn_names_;
  int tmp_counter_ = 0;

  std::string next_var(std::string_view prefix) {
    return std::string(prefix) + "_" + std::to_string(tmp_counter_++);
  }

  void collect_prec_tables_in_prod(const ir::ProdPtr& p) {
    if (!p) {
      return;
    }
    if (p->kind == ir::Prod::Kind::PrecTable) {
      if (!prec_fn_names_.contains(p.get())) {
        std::string fn = "parse_prec_" + std::to_string(prec_tables_.size());
        prec_fn_names_[p.get()] = fn;
        prec_tables_.push_back(p.get());
      }
      collect_prec_tables_in_prod(p->inner);
      for (const auto& lvl : p->levels) {
        for (const auto& op : lvl.ops) {
          for (const auto& pat : op.pattern) {
            collect_prec_tables_in_prod(pat);
          }
        }
      }
      return;
    }
    for (const auto& item : p->items) {
      collect_prec_tables_in_prod(item);
    }
    collect_prec_tables_in_prod(p->inner);
    collect_prec_tables_in_prod(p->sync);
    for (const auto& step : p->ctx_steps) {
      collect_prec_tables_in_prod(step.prod);
    }
  }

  void collect_prec_tables() {
    for (const auto& [_, rule] : g_.gRules) {
      collect_prec_tables_in_prod(rule.rBody);
    }
  }

  void emit_preamble(std::ostringstream& out) {
    out << "/* Generated by Parsley (ISO C23 backend) for module " << safe_comment(g_.module_name)
        << " */\n";
    out << "/* Parser: ordered recursive descent with Pratt tables; analysis mode: " << analysis::strategy_name(analysis_.selected_strategy) << " ("
        << safe_comment(analysis_.strategy_reason) << ") */\n\n";
    out << "#if !defined(PARSLEY_STATIC) && !defined(PARSLEY_BUILDING_DLL)\n";
    out << "#  define PARSLEY_BUILDING_DLL\n";
    out << "#endif\n";
    out << "#define PARSLEY_INTERNAL 1\n#include \"parser.h\"\n\n";
    out << "#include <ctype.h>\n";
    out << "#include <math.h>\n";
    out << "#include <stdio.h>\n";
    out << "#include <stdlib.h>\n";
    out << "#include <string.h>\n\n";
  }

  void emit_nfa_and_lexer_tables(std::ostringstream& out) {
    std::vector<std::array<std::uint8_t, 32>> bitmaps;
    struct TokenMeta {
      const ir::TokenDef* def = nullptr;
      int nfa_idx = -1;
    };
    std::vector<CompiledNfa> nfas;

    auto compile_token = [&](const ir::TokenDef& td) -> int {
      if (!td.is_regex) {
        return -1;
      }
      int idx = static_cast<int>(nfas.size());
      nfas.push_back(compile_regex_to_nfa(td.pattern, bitmaps));
      return idx;
    };

    std::vector<TokenMeta> default_tokens;
    for (const auto& td : g_.gTokens) {
      default_tokens.push_back(TokenMeta{&td, compile_token(td)});
    }
    std::vector<std::pair<std::string, std::vector<TokenMeta>>> mode_tokens;
    for (const auto& m : g_.gModes) {
      std::vector<TokenMeta> mt;
      for (const auto& td : m.tokens) {
        mt.push_back(TokenMeta{&td, compile_token(td)});
      }
      mode_tokens.emplace_back(m.name, std::move(mt));
    }

    // Emit 256-bit charclass bitmaps
    if (bitmaps.empty()) {
      bitmaps.push_back({});
    }
    out << "static const uint8_t parsley_char_bitmaps[" << bitmaps.size() << "][32] = {\n";
    for (std::size_t i = 0; i < bitmaps.size(); ++i) {
      out << "  {";
      for (std::size_t j = 0; j < 32; ++j) {
        if (j > 0) {
          out << ",";
        }
        out << static_cast<unsigned int>(bitmaps[i][j]);
      }
      out << "}" << (i + 1 < bitmaps.size() ? "," : "") << "\n";
    }
    out << "};\n\n";

    out << "typedef struct ParsleyNfaState {\n";
    out << "  int kind; /* 0 = MATCH, 1 = SPLIT, 2 = CHAR */\n";
    out << "  int bitmap_idx;\n";
    out << "  int out1;\n";
    out << "  int out2;\n";
    out << "} ParsleyNfaState;\n\n";

    out << "typedef struct ParsleyNfa {\n";
    out << "  const ParsleyNfaState* states;\n";
    out << "  int state_count;\n";
    out << "  int start_state;\n";
    out << "} ParsleyNfa;\n\n";

    if (nfas.empty()) {
      out << "static const ParsleyNfaState parsley_nfa_dummy[1] = {{0, -1, -1, -1}};\n";
      out << "static const ParsleyNfa parsley_nfas[1] = {{parsley_nfa_dummy, 1, 0}};\n\n";
    } else {
      for (std::size_t ni = 0; ni < nfas.size(); ++ni) {
        const auto& nfa = nfas[ni];
        out << "static const ParsleyNfaState parsley_nfa_states_" << ni << "[" << nfa.states.size()
            << "] = {\n";
        for (std::size_t si = 0; si < nfa.states.size(); ++si) {
          const auto& st = nfa.states[si];
          out << "  {" << st.kind << ", " << st.bitmap_idx << ", " << st.out1 << ", " << st.out2
              << "}" << (si + 1 < nfa.states.size() ? "," : "") << "\n";
        }
        out << "};\n";
      }
      out << "static const ParsleyNfa parsley_nfas[" << nfas.size() << "] = {\n";
      for (std::size_t ni = 0; ni < nfas.size(); ++ni) {
        out << "  {parsley_nfa_states_" << ni << ", " << nfas[ni].states.size() << ", "
            << nfas[ni].start << "}" << (ni + 1 < nfas.size() ? "," : "") << "\n";
      }
      out << "};\n\n";
    }

    // Token definitions
    out << "typedef enum ParsleyTokValType {\n";
    out << "  PARSLEY_TOK_TEXT = 0,\n";
    out << "  PARSLEY_TOK_DOUBLE,\n";
    out << "  PARSLEY_TOK_INT,\n";
    out << "  PARSLEY_TOK_BOOL,\n";
    out << "  PARSLEY_TOK_UNIT\n";
    out << "} ParsleyTokValType;\n\n";

    out << "typedef enum ParsleyLexAct {\n";
    out << "  PARSLEY_LEX_NONE = 0,\n";
    out << "  PARSLEY_LEX_PUSH,\n";
    out << "  PARSLEY_LEX_POP\n";
    out << "} ParsleyLexAct;\n\n";

    out << "typedef struct ParsleyTokenDef {\n";
    out << "  const char* id;\n";
    out << "  const char* display_name;\n";
    out << "  const char* literal;\n";
    out << "  size_t literal_len;\n";
    out << "  bool is_regex;\n";
    out << "  int nfa_idx;\n";
    out << "  bool is_skip;\n";
    out << "  int priority;\n";
    out << "  int decl_order;\n";
    out << "  ParsleyTokValType val_type;\n";
    out << "  ParsleyLexAct action;\n";
    out << "  const char* target_mode;\n";
    out << "} ParsleyTokenDef;\n\n";

    auto emit_token_entry = [&](const TokenMeta& tm) {
      const ir::TokenDef& td = *tm.def;
      std::string display = td.id;
      if (!td.is_regex && (td.is_anonymous || td.is_keyword)) {
        display = c23_text::display_string(td.pattern);
      }
      std::string vt = "PARSLEY_TOK_TEXT";
      if (td.type.kind == ir::Type::Kind::Unit) {
        vt = "PARSLEY_TOK_UNIT";
      } else if (td.type.name == "Double" || td.type.name == "Float") {
        vt = "PARSLEY_TOK_DOUBLE";
      } else if (td.type.name == "Int") {
        vt = "PARSLEY_TOK_INT";
      } else if (td.type.name == "Bool") {
        vt = "PARSLEY_TOK_BOOL";
      }
      std::string act = "PARSLEY_LEX_NONE";
      if (td.action == ir::LexActionKind::Push) {
        act = "PARSLEY_LEX_PUSH";
      } else if (td.action == ir::LexActionKind::Pop) {
        act = "PARSLEY_LEX_POP";
      }
      out << "  {" << c_string_lit(td.id) << ", " << c_string_lit(display) << ", "
          << (td.is_regex ? "NULL" : c_string_lit(td.pattern)) << ", "
          << (td.is_regex ? 0 : td.pattern.size()) << ", " << (td.is_regex ? "true" : "false")
          << ", " << tm.nfa_idx << ", " << (td.is_skip ? "true" : "false") << ", " << td.priority
          << ", " << td.decl_order << ", " << vt << ", " << act << ", "
          << c_string_lit(td.target_mode) << "}";
    };

    if (default_tokens.empty()) {
      out << "static const ParsleyTokenDef parsley_mode_default_tokens[1] = {\n";
      out << "  {\"<dummy>\", \"<dummy>\", NULL, 0, false, -1, true, 0, 0, PARSLEY_TOK_UNIT, PARSLEY_LEX_NONE, \"\"}\n";
      out << "};\n";
    } else {
      out << "static const ParsleyTokenDef parsley_mode_default_tokens[" << default_tokens.size()
          << "] = {\n";
      for (std::size_t i = 0; i < default_tokens.size(); ++i) {
        emit_token_entry(default_tokens[i]);
        out << (i + 1 < default_tokens.size() ? "," : "") << "\n";
      }
      out << "};\n";
    }

    for (std::size_t mi = 0; mi < mode_tokens.size(); ++mi) {
      const auto& mt = mode_tokens[mi].second;
      if (mt.empty()) {
        out << "static const ParsleyTokenDef parsley_mode_tokens_" << mi << "[1] = {\n";
        out << "  {\"<dummy>\", \"<dummy>\", NULL, 0, false, -1, true, 0, 0, PARSLEY_TOK_UNIT, PARSLEY_LEX_NONE, \"\"}\n";
        out << "};\n";
      } else {
        out << "static const ParsleyTokenDef parsley_mode_tokens_" << mi << "[" << mt.size()
            << "] = {\n";
        for (std::size_t ti = 0; ti < mt.size(); ++ti) {
          emit_token_entry(mt[ti]);
          out << (ti + 1 < mt.size() ? "," : "") << "\n";
        }
        out << "};\n";
      }
    }

    out << "typedef struct ParsleyLexMode {\n";
    out << "  const char* name;\n";
    out << "  const ParsleyTokenDef* tokens;\n";
    out << "  size_t count;\n";
    out << "} ParsleyLexMode;\n\n";

    out << "static const ParsleyLexMode parsley_modes[" << (1 + mode_tokens.size()) << "] = {\n";
    out << "  {\"default\", parsley_mode_default_tokens, " << default_tokens.size() << "}";
    for (std::size_t mi = 0; mi < mode_tokens.size(); ++mi) {
      out << ",\n  {" << c_string_lit(mode_tokens[mi].first) << ", parsley_mode_tokens_" << mi
          << ", " << mode_tokens[mi].second.size() << "}";
    }
    out << "\n};\n";
    out << "static const size_t parsley_mode_count = " << (1 + mode_tokens.size()) << ";\n\n";

    // Global lookup table of all tokens (for scannerless fallback / display names)
    std::vector<TokenMeta> all_tokens = default_tokens;
    for (const auto& [_, mt] : mode_tokens) {
      for (const auto& tm : mt) {
        all_tokens.push_back(tm);
      }
    }
    if (all_tokens.empty()) {
      out << "static const ParsleyTokenDef parsley_all_tokens[1] = {\n";
      out << "  {\"<dummy>\", \"<dummy>\", NULL, 0, false, -1, true, 0, 0, PARSLEY_TOK_UNIT, PARSLEY_LEX_NONE, \"\"}\n";
      out << "};\n";
      out << "static const size_t parsley_all_token_count = 0;\n\n";
    } else {
      out << "static const ParsleyTokenDef parsley_all_tokens[" << all_tokens.size() << "] = {\n";
      for (std::size_t i = 0; i < all_tokens.size(); ++i) {
        emit_token_entry(all_tokens[i]);
        out << (i + 1 < all_tokens.size() ? "," : "") << "\n";
      }
      out << "};\n";
      out << "static const size_t parsley_all_token_count = " << all_tokens.size() << ";\n\n";
    }
  }

  void emit_runtime_helpers(std::ostringstream& out) {
    out << c23_runtime::source;
    out << R"C23(static char* p_strdup_n(const char* s, size_t n) {
  char* out = (char*)p_malloc(n + 1);
  if (!out) return NULL;
  if (n > 0 && s) memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char* p_strdup(const char* s) {
  if (!s) return p_strdup_n("", 0);
  return p_strdup_n(s, strlen(s));
}

typedef struct ParsleyStrBuf {
  char* data;
  size_t len;
  size_t cap;
} ParsleyStrBuf;

static void sb_append_n(ParsleyStrBuf* sb, const char* s, size_t n) {
  if (n == 0) return;
  if (sb->len + n + 1 > sb->cap) {
    size_t new_cap = sb->cap == 0 ? 128 : sb->cap * 2;
    while (new_cap < sb->len + n + 1) new_cap *= 2;
    char* next = (char*)realloc(sb->data, new_cap);
    if (!next) return;
    sb->data = next;
    sb->cap = new_cap;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_append(ParsleyStrBuf* sb, const char* s) {
  if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_ch(ParsleyStrBuf* sb, char c) {
  sb_append_n(sb, &c, 1);
}

static ParsleyValue* pv_alloc(ParsleyValueKind kind) {
  ParsleyValue* v = (ParsleyValue*)p_calloc(1, sizeof(ParsleyValue));
  if (v) v->kind = kind;
  return v;
}

static ParsleyValue* pv_unit(void) {
  return pv_alloc(PARSLEY_VAL_UNIT);
}

static ParsleyValue* pv_bool(bool b) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_BOOL);
  if (v) v->bool_val = b;
  return v;
}

static ParsleyValue* pv_number(double num, bool is_int) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_NUMBER);
  if (v) {
    v->number_val = num;
    v->is_int = is_int;
    if (is_int) {
      if (!isfinite(num) || num < -9223372036854775808.0 || num >= 9223372036854775808.0) { p_fatal(3); return NULL; }
      v->integer_val = (int64_t)num;
    }
  }
  return v;
}

static ParsleyValue* pv_integer_literal(const char* text) {
  int64_t value; if (!p_parse_i64(text,strlen(text),&value)) {p_fatal(3);return NULL;}
  ParsleyValue* v=pv_number(0,true); v->integer_val=value;v->number_val=(double)value;return v;
}

static ParsleyValue* pv_string(const char* s, size_t len) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_STRING);
  if (v) {v->string_val = p_strdup_n(s, len);v->string_len=len;}
  return v;
}

static ParsleyValue* pv_list(ParsleyValue** items, size_t count) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_LIST);
  if (v && count > 0) {
    v->items = (ParsleyValue**)p_malloc(sizeof(ParsleyValue*) * count);
    memcpy(v->items, items, sizeof(ParsleyValue*) * count);
    v->item_count = count;
  }
  return v;
}

static ParsleyValue* pv_list_cons(ParsleyValue* head, ParsleyValue* tail) {
  size_t count = tail && tail->kind == PARSLEY_VAL_LIST ? tail->item_count : 0;
  if (count == SIZE_MAX) { p_fatal(2); return NULL; }
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_LIST);
  v->item_count = count + 1; v->list_head = head; v->list_tail = tail;
  return v;
}
static ParsleyValue* pv_tuple(ParsleyValue** items, size_t count) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_TUPLE);
  if (v && count > 0) {
    v->items = (ParsleyValue**)p_malloc(sizeof(ParsleyValue*) * count);
    memcpy(v->items, items, sizeof(ParsleyValue*) * count);
    v->item_count = count;
  }
  return v;
}

static ParsleyValue* pv_ctor(const char* name, ParsleyValue** items, size_t count) {
  ParsleyValue* v = pv_alloc(PARSLEY_VAL_CTOR);
  if (v) {
    v->ctor_name = p_strdup(name);
    if (count > 0) {
      v->items = (ParsleyValue**)p_malloc(sizeof(ParsleyValue*) * count);
      memcpy(v->items, items, sizeof(ParsleyValue*) * count);
      v->item_count = count;
    }
  }
  return v;
}

static void pv_free(ParsleyValue* v) {
  if (!v) return;
  free(v->string_val);
  free(v->ctor_name);
  if (v->items) {
    for (size_t i = 0; i < v->item_count; ++i) {
      pv_free(v->items[i]);
    }
    free(v->items);
  }
  free(v);
}

static ParsleyValue* pv_clone_impl(const ParsleyValue* v);
static ParsleyValue* pv_clone(const ParsleyValue* v) {
  p_enter(); ParsleyValue* result = pv_clone_impl(v); p_leave(); return result;
}
static ParsleyValue* pv_clone_impl(const ParsleyValue* v) {
  if (!v) return NULL;
  ParsleyValue* c = pv_alloc(v->kind);
  if (!c) return NULL;
  c->bool_val = v->bool_val;
  c->number_val = v->number_val;
  c->integer_val = v->integer_val;
  c->is_int = v->is_int;
  c->string_len=v->string_len; if (v->string_val) c->string_val = p_strdup_n(v->string_val,v->string_len);
  if (v->ctor_name) c->ctor_name = p_strdup(v->ctor_name);
  if (v->kind == PARSLEY_VAL_LIST && v->list_head) {
    c->item_count = v->item_count;
    c->items = (ParsleyValue**)p_calloc(c->item_count,sizeof(ParsleyValue*));
    size_t i = 0; const ParsleyValue* cursor = v;
    while (cursor && cursor->list_head) { p_step(); c->items[i++] = pv_clone(cursor->list_head); cursor = cursor->list_tail; }
    if (cursor) for (size_t j=0; j<cursor->item_count; ++j) c->items[i++] = pv_clone(cursor->items[j]);
    return c;
  }
  if (v->item_count > 0 && v->items) {
    c->item_count = v->item_count;
    c->items = (ParsleyValue**)p_malloc(sizeof(ParsleyValue*) * v->item_count);
    for (size_t i = 0; i < v->item_count; ++i) {
      c->items[i] = pv_clone(v->items[i]);
    }
  }
  return c;
}

static void format_value_buf(const ParsleyValue* v, bool wrap_parens, ParsleyStrBuf* sb) {
  if (!v) {
    sb_append(sb, "()");
    return;
  }
  switch (v->kind) {
    case PARSLEY_VAL_UNIT:
      sb_append(sb, "()");
      break;
    case PARSLEY_VAL_BOOL:
      sb_append(sb, v->bool_val ? "True" : "False");
      break;
    case PARSLEY_VAL_NUMBER: {
      char num_buf[64];
      double dv = v->number_val;
      if (v->is_int) {
        snprintf(num_buf, sizeof(num_buf), "%lld", (long long)v->integer_val);
      } else if (isfinite(dv) && floor(dv) == dv && dv >= -9e15 && dv <= 9e15) {
        snprintf(num_buf, sizeof(num_buf), "%lld", (long long)dv);
      } else {
        p_format_double(num_buf, sizeof(num_buf), dv);
      }
      sb_append(sb, num_buf);
      break;
    }
    case PARSLEY_VAL_STRING: {
      sb_append_ch(sb, '"');
      if (v->string_val) {
        for (size_t i=0;i<v->string_len;++i) {
          const char* p=v->string_val+i;
          if (*p == '"') sb_append(sb, "\\\"");
          else if (*p == '\\') sb_append(sb, "\\\\");
          else if (*p == '\n') sb_append(sb, "\\n");
          else if (*p == '\r') sb_append(sb, "\\r");
          else if (*p == '\t') sb_append(sb, "\\t");
          else if (*p == '\0') sb_append(sb, "\\0");
          else if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {char escaped[5];snprintf(escaped,sizeof(escaped),"\\x%02x",(unsigned char)*p);sb_append(sb,escaped);}
          else sb_append_ch(sb, *p);
        }
      }
      sb_append_ch(sb, '"');
      break;
    }
    case PARSLEY_VAL_LIST:
      sb_append_ch(sb, '[');
      for (size_t i = 0; i < v->item_count; ++i) {
        if (i > 0) sb_append(sb, ", ");
        format_value_buf(v->items[i], false, sb);
      }
      sb_append_ch(sb, ']');
      break;
    case PARSLEY_VAL_TUPLE:
      sb_append_ch(sb, '(');
      for (size_t i = 0; i < v->item_count; ++i) {
        if (i > 0) sb_append(sb, ", ");
        format_value_buf(v->items[i], false, sb);
      }
      sb_append_ch(sb, ')');
      break;
    case PARSLEY_VAL_CTOR:
      if (v->item_count == 0) {
        sb_append(sb, v->ctor_name ? v->ctor_name : "");
      } else {
        if (wrap_parens) sb_append_ch(sb, '(');
        sb_append(sb, v->ctor_name ? v->ctor_name : "");
        for (size_t i = 0; i < v->item_count; ++i) {
          sb_append_ch(sb, ' ');
          format_value_buf(v->items[i], true, sb);
        }
        if (wrap_parens) sb_append_ch(sb, ')');
      }
      break;
  }
}

/* Thompson NFA simulation */
static bool bitmap_test(int bitmap_idx, unsigned char c) {
  if (bitmap_idx < 0) return false;
  return (parsley_char_bitmaps[bitmap_idx][c >> 3] & (1u << (c & 7))) != 0;
}

static void nfa_add_eps(const ParsleyNfa* nfa, int s, int* list, int* list_len, int* seen, int stamp) {
  int top = 0; int* stack = p_scope->nfa_stack;
  if (s >= 0) stack[top++] = s;
  while (top) {
    p_step();
    int id = stack[--top];
    if (id < 0 || id >= nfa->state_count || seen[id] == stamp) continue;
    seen[id] = stamp;
    const ParsleyNfaState* st = &nfa->states[id];
    if (st->kind == 1) { if (top + 2 > 8192) p_fatal(2); stack[top++] = st->out1; stack[top++] = st->out2; }
    else list[(*list_len)++] = id;
  }
}
static bool nfa_match_prefix(const ParsleyNfa* nfa, const char* input, size_t len, size_t offset, size_t* out_len) {
  int n = nfa->state_count;
  if (n <= 0 || n > 4096) { p_fatal(2); return false; }
  int* cur = p_scope->nfa_cur;
  int* nxt = p_scope->nfa_next;
  int* seen = p_scope->nfa_seen;
  memset(seen,0,sizeof(int)*(size_t)n);
  int cur_len = 0;
  int mark = 1;
  bool found = false;
  size_t best = 0;

  nfa_add_eps(nfa, nfa->start_state, cur, &cur_len, seen, mark++);
  for (int i = 0; i < cur_len; ++i) {
    if (nfa->states[cur[i]].kind == 0) {
      found = true;
      best = 0;
      break;
    }
  }

  for (size_t pos = offset; pos < len && cur_len > 0; ++pos) {
    p_step();
    unsigned char ch = (unsigned char)input[pos];
    int nxt_len = 0;
    int step_mark = mark++;
    for (int i = 0; i < cur_len; ++i) {
      p_step();
      const ParsleyNfaState* st = &nfa->states[cur[i]];
      if (st->kind == 2 && bitmap_test(st->bitmap_idx, ch)) {
        nfa_add_eps(nfa, st->out1, nxt, &nxt_len, seen, step_mark);
      }
    }
    int* tmp = cur;
    cur = nxt;
    nxt = tmp;
    cur_len = nxt_len;
    for (int i = 0; i < cur_len; ++i) {
      if (nfa->states[cur[i]].kind == 0) {
        found = true;
        best = (pos - offset) + 1;
        break;
      }
    }
  }

  /* Scratch buffers belong to the API invocation. */
  if (out_len) *out_len = best;
  return found;
}

static bool token_match_at(const ParsleyTokenDef* td, const char* input, size_t len, size_t offset, size_t* out_len) {
  if (!td->is_regex) {
    if (td->literal && offset + td->literal_len <= len &&
        memcmp(input + offset, td->literal, td->literal_len) == 0) {
      *out_len = td->literal_len;
      return td->literal_len > 0;
    }
    return false;
  }
  if (td->nfa_idx >= 0) {
    size_t mlen = 0;
    if (nfa_match_prefix(&parsley_nfas[td->nfa_idx], input, len, offset, &mlen) && mlen > 0) {
      *out_len = mlen;
      return true;
    }
  }
  return false;
}

static const ParsleyLexMode* find_lex_mode(const char* name) {
  for (size_t i = 0; i < parsley_mode_count; ++i) {
    if (strcmp(parsley_modes[i].name, name) == 0) {
      return &parsley_modes[i];
    }
  }
  return &parsley_modes[0];
}

static const ParsleyTokenDef* find_token_def(const char* id) {
  for (size_t i = 0; i < parsley_all_token_count; ++i) {
    if (strcmp(parsley_all_tokens[i].id, id) == 0) {
      return &parsley_all_tokens[i];
    }
  }
  return NULL;
}

static const char* display_token_name(const char* id) {
  if (strcmp(id, "eof") == 0) return "end of input";
  const ParsleyTokenDef* td = find_token_def(id);
  return td ? td->display_name : id;
}

static ParsleyValue* token_to_value(const ParsleyTokenDef* td, const char* lexeme, size_t lex_len) {
  if (!td) return pv_string(lexeme, lex_len);
  switch (td->val_type) {
    case PARSLEY_TOK_UNIT:
      return pv_unit();
    case PARSLEY_TOK_BOOL:
      return pv_bool(lex_len == 4 && (memcmp(lexeme, "true", 4) == 0 || memcmp(lexeme, "True", 4) == 0));
    case PARSLEY_TOK_INT: {
      int64_t value;
      if (!p_parse_i64(lexeme, lex_len, &value)) { p_fatal(3); return NULL; }
      ParsleyValue* v = pv_number(0, true); v->integer_val = value; v->number_val=(double)value; return v;
    }
    case PARSLEY_TOK_DOUBLE: {
      double value;
      if (!p_parse_double(lexeme, lex_len, &value)) { p_fatal(3); return NULL; }
      return pv_number(value, false);
    }    case PARSLEY_TOK_TEXT:
    default:
      return pv_string(lexeme, lex_len);
  }
}

static void advance_line_col(const char* s, size_t n, int* line, int* col) {
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '\n') {
      (*line)++;
      *col = 1;
    } else if (((unsigned char)s[i] & 0xc0) != 0x80 && s[i] != '\r') {
      (*col)++;
    }
  }
}

typedef struct ParsleyLexedToken {
  const char* id;
  const ParsleyTokenDef* def;
  char* lexeme;
  size_t lexeme_len;
  int line;
  int col;
  size_t start_offset;
  bool is_eof;
  bool is_lex_error;
} ParsleyLexedToken;

#define PARSLEY_MAX_MODES 64
#define PARSLEY_MAX_EXPECTED 32
#define PARSLEY_MAX_PREFIX 64
#define PARSLEY_MAX_CTX 64
#define PARSLEY_MAX_POOL 4096

typedef struct ParsleyCtxEntry {
  char* name;
  char* value;
  size_t value_len;
} ParsleyCtxEntry;

typedef struct ParsleyLrEntry {
  const char* rule_id;
  size_t start_off;
  bool active;
  bool hit_recursion;
  bool ok;
  ParsleyValue* value;
  size_t end_tok_pos;
  size_t end_char_pos;
  int end_line;
  int end_col;
  const char* end_modes[PARSLEY_MAX_MODES];
  size_t end_mode_depth;
} ParsleyLrEntry;

typedef struct ParsleyMemo {
  size_t rule, start, end;
  bool used, complete, ok;
  ParsleyValue* value;
} ParsleyMemo;
typedef struct ParsleyParser {
  const char* input;
  size_t len;
  bool scannerless;

  ParsleyLexedToken* tokens;
  size_t token_count;
  size_t tok_pos;

  size_t char_pos;
  int line;
  int col;

  const char* mode_stack[PARSLEY_MAX_MODES];
  size_t mode_depth;

  ParsleyValue* prefix_stack[PARSLEY_MAX_PREFIX];
  size_t prefix_depth;

  ParsleyCtxEntry ctx_env[PARSLEY_MAX_CTX];
  size_t ctx_depth;

  bool suppress_err;
  bool has_err;
  size_t err_offset;
  int err_line;
  int err_col;
  char* err_found;
  char* err_expected[PARSLEY_MAX_EXPECTED];
  size_t err_expected_count;

  ParsleyLrEntry* lr_entries;
  size_t lr_count;
  size_t lr_cap;

  /* Allocation arena tracking so failed backtracking branches do not leak */
  ParsleyMemo* memo;
  size_t memo_cap, memo_count;
  ParsleyValue** allocated;
  size_t allocated_count;
  size_t allocated_cap;
} ParsleyParser;

static ParsleyValue* p_track(ParsleyParser* p, ParsleyValue* v) {
  if (!v) return NULL;
  if (p->allocated_count >= p->allocated_cap) {
    size_t nc = p->allocated_cap == 0 ? 256 : p->allocated_cap * 2;
    ParsleyValue** na = (ParsleyValue**)realloc(p->allocated, sizeof(ParsleyValue*) * nc);
    if (na) {
      p->allocated = na;
      p->allocated_cap = nc;
    }
  }
  if (p->allocated_count < p->allocated_cap) {
    p->allocated[p->allocated_count++] = v;
  }
  return v;
}

static void p_free_tracked(ParsleyParser* p) {
  for (size_t i = 0; i < p->allocated_count; ++i) {
    ParsleyValue* v = p->allocated[i];
    if (v) {
      free(v->string_val);
      free(v->ctor_name);
      free(v->items);
      free(v);
    }
  }
  free(p->allocated);
  p->allocated = NULL;
  p->allocated_count = 0;
  p->allocated_cap = 0;
}

typedef struct ParsleyCursor {
  size_t tok_pos;
  size_t char_pos;
  int line;
  int col;
  const char* mode_stack[PARSLEY_MAX_MODES];
  size_t mode_depth;
  size_t ctx_depth;
} ParsleyCursor;

static ParsleyCursor p_save(const ParsleyParser* p) {
  ParsleyCursor c;
  c.tok_pos = p->tok_pos;
  c.char_pos = p->char_pos;
  c.line = p->line;
  c.col = p->col;
  c.mode_depth = p->mode_depth;
  for (size_t i = 0; i < p->mode_depth; ++i) {
    c.mode_stack[i] = p->mode_stack[i];
  }
  c.ctx_depth = p->ctx_depth;
  return c;
}

static void p_restore(ParsleyParser* p, const ParsleyCursor* c) {
  p->tok_pos = c->tok_pos;
  p->char_pos = c->char_pos;
  p->line = c->line;
  p->col = c->col;
  p->mode_depth = c->mode_depth;
  for (size_t i = 0; i < c->mode_depth; ++i) {
    p->mode_stack[i] = c->mode_stack[i];
  }
  while (p->ctx_depth > c->ctx_depth) {
    p->ctx_depth--;
    free(p->ctx_env[p->ctx_depth].name);
    free(p->ctx_env[p->ctx_depth].value);
  }
}

static size_t p_current_offset(const ParsleyParser* p) {
  if (!p->scannerless) {
    if (p->tok_pos < p->token_count) {
      return p->tokens[p->tok_pos].start_offset;
    }
    return p->len;
  }
  return p->char_pos;
}

static char* p_found_text(const char* text,size_t size) {
  ParsleyValue value;memset(&value,0,sizeof(value));value.kind=PARSLEY_VAL_STRING;value.string_val=(char*)text;value.string_len=size;
  ParsleyStrBuf buffer={NULL,0,0};format_value_buf(&value,false,&buffer);return buffer.data;
}
static void p_current_loc(const ParsleyParser* p, int* out_line, int* out_col, char** out_found) {
  if (!p->scannerless && p->tok_pos < p->token_count) {
    const ParsleyLexedToken* t = &p->tokens[p->tok_pos];
    *out_line = t->line;
    *out_col = t->col;
    if (t->is_eof) {
      *out_found = p_strdup("end of input");
    } else {
      *out_found = p_found_text(t->lexeme,t->lexeme_len);
    }
    return;
  }
  *out_line = p->line;
  *out_col = p->col;
  if (p->char_pos >= p->len) {
    *out_found = p_strdup("end of input");
  } else {
    *out_found = p_found_text(p->input+p->char_pos,p_utf8_width(p->input+p->char_pos,p->len-p->char_pos));
  }
}

static void p_record_failure(ParsleyParser* p, const char* expected_label, bool override_same) {
  if (p->suppress_err) return;
  size_t off = p_current_offset(p);
  int ln = 1;
  int cl = 1;
  char* found = NULL;
  p_current_loc(p, &ln, &cl, &found);

  if (!p->has_err || off > p->err_offset) {
    p->has_err = true;
    p->err_offset = off;
    p->err_line = ln;
    p->err_col = cl;
    free(p->err_found);
    p->err_found = found;
    for (size_t i = 0; i < p->err_expected_count; ++i) {
      free(p->err_expected[i]);
    }
    p->err_expected_count = 0;
    if (expected_label && expected_label[0] != '\0') {
      p->err_expected[p->err_expected_count++] = p_strdup(expected_label);
    }
  } else if (off == p->err_offset) {
    free(found);
    if (override_same) {
      for (size_t i = 0; i < p->err_expected_count; ++i) {
        free(p->err_expected[i]);
      }
      p->err_expected_count = 0;
    }
    if (expected_label && expected_label[0] != '\0' && p->err_expected_count < PARSLEY_MAX_EXPECTED) {
      bool dup = false;
      for (size_t i = 0; i < p->err_expected_count; ++i) {
        if (strcmp(p->err_expected[i], expected_label) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        p->err_expected[p->err_expected_count++] = p_strdup(expected_label);
      }
    }
  } else {
    free(found);
  }
}

static void p_apply_mode_action(const ParsleyTokenDef* td, const char* stack[PARSLEY_MAX_MODES], size_t* depth) {
  if (!td) return;
  if (td->action == PARSLEY_LEX_PUSH && td->target_mode && td->target_mode[0] != '\0') {
    if (*depth >= PARSLEY_MAX_MODES) { p_fatal(2); return; }
    stack[(*depth)++] = td->target_mode;
  } else if (td->action == PARSLEY_LEX_POP && *depth > 1) {
    (*depth)--;
  }
}

static void p_lex_all(ParsleyParser* p) {
  size_t cap = 64;
  p->tokens = (ParsleyLexedToken*)p_malloc(sizeof(ParsleyLexedToken) * cap);
  p->token_count = 0;

  const char* modes[PARSLEY_MAX_MODES];
  modes[0] = "default";
  size_t mdepth = 1;

  size_t pos = 0;
  int ln = 1;
  int cl = 1;

  while (pos < p->len) {
    p_step();
    const ParsleyLexMode* mode = find_lex_mode(modes[mdepth - 1]);
    const ParsleyTokenDef* best_def = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < mode->count; ++i) {
      const ParsleyTokenDef* td = &mode->tokens[i];
      size_t mlen = 0;
      if (!token_match_at(td, p->input, p->len, pos, &mlen) || mlen == 0) continue;
      bool better = false;
      if (!best_def || mlen > best_len) {
        better = true;
      } else if (mlen == best_len) {
        if (td->priority > best_def->priority) {
          better = true;
        } else if (td->priority == best_def->priority && td->decl_order < best_def->decl_order) {
          better = true;
        }
      }
      if (better) {
        best_def = td;
        best_len = mlen;
      }
    }

    if (p->token_count + 2 >= cap) {
      cap *= 2;
      p->tokens = (ParsleyLexedToken*)realloc(p->tokens, sizeof(ParsleyLexedToken) * cap);
    }

    if (!best_def) {
      ParsleyLexedToken* t = &p->tokens[p->token_count++];
      t->id = "<lex_error>";
      t->def = NULL;
      size_t width = p_utf8_width(p->input+pos,p->len-pos);
      t->lexeme = p_strdup_n(p->input + pos, width);
      t->lexeme_len = width;
      t->line = ln;
      t->col = cl;
      t->start_offset = pos;
      t->is_eof = false;
      t->is_lex_error = true;
      advance_line_col(p->input + pos, width, &ln, &cl);
      pos+=width;
      continue;
    }

    if (!best_def->is_skip) {
      ParsleyLexedToken* t = &p->tokens[p->token_count++];
      t->id = best_def->id;
      t->def = best_def;
      t->lexeme = p_strdup_n(p->input + pos, best_len);
      t->lexeme_len = best_len;
      t->line = ln;
      t->col = cl;
      t->start_offset = pos;
      t->is_eof = false;
      t->is_lex_error = false;
    }

    advance_line_col(p->input + pos, best_len, &ln, &cl);
    pos += best_len;
    p_apply_mode_action(best_def, modes, &mdepth);
  }

  ParsleyLexedToken* eof_t = &p->tokens[p->token_count++];
  eof_t->id = "eof";
  eof_t->def = NULL;
  eof_t->lexeme = p_strdup("");
  eof_t->lexeme_len = 0;
  eof_t->line = ln;
  eof_t->col = cl;
  eof_t->start_offset = pos;
  eof_t->is_eof = true;
  eof_t->is_lex_error = false;
}

static void p_scannerless_skip(ParsleyParser* p) {
  while (p->char_pos < p->len) {
    const ParsleyLexMode* mode = find_lex_mode(p->mode_stack[p->mode_depth - 1]);
    const ParsleyTokenDef* best_skip = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < mode->count; ++i) {
      const ParsleyTokenDef* td = &mode->tokens[i];
      if (!td->is_skip) continue;
      size_t mlen = 0;
      if (token_match_at(td, p->input, p->len, p->char_pos, &mlen) && mlen > best_len) {
        best_skip = td;
        best_len = mlen;
      }
    }
    if (!best_skip) break;
    advance_line_col(p->input + p->char_pos, best_len, &p->line, &p->col);
    p->char_pos += best_len;
    p_apply_mode_action(best_skip, p->mode_stack, &p->mode_depth);
  }
}

static bool p_match_token(ParsleyParser* p, const char* token_id, ParsleyValue** out) {
  if (!p->scannerless) {
    if (p->tok_pos >= p->token_count) {
      p_record_failure(p, display_token_name(token_id), false);
      return false;
    }
    const ParsleyLexedToken* cur = &p->tokens[p->tok_pos];
    if (strcmp(token_id, "eof") == 0) {
      if (cur->is_eof) {
        *out = p_track(p, pv_unit());
        return true;
      }
      p_record_failure(p, "end of input", false);
      return false;
    }
    if (!cur->is_eof && !cur->is_lex_error && strcmp(cur->id, token_id) == 0) {
      *out = p_track(p, token_to_value(cur->def, cur->lexeme, cur->lexeme_len));
      p->tok_pos++;
      return true;
    }
    p_record_failure(p, display_token_name(token_id), false);
    return false;
  }

  p_scannerless_skip(p);
  if (strcmp(token_id, "eof") == 0) {
    if (p->char_pos >= p->len) {
      *out = p_track(p, pv_unit());
      return true;
    }
    p_record_failure(p, "end of input", false);
    return false;
  }

  const ParsleyLexMode* mode = find_lex_mode(p->mode_stack[p->mode_depth - 1]);
  const ParsleyTokenDef* target = NULL;
  for (size_t i = 0; i < mode->count; ++i) {
    if (!mode->tokens[i].is_skip && strcmp(mode->tokens[i].id, token_id) == 0) {
      target = &mode->tokens[i];
      break;
    }
  }
  if (!target) {
    target = find_token_def(token_id);
  }
  if (!target) {
    p_record_failure(p, display_token_name(token_id), false);
    return false;
  }
  size_t mlen = 0;
  if (!token_match_at(target, p->input, p->len, p->char_pos, &mlen) || mlen == 0) {
    p_record_failure(p, display_token_name(token_id), false);
    return false;
  }
  for (size_t i = 0; i < mode->count; ++i) {
    const ParsleyTokenDef* other = &mode->tokens[i];
    if (other == target || other->is_skip) continue;
    {
      size_t olen = 0;
      if (token_match_at(other, p->input, p->len, p->char_pos, &olen) &&
          (olen > mlen || (olen == mlen && (other->priority > target->priority ||
           (other->priority == target->priority && other->decl_order < target->decl_order))))) {
        p_record_failure(p, display_token_name(token_id), false);
        return false;
      }
    }
  }

  *out = p_track(p, token_to_value(target, p->input + p->char_pos, mlen));
  advance_line_col(p->input + p->char_pos, mlen, &p->line, &p->col);
  p->char_pos += mlen;
  p_apply_mode_action(target, p->mode_stack, &p->mode_depth);
  return true;
}

static bool p_match_exactly(ParsleyParser* p, const char* var_name, ParsleyValue** out) {
  const char* expected_text = NULL;
  size_t elen = 0;
  for (size_t i = p->ctx_depth; i > 0; --i) {
    if (strcmp(p->ctx_env[i - 1].name, var_name) == 0) {
      expected_text = p->ctx_env[i - 1].value;
      elen = p->ctx_env[i - 1].value_len;
      break;
    }
  }
  if (!expected_text) {
    p_record_failure(p, var_name, false);
    return false;
  }
  /* Binding length is explicit, including embedded NUL bytes. */
  if (!p->scannerless) {
    if (p->tok_pos < p->token_count && !p->tokens[p->tok_pos].is_eof &&
        p->tokens[p->tok_pos].lexeme_len == elen &&
        memcmp(p->tokens[p->tok_pos].lexeme, expected_text, elen) == 0) {
      *out = p_track(p, pv_string(expected_text, elen));
      p->tok_pos++;
      return true;
    }
    p_record_failure(p, expected_text, false);
    return false;
  }
  p_scannerless_skip(p);
  if (p->char_pos + elen <= p->len &&
      memcmp(p->input + p->char_pos, expected_text, elen) == 0) {
    *out = p_track(p, pv_string(expected_text, elen));
    advance_line_col(p->input + p->char_pos, elen, &p->line, &p->col);
    p->char_pos += elen;
    return true;
  }
  p_record_failure(p, expected_text, false);
  return false;
}

static void p_push_ctx(ParsleyParser* p, const char* name, const ParsleyValue* val) {
  if (p->ctx_depth >= PARSLEY_MAX_CTX) { p_fatal(2); return; }
  if (p->ctx_depth < PARSLEY_MAX_CTX && val && val->kind == PARSLEY_VAL_STRING && val->string_val) {
    p->ctx_env[p->ctx_depth].name = p_strdup(name);
    p->ctx_env[p->ctx_depth].value = p_strdup_n(val->string_val,val->string_len);
    p->ctx_env[p->ctx_depth].value_len = val->string_len;
    p->ctx_depth++;
  }
}

static ParsleyValue* p_prefix_get(ParsleyParser* p, int arg_index) {
  if (arg_index < 0) {
    ptrdiff_t pos = (ptrdiff_t)p->prefix_depth + (ptrdiff_t)arg_index;
    if (pos >= 0 && (size_t)pos < p->prefix_depth) {
      return p->prefix_stack[pos];
    }
  }
  return p_track(p, pv_unit());
}

static size_t p_memo_hash(size_t rule,size_t start) { return (start*2654435761u) ^ (rule*2246822519u); }
static ParsleyMemo* p_memo_get(ParsleyParser* p,size_t rule,size_t start) {
  if (!p->memo_cap || p->memo_count*4u >= p->memo_cap*3u) {
    size_t cap=p->memo_cap ? p->memo_cap*2u : 128u;
    ParsleyMemo* next=(ParsleyMemo*)p_calloc(cap,sizeof(ParsleyMemo));
    for(size_t i=0;i<p->memo_cap;++i)if(p->memo[i].used){size_t k=p_memo_hash(p->memo[i].rule,p->memo[i].start)&(cap-1u);while(next[k].used)k=(k+1u)&(cap-1u);next[k]=p->memo[i];}
    free(p->memo);p->memo=next;p->memo_cap=cap;
  }
  size_t k=p_memo_hash(rule,start)&(p->memo_cap-1u);
  while(p->memo[k].used){p_step();if(p->memo[k].rule==rule&&p->memo[k].start==start)return &p->memo[k];k=(k+1u)&(p->memo_cap-1u);}
  p->memo[k].used=true;p->memo[k].rule=rule;p->memo[k].start=start;++p->memo_count;return &p->memo[k];
}
)C23";
  }

  void emit_rule_forward_decls(std::ostringstream& out) {
    for (const auto& [rid, _] : g_.gRules) {
      out << "static bool " << rule_c_names_[rid] << "(ParsleyParser* p, ParsleyValue** out);\n";
    }
    for (std::size_t i = 0; i < prec_tables_.size(); ++i) {
      out << "static bool " << prec_fn_names_[prec_tables_[i]]
          << "(ParsleyParser* p, int min_prec, ParsleyValue** out);\n";
    }
    out << "\n";
  }

  std::string emit_action_expr(std::ostringstream& out,
                               const ir::ActionExpr& act,
                               const std::vector<std::string>& child_vars) {
    switch (act.kind) {
      case ir::ActionExpr::Kind::UnitLit:
        return "p_track(p, pv_unit())";
      case ir::ActionExpr::Kind::BoolLit:
        return std::string("p_track(p, pv_bool(") + (act.bool_value ? "true" : "false") + "))";
      case ir::ActionExpr::Kind::IntLit: {
        std::ostringstream s;
        if (!act.name.empty()) s << "p_track(p, pv_integer_literal(" << c_string_lit(act.name) << "))";
        else s << "p_track(p, pv_number(" << static_cast<long long>(act.number_value) << ".0, true))";
        return s.str();
      }
      case ir::ActionExpr::Kind::FloatLit: {
        std::ostringstream s;
        s << "p_track(p, pv_number(" << std::setprecision(17) << act.number_value << ", false))";
        return s.str();
      }
      case ir::ActionExpr::Kind::StringLit:
        return "p_track(p, pv_string(" + c_string_lit(act.name) + ", " +
               std::to_string(act.name.size()) + "))";
      case ir::ActionExpr::Kind::Arg:
      case ir::ActionExpr::Kind::Identity: {
        if (act.arg_index >= 0) {
          const std::size_t idx = static_cast<std::size_t>(act.arg_index);
          if (idx < child_vars.size()) {
            return child_vars[idx];
          }
          return "p_track(p, pv_unit())";
        }
        return "p_prefix_get(p, " + std::to_string(act.arg_index) + ")";
      }
      case ir::ActionExpr::Kind::List: {
        if (act.name == ":" && act.args.size() == 2) {
          std::string h = emit_action_expr(out, act.args[0], child_vars);
          std::string t = emit_action_expr(out, act.args[1], child_vars);
          return "p_track(p, pv_list_cons(" + h + ", " + t + "))";
        }
        if (act.args.empty()) {
          return "p_track(p, pv_list(NULL, 0))";
        }
        std::string arr = next_var("larr");
        std::vector<std::string> elems;
        for (const auto& a : act.args) {
          elems.push_back(emit_action_expr(out, a, child_vars));
        }
        out << "    ParsleyValue* " << arr << "[" << elems.size() << "] = {";
        for (std::size_t i = 0; i < elems.size(); ++i) {
          if (i > 0) out << ", ";
          out << elems[i];
        }
        out << "};\n";
        return "p_track(p, pv_list(" + arr + ", " + std::to_string(elems.size()) + "))";
      }
      case ir::ActionExpr::Kind::Tuple: {
        if (act.args.empty()) {
          return "p_track(p, pv_tuple(NULL, 0))";
        }
        std::string arr = next_var("tarr");
        std::vector<std::string> elems;
        for (const auto& a : act.args) {
          elems.push_back(emit_action_expr(out, a, child_vars));
        }
        out << "    ParsleyValue* " << arr << "[" << elems.size() << "] = {";
        for (std::size_t i = 0; i < elems.size(); ++i) {
          if (i > 0) out << ", ";
          out << elems[i];
        }
        out << "};\n";
        return "p_track(p, pv_tuple(" + arr + ", " + std::to_string(elems.size()) + "))";
      }
      case ir::ActionExpr::Kind::Ctor: {
        if (act.args.empty()) {
          return "p_track(p, pv_ctor(" + c_string_lit(act.name) + ", NULL, 0))";
        }
        std::string arr = next_var("carr");
        std::vector<std::string> elems;
        for (const auto& a : act.args) {
          elems.push_back(emit_action_expr(out, a, child_vars));
        }
        out << "    ParsleyValue* " << arr << "[" << elems.size() << "] = {";
        for (std::size_t i = 0; i < elems.size(); ++i) {
          if (i > 0) out << ", ";
          out << elems[i];
        }
        out << "};\n";
        return "p_track(p, pv_ctor(" + c_string_lit(act.name) + ", " + arr + ", " +
               std::to_string(elems.size()) + "))";
      }
    }
    return "p_track(p, pv_unit())";
  }

  // Emits code that attempts to parse `prod`, storing result in `res_var` and jumping to `fail_lbl` on failure.
  void emit_prod(std::ostringstream& out,
                 const ir::ProdPtr& prod,
                 const std::string& res_var,
                 const std::string& fail_lbl,
                 int indent = 2) {
    std::string ind(static_cast<std::size_t>(indent), ' ');
    if (!prod || prod->kind == ir::Prod::Kind::Eps) {
      out << ind << res_var << " = p_track(p, pv_unit());\n";
      return;
    }

    switch (prod->kind) {
      case ir::Prod::Kind::Eps:
        out << ind << res_var << " = p_track(p, pv_unit());\n";
        break;

      case ir::Prod::Kind::Tok:
        out << ind << "if (!p_match_token(p, " << c_string_lit(prod->token_id) << ", &"
            << res_var << ")) goto " << fail_lbl << ";\n";
        break;

      case ir::Prod::Kind::Ref: {
        auto it = rule_c_names_.find(prod->rule_id);
        if (it == rule_c_names_.end()) {
          out << ind << "p_record_failure(p, " << c_string_lit(prod->rule_id)
              << ", false); goto " << fail_lbl << ";\n";
        } else {
          out << ind << "if (!" << it->second << "(p, &" << res_var << ")) goto " << fail_lbl
              << ";\n";
        }
        break;
      }

      case ir::Prod::Kind::PrecTable: {
        const std::string& fn = prec_fn_names_[prod.get()];
        out << ind << "if (!" << fn << "(p, 0, &" << res_var << ")) goto " << fail_lbl << ";\n";
        break;
      }

      case ir::Prod::Kind::Seq: {
        if (prod->items.empty()) {
          out << ind << res_var << " = p_track(p, pv_unit());\n";
          break;
        }
        const bool is_factored =
            prod->items.size() >= 2 && prod->items.back() &&
            prod->items.back()->kind == ir::Prod::Kind::Alt;
        const std::size_t prefix_count = is_factored ? (prod->items.size() - 1) : 0;
        std::string saved_pref = next_var("spref");
        std::string seq_fail = next_var("seq_fail");
        if (prefix_count > 0) {
          out << ind << "size_t " << saved_pref << " = p->prefix_depth;\n";
        }
        for (std::size_t i = 0; i < prod->items.size(); ++i) {
          std::string iv = next_var("si");
          out << ind << "ParsleyValue* " << iv << " = NULL;\n";
          emit_prod(out, prod->items[i], iv, prefix_count > 0 ? seq_fail : fail_lbl, indent);
          if (i < prefix_count) {
            out << ind << "if (p->prefix_depth >= PARSLEY_MAX_PREFIX) p_fatal(2);\n";
            out << ind << "p->prefix_stack[p->prefix_depth++] = "
                << iv << ";\n";
          }
          if (i + 1 == prod->items.size()) {
            out << ind << res_var << " = " << iv << ";\n";
          }
        }
        if (prefix_count > 0) {
          std::string seq_ok = next_var("seq_ok");
          out << ind << "p->prefix_depth = " << saved_pref << ";\n";
          out << ind << "goto " << seq_ok << ";\n";
          out << seq_fail << ":\n";
          out << ind << "p->prefix_depth = " << saved_pref << ";\n";
          out << ind << "goto " << fail_lbl << ";\n";
          out << seq_ok << ":\n";
        }
        break;
      }

      case ir::Prod::Kind::Alt: {
        if (prod->items.empty()) {
          out << ind << "goto " << fail_lbl << ";\n";
          break;
        }
        std::string saved = next_var("alt_save");
        std::string ok_lbl = next_var("alt_ok");
        out << ind << "ParsleyCursor " << saved << " = p_save(p);\n";
        for (std::size_t i = 0; i < prod->items.size(); ++i) {
          std::string next_alt = next_var("alt_next");
          out << ind << "{\n";
          emit_prod(out, prod->items[i], res_var, next_alt, indent + 2);
          out << ind << "  goto " << ok_lbl << ";\n";
          out << ind << "}\n";
          out << next_alt << ":\n";
          out << ind << "p_restore(p, &" << saved << ");\n";
        }
        out << ind << "goto " << fail_lbl << ";\n";
        out << ok_lbl << ":\n";
        break;
      }

      case ir::Prod::Kind::Rep: {
        std::string vec = next_var("rvec");
        std::string cap = next_var("rcap");
        std::string cnt = next_var("rcnt");
        std::string loop_end = next_var("rep_end");
        std::string iter_fail = next_var("rep_ifail");
        out << ind << "size_t " << cap << " = 8, " << cnt << " = 0;\n";
        out << ind << "ParsleyValue** " << vec << " = (ParsleyValue**)p_malloc(sizeof(ParsleyValue*) * "
            << cap << ");\n";
        out << ind << "while (";
        if (prod->max_rep) {
          out << "(int)" << cnt << " < " << *prod->max_rep;
        } else {
          out << "1";
        }
        out << ") {\n";
        std::string rep_save = next_var("rsave");
        std::string rep_off = next_var("roff");
        std::string rep_item = next_var("ritem");
        out << ind << "  ParsleyCursor " << rep_save << " = p_save(p);\n";
        out << ind << "  size_t " << rep_off << " = p_current_offset(p);\n";
        out << ind << "  ParsleyValue* " << rep_item << " = NULL;\n";
        emit_prod(out, prod->inner, rep_item, iter_fail, indent + 2);
        out << ind << "  if (" << cnt << " >= " << cap << ") { " << cap << " *= 2; " << vec
            << " = (ParsleyValue**)realloc(" << vec << ", sizeof(ParsleyValue*) * " << cap
            << "); }\n";
        out << ind << "  " << vec << "[" << cnt << "++] = " << rep_item << ";\n";
        out << ind << "  if (p_current_offset(p) == " << rep_off << ") break;\n";
        out << ind << "  continue;\n";
        out << iter_fail << ":\n";
        out << ind << "  p_restore(p, &" << rep_save << ");\n";
        out << ind << "  break;\n";
        out << ind << "}\n";
        out << loop_end << ":;\n";
        if (prod->min_rep > 0) {
          out << ind << "if ((int)" << cnt << " < " << prod->min_rep << ") { free(" << vec
              << "); goto " << fail_lbl << "; }\n";
        }
        out << ind << res_var << " = p_track(p, pv_list(" << vec << ", " << cnt << "));\n";
        out << ind << "free(" << vec << ");\n";
        break;
      }

      case ir::Prod::Kind::Look: {
        std::string lsave = next_var("lsave");
        std::string lsup = next_var("lsup");
        std::string lval = next_var("lval");
        std::string lfail = next_var("lfail");
        std::string ldone = next_var("ldone");
        out << ind << "ParsleyCursor " << lsave << " = p_save(p);\n";
        out << ind << "bool " << lsup << " = p->suppress_err;\n";
        if (prod->polarity == ir::Polarity::Negative) {
          out << ind << "p->suppress_err = true;\n";
        }
        out << ind << "ParsleyValue* " << lval << " = NULL;\n";
        emit_prod(out, prod->inner, lval, lfail, indent);
        out << ind << "(void)" << lval << ";\n";
        out << ind << "p->suppress_err = " << lsup << ";\n";
        out << ind << "p_restore(p, &" << lsave << ");\n";
        if (prod->polarity == ir::Polarity::Positive) {
          out << ind << res_var << " = p_track(p, pv_unit());\n";
          out << ind << "goto " << ldone << ";\n";
          out << lfail << ":\n";
          out << ind << "p->suppress_err = " << lsup << ";\n";
          out << ind << "p_restore(p, &" << lsave << ");\n";
          out << ind << "goto " << fail_lbl << ";\n";
        } else {
          out << ind << "p_record_failure(p, \"not followed by lookahead\", false);\n";
          out << ind << "goto " << fail_lbl << ";\n";
          out << lfail << ":\n";
          out << ind << "p->suppress_err = " << lsup << ";\n";
          out << ind << "p_restore(p, &" << lsave << ");\n";
          out << ind << res_var << " = p_track(p, pv_unit());\n";
        }
        out << ldone << ":;\n";
        break;
      }

      case ir::Prod::Kind::Act: {
        std::vector<std::string> child_vars;
        for (std::size_t i = 0; i < prod->items.size(); ++i) {
          std::string cv = next_var("act_c");
          out << ind << "ParsleyValue* " << cv << " = NULL;\n";
          emit_prod(out, prod->items[i], cv, fail_lbl, indent);
          child_vars.push_back(cv);
        }
        std::string built = emit_action_expr(out, prod->action, child_vars);
        out << ind << res_var << " = " << built << ";\n";
        break;
      }

      case ir::Prod::Kind::Ctx: {
        if (!prod->ctx_var.empty()) {
          out << ind << "if (!p_match_exactly(p, " << c_string_lit(prod->ctx_var) << ", &"
              << res_var << ")) goto " << fail_lbl << ";\n";
          break;
        }
        std::string csave = next_var("csave");
        std::string cfail = next_var("cfail");
        std::string cok = next_var("cok");
        out << ind << "ParsleyCursor " << csave << " = p_save(p);\n";
        std::vector<std::string> step_vars;
        for (const auto& step : prod->ctx_steps) {
          std::string sv = next_var("cstep");
          out << ind << "ParsleyValue* " << sv << " = NULL;\n";
          emit_prod(out, step.prod, sv, cfail, indent);
          if (!step.binder.empty()) {
            out << ind << "p_push_ctx(p, " << c_string_lit(step.binder) << ", " << sv << ");\n";
          }
          step_vars.push_back(sv);
        }
        std::string built = emit_action_expr(out, prod->action, step_vars);
        out << ind << res_var << " = " << built << ";\n";
        out << ind << "while (p->ctx_depth > " << csave << ".ctx_depth) { "
            << "p->ctx_depth--; free(p->ctx_env[p->ctx_depth].name); "
            << "free(p->ctx_env[p->ctx_depth].value); }\n";
        out << ind << "goto " << cok << ";\n";
        out << cfail << ":\n";
        out << ind << "p_restore(p, &" << csave << ");\n";
        out << ind << "goto " << fail_lbl << ";\n";
        out << cok << ":;\n";
        break;
      }

      case ir::Prod::Kind::Label: {
        std::string loff = next_var("loff");
        std::string lfail = next_var("lbl_fail");
        std::string lok = next_var("lbl_ok");
        out << ind << "size_t " << loff << " = p_current_offset(p);\n";
        emit_prod(out, prod->inner, res_var, lfail, indent);
        out << ind << "goto " << lok << ";\n";
        out << lfail << ":\n";
        out << ind << "if (p_current_offset(p) == " << loff << ") p_record_failure(p, "
            << c_string_lit(prod->label) << ", true);\n";
        out << ind << "goto " << fail_lbl << ";\n";
        out << lok << ":;\n";
        break;
      }

      case ir::Prod::Kind::Recover: {
        std::string rfail = next_var("rec_fail");
        std::string rok = next_var("rec_ok");
        emit_prod(out, prod->inner, res_var, rfail, indent);
        out << ind << "goto " << rok << ";\n";
        out << rfail << ":\n";
        out << ind << "while (1) {\n";
        std::string ssave = next_var("ssave");
        std::string ssup = next_var("ssup");
        std::string sval = next_var("sval");
        std::string sfail = next_var("sfail");
        out << ind << "  ParsleyCursor " << ssave << " = p_save(p);\n";
        out << ind << "  bool " << ssup << " = p->suppress_err;\n";
        out << ind << "  p->suppress_err = true;\n";
        out << ind << "  ParsleyValue* " << sval << " = NULL;\n";
        emit_prod(out, prod->sync, sval, sfail, indent + 2);
        out << ind << "  (void)" << sval << ";\n";
        out << ind << "  p->suppress_err = " << ssup << ";\n";
        out << ind << "  " << res_var << " = p_track(p, pv_unit());\n";
        out << ind << "  goto " << rok << ";\n";
        out << sfail << ":\n";
        out << ind << "  p->suppress_err = " << ssup << ";\n";
        out << ind << "  p_restore(p, &" << ssave << ");\n";
        out << ind << "  if (!p->scannerless) {\n";
        out << ind << "    if (p->tok_pos >= p->token_count || p->tokens[p->tok_pos].is_eof) goto "
            << fail_lbl << ";\n";
        out << ind << "    p->tok_pos++;\n";
        out << ind << "  } else {\n";
        out << ind << "    if (p->char_pos >= p->len) goto " << fail_lbl << ";\n";
        out << ind << "    advance_line_col(p->input + p->char_pos, 1, &p->line, &p->col);\n";
        out << ind << "    p->char_pos++;\n";
        out << ind << "  }\n";
        out << ind << "}\n";
        out << rok << ":;\n";
        break;
      }
    }
  }

  void emit_prec_table_functions(std::ostringstream& out) {
    for (const ir::Prod* pt : prec_tables_) {
      const std::string& fn = prec_fn_names_[pt];
      out << "/* Precedence-climbing parser for operators table */\n";
      out << "static bool " << fn << "_impl(ParsleyParser* p, int min_prec, ParsleyValue** out);\n";
      out << "static bool " << fn << "(ParsleyParser* p, int min_prec, ParsleyValue** out) { p_enter(); bool ok = " << fn << "_impl(p,min_prec,out); p_leave(); return ok; }\n";
      out << "static bool " << fn << "_impl(ParsleyParser* p, int min_prec, ParsleyValue** out) {\n";
      out << "  size_t start_off = p_current_offset(p);\n";
      out << "  ParsleyValue* lhs = NULL;\n\n";

      // Prefix operators (highest precedence first)
      for (std::size_t li = pt->levels.size(); li > 0; --li) {
        const auto& lvl = pt->levels[li - 1];
        if (lvl.assoc != ast::AssocKind::Prefix) {
          continue;
        }
        for (const auto& op : lvl.ops) {
          if (op.pattern.empty()) {
            continue;
          }
          std::string pfail = next_var("pref_fail");
          std::string psave = next_var("pref_save");
          out << "  if (!lhs) {\n";
          out << "    ParsleyCursor " << psave << " = p_save(p);\n";
          for (std::size_t pi = 0; pi < op.pattern.size(); ++pi) {
            std::string pv = next_var("ppat");
            out << "    ParsleyValue* " << pv << " = NULL;\n";
            emit_prod(out, op.pattern[pi], pv, pfail, 4);
            out << "    (void)" << pv << ";\n";
          }
          out << "    ParsleyValue* operand = NULL;\n";
          out << "    if (!" << fn << "(p, " << lvl.precedence << ", &operand)) {\n";
          out << "      p_restore(p, &" << psave << ");\n";
          out << "      return false;\n";
          out << "    }\n";
          std::string built = emit_action_expr(out, op.action, {"operand"});
          out << "    lhs = " << built << ";\n";
          std::string pok = next_var("pref_ok");
          out << "    goto " << pok << ";\n";
          out << pfail << ":\n";
          out << "    p_restore(p, &" << psave << ");\n";
          out << pok << ":;\n";
          out << "  }\n";
        }
      }

      // Base atom fallback
      out << "  if (!lhs) {\n";
      std::string atom_fail = next_var("atom_fail");
      std::string atom_ok = next_var("atom_ok");
      emit_prod(out, pt->inner, "lhs", atom_fail, 4);
      out << "    goto " << atom_ok << ";\n";
      out << atom_fail << ":\n";
      out << "    if (p_current_offset(p) == start_off) p_record_failure(p, \"expression\", true);\n";
      out << "    return false;\n";
      out << atom_ok << ":;\n";
      out << "  }\n\n";

      // Infix / Postfix / Mixfix climbing loop
      out << "  while (1) {\n";
      out << "    bool advanced = false;\n";
      for (std::size_t li = pt->levels.size(); li > 0; --li) {
        const auto& lvl = pt->levels[li - 1];
        if (lvl.assoc == ast::AssocKind::Prefix) {
          continue;
        }
        for (const auto& op : lvl.ops) {
          if (op.pattern.empty()) {
            continue;
          }
          std::string ifail = next_var("inf_fail");
          std::string isave = next_var("inf_save");
          std::string iok = next_var("inf_ok");
          out << "    if (!advanced && " << lvl.precedence << " >= min_prec) {\n";
          out << "      ParsleyCursor " << isave << " = p_save(p);\n";
          std::vector<std::string> captured = {"lhs"};
          for (std::size_t pi = 0; pi < op.pattern.size(); ++pi) {
            std::string pv = next_var("ipat");
            out << "      ParsleyValue* " << pv << " = NULL;\n";
            emit_prod(out, op.pattern[pi], pv, ifail, 6);
            if (op.pattern[pi] && op.pattern[pi]->kind != ir::Prod::Kind::Tok) {
              captured.push_back(pv);
            } else {
              out << "      (void)" << pv << ";\n";
            }
          }
          if (lvl.assoc == ast::AssocKind::Postfix) {
            std::string built = emit_action_expr(out, op.action, captured);
            out << "      lhs = " << built << ";\n";
            out << "      advanced = true;\n";
            out << "      goto " << iok << ";\n";
          } else {
            const int next_min = (lvl.assoc == ast::AssocKind::InfixR ||
                                  lvl.assoc == ast::AssocKind::Mixfix)
                                     ? lvl.precedence
                                     : (lvl.precedence + 1);
            std::string rhs_var = next_var("rhs");
            out << "      ParsleyValue* " << rhs_var << " = NULL;\n";
            out << "      if (!" << fn << "(p, " << next_min << ", &" << rhs_var
                << ")) return false;\n";
            captured.push_back(rhs_var);
            std::string built = emit_action_expr(out, op.action, captured);
            out << "      lhs = " << built << ";\n";
            if (lvl.assoc == ast::AssocKind::Infix) {
              out << "      *out = lhs;\n";
              out << "      return true;\n";
            } else {
              out << "      advanced = true;\n";
              out << "      goto " << iok << ";\n";
            }
          }
          out << ifail << ":\n";
          out << "      p_restore(p, &" << isave << ");\n";
          out << iok << ":;\n";
          out << "    }\n";
        }
      }
      out << "    if (!advanced) break;\n";
      out << "  }\n";
      out << "  *out = lhs;\n";
      out << "  return true;\n";
      out << "}\n\n";
    }
  }

  void emit_rule_functions(std::ostringstream& out) {
    out << "static ParsleyLrEntry* p_lr_find_or_insert(ParsleyParser* p, const char* rule_id, size_t off) {\n";
    out << "  for (size_t i = 0; i < p->lr_count; ++i) {\n";
    out << "    if (p->lr_entries[i].start_off == off && strcmp(p->lr_entries[i].rule_id, rule_id) == 0) {\n";
    out << "      return &p->lr_entries[i];\n";
    out << "    }\n";
    out << "  }\n";
    out << "  if (p->lr_count >= p->lr_cap) {\n";
    out << "    size_t nc = p->lr_cap == 0 ? 32 : p->lr_cap * 2;\n";
    out << "    p->lr_entries = (ParsleyLrEntry*)realloc(p->lr_entries, sizeof(ParsleyLrEntry) * nc);\n";
    out << "    p->lr_cap = nc;\n";
    out << "  }\n";
    out << "  ParsleyLrEntry* e = &p->lr_entries[p->lr_count++];\n";
    out << "  memset(e, 0, sizeof(ParsleyLrEntry));\n";
    out << "  e->rule_id = rule_id;\n";
    out << "  e->start_off = off;\n";
    out << "  return e;\n";
    out << "}\n\n";

    out << "static void p_lr_remove(ParsleyParser* p, const char* rule_id, size_t off) {\n";
    out << "  for (size_t i = 0; i < p->lr_count; ++i) {\n";
    out << "    if (p->lr_entries[i].start_off == off && strcmp(p->lr_entries[i].rule_id, rule_id) == 0) {\n";
    out << "      p->lr_entries[i] = p->lr_entries[--p->lr_count];\n";
    out << "      return;\n";
    out << "    }\n";
    out << "  }\n";
    out << "}\n\n";

    for (const auto& [rid, rule] : g_.gRules) {
      auto ra_it = analysis_.rules.find(rid);
      out << "/* Rule `" << safe_comment(rid) << "` :: "
          << safe_comment(ir::type_to_string(rule.rType));
      if (ra_it != analysis_.rules.end()) {
        out << " | nullable=" << (ra_it->second.nullable ? "true" : "false");
        out << " | FIRST={";
        bool first = true;
        for (const auto& t : ra_it->second.first) {
          if (!first) out << ", ";
          out << safe_comment(t);
          first = false;
        }
        out << "}";
      }
      out << " */\n";

      const bool may_lr = (ra_it != analysis_.rules.end() && ra_it->second.left_recursive);
      const std::string& fn = rule_c_names_[rid];
      out << "static bool " << fn << "_impl(ParsleyParser* p, ParsleyValue** out);\n";
      const bool memoize = !analysis_.has_context_rules && !rule.is_generated &&
          std::none_of(analysis_.rules.begin(),analysis_.rules.end(),[](const auto& item){return item.second.left_recursive;});
      if (memoize) {
        const auto key = static_cast<std::size_t>(std::distance(g_.gRules.begin(),g_.gRules.find(rid)))+1;
        out << "static bool " << fn << "(ParsleyParser* p, ParsleyValue** out) {\n  p_enter(); size_t start=p->tok_pos;\n";
        out << "  ParsleyMemo* memo=p_memo_get(p," << key << ",start);\n";
        out << "  if(memo->complete){p->tok_pos=memo->end;*out=memo->value;bool ok=memo->ok;p_leave();return ok;}\n";
        out << "  bool ok=" << fn << "_impl(p,out);memo=p_memo_get(p," << key << ",start);\n";
        out << "  memo->complete=true;memo->ok=ok;memo->end=p->tok_pos;memo->value=ok?*out:NULL;p_leave();return ok;\n}\n";
      } else out << "static bool " << fn << "(ParsleyParser* p, ParsleyValue** out) { p_enter(); bool ok = " << fn << "_impl(p,out); p_leave(); return ok; }\n";
      out << "static bool " << fn << "_impl(ParsleyParser* p, ParsleyValue** out) {\n";

      if (may_lr) {
        out << "  size_t start_off = p_current_offset(p);\n";
        out << "  ParsleyLrEntry* lr = p_lr_find_or_insert(p, " << c_string_lit(rid)
            << ", start_off);\n";
        out << "  if (lr->active) {\n";
        out << "    lr->hit_recursion = true;\n";
        out << "    if (!lr->ok) return false;\n";
        out << "    p->tok_pos = lr->end_tok_pos;\n";
        out << "    p->char_pos = lr->end_char_pos;\n";
        out << "    p->line = lr->end_line;\n";
        out << "    p->col = lr->end_col;\n";
        out << "    p->mode_depth = lr->end_mode_depth;\n";
        out << "    for (size_t i = 0; i < lr->end_mode_depth; ++i) p->mode_stack[i] = lr->end_modes[i];\n";
        out << "    *out = lr->value;\n";
        out << "    return true;\n";
        out << "  }\n";
        out << "  ParsleyCursor entry = p_save(p);\n";
        out << "  size_t saved_pref = p->prefix_depth;\n";
        out << "  lr->active = true;\n";
        out << "  lr->hit_recursion = false;\n";
        out << "  lr->ok = false;\n";
        out << "  ParsleyValue* res = NULL;\n";
        out << "  bool ok = true;\n";
        std::string body_fail = next_var("lr_bfail");
        std::string body_done = next_var("lr_bdone");
        emit_prod(out, rule.rBody, "res", body_fail, 2);
        out << "  goto " << body_done << ";\n";
        out << body_fail << ":\n";
        out << "  ok = false;\n";
        out << body_done << ":;\n";
        out << "  lr = p_lr_find_or_insert(p, " << c_string_lit(rid) << ", start_off);\n";
        out << "  if (!lr->hit_recursion) {\n";
        out << "    p_lr_remove(p, " << c_string_lit(rid) << ", start_off);\n";
        out << "    p->prefix_depth = saved_pref;\n";
        out << "    if (ok) { *out = res; return true; }\n";
        out << "    return false;\n";
        out << "  }\n";
        out << "  while (ok) {\n";
        out << "    size_t cur_u = p->scannerless ? p->char_pos : p->tok_pos;\n";
        out << "    size_t prev_u = lr->ok ? (p->scannerless ? lr->end_char_pos : lr->end_tok_pos) : 0;\n";
        out << "    if (lr->ok && cur_u <= prev_u) break;\n";
        out << "    lr->ok = true;\n";
        out << "    lr->value = res;\n";
        out << "    lr->end_tok_pos = p->tok_pos;\n";
        out << "    lr->end_char_pos = p->char_pos;\n";
        out << "    lr->end_line = p->line;\n";
        out << "    lr->end_col = p->col;\n";
        out << "    lr->end_mode_depth = p->mode_depth;\n";
        out << "    for (size_t i = 0; i < p->mode_depth; ++i) lr->end_modes[i] = p->mode_stack[i];\n";
        out << "    p_restore(p, &entry);\n";
        out << "    p->prefix_depth = saved_pref;\n";
        out << "    res = NULL;\n";
        out << "    ok = true;\n";
        std::string gfail = next_var("lr_gfail");
        std::string gdone = next_var("lr_gdone");
        emit_prod(out, rule.rBody, "res", gfail, 4);
        out << "    goto " << gdone << ";\n";
        out << gfail << ":\n";
        out << "    ok = false;\n";
        out << gdone << ":;\n";
        out << "    lr = p_lr_find_or_insert(p, " << c_string_lit(rid) << ", start_off);\n";
        out << "  }\n";
        out << "  p->prefix_depth = saved_pref;\n";
        out << "  if (lr->ok) {\n";
        out << "    *out = lr->value;\n";
        out << "    p->tok_pos = lr->end_tok_pos;\n";
        out << "    p->char_pos = lr->end_char_pos;\n";
        out << "    p->line = lr->end_line;\n";
        out << "    p->col = lr->end_col;\n";
        out << "    p->mode_depth = lr->end_mode_depth;\n";
        out << "    for (size_t i = 0; i < lr->end_mode_depth; ++i) p->mode_stack[i] = lr->end_modes[i];\n";
        out << "    p_lr_remove(p, " << c_string_lit(rid) << ", start_off);\n";
        out << "    return true;\n";
        out << "  }\n";
        out << "  p_lr_remove(p, " << c_string_lit(rid) << ", start_off);\n";
        out << "  return false;\n";
      } else {
        out << "  size_t saved_pref = p->prefix_depth;\n";
        out << "  ParsleyValue* res = NULL;\n";
        std::string rfail = next_var("rule_fail");
        emit_prod(out, rule.rBody, "res", rfail, 2);
        out << "  p->prefix_depth = saved_pref;\n";
        out << "  *out = res;\n";
        out << "  return true;\n";
        out << rfail << ":\n";
        out << "  p->prefix_depth = saved_pref;\n";
        out << "  return false;\n";
      }
      out << "}\n\n";
    }
  }

  void emit_field_conv(std::ostringstream& out,
                       const ir::Type& t,
                       const std::string& dst,
                       const std::string& src_pv,
                       int indent) {
    std::string ind(static_cast<std::size_t>(indent), ' ');
    if (t.kind == ir::Type::Kind::Named || t.kind == ir::Type::Kind::Var) {
      if (t.name == "Text" || t.name == "String") {
        out << ind << dst << " = (" << src_pv << " && " << src_pv << "->string_val) ? p_strdup("
            << src_pv << "->string_val) : p_strdup(\"\");\n";
        return;
      }
      if (t.name == "Double" || t.name == "Float") {
        out << ind << dst << " = (" << src_pv << ") ? " << src_pv << "->number_val : 0.0;\n";
        return;
      }
      if (t.name == "Int") {
        out << ind << dst << " = (" << src_pv << ") ? " << src_pv << "->integer_val : 0;\n";
        return;
      }
      if (t.name == "Bool") {
        out << ind << dst << " = (" << src_pv << ") ? " << src_pv << "->bool_val : false;\n";
        return;
      }
      if (info_.data_map.contains(t.name)) {
        const std::string fn = "pv_to_" + sanitize_c_ident(t.name);
        if (info_.enum_only_types.contains(t.name)) {
          out << ind << "{\n";
          out << ind << "  " << sanitize_c_ident(t.name) << "* tmp = " << fn << "(" << src_pv
              << ");\n";
          out << ind << "  if (tmp) { " << dst << " = *tmp; free(tmp); }\n";
          out << ind << "}\n";
        } else {
          out << ind << dst << " = " << fn << "(" << src_pv << ");\n";
        }
        return;
      }
    }
    if (t.kind == ir::Type::Kind::List && !t.args.empty()) {
      const auto& elem = t.args[0];
      const std::string lvar = next_var("lsrc");
      out << ind << "const ParsleyValue* " << lvar << " = " << src_pv << ";\n";
      out << ind << "if (" << lvar << " && " << lvar << "->kind == PARSLEY_VAL_LIST && " << lvar
          << "->item_count > 0) {\n";
      out << ind << "  " << dst << ".len = " << lvar << "->item_count;\n";
      if (elem.name == "Text" || elem.name == "String") {
        out << ind << "  " << dst << ".items = (char**)p_calloc(" << dst << ".len, sizeof(char*));\n";
      } else if (elem.name == "Double") {
        out << ind << "  " << dst << ".items = (double*)p_calloc(" << dst << ".len, sizeof(double));\n";
      } else if (elem.name == "Int") {
        out << ind << "  " << dst << ".items = (int64_t*)p_calloc(" << dst << ".len, sizeof(int64_t));\n";
      } else if (elem.name == "Bool") {
        out << ind << "  " << dst << ".items = (bool*)p_calloc(" << dst << ".len, sizeof(bool));\n";
      } else if (info_.data_map.contains(elem.name)) {
        const std::string ec = sanitize_c_ident(elem.name);
        if (info_.enum_only_types.contains(elem.name)) {
          out << ind << "  " << dst << ".items = (" << ec << "*)p_calloc(" << dst << ".len, sizeof("
              << ec << "));\n";
        } else {
          out << ind << "  " << dst << ".items = (" << ec << "**)p_calloc(" << dst
              << ".len, sizeof(" << ec << "*));\n";
        }
      } else {
        out << ind << "  " << dst << ".items = (ParsleyValue**)p_calloc(" << dst
            << ".len, sizeof(ParsleyValue*));\n";
      }
      std::string idx = next_var("li");
      out << ind << "  for (size_t " << idx << " = 0; " << idx << " < " << dst << ".len; ++" << idx
          << ") {\n";
      if (elem.name == "Text" || elem.name == "String" || elem.name == "Double" ||
          elem.name == "Int" || elem.name == "Bool" || info_.data_map.contains(elem.name)) {
        emit_field_conv(out, elem, dst + ".items[" + idx + "]", lvar + "->items[" + idx + "]",
                        indent + 4);
      } else {
        out << ind << "    " << dst << ".items[" << idx << "] = pv_clone(" << lvar
            << "->items[" << idx << "]);\n";
      }
      out << ind << "  }\n";
      out << ind << "}\n";
      return;
    }
    out << ind << dst << " = pv_clone(" << src_pv << ");\n";
  }

  void emit_field_free(std::ostringstream& out,
                       const ir::Type& t,
                       const std::string& src,
                       int indent) {
    std::string ind(static_cast<std::size_t>(indent), ' ');
    if (t.kind == ir::Type::Kind::Named || t.kind == ir::Type::Kind::Var) {
      if (t.name == "Text" || t.name == "String") {
        out << ind << "free(" << src << ");\n";
        return;
      }
      if (t.name == "Double" || t.name == "Float" || t.name == "Int" || t.name == "Bool" ||
          t.name == "Unit" || t.name == "()") {
        return;
      }
      if (info_.data_map.contains(t.name)) {
        if (!info_.enum_only_types.contains(t.name)) {
          out << ind << "free_" << sanitize_c_ident(t.name) << "(" << src << ");\n";
        }
        return;
      }
      out << ind << "pv_free(" << src << ");\n";
      return;
    }
    if (t.kind == ir::Type::Kind::List && !t.args.empty()) {
      const auto& elem = t.args[0];
      std::string idx = next_var("fi");
      out << ind << "for (size_t " << idx << " = 0; " << idx << " < " << src << ".len; ++" << idx
          << ") {\n";
      emit_field_free(out, elem, src + ".items[" + idx + "]", indent + 2);
      out << ind << "}\n";
      out << ind << "free(" << src << ".items);\n";
      return;
    }
    out << ind << "pv_free(" << src << ");\n";
  }

  void emit_adt_converters(std::ostringstream& out) {
    // Forward declare converters and destructors
    for (const auto& dd : g_.gTypes) {
      if (dd.is_foreign) continue;
      const std::string cname = sanitize_c_ident(dd.name);
      out << "static " << cname << "* pv_to_" << cname << "(const ParsleyValue* v);\n";
      out << "static void free_" << cname << "(" << cname << "* node);\n";
    }
    if (!g_.gTypes.empty()) {
      out << "\n";
    }

    for (const auto& dd : g_.gTypes) {
      if (dd.is_foreign) continue;
      const std::string cname = sanitize_c_ident(dd.name);
      const std::string upper = to_upper_ascii(dd.name);

      // Converter
      out << "static " << cname << "* pv_to_" << cname << "(const ParsleyValue* v) {\n";
      out << "  if (!v || v->kind != PARSLEY_VAL_CTOR || !v->ctor_name) return NULL;\n";
      out << "  " << cname << "* node = (" << cname << "*)p_calloc(1, sizeof(" << cname << "));\n";
      out << "  if (!node) return NULL;\n";
      for (std::size_t ci = 0; ci < dd.constructors.size(); ++ci) {
        const auto& c = dd.constructors[ci];
        const std::string tag = upper + "_" + to_upper_ascii(c.name);
        const std::string member = sanitize_c_ident(to_lower_ascii(c.name));
        out << "  " << (ci == 0 ? "if" : "else if") << " (strcmp(v->ctor_name, "
            << c_string_lit(c.name) << ") == 0) {\n";
        out << "    node->tag = " << tag << ";\n";
        if (c.is_record) {
          for (std::size_t fi = 0; fi < c.record_fields.size(); ++fi) {
            const auto& rf = c.record_fields[fi];
            out << "    if (" << fi << " < v->item_count) {\n";
            emit_field_conv(out, rf.type, "node->as." + member + "." + sanitize_c_ident(rf.name),
                            "v->items[" + std::to_string(fi) + "]", 6);
            out << "    }\n";
          }
        } else {
          for (std::size_t fi = 0; fi < c.positional_fields.size(); ++fi) {
            out << "    if (" << fi << " < v->item_count) {\n";
            emit_field_conv(out, c.positional_fields[fi],
                            "node->as." + member + ".f" + std::to_string(fi),
                            "v->items[" + std::to_string(fi) + "]", 6);
            out << "    }\n";
          }
        }
        out << "  }\n";
      }
      out << "  return node;\n";
      out << "}\n\n";

      // Destructor
      out << "static void free_" << cname << "(" << cname << "* node) {\n";
      out << "  if (!node) return;\n";
      bool has_any_fields = false;
      for (const auto& c : dd.constructors) {
        if (!c.positional_fields.empty() || !c.record_fields.empty()) {
          has_any_fields = true;
          break;
        }
      }
      if (has_any_fields) {
        out << "  switch (node->tag) {\n";
        for (const auto& c : dd.constructors) {
          if (c.positional_fields.empty() && c.record_fields.empty()) {
            continue;
          }
          const std::string tag = upper + "_" + to_upper_ascii(c.name);
          const std::string member = sanitize_c_ident(to_lower_ascii(c.name));
          out << "    case " << tag << ":\n";
          if (c.is_record) {
            for (const auto& rf : c.record_fields) {
              emit_field_free(out, rf.type,
                              "node->as." + member + "." + sanitize_c_ident(rf.name), 6);
            }
          } else {
            for (std::size_t fi = 0; fi < c.positional_fields.size(); ++fi) {
              emit_field_free(out, c.positional_fields[fi],
                              "node->as." + member + ".f" + std::to_string(fi), 6);
            }
          }
          out << "      break;\n";
        }
        out << "    default: break;\n";
        out << "  }\n";
      }
      out << "  free(node);\n";
      out << "}\n\n";
    }
  }

  void emit_public_api(std::ostringstream& out) {
    const std::string start_fn =
        info_.start_rule ? rule_c_names_[*info_.start_rule] : std::string{};
    const bool scannerless = analysis_.has_context_rules;

    out << "static ParsleyResult parsley_parse_n_impl(const char* input, size_t len) {\n";
    out << "  ParsleyResult res;\n";
    out << "  memset(&res, 0, sizeof(res));\n";
    out << "  if (!input) { input = \"\"; len = 0; }\n\n";
    out << "  ParsleyParser p;\n";
    out << "  memset(&p, 0, sizeof(p));\n";
    out << "  p.input = input;\n";
    out << "  p.len = len;\n";
    out << "  p.scannerless = " << (scannerless ? "true" : "false") << ";\n";
    out << "  p.line = 1;\n";
    out << "  p.col = 1;\n";
    out << "  p.mode_stack[0] = \"default\";\n";
    out << "  p.mode_depth = 1;\n";
    out << "  if (!p.scannerless) p_lex_all(&p);\n\n";

    out << "  ParsleyValue* raw = NULL;\n";
    if (!start_fn.empty()) {
      out << "  bool ok = " << start_fn << "(&p, &raw);\n";
    } else {
      out << "  bool ok = false;\n";
    }

    out << "  if (ok && raw) {\n";
    out << "    res.ok = true;\n    res.consumed = p_current_offset(&p);\n";
    out << "    res.value = pv_clone(raw);\n";
    if (info_.start_data_type) {
      out << "    res.root = pv_to_" << sanitize_c_ident(*info_.start_data_type) << "(res.value);\n";
    }
    out << "  } else {\n";
    out << "    res.ok = false;\n";
    out << "    if (!p.has_err) {\n";
    out << "      res.error.line = 1;\n";
    out << "      res.error.col = 1;\n";
    out << "      res.error.expected = p_strdup(\"expression\");\n";
    out << "      res.error.found = p_strdup(\"end of input\");\n";
    out << "      res.error.message = p_strdup(\"parse error\");\n";
    out << "    } else {\n";
    out << "      res.error.line = p.err_line;\n";
    out << "      res.error.col = p.err_col;\n";
    out << "      res.error.found = p.err_found ? p_strdup(p.err_found) : p_strdup(\"end of input\");\n";
    out << "      ParsleyStrBuf esb = {NULL, 0, 0};\n";
    out << "      if (p.err_expected_count == 0) {\n";
    out << "        sb_append(&esb, \"valid token\");\n";
    out << "      } else {\n";
    out << "        for (size_t i = 0; i < p.err_expected_count; ++i) {\n";
    out << "          if (i > 0) sb_append(&esb, (i + 1 == p.err_expected_count) ? \" or \" : \", \");\n";
    out << "          sb_append(&esb, p.err_expected[i]);\n";
    out << "        }\n";
    out << "      }\n";
    out << "      res.error.expected = esb.data ? esb.data : p_strdup(\"valid token\");\n";
    out << "      ParsleyStrBuf msb = {NULL, 0, 0};\n";
    out << "      sb_append(&msb, \"unexpected \");\n";
    out << "      sb_append(&msb, res.error.found);\n";
    out << "      sb_append(&msb, \", expecting \");\n";
    out << "      sb_append(&msb, res.error.expected);\n";
    out << "      res.error.message = msb.data ? msb.data : p_strdup(\"parse error\");\n";
    out << "    }\n";
    out << "  }\n\n";

    out << "  /* Clean up temporary parser buffers */\n";
    out << "  for (size_t i = 0; i < p.token_count; ++i) free(p.tokens[i].lexeme);\n";
    out << "  free(p.tokens);\n";
    out << "  free(p.err_found);\n";
    out << "  for (size_t i = 0; i < p.err_expected_count; ++i) free(p.err_expected[i]);\n";
    out << "  while (p.ctx_depth > 0) {\n";
    out << "    p.ctx_depth--;\n";
    out << "    free(p.ctx_env[p.ctx_depth].name);\n";
    out << "    free(p.ctx_env[p.ctx_depth].value);\n";
    out << "  }\n";
    out << "  free(p.lr_entries);\n  free(p.memo);\n";
    out << "  p_free_tracked(&p);\n";
    out << "  return res;\n";
    out << "}\n\n";

    out << "ParsleyResult parsley_parse(const char* input) {\n";
    out << "  if (!input) return parsley_parse_n(\"\", 0);\n";
    out << "  return parsley_parse_n(input, strlen(input));\n";
    out << "}\n\n";

    out << "void parsley_result_free(ParsleyResult* result) {\n";
    out << "  if (!result) return;\n";
    if (info_.start_data_type) {
      out << "  free_" << sanitize_c_ident(*info_.start_data_type) << "(result->root);\n";
      out << "  result->root = NULL;\n";
    }
    out << "  pv_free(result->value);\n";
    out << "  result->value = NULL;\n";
    out << "  free(result->error.expected);\n";
    out << "  free(result->error.found);\n";
    out << "  free(result->error.message);\n";
    out << "  result->error.expected = NULL;\n";
    out << "  result->error.found = NULL;\n";
    out << "  result->error.message = NULL;\n";
    out << "}\n\n";

    out << "static int parsley_format_ast_impl(const ParsleyResult* result, char* buf, size_t buf_size) {\n";
    out << "  if (!result || !result->ok || !result->value) {\n";
    out << "    if (buf && buf_size > 0) buf[0] = '\\0';\n";
    out << "    return -1;\n";
    out << "  }\n";
    out << "  ParsleyStrBuf sb = {NULL, 0, 0};\n";
    out << "  format_value_buf(result->value, false, &sb);\n";
    out << "  if (sb.len > INT_MAX) p_fatal(2);\n";
    out << "  int needed = (int)sb.len;\n";
    out << "  if (buf && buf_size > 0) {\n";
    out << "    size_t copy_n = sb.len < buf_size - 1 ? sb.len : buf_size - 1;\n";
    out << "    if (copy_n > 0 && sb.data) memcpy(buf, sb.data, copy_n);\n";
    out << "    buf[copy_n] = '\\0';\n";
    out << "  }\n";
    out << "  free(sb.data);\n";
    out << "  return needed;\n";
    out << "}\n\n";
    out << R"C23(
ParsleyResult parsley_parse_n(const char* input, size_t len) {
  ParsleyResult result; memset(&result,0,sizeof(result));
  if ((!input && len) || len > PARSLEY_MAX_BYTES) { result.error.code = 3; return result; }
  PAllocationScope* scope = p_begin_scope();
  if (!scope) { result.error.code = 1; return result; }
  int failure = setjmp(scope->escape);
  if (failure) { p_end_scope(scope,false); memset(&result,0,sizeof(result)); result.error.code=failure; result.error.line=1; result.error.col=1; return result; }
  result = parsley_parse_n_impl(input,len); p_end_scope(scope,true); return result;
}
int parsley_format_ast(const ParsleyResult* result, char* buf, size_t size) {
  PAllocationScope* scope = p_begin_scope(); if (!scope) return -1;
  if (setjmp(scope->escape)) { p_end_scope(scope,false); if (buf && size) buf[0]='\0'; return -1; }
  int needed = parsley_format_ast_impl(result,buf,size); p_end_scope(scope,false); return needed;
}
)C23";
  }

  std::string format_pattern_expected(const ast::AstPattern& pat, bool wrap_parens = false) {
    switch (pat.kind) {
      case ast::AstPattern::Kind::Wildcard:
        return "_";
      case ast::AstPattern::Kind::Number: {
        if (!pat.name.empty() && pat.name.find_first_of(".eE") == std::string::npos) {
          std::int64_t value=0;auto [end,ec]=std::from_chars(pat.name.data(),pat.name.data()+pat.name.size(),value);
          if(ec==std::errc{}&&end==pat.name.data()+pat.name.size())return std::to_string(value);
        }
        std::ostringstream os;
        if (std::isfinite(pat.number_value) && std::floor(pat.number_value) == pat.number_value &&
            pat.number_value >= -9e15 && pat.number_value <= 9e15) {
          os << static_cast<long long>(pat.number_value);
        } else {
          os << std::setprecision(15) << pat.number_value;
        }
        return os.str();
      }
      case ast::AstPattern::Kind::String: return c23_text::display_string(pat.string_value);
      case ast::AstPattern::Kind::List: {
        std::string out = "[";
        for (std::size_t i = 0; i < pat.args.size(); ++i) {
          if (i > 0) out += ", ";
          out += format_pattern_expected(pat.args[i], false);
        }
        out.push_back(']');
        return out;
      }
      case ast::AstPattern::Kind::Ident:
        return pat.name;
      case ast::AstPattern::Kind::Ctor: {
        if (pat.args.empty()) {
          return pat.name;
        }
        std::string out;
        if (wrap_parens) out.push_back('(');
        out += pat.name;
        for (const auto& a : pat.args) {
          out.push_back(' ');
          out += format_pattern_expected(a, true);
        }
        if (wrap_parens) out.push_back(')');
        return out;
      }
    }
    return "";
  }

  void emit_examples_runner(std::ostringstream& out) {
    out << "int parsley_run_examples(void) {\n";
    if (!emit_tests_ || g_.gExamples.empty()) {
      out << "  return 0;\n";
      out << "}\n";
      return;
    }
    out << "  int failures = 0;\n";
    for (std::size_t i = 0; i < g_.gExamples.size(); ++i) {
      const auto& ex = g_.gExamples[i];
      out << "  /* example " << c_string_lit(ex.name) << " */\n";
      out << "  {\n";
      out << "    ParsleyResult r = parsley_parse(" << c_string_lit(ex.input) << ");\n";
      if (ex.expect_success) {
        out << "    if (!r.ok) {\n";
        out << "      fprintf(stderr, \"[FAIL] example %s: unexpected error at %d:%d: %s\\n\", "
            << c_string_lit(ex.name)
            << ", r.error.line, r.error.col, r.error.message ? r.error.message : \"\");\n";
        out << "      failures++;\n";
        out << "    }";
        if (ex.expected_ast) {
          const std::string expected_str = format_pattern_expected(*ex.expected_ast);
          out << " else {\n";
          out << "      int needed = parsley_format_ast(&r, NULL, 0);\n";
          out << "      char* buf = needed >= 0 ? (char*)p_malloc((size_t)needed + 1u) : NULL;\n";
          out << "      if (buf) parsley_format_ast(&r, buf, (size_t)needed + 1u);\n";
          out << "      if (!buf || strcmp(buf, " << c_string_lit(expected_str) << ") != 0) {\n";
          out << "        fprintf(stderr, \"[FAIL] example %s: expected `%s`, got `%s`\\n\", "
              << c_string_lit(ex.name) << ", " << c_string_lit(expected_str) << ", buf ? buf : \"<formatting failed>\");\n";
          out << "        failures++;\n";
          out << "      }\n      free(buf);\n";          out << "    }\n";
        } else {
          out << "\n";
        }
      } else {
        out << "    if (r.ok) {\n";
        out << "      fprintf(stderr, \"[FAIL] example %s: expected failure at (%d,%d), but parse succeeded\\n\", "
            << c_string_lit(ex.name) << ", " << ex.fail_line << ", " << ex.fail_col << ");\n";
        out << "      failures++;\n";
        out << "    } else if (r.error.line != " << ex.fail_line << " || r.error.col != "
            << ex.fail_col << ") {\n";
        out << "      fprintf(stderr, \"[FAIL] example %s: expected failure at (%d,%d), got (%d,%d)\\n\", "
            << c_string_lit(ex.name) << ", " << ex.fail_line << ", " << ex.fail_col
            << ", r.error.line, r.error.col);\n";
        out << "      failures++;\n";
        out << "    }";
        if (!ex.expecting.empty()) {
          out << " else if ((!r.error.expected || strstr(r.error.expected, "
              << c_string_lit(ex.expecting)
              << ") == NULL) && (!r.error.message || strstr(r.error.message, "
              << c_string_lit(ex.expecting) << ") == NULL)) {\n";
          out << "      fprintf(stderr, \"[FAIL] example %s: expected expecting '%s', got '%s'\\n\", "
              << c_string_lit(ex.name) << ", " << c_string_lit(ex.expecting)
              << ", r.error.expected ? r.error.expected : \"\");\n";
          out << "      failures++;\n";
          out << "    }\n";
        } else {
          out << "\n";
        }
      }
      out << "    parsley_result_free(&r);\n";
      out << "  }\n";
    }
    out << "  return failures == 0 ? 0 : 1;\n";
    out << "}\n";
  }
};

// ---------------------------------------------------------------------------
// Build system & integration file generators
// ---------------------------------------------------------------------------

inline std::string generate_test_parser_c(const ir::Grammar& g,
                                          const std::string& mod_low = "",
                                          bool has_psl_selftest = false) {
  std::ostringstream out;
  out << "/* Standalone ISO C23 conformance test runner for " << safe_comment(g.module_name)
      << " */\n";
  out << "#include \"parser.h\"\n";
  if (has_psl_selftest && !mod_low.empty()) {
    out << "#include \"" << mod_low << "_sem.h\"\n";
  }
  out << "\n#include <stdio.h>\n";
  out << "#include <string.h>\n\n";
  out << "int main(void) {\n";
  out << "  if (parsley_run_examples() != 0) {\n";
  out << "    fprintf(stderr, \"Parsley conformance examples failed.\\n\");\n";
  out << "    return 1;\n";
  out << "  }\n";
  if (has_psl_selftest && !mod_low.empty()) {
    out << "  if (" << mod_low << "_psl_selftest() != 0) {\n";
    out << "    fprintf(stderr, \"Parsley PSL semantic selftest failed.\\n\");\n";
    out << "    return 1;\n";
    out << "  }\n";
  }
  for (const auto& ex : g.gExamples) {
    if (ex.expect_success) {
      out << "  {\n";
      out << "    ParsleyResult r = parsley_parse(" << c_string_lit(ex.input) << ");\n";
      out << "    char buf[1024];\n";
      out << "    if (!r.ok || parsley_format_ast(&r, buf, sizeof(buf)) < 0) {\n";
      out << "      parsley_result_free(&r);\n";
      out << "      return 1;\n";
      out << "    }\n";
      out << "    parsley_result_free(&r);\n";
      out << "  }\n";
      break;
    }
  }
  out << "  printf(\"All " << safe_comment(g.module_name)
      << " C23 parser tests passed.\\n\");\n";
  out << "  return 0;\n";
  out << "}\n";
  return out.str();
}

inline std::string generate_cmake(const std::string& mod_cap,
                                  const std::string& mod_low,
                                  const std::vector<std::string>& extra_c_files = {},
                                  const std::vector<std::string>& extra_h_files = {}) {
  const std::string tgt = mod_low + "_parser";
  std::string c_srcs = "parser.c";
  for (const auto& cf : extra_c_files) {
    c_srcs += " " + cf;
  }
  std::ostringstream out;
  out << "cmake_minimum_required(VERSION 3.21)\n";
  out << "project(" << tgt << " VERSION 0.1.0 LANGUAGES C)\n\n";
  out << "set(CMAKE_C_STANDARD 23)\n";
  out << "set(CMAKE_C_STANDARD_REQUIRED ON)\n";
  out << "set(CMAKE_C_EXTENSIONS OFF)\n\n";
  out << "include(GNUInstallDirs)\n\n";
  out << "add_library(" << tgt << " SHARED " << c_srcs << ")\n";
  out << "add_library(" << mod_cap << "::parser ALIAS " << tgt << ")\n";
  out << "target_compile_definitions(" << tgt << " PRIVATE PARSLEY_BUILDING_DLL)\n";
  out << "target_include_directories(" << tgt << "\n";
  out << "  PUBLIC\n";
  out << "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n";
  out << "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>\n";
  out << "    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>\n";
  out << ")\n";
  out << "if(UNIX AND NOT APPLE)\n";
  out << "  target_link_libraries(" << tgt << " PRIVATE m)\n";
  out << "endif()\n\n";
  out << "add_library(" << tgt << "_static STATIC " << c_srcs << ")\n";
  out << "add_library(" << mod_cap << "::parser_static ALIAS " << tgt << "_static)\n";
  out << "target_compile_definitions(" << tgt << "_static PUBLIC PARSLEY_STATIC)\n";
  out << "target_include_directories(" << tgt << "_static\n";
  out << "  PUBLIC\n";
  out << "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n";
  out << "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>\n";
  out << "    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>\n";
  out << ")\n";
  out << "if(UNIX AND NOT APPLE)\n";
  out << "  target_link_libraries(" << tgt << "_static PUBLIC m)\n";
  out << "endif()\n\n";
  out << "set_target_properties(" << tgt << " PROPERTIES EXPORT_NAME parser)\n";
  out << "set_target_properties(" << tgt << "_static PROPERTIES EXPORT_NAME parser_static)\n";
  out << "install(EXPORT " << tgt << "_targets FILE " << tgt << "Config.cmake NAMESPACE " << mod_cap << ":: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/" << tgt << ")\n";
  out << "install(TARGETS " << tgt << " " << tgt << "_static\n";
  out << "  EXPORT " << tgt << "_targets\n";
  out << "  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}\n";
  out << "  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}\n";
  out << "  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n";
  out << ")\n";
  out << "install(FILES include/" << tgt << ".h";
  for (const auto& hf : extra_h_files) {
    out << " " << hf;
  }
  out << " DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n";
  out << "install(FILES " << tgt << ".pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)\n\n";
  out << "enable_testing()\n";
  out << "add_executable(test_" << tgt << " test_parser.c)\n";
  out << "target_link_libraries(test_" << tgt << " PRIVATE " << tgt << ")\n";
  out << "add_test(NAME " << tgt << "_examples COMMAND test_" << tgt << ")\n";
  return out.str();
}

inline std::string generate_meson(const std::string& mod_cap,
                                  const std::string& mod_low,
                                  const std::vector<std::string>& extra_c_files = {},
                                  const std::vector<std::string>& extra_h_files = {}) {
  const std::string tgt = mod_low + "_parser";
  std::string meson_srcs = "  'parser.c',\n";
  for (const auto& cf : extra_c_files) {
    meson_srcs += "  '" + cf + "',\n";
  }
  std::ostringstream out;
  out << "project('" << tgt << "', 'c',\n";
  out << "  version: '0.1.0',\n";
  out << "  default_options: ['c_std=c23', 'warning_level=2']\n";
  out << ")\n\n";
  out << "cc = meson.get_compiler('c')\n";
  out << "m_dep = cc.find_library('m', required: false)\n";
  out << "inc = include_directories('include', '.')\n\n";
  out << tgt << "_shared = shared_library('" << tgt << "',\n";
  out << meson_srcs;
  out << "  include_directories: inc,\n";
  out << "  c_args: ['-DPARSLEY_BUILDING_DLL'],\n";
  out << "  dependencies: [m_dep],\n";
  out << "  install: true\n";
  out << ")\n\n";
  out << tgt << "_static = static_library('" << tgt << "',\n";
  out << meson_srcs;
  out << "  include_directories: inc,\n";
  out << "  c_args: ['-DPARSLEY_STATIC'],\n";
  out << "  dependencies: [m_dep],\n";
  out << "  install: true\n";
  out << ")\n\n";
  out << "install_headers('include/" << tgt << ".h'";
  for (const auto& hf : extra_h_files) {
    out << ", '" << hf << "'";
  }
  out << ")\n\n";
  out << tgt << "_dep = declare_dependency(\n";
  out << "  include_directories: inc,\n";
  out << "  link_with: " << tgt << "_shared\n";
  out << ")\n\n";
  out << "pkg = import('pkgconfig')\n";
  out << "pkg.generate(" << tgt << "_shared,\n";
  out << "  name: '" << tgt << "',\n";
  out << "  description: 'Parsley ISO C23 parser for " << mod_cap << "'\n";
  out << ")\n\n";
  out << "test_exe = executable('test_" << tgt << "',\n";
  out << "  'test_parser.c',\n";
  out << "  dependencies: [" << tgt << "_dep]\n";
  out << ")\n";
  out << "test('" << tgt << "_examples', test_exe)\n";
  return out.str();
}

inline std::string generate_makefile(const std::string& mod_low,
                                     const std::vector<std::string>& extra_c_files = {}) {
  const std::string tgt = mod_low + "_parser";
  std::string c_srcs = "parser.c";
  for (const auto& cf : extra_c_files) {
    c_srcs += " " + cf;
  }
  std::ostringstream out;
  out << "CC ?= cc\n";
  out << "CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Wpedantic\n";
  out << "PREFIX ?= /usr/local\n";
  out << "AR ?= ar\n\n";
  out << "ifeq ($(OS),Windows_NT)\n";
  out << "  SHARED_LIB = " << tgt << ".dll\n";
  out << "  SHARED_FLAGS = -shared\n";
  out << "  EXE_EXT = .exe\n";
  out << "  LDFLAGS_MATH =\n";
  out << "else\n";
  out << "  UNAME_S := $(shell uname -s)\n";
  out << "  ifeq ($(UNAME_S),Darwin)\n";
  out << "    SHARED_LIB = lib" << tgt << ".dylib\n";
  out << "    SHARED_FLAGS = -dynamiclib -fPIC\n";
  out << "  else\n";
  out << "    SHARED_LIB = lib" << tgt << ".so\n";
  out << "    SHARED_FLAGS = -shared -fPIC\n";
  out << "  endif\n";
  out << "  EXE_EXT =\n";
  out << "  LDFLAGS_MATH = -lm\n";
  out << "endif\n\n";
  out << "STATIC_LIB = lib" << tgt << ".a\n";
  out << "TEST_BIN = test_" << tgt << "$(EXE_EXT)\n\n";
  out << ".PHONY: all shared static test install clean\n\n";
  out << "all: shared static\n\n";
  out << "shared: $(SHARED_LIB)\n\n";
  out << "static: $(STATIC_LIB)\n\n";
  out << "$(SHARED_LIB): " << c_srcs << " parser.h\n";
  out << "\t$(CC) $(CFLAGS) $(SHARED_FLAGS) -DPARSLEY_BUILDING_DLL -Iinclude -I. " << c_srcs
      << " -o $@ $(LDFLAGS_MATH)\n\n";
  out << "parser_static.o: parser.c parser.h\n";
  out << "\t$(CC) $(CFLAGS) -DPARSLEY_STATIC -Iinclude -I. -c parser.c -o $@\n\n";
  std::string static_objs = "parser_static.o";
  for (std::size_t i = 0; i < extra_c_files.size(); ++i) {
    std::string obj = "psl_extra_" + std::to_string(i) + ".o";
    static_objs += " " + obj;
    out << obj << ": " << extra_c_files[i] << "\n";
    out << "\t$(CC) $(CFLAGS) -DPARSLEY_STATIC -Iinclude -I. -c " << extra_c_files[i]
        << " -o $@\n\n";
  }
  out << "$(STATIC_LIB): " << static_objs << "\n";
  out << "\t$(AR) rcs $@ " << static_objs << "\n\n";
  out << "$(TEST_BIN): test_parser.c $(SHARED_LIB)\n";
  out << "\t$(CC) $(CFLAGS) -Iinclude -I. test_parser.c -L. -l" << tgt << " -Wl,-rpath,. -o $@ $(LDFLAGS_MATH)\n\n";
  out << "test: $(TEST_BIN)\n";
  out << "\t./$(TEST_BIN)\n\n";
  out << "install: all\n";
  out << "\tmkdir -p $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/lib/pkgconfig\n";
  out << "\tcp include/" << tgt << ".h $(DESTDIR)$(PREFIX)/include/\n";
  out << "\tcp $(SHARED_LIB) $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/\n";
  out << "\tcp " << tgt << ".pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/\n\n";
  out << "clean:\n";
  out << "\trm -f $(SHARED_LIB) $(STATIC_LIB) " << static_objs << " $(TEST_BIN)\n";
  return out.str();
}

inline std::string generate_xmake(const std::string& mod_low,
                                  const std::vector<std::string>& extra_c_files = {}) {
  const std::string tgt = mod_low + "_parser";
  std::string x_srcs = "\"parser.c\"";
  for (const auto& cf : extra_c_files) {
    x_srcs += ", \"" + cf + "\"";
  }
  std::ostringstream out;
  out << "set_project(\"" << tgt << "\")\n";
  out << "set_version(\"0.1.0\")\n";
  out << "set_languages(\"c23\")\n";
  out << "add_rules(\"mode.debug\", \"mode.release\")\n\n";
  out << "target(\"" << tgt << "\")\n";
  out << "    set_kind(\"shared\")\n";
  out << "    add_files(" << x_srcs << ")\n";
  out << "    add_includedirs(\"include\", \".\", {public = true})\n";
  out << "    add_headerfiles(\"include/" << tgt << ".h\")\n";
  out << "    add_defines(\"PARSLEY_BUILDING_DLL\")\n";
  out << "    if is_plat(\"linux\", \"bsd\") then\n";
  out << "        add_syslinks(\"m\")\n";
  out << "    end\n\n";
  out << "target(\"" << tgt << "_static\")\n";
  out << "    set_kind(\"static\")\n";
  out << "    add_files(" << x_srcs << ")\n";
  out << "    add_includedirs(\"include\", \".\", {public = true})\n";
  out << "    add_headerfiles(\"include/" << tgt << ".h\")\n";
  out << "    add_defines(\"PARSLEY_STATIC\", {public = true})\n";
  out << "    if is_plat(\"linux\", \"bsd\") then\n";
  out << "        add_syslinks(\"m\")\n";
  out << "    end\n\n";
  out << "target(\"test_" << tgt << "\")\n";
  out << "    set_kind(\"binary\")\n";
  out << "    set_default(false)\n";
  out << "    add_files(\"test_parser.c\")\n";
  out << "    add_deps(\"" << tgt << "\")\n";
  return out.str();
}

inline std::string generate_bazel_build(const std::string& mod_low,
                                        const std::vector<std::string>& extra_c_files = {},
                                        const std::vector<std::string>& extra_h_files = {}) {
  const std::string tgt = mod_low + "_parser";
  std::string b_srcs = "\"parser.c\"";
  for (const auto& cf : extra_c_files) {
    b_srcs += ", \"" + cf + "\"";
  }
  std::string b_hdrs = "\"parser.h\", \"include/" + tgt + ".h\"";
  for (const auto& hf : extra_h_files) {
    b_hdrs += ", \"" + hf + "\"";
  }
  std::ostringstream out;
  out << "load(\"@rules_cc//cc:defs.bzl\", \"cc_binary\", \"cc_library\", \"cc_test\")\n\n";
  out << "cc_library(\n";
  out << "    name = \"" << tgt << "\",\n";
  out << "    srcs = [" << b_srcs << "],\n";
  out << "    hdrs = [" << b_hdrs << "],\n";
  out << "    includes = [\"include\", \".\"],\n";
  out << "    defines = [\"PARSLEY_STATIC\"],\n";
  out << "    copts = select({\n";
  out << "        \"@platforms//os:windows\": [\"/std:clatest\"],\n";
  out << "        \"//conditions:default\": [\"-std=c23\"],\n";
  out << "    }),\n";
  out << "    linkopts = select({\n";
  out << "        \"@platforms//os:linux\": [\"-lm\"],\n";
  out << "        \"//conditions:default\": [],\n";
  out << "    }),\n";
  out << "    visibility = [\"//visibility:public\"],\n";
  out << ")\n\n";
  out << "cc_binary(\n";
  out << "    name = \"" << tgt << "_shared\",\n";
  out << "    srcs = [" << b_srcs << ", " << b_hdrs << "],\n";
  out << "    includes = [\"include\", \".\"],\n";
  out << "    local_defines = [\"PARSLEY_BUILDING_DLL\"],\n";
  out << "    linkshared = True,\n";
  out << "    visibility = [\"//visibility:public\"],\n";
  out << ")\n\n";
  out << "cc_test(\n";
  out << "    name = \"test_" << tgt << "\",\n";
  out << "    srcs = [\"test_parser.c\"],\n";
  out << "    deps = [\":" << tgt << "\"],\n";
  out << ")\n";
  return out.str();
}

inline std::string generate_bazel_module(const std::string& mod_low) {
  std::ostringstream out;
  out << "module(\n";
  out << "    name = \"" << mod_low << "_parser\",\n";
  out << "    version = \"0.1.0\",\n";
  out << ")\n\n";
  out << "bazel_dep(name = \"rules_cc\", version = \"0.0.17\")\n";
  return out.str();
}

inline std::string generate_pkgconfig(const std::string& mod_cap, const std::string& mod_low) {
  const std::string tgt = mod_low + "_parser";
  std::ostringstream out;
  out << "prefix=${pcfiledir}/../..\n";
  out << "exec_prefix=${prefix}\n";
  out << "libdir=${exec_prefix}/lib\n";
  out << "includedir=${prefix}/include\n\n";
  out << "Name: " << tgt << "\n";
  out << "Description: Parsley generated ISO C23 parser for " << mod_cap << "\n";
  out << "Version: 0.1.0\n";
  out << "Libs: -L${libdir} -l" << tgt << "\n";
  out << "Libs.private: -lm\n";
  out << "Cflags: -I${includedir}\n";
  return out.str();
}

inline std::string generate_project_readme(const std::string& mod_cap,
                                           const std::string& mod_low,
                                           const analysis::AnalysisResult& analysis) {
  const std::string tgt = mod_low + "_parser";
  std::ostringstream out;
  out << "# " << mod_cap << " — Generated ISO C23 Parser\n\n";
  out << "Generated by Parsley (C23 ordered recursive descent; analysis mode: `" << analysis::strategy_name(analysis.selected_strategy)
      << "`).\n\n";
  out << "## Files\n\n";
  out << "- `parser.h` and `include/" << tgt << ".h`: Public C23 ABI (`PARSLEY_API`, `extern \"C\"`, tagged union ADTs)\n";
  out << "- `parser.c`: Self-contained ISO C23 lexer, parser, ADT memory management, and example runner\n";
  out << "- `test_parser.c`: Standalone C23 test executable\n";
  out << "- Build system manifests: `CMakeLists.txt`, `meson.build`, `Makefile`, `xmake.lua`, `BUILD.bazel`, `MODULE.bazel`, `" << tgt << ".pc`\n\n";
  out << "## Integration & Build\n\n";
  out << "### CMake (`add_subdirectory` / `FetchContent` / standalone)\n\n";
  out << "```cmake\n";
  out << "add_subdirectory(path/to/" << tgt << ")\n";
  out << "target_link_libraries(my_app PRIVATE " << mod_cap << "::parser)\n";
  out << "```\n\n";
  out << "Standalone:\n\n";
  out << "```sh\n";
  out << "cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure\n";
  out << "```\n\n";
  out << "### Meson (`subproject` / standalone)\n\n";
  out << "```sh\n";
  out << "meson setup build && meson compile -C build && meson test -C build\n";
  out << "```\n\n";
  out << "### Make\n\n";
  out << "```sh\n";
  out << "make all && make test\n";
  out << "```\n\n";
  out << "### XMake\n\n";
  out << "```sh\n";
  out << "xmake && xmake run test_" << tgt << "\n";
  out << "```\n\n";
  out << "### Bazel\n\n";
  out << "```sh\n";
  out << "bazel test //:test_" << tgt << "\n";
  out << "```\n\n";
  out << "### pkg-config\n\n";
  out << "```sh\n";
  out << "cc -std=c23 app.c $(pkg-config --cflags --libs " << tgt << ".pc) -o app\n";
  out << "```\n\n";
  out << "## C23 API Quickstart\n\n";
  out << "```c\n";
  out << "#include \"" << tgt << ".h\"\n";
  out << "#include <stdio.h>\n\n";
  out << "int main(void) {\n";
  out << "  ParsleyResult res = parsley_parse(\"...\");\n";
  out << "  if (res.ok) {\n";
  out << "    char buf[512];\n";
  out << "    parsley_format_ast(&res, buf, sizeof(buf));\n";
  out << "    printf(\"AST: %s\\n\", buf);\n";
  out << "  } else {\n";
  out << "    fprintf(stderr, \"Error at %d:%d: %s\\n\", res.error.line, res.error.col, res.error.message);\n";
  out << "  }\n";
  out << "  parsley_result_free(&res);\n";
  out << "  return 0;\n";
  out << "}\n";
  out << "```\n";
  return out.str();
}

} // namespace parsley::codegen::detail

export namespace parsley::codegen {

inline std::vector<GeneratedFile> generate_backend(const ir::Grammar& grammar,
                                                   const analysis::AnalysisResult& analysis,
                                                   const CodegenOptions& opts = {}) {
  (void)opts.target;
  const std::string raw_mod =
      opts.module_override.empty() ? grammar.module_name : opts.module_override;
  const std::string mod_cap = detail::sanitize_c_ident(raw_mod);
  const std::string mod_low = c23_text::module_stem(raw_mod);
  const std::string mod_upper = detail::to_upper_ascii(mod_low);

  const detail::CTypeInfo type_info = detail::analyze_c_types(grammar);
  const std::string header_content =
      detail::namespace_c_header(detail::generate_c23_header(grammar, analysis, type_info, mod_upper), grammar, mod_low);

  detail::C23Emitter emitter(grammar, analysis, type_info, opts.emit_tests);
  const std::string source_content = emitter.emit();

  std::vector<GeneratedFile> psl_files;
  std::vector<std::string> extra_c_files;
  std::vector<std::string> extra_h_files;
  bool has_psl_selftest = false;

  if (opts.psl_module != nullptr) {
    psl::PslCodegenOptions psl_opts;
    psl_opts.emit_tests = opts.emit_tests;
    psl_opts.min_status = opts.min_status;
    psl_opts.profile = opts.psl_profile;
    psl_opts.module_override = opts.module_override;
    auto emitted_psl = psl::emit_psl_c23_files(*opts.psl_module, grammar, psl_opts);
    for (auto& file : optimizer::generate(grammar, *opts.psl_module, psl_opts)) emitted_psl.push_back(std::move(file));
    for (const auto& pf : emitted_psl) {
      psl_files.push_back(GeneratedFile{pf.relative_path, pf.content});
      if (pf.relative_path.ends_with(".c") && pf.relative_path != "parser.c") {
        extra_c_files.push_back(pf.relative_path);
        if (pf.relative_path == mod_low + "_sem.c") {
          has_psl_selftest = true;
        }
      } else if (pf.relative_path.ends_with(".h") &&
                 pf.relative_path.find('/') == std::string::npos) {
        extra_h_files.push_back(pf.relative_path);
        psl_files.push_back(GeneratedFile{"include/" + pf.relative_path, pf.content});
      }
    }
    const auto lock_data =
        psl::compute_psl_lock_data(grammar, opts.psl_module->language_version);
    psl_files.push_back(
        GeneratedFile{raw_mod + ".psem.lock", psl::serialize_psl_lock(lock_data)});
  }

  std::vector<GeneratedFile> files;
  files.push_back(GeneratedFile{"parser.h", header_content});
  files.push_back(GeneratedFile{"include/" + mod_low + "_parser.h", header_content});
  files.push_back(GeneratedFile{"parser.c", source_content});
  for (auto& pf : psl_files) {
    files.push_back(std::move(pf));
  }
  files.push_back(GeneratedFile{
      "test_parser.c",
      detail::generate_test_parser_c(grammar, mod_low, has_psl_selftest && opts.emit_tests)});
  files.push_back(GeneratedFile{
      "CMakeLists.txt", detail::generate_cmake(mod_cap, mod_low, extra_c_files, extra_h_files)});
  files.push_back(GeneratedFile{
      "meson.build", detail::generate_meson(mod_cap, mod_low, extra_c_files, extra_h_files)});
  files.push_back(
      GeneratedFile{"Makefile", detail::generate_makefile(mod_low, extra_c_files)});
  files.push_back(GeneratedFile{"xmake.lua", detail::generate_xmake(mod_low, extra_c_files)});
  files.push_back(GeneratedFile{
      "BUILD.bazel", detail::generate_bazel_build(mod_low, extra_c_files, extra_h_files)});
  files.push_back(GeneratedFile{"MODULE.bazel", detail::generate_bazel_module(mod_low)});
  files.push_back(
      GeneratedFile{mod_low + "_parser.pc", detail::generate_pkgconfig(mod_cap, mod_low)});
  files.push_back(
      GeneratedFile{"README.md", detail::generate_project_readme(mod_cap, mod_low, analysis)});
  return files;
}

inline std::vector<GeneratedFile> generate_backend(const ir::Grammar& grammar,
                                                   const analysis::AnalysisResult& analysis,
                                                   const psl::PslModule& psl_mod,
                                                   CodegenOptions opts) {
  opts.psl_module = &psl_mod;
  return generate_backend(grammar, analysis, opts);
}

} // namespace parsley::codegen
