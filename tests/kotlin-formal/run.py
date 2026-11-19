"""Build and run the Parsley-generated C23 frontend against Kotlin conformance cases."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from jetbrains import compare_diagnostics, sample, unmark

ROOT = Path(__file__).resolve().parent


def run(*args, source=None, env=None):
    result = subprocess.run(args, input=source, capture_output=True, encoding="utf-8",
                            timeout=90, env=env)
    if result.returncode:
        raise RuntimeError(f"{args[0]} exited {result.returncode}\n{result.stdout}{result.stderr}")
    return result.stdout


def check_module_failures(cli, directory):
    entry = directory / "Entry.parsley"
    component = directory / "Piece.parsley"
    entry.write_text(
        'module Entry (start) where\nimport Piece\nstart :: Text\nstart = piece\n',
        encoding="utf-8",
    )

    def expect_error(message):
        result = subprocess.run(
            [cli, "check", entry], capture_output=True, encoding="utf-8", timeout=20
        )
        if result.returncode == 0 or "E0610" not in result.stderr or message not in result.stderr:
            raise RuntimeError(f"Expected module error {message!r}\n{result.stdout}{result.stderr}")

    expect_error("cannot find source module")
    component.write_text('module Other where\npiece :: Text\npiece = "x"\n', encoding="utf-8")
    expect_error("module header does not match")
    component.write_text(
        'module Piece where\nimport Entry\npiece :: Text\npiece = "x"\n',
        encoding="utf-8",
    )
    expect_error("dependency cycle")

    component.write_text('module Piece where\npiece :: Text\npiece = "x"\n', encoding="utf-8")
    (directory / "Entry.psem").write_text(
        'semantics Entry where\nimport Piece\n', encoding="utf-8"
    )
    (directory / "Piece.psem").write_text('semantics Piece where\nimport EntryCycle\n', encoding="utf-8")
    (directory / "EntryCycle.psem").write_text('semantics EntryCycle where\nimport Piece\n', encoding="utf-8")
    # Semantic files are explicitly linked through PSL, independently of syntax.
    expect_error("semantic dependency cycle")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--cc", required=True, help="C23 Clang executable")
    parser.add_argument("--strict", action="store_true", help="Fail on every observed conformance gap")
    parser.add_argument("--jetbrains", action="append", default=[], metavar="PATH",
                        help="Sample a path relative to integration/ (repeatable)")
    parser.add_argument("--sample-limit", type=int, default=12,
                        help="Maximum Kotlin files to open per selected path (default: 12)")
    args = parser.parse_args()
    if args.sample_limit < 1:
        parser.error("--sample-limit must be positive")
    cases = json.loads((ROOT / "cases.json").read_text(encoding="utf-8"))
    if len({case["id"] for case in cases}) != len(cases):
        raise ValueError("Duplicate case IDs")
    grammar = ROOT / "Kotlin.parsley"
    print(run(args.cli, "check", grammar), end="")
    graph = run(args.cli, "graph", grammar, "--format", "modules-dot")
    for edge in (
        '"Kotlin" -> "Kotlin.Syntax"',
        '"Kotlin.Syntax" -> "Kotlin.Lexical"',
        '"Kotlin.Syntax" -> "Kotlin.SyntaxTree"',
        '"Kotlin" -> "Kotlin.Lints"',
    ):
        if edge not in graph:
            raise RuntimeError(f"Missing module dependency: {edge}\n{graph}")
    print(run(args.cli, "test", grammar, "--fuzz", "30"), end="")
    with tempfile.TemporaryDirectory(prefix="parsley kotlin formal ") as directory:
        out = Path(directory)
        module_check = out / "module-check"
        module_check.mkdir()
        check_module_failures(args.cli, module_check)
        run(args.cli, "build", grammar, "-o", out, "--no-compile-shared", "--compile-compiler",
            env={**os.environ, "PARSLEY_CC": args.cc})
        compiler = out / "kotlin_compiler.exe"
        if "--version" not in run(compiler, "--help") or "--emit" not in run(compiler, "-h"):
            raise RuntimeError("Generated Kotlin compiler help is incomplete")
        version = run(args.cli, "--version").strip().removeprefix("parsley ")
        expected_version = f"kotlin_compiler (Parsley {version})"
        if any(run(compiler, flag).strip() != expected_version for flag in ("--version", "-V")):
            raise RuntimeError("Generated Kotlin compiler version does not match Parsley")
        malformed = out / "malformed.kt"
        malformed.write_text("fun main(): Int = )", encoding="utf-8")
        result = subprocess.run([compiler, malformed, "--emit", "mir"], capture_output=True,
                                encoding="utf-8", timeout=15)
        if result.returncode != 1 or ":1:" not in result.stderr or "expecting" not in result.stderr:
            raise RuntimeError(f"Missing generated compiler parse details: {result.stderr!r}")
        source = ROOT / "integration" / "test.kt"
        if run(compiler, source, "--run").strip() != "42":
            raise RuntimeError("Expected the Kotlin JIT entry to return 42")
        executable = out / "kotlin-test.exe"
        run(compiler, source, "--emit", "exe", "-o", executable)
        result = subprocess.run([executable], capture_output=True, timeout=15)
        if result.returncode != 42:
            raise RuntimeError(f"Expected Kotlin PE exit code 42, got {result.returncode}: {result.stderr!r}")
        declarative = out / "without-adapter"
        declarative.mkdir()
        shutil.copyfile(grammar, declarative / grammar.name)
        shutil.copyfile(ROOT / "Kotlin.psem", declarative / "Kotlin.psem")
        shutil.copytree(ROOT / "Kotlin", declarative / "Kotlin")
        generated = declarative / "generated"
        run(args.cli, "build", declarative / grammar.name, "-o", generated,
            "--no-compile-shared", "--compile-compiler", env={**os.environ, "PARSLEY_CC": args.cc})
        if (generated / "tools" / "native_adapter.h").exists():
            raise RuntimeError("Declarative test unexpectedly used a native adapter")
        declarative_compiler = generated / "kotlin_compiler.exe"
        if run(declarative_compiler, source, "--run", "--return", "i32").strip() != "42":
            raise RuntimeError("Kotlin main did not execute from its PSL description")
        arithmetic = declarative / "arithmetic.kt"
        for expression, expected_result in (("-(7 * 6) + 46 - 1", "3"),
                                            ("2147483647 + 1", "-2147483648")):
            arithmetic.write_text(f"fun main(): Int = {expression}", encoding="utf-8")
            actual = run(declarative_compiler, arithmetic, "--run", "--return", "i32",
                         "--no-ast-opt", "-O0").strip()
            if actual != expected_result:
                raise RuntimeError(f"Wrong declarative i32 result for {expression}: {actual}")
        arithmetic.write_text("fun main(): Int = -(7 * 6) + 46 - 1", encoding="utf-8")
        mir = run(declarative_compiler, arithmetic, "--emit", "mir", "--return", "i32",
                  "--no-ast-opt")
        if any(opcode not in mir for opcode in ("mul", "add", "sub")):
            raise RuntimeError(f"Declarative arithmetic did not emit operations: {mir}")
        string_entry = declarative / "string.kt"
        for kotlin_literal, value in (('"OK"', "OK"), (r'"a\tb\$\\"', "a\tb$\\")):
            string_entry.write_text(f"fun box(): String = {kotlin_literal}", encoding="utf-8")
            if run(declarative_compiler, string_entry, "--run", "--return", "cstr") != value + "\n":
                raise RuntimeError("Kotlin box literal did not execute from its PSL description")
        invalid_entry = declarative / "invalid.kt"
        for text in ("fun other(): Int = 42", "fun main(): Int = 1; fun main(): Int = 2",
                     "val x = 1; fun main(): Int = 42"):
            invalid_entry.write_text(text, encoding="utf-8")
            result = subprocess.run([declarative_compiler, invalid_entry, "--emit", "mir",
                                     "--return", "i32"], capture_output=True, encoding="utf-8", timeout=15)
            if result.returncode != 1 or "declarative native entry" not in result.stderr:
                raise RuntimeError(f"Accepted invalid declarative entry: {text}")
        print("Standalone Kotlin compiler: help, version, JIT and PE exit code verified")
        strict_c = ["-std=c23", "-Wall", "-Wextra", "-pedantic", "-Werror",
                    "-DPARSLEY_STATIC", "-I", out]
        frontend = [out / "parser.c", out / "kotlin_parse.c", out / "kotlin_sem.c"]
        native = out / "native.exe"
        run(args.cc, *strict_c, *frontend, out / "kotlin_opt.c", out / "kotlin_backend.c",
            *sorted((out / "nextc").rglob("*.c")), ROOT / "native.c", "-o", native)
        print(run(native), end="")
        probe = out / "probe.exe"
        run(args.cc, *strict_c, *frontend, ROOT / "probe.c", "-o", probe)
        marked = b"// probe\r\nfun f(): Int = <!UNRESOLVED_REFERENCE!>missing<!>"
        clean, expected = unmark(marked)
        located = subprocess.run([probe, "--diagnostic-spans"], input=clean,
                                 capture_output=True, timeout=15, check=True)
        verdict, *messages = located.stdout.decode().splitlines()
        comparison = compare_diagnostics(expected, messages)
        if verdict != "accept" or comparison[0] != "diagnostics-match":
            raise RuntimeError(f"Lost Kotlin diagnostic location: {comparison}")
        marked = (b"fun f(x: Int) {}\n"
                  b"fun g() { f(1); f(<!ARGUMENT_TYPE_MISMATCH!>false<!>); "
                  b"<!NO_VALUE_FOR_PARAMETER!>f<!>(<!NAMED_PARAMETER_NOT_FOUND!>y<!> = 1) }")
        clean, expected = unmark(marked)
        located = subprocess.run([probe, "--diagnostic-spans"], input=clean,
                                 capture_output=True, timeout=15, check=True)
        verdict, *messages = located.stdout.decode().splitlines()
        comparison = compare_diagnostics(expected, messages)
        if verdict != "accept" or comparison[0] != "diagnostics-match":
            raise RuntimeError(f"Lost Kotlin call diagnostics: {comparison}")
        invalid_calls = out / "invalid-calls.kt"
        invalid_calls.write_bytes(clean)
        result = subprocess.run([compiler, invalid_calls, "--emit", "mir"], capture_output=True,
                                encoding="utf-8", timeout=15)
        if result.returncode != 1 or "project semantic validation failed (3 errors)" not in result.stderr:
            raise RuntimeError(f"Standalone compiler missed call diagnostics: {result.stderr!r}")
        bottom_calls = out / "bottom-calls.kt"
        marked = (b"fun <T> collect(vararg xs: T) {}\n"
                  b"fun test() { collect<Nothing?>(); collect<"
                  b"<!ILLEGAL_TYPE_ARGUMENT_FOR_VARARG_PARAMETER_WARNING!>Nothing<!>>() }\n"
                  b"fun main(): Int = 42")
        clean, expected = unmark(marked)
        located = subprocess.run([probe, "--diagnostic-spans"], input=clean,
                                 capture_output=True, timeout=15, check=True)
        verdict, *messages = located.stdout.decode().splitlines()
        comparison = compare_diagnostics(expected, messages)
        if verdict != "accept" or comparison[0] != "diagnostics-match":
            raise RuntimeError(f"Incorrect Kotlin vararg warning: {comparison}")
        bottom_calls.write_bytes(clean)
        result = subprocess.run([compiler, bottom_calls, "--run"], capture_output=True,
                                encoding="utf-8", timeout=15, check=True)
        if result.stdout.strip() != "42" or result.stderr.count(
                "ILLEGAL_TYPE_ARGUMENT_FOR_VARARG_PARAMETER_WARNING") != 1:
            raise RuntimeError(f"Standalone compiler mishandled vararg warning: {result.stderr!r}")
        box = out / "box.kt"
        box.write_text('fun value(): String = "OK"\nfun box() = value()', encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "OK":
            raise RuntimeError("Kotlin box did not return OK")
        box.write_text('class A { companion object { fun answer() = "OK" } }\n'
                       'fun box() = A.answer()\n', encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "OK":
            raise RuntimeError("Kotlin companion call did not return OK")
        template_box = ('fun box(): String { val n = 2; val word = "OK"; '
                        'val got = "$word:${n * 3}"; '
                        'if (got != "OK:6") return "FAIL: $got"; return word }')
        box.write_text(template_box, encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "OK":
            raise RuntimeError("Kotlin string interpolation produced an unexpected result")
        box.write_text(template_box.replace("OK:6", "OK:7"), encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "FAIL: OK:6":
            raise RuntimeError("Kotlin template box did not take its failure branch")
        mutable_box = ('fun compute(n: Int): Int { var x = n; x = x + 4; return x }\n'
                       'fun box(): String { if (compute(3) != 7) return "FAIL"; return "OK" }')
        box.write_text(mutable_box, encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "OK":
            raise RuntimeError("Kotlin box did not evaluate a call with a mutable local")
        box.write_text(mutable_box.replace("compute(3)", "compute(4)"), encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "FAIL":
            raise RuntimeError("Kotlin box ignored the changed argument")
        alias_box = ('class Pair(val x: Int, val y: Int)\n'
                     'typealias First = Pair\n'
                     'typealias Second = First\n'
                     'fun box(): String { if (Second(1, 2).y != 2) return "FAIL"; return "OK" }')
        box.write_text(alias_box, encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "OK":
            raise RuntimeError("Kotlin alias constructor returned the wrong property")
        box.write_text(alias_box.replace("Second(1, 2)", "Second(1, 3)"), encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "FAIL":
            raise RuntimeError("Kotlin alias constructor ignored the changed argument")
        box.write_text('fun box(): String = "FAIL"\n', encoding="utf-8")
        if run(compiler, box, "--return", "cstr", "--run").strip() != "FAIL":
            raise RuntimeError("Kotlin box did not return its own string value")
        marked = (b"fun take(x: String) {}\n"
                  b"fun test() { val n = 1; val copy = n; "
                  b"take(<!ARGUMENT_TYPE_MISMATCH!>copy<!>); "
                  b"take(<!ARGUMENT_TYPE_MISMATCH!>run { n }<!>); take(run { \"ok\" }) }")
        clean, expected = unmark(marked)
        located = subprocess.run([probe, "--diagnostic-spans"], input=clean,
                                 capture_output=True, timeout=15, check=True)
        verdict, *messages = located.stdout.decode().splitlines()
        comparison = compare_diagnostics(expected, messages)
        if verdict != "accept" or comparison[0] != "diagnostics-match":
            raise RuntimeError(f"Incorrect Kotlin inferred argument diagnostics: {comparison}")
        failures = gaps = 0
        for case in cases:
            verdict, *messages = run(probe, source=case["source"]).splitlines()
            if verdict not in {"accept", "reject"}:
                raise ValueError(f"Unexpected probe verdict: {verdict}")
            agrees = (verdict == "accept") == case["parse"]
            if agrees and "diag" in case:
                code = case["diag"]
                present = f"diagnostic {code.lstrip('!')}" in messages
                agrees = present != code.startswith("!")
            if not agrees and "gap" in case:
                gaps += 1
                print(f"GAP  {case['id']}: {case['gap']} (actual {verdict}, {messages})")
            elif not agrees:
                failures += 1
                print(f"FAIL {case['id']}: {verdict}, {messages}")
            else:
                print(f"PASS {case['id']}")
        print(f"{len(cases) - failures - gaps} passed, {gaps} known gaps, {failures} regressions")
        for selection in args.jetbrains:
            failures += sample(ROOT / "integration", selection, args.sample_limit, probe, compiler)
        return int(bool(failures or (args.strict and gaps)))


if __name__ == "__main__":
    raise SystemExit(main())
