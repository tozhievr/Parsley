# Changelog

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