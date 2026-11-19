# Changelog

## 2.23.0

- Parse assignments to indexed Kotlin variables and preserve ordered indices in the AST.

## 2.22.0

- Parse Kotlin use-site type projections and preserve them in structural type nodes.

## 2.21.0

- Parse Kotlin prefix and postfix updates and unary plus.

## 2.20.0

- Add reproducible offsets to JetBrains Kotlin corpus batches.

## 2.19.0

- Parse Kotlin type annotations and retain bracketed collection literals in the AST.

## 2.18.0

- Preserve spread arguments in Kotlin call and annotation syntax.
- Generate the native Kotlin `box()` entry for a string literal with inferred return type.

## 2.17.0

- Preserve variadic constructor parameters and their default expressions in the Kotlin AST.

## 2.16.1

- Allow annotation class declarations alongside declarative Kotlin native entry points.

## 2.16.0

- Parse Kotlin annotation classes and annotation prefixes on declarations and parameters.

## 2.15.1

- Lower Kotlin conditionals used as the sole statement in a value-producing branch.

## 2.15.0

- Generate signed integer comparisons and conditional expressions directly from PSL.

## 2.14.0

- Lower declared signed 32-bit arithmetic directly to MIR and use it for Kotlin integer expressions.

## 2.13.0

- Generate declarative C string entries and lower quoted string literals to nextc read-only data.

## 2.12.0

- Generate signed 32-bit entry selection from PSL AST patterns and use it for Kotlin `main`, including checked integer literal lowering.

## 2.11.4

- Require an element before an optional trailing separator in `sepEndBy` and support Kotlin trailing commas in parameter, argument and type lists.
- Parse Kotlin `is` and `!is` type checks at their own precedence tier.

## 2.11.3

- Infer primitive argument types from sequential local bindings and simple `run` lambdas, with full lambda source ranges.

## 2.11.2

- Parse Kotlin lambda literals and trailing lambdas, preserving explicit parameter scopes.

## 2.11.1

- Preserve Kotlin type alias targets and evaluate immutable constructor-property reads in closed native box bodies.

## 2.11.0

- Add PSL `predeclare` for sibling declarations and enable forward function and class references in Kotlin.

## 2.10.3

- Parse Kotlin try expressions with catch and finally clauses and scope catch parameters to their handlers.

## 2.10.2

- Evaluate direct calls with typed primitive parameters and local assignments in closed Kotlin box bodies.

## 2.10.1

- Compile closed Kotlin box bodies with immutable locals, string interpolation and constant branches.

## 2.10.0

- Expose `isResolved` in PSL and support Kotlin companion-qualified string calls in the native box entry.

## 2.9.0

- Add a C string JIT result ABI and run supported Kotlin box functions through the generated compiler.

## 2.8.1

- Preserve Kotlin return-type structure and diagnose explicit and inferred bottom types in generic varargs.

- Show project semantic diagnostics with source positions in the standalone compiler.

## 2.8.0

- Allow a project native adapter to validate the parsed AST before optimization.
- Check argument binding and primitive literal compatibility for unambiguous non-generic Kotlin top-level functions.

## 2.7.5

- Carry token-derived source ranges into generated PSL ASTs and compare located Kotlin diagnostics with JetBrains expectations.

## 2.7.4

- Parse Kotlin companion declarations and type parameters with bounds on classes, interfaces and functions, and explicit type arguments on calls.

## 2.7.3

- Parse chained Kotlin postfix expressions, casts and throw expressions.
- Preserve the original AST value when left-factoring consumes an entire alternative.

## 2.7.2

- Preserve detailed syntax errors in the generated PSL adapter and standalone compiler.
- Preserve Kotlin parameter defaults, varargs and named call arguments in the syntax tree.

## 2.7.1

- Add bounded Windows probes for locally supplied JetBrains Kotlin PSI, diagnostics and codegen fixtures.
- Parse Kotlin exclusive ranges (`..<`) as a distinct AST operator.

## 2.7.0

- Add help and generator version flags to standalone compilers, plus a signed i32 entry ABI.
- Load optional project-native adapters and compile the Kotlin conformance fixture through its standalone compiler.

## 2.6.1

- Run a Kotlin function through the generated C23 frontend, optimizer and nextc backend in the Windows conformance suite.
- Diagnose a known String result returned from a single-return function declared as Int.

## 2.6.0

- Resolve file modules in LSP using open-document overlays, enable cross-file definitions and display module graphs in VS Code and Cursor (extension 2.2.0).
- Port basic escaped names and radix literals in the Kotlin conformance project, and scope function parameters to their bodies.

## 2.5.0

- Compose grammar and semantic source modules, report module errors and export dependency graphs.
- Split the Kotlin conformance project into syntax, lexical, AST and lint components, and cover simple safe navigation and Elvis syntax.

## 2.4.1

- Fix generated C23 warnings for discarded parser action results.
- Add a Kotlin conformance project with the pinned reference grammar and executable Windows tests.

## 2.4.0

- Add Windows one-command installation and updates for Parsley, VS Code and Cursor.
- Verify release archives and use a stable per-user executable path.

## 2.3.0

- Release extension 2.1 with resource-scoped build/test profiles, native compiler actions and declarative settings schemas for VS Code/Cursor.
- Configure toolchain/environment, path variables, CLI limits, artifact choices, semantic status and full uint64 fuzz seeds; preserve legacy output/fuzz settings.
- Route documents through independent folder-specific LSP sessions and apply tool/profile changes live.
- Add editor feature switches, profile selection and effective configuration reporting without environment values.
- Extend behavioral tests to cover command argv, multi-root JSON-RPC startup, scoped settings and invalid configuration.
- Finish native compiler Make installation directory setup and platform-specific Bazel compiler flags.

## 2.2.1

- Verify the bundled nextc snapshot with normalized LF hashes while retaining the original upstream byte hashes, so Git CRLF/LF checkout policy does not invalidate provenance checks.

## 2.2.0

- Bundle the real nextc C23 backend and generate language-to-MIR adapters from value/effect equations.
- Generate standalone compiler targets with AST/semantic validation, optimizer integration, Windows JIT, MIR/assembly, COFF/ELF objects and PE/flat emission.
- Expose language lowering hooks, named MIR passes, verified module ownership and shared-library cg_* exports.
- Embed source resources in Parsley; record upstream revision/hashes and preserve standalone deployment.
- Add Windows native-code, PE execution and interpreter/JIT effect-equivalence regressions.

## 2.1.0

- Generate semantic C23 optimizers automatically from AST schemas and PSL denotational value/effect equations, without handwritten rewrite rules.
- Infer constant propagation/folding, semantic projections and known-branch selection with semantic type/effect preservation.
- Expose named native passes, analysis queries, checked builders, memoized immutable traversal, trace/control options and versioned inference plans.
- Add adaptive DAG caches, phase-local arena scratch, atomic rollback and explicit allocation/work/depth/convergence failures.
- Add end-to-end generated C23 optimizer/pass regression coverage and document the future nextc integration boundary.
- Keep generated PSL with no lints/examples warning-clean.

## 2.0.0

- BREAKING: generate ISO C23 projects, compiler flags and IDE commands; require a C23 compiler and CMake 3.21+ for generated builds.
- Rename CLI target to c23 and VS Code command/settings to buildC23/c23.outputDir; reserve new C23 keywords.

## 1.3.1

- Fix CI toolchain installation, Windows Node 20 test invocation and macOS runner selection.

- Stabilize the C17 backend: byte-preserving literals, exact Int64 parsing, checked allocation transactions, explicit resource failures, reusable NFA scratch buffers, memoization of eligible rules, and module-scoped ABI names.
- Share strict regex parsing and NFA semantics between interpreter and playground.
- Harden CLI arguments, PSL lock checking, package scaffolding and JSON-RPC framing/Unicode positions.
- Repair VS Code activation, command dispatch, example assertions and LSP lifecycle; add a reproducible TypeScript build and behavioral tests.
- Add end-to-end adversarial, allocation-failure, ABI and Unicode regression coverage.

Compiler version is read from `VERSION`; the extension is versioned independently.