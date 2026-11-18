# File modules (2.5)

Use plain imports to compose a file-based grammar or PSL entry:

    import Kotlin.Syntax

A dotted name maps to a path below the entry file directory. For example,
Kotlin.Syntax resolves to Kotlin/Syntax.parsley in a grammar and to
Kotlin/Syntax.psem in PSL. Component headers must match the full imported
name. Bundled grammar imports keep their existing import rules. PSL may
also import the paired grammar by name and Parsley.Sem.Prelude.

File modules share a lexer and namespaces. Use distinct declaration names
and declare dependencies explicitly even though the shared namespace also
makes transitive declarations visible. Files are loaded once and linked
in deterministic dependency order. The entry header, pragmas and examples
define the root grammar. Grammar components may contain lexical rules, AST
declarations and syntax rules. PSL components contribute semantic declarations
and examples. Their language version must match the entry. The root PSL
retains its project declaration.

Missing components, mismatched headers, cycles and source paths resolving
outside the entry directory are diagnosed as E0610. Local grammar component
imports currently cannot be qualified or filtered. Export lists and per-file
entry pragmas are reserved for entry grammars. Built-in and local names should
be distinct.

Inspect the dependency graph:

    parsley graph tests/kotlin-formal/Kotlin.parsley --format modules-dot -o kotlin-modules.dot

The combined grammar goes through one resolver, grammar analysis and C23
generation pipeline. This version does not provide per-module visibility,
independent binaries or incremental compilation. The in-memory compiler API
and LSP do not yet load file components.