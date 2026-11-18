# Changelog

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