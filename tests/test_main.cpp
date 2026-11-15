#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

import parsley.version;
import parsley.diagnostic;

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "[FAIL] " << msg << "\n";
    ++g_failures;
  }
}

void test_version() {
  check(parsley::VERSION_MAJOR == 0, "VERSION_MAJOR == 0");
  check(parsley::VERSION_MINOR == 1, "VERSION_MINOR == 1");
  check(parsley::VERSION_PATCH == 0, "VERSION_PATCH == 0");
  check(parsley::VERSION_STRING == "0.1.0", "VERSION_STRING == 0.1.0");
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

} // namespace

int main() {
  test_version();
  test_diagnostics();

  if (g_failures > 0) {
    std::cerr << g_failures << " test check(s) failed.\n";
    return 1;
  }
  std::cout << "All Phase v0.1.0 tests passed.\n";
  return 0;
}
