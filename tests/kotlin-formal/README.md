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