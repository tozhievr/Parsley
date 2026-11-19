"""Run bounded stage probes against a local copy of JetBrains Kotlin test data."""
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from itertools import islice
from pathlib import Path
import os
import re
import subprocess
import sys

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


MARKER_ENTRY = re.compile(rb'\s*([A-Z][A-Z0-9_]*)(?:\((?:"(?:\\.|[^"\\])*"|[^"()])*\))?\s*(,|$)')
KOTLIN_DIAGNOSTIC = {"E_UNDECLARED": "UNRESOLVED_REFERENCE"}


def marker_codes(text):
    codes = []
    while text:
        entry = MARKER_ENTRY.match(text)
        if not entry:
            raise ValueError("unsupported diagnostic marker")
        codes.append(entry[1].decode("ascii"))
        text = text[entry.end():]
    return codes


def unmark(source):
    clean = bytearray()
    expected = Counter()
    pending = []
    start = 0
    for marker in DIAGNOSTIC.finditer(source):
        clean.extend(source[start:marker.start()])
        start = marker.end()
        if marker[0] == b"<!>":
            if not pending:
                raise ValueError("unpaired diagnostic marker")
            offset, codes = pending.pop()
            expected.update((code, offset, len(clean) - offset) for code in codes)
        else:
            pending.append((len(clean), marker_codes(marker[0][2:-2])))
    if pending:
        raise ValueError("unclosed diagnostic marker")
    clean.extend(source[start:])
    return bytes(clean), expected


def compare_diagnostics(expected, messages):
    actual = Counter()
    for message in messages:
        code, offset, length = message.split()
        actual[KOTLIN_DIAGNOSTIC.get(code, code), int(offset), int(length)] += 1
    missing, extra = expected - actual, actual - expected

    def detail(label, diagnostics):
        values = ", ".join(f"{code}@{offset}+{length}" for code, offset, length in diagnostics.elements())
        return f"{label}: {values}" if values else ""

    if missing or extra:
        return "diagnostics-diff", "; ".join(filter(None, (detail("missing", missing), detail("extra", extra))))
    return "diagnostics-match", f"{sum(expected.values())} diagnostic(s)"


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
    if suite == "diagnostics":
        try:
            source, expected_diagnostics = unmark(source)
        except ValueError as error:
            return "unsupported-fixture", str(error)
    args = [probe, "--diagnostic-spans" if suite == "diagnostics" else "--parse-only"]
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
            parse_detail += f"; {sum(expected_diagnostics.values())} expected diagnostic(s)"
        return "parse-reject", parse_detail
    if suite == "diagnostics":
        try:
            return compare_diagnostics(expected_diagnostics, messages)
        except ValueError as error:
            return "probe-error", f"invalid located diagnostic: {error}"
    result = invoke([compiler, path, "--return", "cstr", "--run"])
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        return "box-reject" if result.returncode == 1 else "compiler-error", detail
    if result.stdout in (b"OK\n", b"OK\r\n"):
        return "box-pass", ""
    return "box-fail", repr(result.stdout.decode("utf-8", errors="replace"))


def sample(integration, selection, limit, probe, compiler, *, offset=0, jobs=1):
    directory = (integration / selection).resolve()
    if not directory.is_relative_to(integration) or not directory.exists():
        raise ValueError(f"Unknown JetBrains corpus path: {selection}")
    parts = directory.relative_to(integration).parts
    if not parts or parts[0] not in ("psi", "diagnostics", "codegen"):
        raise ValueError(f"Choose a path under psi, diagnostics or codegen: {selection}")
    def run_case(path):
        if path.suffix != ".kt":
            raise ValueError(f"Expected a .kt input: {path}")
        try:
            return classify(path, parts[0], probe, compiler)
        except subprocess.TimeoutExpired:
            return "timeout", "stage exceeded 10 seconds"
        except OSError as error:
            return "probe-error", str(error)

    counts = Counter()
    stop = None if limit is None else offset + limit
    paths = islice(source_files(directory), offset, stop)
    with ThreadPoolExecutor(max_workers=jobs) as workers:
        while batch := list(islice(paths, jobs * 4)):
            for path, (status, detail) in zip(batch, workers.map(run_case, batch)):
                counts[status] += 1
                detail = " ".join(detail.split())[:240]
                print(f"{status:22} {path.relative_to(integration).as_posix()}  {detail}")
            if limit is None:
                sys.stdout.flush()
    if not counts:
        raise ValueError(f"No .kt files found in {selection} at offset {offset}")
    print(f"JetBrains batch {selection}: offset={offset}, files={sum(counts.values())}")
    print(f"JetBrains {selection}: " + ", ".join(f"{key}={value}" for key, value in sorted(counts.items())))
    return bool(counts.keys() & {"probe-error", "compiler-error", "timeout", "box-fail"})
