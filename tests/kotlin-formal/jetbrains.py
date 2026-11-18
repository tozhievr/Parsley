"""Run bounded stage probes against a local copy of JetBrains Kotlin test data."""
from collections import Counter
from itertools import islice
from pathlib import Path
import os
import re
import subprocess

MULTI_FILE = re.compile(rb"(?m)^[ \t]*//[ \t]*(?:FILE|MODULE):")
DIAGNOSTIC = re.compile(rb"<!>|<![A-Z][^\r\n]*?!>")
PSI_ERROR = re.compile(rb"(?m)^\s*PsiErrorElement\b")


def source_files(directory):
    if directory.is_file():
        yield directory
        return
    for root, dirs, names in os.walk(directory):
        dirs.sort()
        for name in sorted(names):
            if name.endswith(".kt"):
                yield Path(root) / name


def unmark(source):
    depth = count = 0
    for marker in DIAGNOSTIC.finditer(source):
        if marker[0] == b"<!>":
            depth -= 1
            if depth < 0:
                return None, 0
        else:
            depth += 1
            count += 1
    if depth:
        return None, 0
    return DIAGNOSTIC.sub(b"", source), count


def invoke(argv, source=None):
    return subprocess.run(argv, input=source, capture_output=True, timeout=10)


def classify(path, suite, probe, compiler):
    with path.open("rb") as stream:
        source = stream.read(65536)
    if len(source) == 65536:
        return "unsupported-size", "probe limit is 65535 bytes"
    source = source.removeprefix(b"\xef\xbb\xbf")
    if MULTI_FILE.search(source):
        return "unsupported-fixture", "FILE/MODULE directives"
    count = 0
    if suite == "diagnostics":
        source, count = unmark(source)
        if source is None:
            return "unsupported-fixture", "unbalanced diagnostic annotations"
    args = [probe, "--parse-only"] if suite != "diagnostics" else [probe]
    result = invoke(args, source)
    if result.returncode:
        return "probe-error", f"frontend probe exit {result.returncode}"
    lines = result.stdout.decode("utf-8", errors="replace").splitlines()
    verdict = lines[0] if lines else ""
    messages = lines[1:]
    if verdict not in ("accept", "reject"):
        return "probe-error", f"unexpected result {verdict!r}"
    parse_detail = result.stderr.decode("utf-8", errors="replace").strip()
    if suite == "psi":
        expected = path.with_suffix(".txt")
        if not expected.is_file():
            return f"parse-{verdict}", "no PSI snapshot"
        has_error = bool(PSI_ERROR.search(expected.read_bytes()))
        matches = (verdict == "reject") == has_error
        detail = f"Parsley {verdict}, snapshot {'has' if has_error else 'has no'} parse error"
        if parse_detail:
            detail += f"; {parse_detail}"
        return ("psi-syntax-agrees" if matches else "psi-syntax-diff"), detail
    if verdict == "reject":
        if suite == "diagnostics":
            parse_detail += f"; {count} annotated diagnostic span(s)"
        return "parse-reject", parse_detail
    if suite == "diagnostics":
        return "diagnostics-observed", f"{count} annotated span(s); " + (
            ", ".join(messages) if messages else "Parsley emitted none")
    result = invoke([compiler, path, "--emit", "mir"])
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        return "mir-reject" if result.returncode == 1 else "compiler-error", detail
    return "mir-emitted", ""


def sample(integration, selection, limit, probe, compiler):
    directory = (integration / selection).resolve()
    if not directory.is_relative_to(integration) or not directory.exists():
        raise ValueError(f"Unknown JetBrains corpus path: {selection}")
    parts = directory.relative_to(integration).parts
    if not parts or parts[0] not in ("psi", "diagnostics", "codegen"):
        raise ValueError(f"Choose a path under psi, diagnostics or codegen: {selection}")
    counts = Counter()
    for path in islice(source_files(directory), limit):
        if path.suffix != ".kt":
            raise ValueError(f"Expected a .kt input: {path}")
        try:
            status, detail = classify(path, parts[0], probe, compiler)
        except subprocess.TimeoutExpired:
            status, detail = "timeout", "stage exceeded 10 seconds"
        counts[status] += 1
        detail = " ".join(detail.split())[:240]
        print(f"{status:22} {path.relative_to(integration).as_posix()}  {detail}")
    if not counts:
        raise ValueError(f"No .kt files found: {selection}")
    print(f"JetBrains {selection}: " + ", ".join(f"{key}={value}" for key, value in sorted(counts.items())))
    return bool(counts.keys() & {"probe-error", "compiler-error", "timeout"})
