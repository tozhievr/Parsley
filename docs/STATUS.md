# Implementation status and runtime contracts

Updated: 2026-11-16. This describes the C17 implementation; the older architecture documents are design proposals. Compiler version comes from `VERSION`, and the extension has its own version in `editors/vscode/package.json`.

## Delivered scope

- **Only ISO C17 is a code-generation target.** JSON/S-expression IR dumps are inspection formats. Other language targets are outside the current scope.
- The runtime executes ordered recursive descent, precedence climbing and direct-left-recursion seed growing. `MODE LL1|LLk|LALR|PEG|GLL` selects analysis constraints/labels; it does not select an independent LALR/GLL implementation or return an ambiguity forest.
- Safe, context-free, non-left-recursive rules use memoization. Context-dependent rules and left-recursive grammars use checked depth/step budgets. Mutual left-recursion cycles and combining context rules with left recursion are diagnosed instead of generating order-dependent results.
- FIRST/FOLLOW analysis is conservative around predicates and dynamic `exactly` bindings. Prefix overlap is not a proof of ambiguity for ordered choice. `NOFACTOR` deliberately reports overlap as an inspection error; automatic-factoring notes are printed by the CLI only with `--verbose`.
- Python, Swift, Kotlin, Haskell, SQL and JSON examples are supported **subsets**, not complete implementations of their language specifications. Their passing examples are the executable compatibility corpus.
- `Parsley.Ascii` provides ASCII token classes. `Parsley.Unicode` remains a legacy alias with the same ASCII-only behavior. See [REGEX.md](REGEX.md).
- `pkg list/show` inspect bundled modules; `pkg init` scaffolds files. It is not a dependency downloader, and `parsley.toml` is scaffolding metadata rather than an active project loader.

## Generated C ABI

Regenerate headers and libraries together after these unreleased ABI changes. For module `Calc`, exported parser symbols are `calc_parser_parse`, `calc_parser_parse_n`, `calc_parser_result_free`, `calc_parser_format_ast`, and `calc_parser_run_examples`. The semantic adapter is separately named `calc_parse`.

The header provides the familiar `parsley_parse`/`ParsleyResult` aliases for one-parser consumers. Define `PARSLEY_NO_COMPAT` before including multiple generated headers and use explicit names such as `calc_ParsleyResult` and `calc_parser_parse_n`. Data types and enum constants are also module-prefixed. Use `PARSLEY_STATIC` when statically linking; Windows shared-library builds use `PARSLEY_BUILDING_DLL`. C and C++ callers are supported.

- A successful parse can be a **prefix parse**. `result.consumed` is the input-byte position after the start rule. Add `<* eof` to require the entire input; the semantic adapter requires complete consumption.
- `parsley_parse` reads a NUL-terminated string; `parsley_parse_n` accepts explicit byte lengths, including embedded NUL. `(NULL, 0)` means empty input; `(NULL, nonzero)` is an invalid input.
- Generic strings carry `string_len`; typed `Text` fields are C strings, so use the generic value and its length for binary text containing NUL.
- `Int` tokens/actions preserve signed 64-bit values separately from `Double`. Out-of-range or malformed numeric tokens produce an explicit error. Decimal parsing is independent of the host C locale. AST display uses the same escaping and 15-digit Double formatting in the interpreter and C runtime. Playground JSON encodes integers outside JavaScript's safe-integer range as decimal strings.
- Call `parsley_result_free` on both success and failure. Zero-initialized results and repeated frees of the same cleared result are supported. Do not mix headers/libraries generated from different revisions.
- `parsley_format_ast(result, NULL, 0)` returns the required byte length excluding the terminator; allocate that length plus one. A negative return means formatting failure. A shorter caller buffer is terminated and truncated, while the return still reports the full required length. Generated assertion tests use dynamic buffers.

### Failure and ownership

`result.error.code` is 0 for an ordinary syntax failure, 1 for allocation failure, 2 for a resource limit, and 3 for invalid numeric/input arguments. Allocation/resource failures may have NULL message strings; callers must inspect the numeric code. All allocations made during an API invocation are checked and tracked. A failed allocation unwinds the invocation and releases its allocations without returning a partially successful AST. Successful results own their allocations until `result_free`.

The parser uses an internal checked allocation transaction, **not a caller-supplied PSL arena**. Temporary backtracking/memo values live until the invocation ends. PSL nodes and analysis data instead use the caller's `psl_arena`; `arena.error` distinguishes exhaustion from invalid size/alignment, and analysis/semantic parsing returns a negative status on failure. The semantic parse adapter converts the actual grammar AST into the PSL arena and copies its text before freeing the temporary parser result.

Default C17 limits (overridable at compile time where a macro is named):

| Limit | Default | Behavior |
|---|---:|---|
| `PARSLEY_MAX_DEPTH` | 128 | Explicit resource error for deep rule/Pratt/clone recursion |
| `PARSLEY_MAX_BYTES` | 64 MiB | Bounded input/active allocation budget |
| `PARSLEY_MAX_STEPS` | 10,000,000 | Bounded parsing/lexing work |
| Lexer modes, context bindings, factored prefixes | 64 each | Explicit failure, never silent push/drop |
| Expected-token labels | 32 | Diagnostic list is capped |
| Grammar nesting / token budget | 128 / 200,000 | Compiler diagnostic |

`PARSLEY_MALLOC`, `PARSLEY_REALLOC`, `PARSLEY_FREE` can be supplied together when compiling generated C for allocation-failure testing. They must obey the standard allocation/alignment contracts. Each API invocation has its own thread-local transaction; returned values do not retain a pointer to a finished transaction.

## Source and IDE positions

Grammar identifiers are ASCII. UTF-8 BOM is accepted. Tabs outside literals/comments are rejected in layout rather than given an ambiguous tab width. Source/runtime diagnostics use one-based Unicode-codepoint columns and byte offsets; LSP positions and semantic-token lengths use UTF-16 units. Supported source line endings are LF and CRLF.

Reserved DSL words are `module where import qualified as hiding data newtype foreign token skip keywords mode operators infixl infixr infix prefix postfix mixfix do pure context example parses-to fails-at expecting push pop`. They are not contextual identifiers; rename a conflicting rule/token, for example `prefix_`.

The LSP uses full-document synchronization. Changes to an open grammar refresh its loaded `.psem` sibling; opening a semantic document first loads its grammar sibling from disk. JSON-RPC payloads are capped at 8 MiB, header lines at 8 KiB and header count at 64. JSON nesting is capped at 128. Invalid JSON with a trusted frame receives a parse error and the server continues. Invalid/oversized framing that cannot be resynchronized receives an error and closes with a controlled status.

Extension 1.6.x supports compiler/LSP 1.3.x. Newer per-example assertions use `parsley/runExample`; if unavailable, the extension falls back to the CLI test suite. The extension runs CLI processes with argv rather than a shell, checks save failures, and preserves a replacement LSP process when an older process exits.

## Verification and audit mapping

The automated suites are [core tests](../tests/test_main.cpp), [audit regressions](../tests/audit_regression.py), and [extension command tests](../editors/vscode/test/extension.test.cjs). Audit regressions compile generated C with `-std=c17 -Wall -Wextra -pedantic -Werror`, inject allocation failures, link two parser ABIs, exercise Int64 endpoints, long ASTs, nesting limits, malformed CLI/regex/lock inputs and LSP Unicode/framing. PSL examples include positive `fires` and negative `silent` cases. Fuzzing is deterministic with default seed 42; `--seed` overrides it. Fuzz samples are evidence for those inputs, not proof of absence of all parser faults.

| Audit findings | Resolution |
|---|---|
| C-01 / F-01, H-06, M-09 / F-19 | Shared C/PSL byte escaping, trigraph-safe output, long constants as arrays, consistent display and dynamic assertion buffers |
| C-02 / F-05/F-06, H-01/H-02, F-17 | Shared strict regex parser/NFA; explicit byte dialect and resource diagnostics; no ECMAScript playground lexer |
| C-03/C-04/C-05 / F-03/F-10, M-05, F-24 | Checked allocation transactions, explicit limits/arena status, fault-injection coverage and complete cleanup |
| C-06 / F-09 | `CreateProcessW` / `posix_spawnp` with separate argv; no shell interpolation or developer-drive compiler fallback |
| C-07 / F-08/F-13, H-09 | Checked numeric arguments, exception boundary, bounded framing, strict JSON and protocol regression cases |
| F-02 / H-05 | Separate Int64 representation; locale-independent numeric parsing/formatting and exact integer JSON |
| F-04 | Same maximal-munch length/priority/order in tokenized, scannerless and playground lexing; HTML fixture uses separate text/tag modes |
| F-07 / H-10 | UTF-16 IDE conversion, codepoint runtime columns and Unicode regression tests |
| F-11 / H-03 / D-04/D-05/F-21 | Runtime algorithm and analysis profiles separated in UI/docs; design proposals explicitly marked; C17 remains the sole target |
| F-12 | Existing unreadable/malformed lockfiles are errors rather than silently disabling verification |
| F-14, M-07/M-12 | Module-scoped C ABI, distinct module stems, exported installed CMake targets and relocatable pkg-config prefix |
| F-15/F-16, M-03/M-04 | Reused C NFA scratch, iterative epsilon traversal, memoization where valid, cons-list materialization at the API boundary, linear diagnostic-string building and append-based FIRST accumulation |
| F-18 | Sibling-document dependency refresh and close/open handling |
| F-20, M-11 | Explicit prefix/length/NULL contracts and empty-grammar diagnostics |
| H-07/H-08, M-01/M-06/M-08/M-10 | BOM/escapes/layout diagnostics, documented reserved words, checked START/numerics, no-overwrite scaffolding and exclusive formatting flags |
| D-01…D-12 / F-22, T-01…T-06 / F-23, R-01…R-08 | Corrected documentation, MIT text, one compiler-version source, independent extension version, reproducible extension build, pinned Actions, fail-fast toolchain checks and expanded CI tests/timeouts |

CI covers Linux/Windows/macOS builds, a Linux Debug ASan/UBSan configuration, and generated C with GCC in addition to Clang. Workflow definitions do not imply that a remote run has already passed. Meson/Make/XMake/Bazel files are generated integration options; use their own toolchains to verify them for a deployment. `find_package(<module>_parser CONFIG)` imports the installed CMake targets.