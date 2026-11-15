module;

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module parsley.diagnostic;

namespace parsley::detail {

inline std::vector<std::string_view> split_lines(std::string_view source) {
  std::vector<std::string_view> lines;
  std::size_t pos = 0;
  while (pos <= source.size()) {
    const std::size_t nl = source.find('\n', pos);
    if (nl == std::string_view::npos) {
      lines.push_back(source.substr(pos));
      break;
    }
    std::size_t len = nl - pos;
    if (len > 0 && source[pos + len - 1] == '\r') {
      --len;
    }
    lines.push_back(source.substr(pos, len));
    pos = nl + 1;
    if (pos == source.size()) {
      lines.push_back(std::string_view{});
      break;
    }
  }
  return lines;
}

inline int count_digits(int value) noexcept {
  if (value <= 0) {
    return 1;
  }
  int digits = 0;
  while (value > 0) {
    value /= 10;
    ++digits;
  }
  return digits;
}

inline std::string pad_left(std::string_view text, std::size_t width, char fill = ' ') {
  if (text.size() >= width) {
    return std::string(text);
  }
  std::string out(width - text.size(), fill);
  out.append(text);
  return out;
}

inline void append_snippet(std::ostringstream& out,
                           const std::vector<std::string_view>& lines,
                           int start_line,
                           int start_col,
                           int end_line,
                           int end_col,
                           std::string_view label,
                           char marker,
                           int gutter_width) {
  if (lines.empty() || start_line < 1 ||
      static_cast<std::size_t>(start_line) > lines.size()) {
    return;
  }
  const int clamped_end = std::min(
      std::max(start_line, end_line), static_cast<int>(lines.size()));

  const std::string blank_gutter = std::string(static_cast<std::size_t>(gutter_width), ' ') + " |";
  out << blank_gutter << '\n';

  if (start_line == clamped_end) {
    const std::string_view line_text = lines[static_cast<std::size_t>(start_line - 1)];
    out << pad_left(std::to_string(start_line), static_cast<std::size_t>(gutter_width))
        << " | " << line_text << '\n';

    const int col_start = std::max(1, start_col);
    const int col_end = std::max(col_start + 1, end_col);
    const std::size_t indent = static_cast<std::size_t>(col_start - 1);
    const std::size_t span_len = static_cast<std::size_t>(col_end - col_start);

    out << blank_gutter << ' ' << std::string(indent, ' ')
        << std::string(span_len, marker);
    if (!label.empty()) {
      out << ' ' << label;
    }
    out << '\n';
    return;
  }

  // Multi-line span: show up to 4 lines (or first 2 + ... + last 2)
  const int total_lines = clamped_end - start_line + 1;
  auto emit_line = [&](int line_no) {
    const std::string_view line_text = lines[static_cast<std::size_t>(line_no - 1)];
    out << pad_left(std::to_string(line_no), static_cast<std::size_t>(gutter_width))
        << " | " << line_text << '\n';
  };

  if (total_lines <= 6) {
    for (int ln = start_line; ln <= clamped_end; ++ln) {
      emit_line(ln);
    }
  } else {
    emit_line(start_line);
    emit_line(start_line + 1);
    out << std::string(static_cast<std::size_t>(gutter_width), ' ') << "...\n";
    emit_line(clamped_end - 1);
    emit_line(clamped_end);
  }

  const std::string_view last_line = lines[static_cast<std::size_t>(clamped_end - 1)];
  const int col_start = std::max(1, start_col);
  int col_end = end_col;
  if (col_end <= col_start) {
    col_end = static_cast<int>(last_line.size()) + 1;
  }
  const std::size_t indent = static_cast<std::size_t>(std::min<int>(
      col_start - 1, static_cast<int>(last_line.size())));
  const std::size_t span_len = static_cast<std::size_t>(std::max(1, col_end - col_start));

  out << blank_gutter << ' ' << std::string(indent, ' ')
      << std::string(span_len, marker);
  if (!label.empty()) {
    out << ' ' << label;
  }
  out << '\n';
}

} // namespace parsley::detail

export namespace parsley {

struct Span {
  std::string file;
  int start_line = 1;
  int start_col = 1;
  int end_line = 1;
  int end_col = 1;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;

  bool is_valid() const noexcept {
    if (start_line <= 0 || start_col <= 0 || end_line <= 0 || end_col <= 0) {
      return false;
    }
    return !file.empty() || end_offset > start_offset ||
           end_line > start_line || end_col > start_col;
  }

  static Span merge(const Span& a, const Span& b) {
    if (!a.is_valid()) {
      return b;
    }
    if (!b.is_valid()) {
      return a;
    }
    Span out;
    out.file = a.file.empty() ? b.file : a.file;

    const bool a_starts_earlier =
        (a.start_line < b.start_line) ||
        (a.start_line == b.start_line && a.start_col < b.start_col) ||
        (a.start_line == b.start_line && a.start_col == b.start_col &&
         a.start_offset <= b.start_offset);

    if (a_starts_earlier) {
      out.start_line = a.start_line;
      out.start_col = a.start_col;
      out.start_offset = a.start_offset;
    } else {
      out.start_line = b.start_line;
      out.start_col = b.start_col;
      out.start_offset = b.start_offset;
    }

    const bool a_ends_later =
        (a.end_line > b.end_line) ||
        (a.end_line == b.end_line && a.end_col > b.end_col) ||
        (a.end_line == b.end_line && a.end_col == b.end_col &&
         a.end_offset >= b.end_offset);

    if (a_ends_later) {
      out.end_line = a.end_line;
      out.end_col = a.end_col;
      out.end_offset = a.end_offset;
    } else {
      out.end_line = b.end_line;
      out.end_col = b.end_col;
      out.end_offset = b.end_offset;
    }

    return out;
  }

  bool operator==(const Span&) const = default;
};

enum class Severity { Error, Warning, Note };

struct RelatedSpan {
  Span span;
  std::string label;
};

struct Diagnostic {
  std::string code;
  Severity severity = Severity::Error;
  std::string message;
  Span span;
  std::string label;
  std::vector<RelatedSpan> related;
  std::vector<std::string> notes;
  std::vector<std::string> helps;
};

class DiagnosticBag {
public:
  void error(std::string code,
             std::string message,
             Span span = {},
             std::string label = {}) {
    Diagnostic d;
    d.code = std::move(code);
    d.severity = Severity::Error;
    d.message = std::move(message);
    d.span = std::move(span);
    d.label = std::move(label);
    items_.push_back(std::move(d));
  }

  void warning(std::string code,
               std::string message,
               Span span = {},
               std::string label = {}) {
    Diagnostic d;
    d.code = std::move(code);
    d.severity = Severity::Warning;
    d.message = std::move(message);
    d.span = std::move(span);
    d.label = std::move(label);
    items_.push_back(std::move(d));
  }

  void note(std::string code,
            std::string message,
            Span span = {},
            std::string label = {}) {
    Diagnostic d;
    d.code = std::move(code);
    d.severity = Severity::Note;
    d.message = std::move(message);
    d.span = std::move(span);
    d.label = std::move(label);
    items_.push_back(std::move(d));
  }

  void push(Diagnostic diag) {
    items_.push_back(std::move(diag));
  }

  void append(const DiagnosticBag& other) {
    items_.insert(items_.end(), other.items_.begin(), other.items_.end());
  }

  void append(const std::vector<Diagnostic>& other) {
    items_.insert(items_.end(), other.begin(), other.end());
  }

  bool has_errors() const noexcept {
    return error_count() > 0;
  }

  const std::vector<Diagnostic>& items() const noexcept {
    return items_;
  }

  std::size_t error_count() const noexcept {
    std::size_t count = 0;
    for (const auto& d : items_) {
      if (d.severity == Severity::Error) {
        ++count;
      }
    }
    return count;
  }

  std::size_t warning_count() const noexcept {
    std::size_t count = 0;
    for (const auto& d : items_) {
      if (d.severity == Severity::Warning) {
        ++count;
      }
    }
    return count;
  }

  void clear() noexcept {
    items_.clear();
  }

private:
  std::vector<Diagnostic> items_;
};

inline std::string_view severity_name(Severity sev) noexcept {
  switch (sev) {
    case Severity::Error:
      return "error";
    case Severity::Warning:
      return "warning";
    case Severity::Note:
      return "note";
  }
  return "error";
}

inline std::string render_diagnostic(const Diagnostic& diag, std::string_view source) {
  std::ostringstream out;
  out << severity_name(diag.severity);
  if (!diag.code.empty()) {
    out << '[' << diag.code << ']';
  }
  out << ": " << diag.message << '\n';

  int max_line = 1;
  if (diag.span.is_valid()) {
    max_line = std::max(max_line, diag.span.end_line);
  }
  for (const auto& rel : diag.related) {
    if (rel.span.is_valid()) {
      max_line = std::max(max_line, rel.span.end_line);
    }
  }
  const int gutter_width = detail::count_digits(max_line);
  const std::string arrow_indent(static_cast<std::size_t>(gutter_width), ' ');
  const std::string blank_gutter = arrow_indent + " |";

  const std::vector<std::string_view> lines =
      source.empty() ? std::vector<std::string_view>{} : detail::split_lines(source);

  bool emitted_gutter = false;
  if (diag.span.is_valid()) {
    const std::string_view fname =
        diag.span.file.empty() ? std::string_view{"<input>"} : std::string_view{diag.span.file};
    out << arrow_indent << "--> " << fname << ':' << diag.span.start_line << ':'
        << diag.span.start_col << '\n';
    if (!lines.empty()) {
      detail::append_snippet(out,
                             lines,
                             diag.span.start_line,
                             diag.span.start_col,
                             diag.span.end_line,
                             diag.span.end_col,
                             diag.label,
                             '^',
                             gutter_width);
      emitted_gutter = true;
    }
  }

  for (const auto& rel : diag.related) {
    if (!rel.span.is_valid()) {
      continue;
    }
    const std::string_view fname =
        rel.span.file.empty() ? std::string_view{"<input>"} : std::string_view{rel.span.file};
    const bool same_file =
        rel.span.file.empty() || diag.span.file.empty() || rel.span.file == diag.span.file;
    if (!same_file || !diag.span.is_valid()) {
      out << arrow_indent << "::: " << fname << ':' << rel.span.start_line << ':'
          << rel.span.start_col << '\n';
    } else {
      out << arrow_indent << "--> " << fname << ':' << rel.span.start_line << ':'
          << rel.span.start_col << '\n';
    }
    if (same_file && !lines.empty()) {
      detail::append_snippet(out,
                             lines,
                             rel.span.start_line,
                             rel.span.start_col,
                             rel.span.end_line,
                             rel.span.end_col,
                             rel.label,
                             '-',
                             gutter_width);
      emitted_gutter = true;
    } else if (!rel.label.empty()) {
      out << arrow_indent << " = note: " << rel.label << '\n';
    }
  }

  if (emitted_gutter && (!diag.notes.empty() || !diag.helps.empty())) {
    out << blank_gutter << '\n';
  }
  for (const auto& n : diag.notes) {
    out << arrow_indent << " = note: " << n << '\n';
  }
  for (const auto& h : diag.helps) {
    out << arrow_indent << " = help: " << h << '\n';
  }

  return out.str();
}

inline std::string render_diagnostics(const std::vector<Diagnostic>& diags,
                                      std::string_view source) {
  std::string out;
  for (std::size_t i = 0; i < diags.size(); ++i) {
    if (i > 0) {
      out.push_back('\n');
    }
    out.append(render_diagnostic(diags[i], source));
  }
  return out;
}

struct ErrorExplanation {
  std::string code;
  std::string title;
  std::string summary;
  std::string example_bad;
  std::string example_good;
};

inline const std::vector<ErrorExplanation>& all_error_explanations() {
  static const std::vector<ErrorExplanation> table = {
      {
          "E0001",
          "lexical error or invalid layout",
          "The lexer encountered an invalid character, an unterminated string/regex/comment literal, or malformed indentation in a layout block.",
          "token bad = \"unterminated\n",
          "token good = \"terminated\"\n",
      },
      {
          "E0002",
          "unresolvable lexer token conflict",
          "Two token definitions have identical or ambiguous regular expressions at the same priority level, so maximal munch cannot disambiguate them.",
          "token ident1 = /[a-z]+/\ntoken ident2 = /[a-z]+/\n",
          "token kw = \"let\"\ntoken ident = /[a-z]+/\n-- or use {-# PREC kw < ident #-}\n",
      },
      {
          "E0100",
          "syntax error in grammar module",
          "The parser encountered an unexpected token or missing construct while parsing a Parsley declaration or rule expression.",
          "expr :: Expr\nexpr = \n",
          "expr :: Expr\nexpr = number\n",
      },
      {
          "E0200",
          "name resolution error",
          "An identifier, rule, token, type, constructor, or module could not be resolved, was defined multiple times, or was missing from an import/export.",
          "module M (missingRule) where\n",
          "module M (presentRule) where\npresentRule :: Text\npresentRule = ident\n",
      },
      {
          "E0214",
          "ambiguous alternatives in rule",
          "Two or more alternatives in a rule share a common prefix in their FIRST sets. By default Parsley resolves this via automatic left-factoring unless {-# NOFACTOR #-} is specified.",
          "atom\n  = Call <$> ident <*> parens (expr `sepBy` \",\")\n  | Var  <$> ident\n",
          "-- Automatic left-factoring resolves this, or factor manually:\natom = ident <**> (CallSuffix <$> parens (expr `sepBy` \",\") <|> pure VarSuffix)\n",
      },
      {
          "E0300",
          "type mismatch in rule or action",
          "The inferred or declared type of a rule, combinator, constructor, or semantic action does not unify with the expected type.",
          "n :: Int\nn = ident  -- ident has type Text\n",
          "n :: Text\nn = ident\n",
      },
      {
          "E0301",
          "missing type signature on exported rule",
          "Every rule exported in a module's public API must declare an explicit type signature so downstream modules and code generators know its type.",
          "module Calc (program) where\nprogram = expr <* eof\n",
          "module Calc (program) where\nprogram :: Expr\nprogram = expr <* eof\n",
      },
      {
          "E0310",
          "context-sensitive binding outside `context` block",
          "A variable bound with `<-` inside a regular `do` block was referenced in a subsequent parser combinator instead of only in the trailing `pure` action. Static ApplicativeDo grammars cannot depend on parsed values unless opted into `context do`.",
          "htmlTag = do\n  name <- \"<\" *> ident <* \">\"\n  \"</\" *> exactly name <* \">\"\n  pure name\n",
          "htmlTag = context do\n  name <- \"<\" *> ident <* \">\"\n  \"</\" *> exactly name <* \">\"\n  pure name\n",
      },
      {
          "E0400",
          "unguarded recursion in parameterized rule",
          "A parameterized rule expands itself recursively without consuming a token first, causing infinite monomorphization.",
          "loop p = loop (parens p)\n",
          "parens p = \"(\" *> p <* \")\"\n",
      },
      {
          "E0500",
          "left recursion in LL mode",
          "A rule can derive itself as its leftmost symbol without consuming input, which is not permitted when targeting LL(1)/LL(k) parsing.",
          "{-# MODE LL1 #-}\nexpr = expr \"+\" term | term\n",
          "expr = operators term [ infixl 6 [\"+\" => Add] ]\n-- or use {-# MODE LALR #-}\n",
      },
      {
          "E0510",
          "context-sensitive rule incompatible with selected strategy",
          "A rule uses `context do` / `exactly`, which requires a monadic/backtracking strategy (PEG or GLL) and cannot be compiled under LL or LALR.",
          "{-# MODE LALR #-}\ntag = context do\n  n <- ident\n  exactly n\n",
          "{-# MODE PEG #-}\ntag = context do\n  n <- ident\n  exactly n\n",
      },
      {
          "E0520",
          "unreachable (dead) rule",
          "A rule is defined in the module but cannot be reached from any start/exported rule.",
          "module M (program) where\nprogram :: Text\nprogram = ident\nunused = number\n",
          "-- Remove `unused` or reference it from `program` / export it.\n",
      },
      {
          "E0600",
          "inline example assertion failed",
          "An `example` test case either failed to parse when `parses-to` was expected, produced a different AST, or failed at a different position/token than `fails-at` specified.",
          "example \"bad\" : \"1 +\" parses-to Num 1\n",
          "example \"err\" : \"1 +\" fails-at (1, 4) expecting \"expression\"\n",
      },
  };
  return table;
}

inline std::vector<std::string> known_error_codes() {
  std::vector<std::string> codes;
  const auto& table = all_error_explanations();
  codes.reserve(table.size());
  for (const auto& e : table) {
    codes.push_back(e.code);
  }
  return codes;
}

inline std::optional<ErrorExplanation> find_error_explanation(std::string_view code) {
  for (const auto& entry : all_error_explanations()) {
    if (entry.code == code) {
      return entry;
    }
  }
  return std::nullopt;
}

inline std::string format_error_explanation(std::string_view code) {
  const auto found = find_error_explanation(code);
  if (!found) {
    return std::string("Unknown diagnostic code: ") + std::string(code) + "\n";
  }
  std::ostringstream out;
  out << found->code << ": " << found->title << "\n\n";
  out << found->summary << "\n\n";
  out << "Example (problematic):\n" << found->example_bad << "\n";
  out << "Example (fixed):\n" << found->example_good;
  return out.str();
}

} // namespace parsley
