"""Build and run the Parsley-generated C23 frontend against Kotlin conformance cases."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def run(*args, source=None):
    result = subprocess.run(args, input=source, capture_output=True, encoding="utf-8", timeout=90)
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
    args = parser.parse_args()
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
        run(args.cli, "build", grammar, "-o", out, "--no-compile-shared")
        probe = out / "probe.exe"
        run(args.cc, "-std=c23", "-Wall", "-Wextra", "-pedantic", "-Werror",
            "-DPARSLEY_STATIC", "-I", out, out / "parser.c",
            out / "kotlin_parse.c", out / "kotlin_sem.c", ROOT / "probe.c", "-o", probe)
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
        return int(bool(failures or (args.strict and gaps)))


if __name__ == "__main__":
    raise SystemExit(main())
