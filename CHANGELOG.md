# Changelog

## Unreleased

- Stabilize the C17 backend: byte-preserving literals, exact Int64 parsing, checked allocation transactions, explicit resource failures, reusable NFA scratch buffers, memoization of eligible rules, and module-scoped ABI names.
- Share strict regex parsing and NFA semantics between interpreter and playground.
- Harden CLI arguments, PSL lock checking, package scaffolding and JSON-RPC framing/Unicode positions.
- Repair VS Code activation, command dispatch, example assertions and LSP lifecycle; add a reproducible TypeScript build and behavioral tests.
- Add end-to-end adversarial, allocation-failure, ABI and Unicode regression coverage.

Compiler version is read from `VERSION`; the extension is versioned independently.