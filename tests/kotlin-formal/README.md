# Kotlin conformance project

The reference lexer, parser, and Unicode classes in upstream/ are the complete
ANTLR grammar files from Kotlin/kotlin-spec revision
0f762a2314a9304e4b3fc386b1aceef1d56c7e4c, under the accompanying Apache 2.0 license.
They are reference inputs. The executable Parsley port is Kotlin.parsley and
Kotlin.psem, based on the existing Kotlin fixture and extended with object and
type alias declarations. The port is incomplete. Its entry loads Kotlin.Syntax, which depends on the
independent Kotlin.Lexical and Kotlin.SyntaxTree components. Kotlin.psem
loads Kotlin.Lints. See ../../docs/MODULES.md for file composition.

- [Official grammar](https://kotlinlang.org/grammar/)
- [Language specification (1.9-rfc+0.1)](https://kotlinlang.org/spec/kotlin-spec.html)
- [Pinned upstream source](https://github.com/Kotlin/kotlin-spec/tree/0f762a2314a9304e4b3fc386b1aceef1d56c7e4c)

Run from the Parsley root on Windows:

    python tests/kotlin-formal/run.py --cli build/parsley.exe --cc clang

The runner checks and tests the grammar, generates the C23 frontend, compiles it
with warnings as errors, and runs the corpus through the generated parser and
semantic analyzer. Cases distinguish syntax rejection from semantic diagnostics.
The Windows native test also generates the nextc bridge, checks a Kotlin
function with PSL and the optimizer, and lowers its Double arithmetic using
Kotlin-specific lowering hooks plus the generated literal lowering. It emits a
Win64 COFF object and checks the MIR interpreter and JIT against the expected
results at optimization levels 0 and 1.
Known disagreements carry a reason in cases.json. Add --strict to make existing
disagreements fail the run. Normal mode still fails on unclassified disagreements
or a failed generation, compilation, or analyzer invocation.

To generate and use the standalone compiler on Windows:

    parsley build tests/kotlin-formal/Kotlin.parsley -o build/kotlin-native --compile-compiler --no-compile-shared
    build/kotlin-native/kotlin_compiler.exe --help
    build/kotlin-native/kotlin_compiler.exe --version
    build/kotlin-native/kotlin_compiler.exe tests/kotlin-formal/integration/test.kt --run
    build/kotlin-native/kotlin_compiler.exe tests/kotlin-formal/integration/test.kt --emit exe -o build/kotlin-native/test.exe

The standalone adapter supports a parameterless top-level `main` with an
explicit `Int` result and a single decimal Int literal return. The automated
test runs that entry with the JIT and as an emitted executable.

## Local JetBrains test data

Run a bounded sample under `integration/` on Windows:

    python tests/kotlin-formal/run.py --cli build/parsley.exe --cc clang --jetbrains psi/operators --jetbrains diagnostics/tests/arguments --jetbrains codegen/box/basics --sample-limit 6

Each selection can be a directory or an individual `.kt` file. Directories
are traversed in sorted order, stopping at the limit for each selection.
The PSI probe compares the syntax verdict with the presence of an error node
in the adjacent snapshot. The diagnostics probe removes inline expectation
markers, then compares diagnostic codes and UTF-8 byte ranges. The codegen probe attempts MIR
emission after parsing. Fixtures with embedded files or modules are reported
separately. The sample reports syntax differences and MIR rejections, and fails
on tool errors and timeouts. The diagnostic comparison maps known equivalent
codes and reports missing and extra entries. Token-derived ranges are available
when the AST retains source tokens. Full PSI trees and `box()` execution with
JetBrains' runtime are outside the current probe.

## Coverage boundaries

| Spec area | Executed here | Remaining |
| --- | --- | --- |
| Lexical grammar | ASCII and escaped names, basic literals including binary/hexadecimal syntax, flat comments | Unicode identifiers, escaped/name equivalence, integer suffixes and range typing, nested comments, full string-template modes |
| Source and declarations | Files, package/import, basic functions/properties/classes/interfaces, simple objects/type aliases | Scripts, annotation/modifier restrictions, complete declaration syntax and preservation of type structure |
| Expressions and statements | Basic calls, arithmetic, conditionals, loops, when, simple safe navigation and Elvis syntax | Lambdas, chained navigation, nullability semantics, exceptions, destructuring, context-sensitive statement separators |
| Static semantics | Selected symbol lookups and diagnostics, function parameter scopes, simple CFG/dataflow rules | Kotlin constraint solving, nominal subtyping, variance, overload resolution, smart casts, exhaustive when, complete initialization and visibility rules |
| Execution | Win64 native compilation and JIT execution of one Double arithmetic function | General Kotlin lowering, runtime, interop and platform semantics |

The official specification leaves some semantics to platforms and compiler
implementations. A complete conformance claim would need an explicit Kotlin
version and platform, an AST that preserves the full syntax, and semantic
rules that can be checked against a reference compiler. This suite does not
invoke a reference compiler. Corpus expectations are curated from the spec.