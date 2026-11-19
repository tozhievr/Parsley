# Generated nextc backend — ABI 1 (Parsley 2.2)

Parsley generates C23 source for the frontend, optimizer, language-to-MIR adapter
and a standalone native compiler. It embeds a pinned snapshot of the real nextc
backend, rather than invoking the C frontend of nextc or requiring D:/nextc at
runtime. Source provenance and SHA-256 hashes are in resources/nextc/UPSTREAM.json.

## Build a compiler

~~~powershell
parsley build Language.parsley -o generated --emit-nextc --compile-compiler --no-compile-shared
generated/language_compiler.exe program.lang --run
generated/language_compiler.exe program.lang --emit mir --no-ast-opt
generated/language_compiler.exe program.lang --emit object -o program.obj
generated/language_compiler.exe program.lang --emit exe -o program.exe
~~~

--emit-nextc adds backend resources and compiler targets. --compile-compiler
also builds the executable using PARSLEY_CC/the configured C compiler and
implies --emit-nextc. --no-compile-shared independently suppresses the DLL build.
The input needs an adjacent grammar and PSL. Generated compilers accept
--help and --version, which reports the Parsley generator version. The sole implementation-language
generation target remains C23.

Generated CMake, Meson, Make, XMake and Bazel projects include a
<module>_compiler target. CMake installs the public nextc header tree and the
compiler. Only Windows/Clang execution is locally verified. Other build tools
and hosts need their own validation; no Linux/WSL tests were run for this work.

The current upstream runtime supports x86-64: Windows COFF, Linux ELF64, text
assembly, PE executables, flat images and Windows JIT. Architecture proposals
in nextc/docs are broader than this implemented snapshot. A PE image uses the
Windows ABI and imports ExitProcess from kernel32.dll. PE/flat entry adapters
convert the entry result to an i32 exit status. --return f64 (default), bool,
i32, or i64 selects the language entry ABI. --os selects the nextc target ABI;
cross-target execution is not implicit. With `--return cstr`, a language
adapter may return a pointer to a NUL-terminated string that remains valid
until JIT cleanup. `--run` writes it followed by one newline. Image entry
adapters (`exe` and `flat`) require a scalar exit status. -O0/-O1 select nextc optimization.
--no-ast-opt disables the earlier inferred AST optimization.

The standalone driver parses the complete input, checks its original semantics,
optionally optimizes its AST, constructs and verifies MIR, then emits machine
code. Parse/semantic/lowering/verification/output failures are explicit.
Frontend input is bounded at 64 MiB; each frontend arena is 16 MiB. Projects
can customize the generated driver or supply their own embedding driver.

## Generated adapter

<module>_backend.h/.c derives lowering from the same value/effects equations
described in OPTIMIZER.md. Flat pure Double/Bool equations become MIR constants,
floating operations, comparisons and projections. if/then/else and &&/|| become
conditional blocks and PHI nodes, preserving lazy evaluation and effect order.
The source constructor names are irrelevant.

Opaque/effectful operations require an explicit language lowering hook.
Bindings, functions, calls, loops, storage, integer overflow policies and
aggregate/Text representations must have language-specific lowering; types
alone cannot establish their execution semantics. The generic PSL numeric
adapter still represents numbers as doubles, so automatic Int64 lowering is
not enabled. A frontend needing exact integers must use its exact typed parser
AST/custom adapter instead of reconstructing Int64 from a PSL double.
Unsupported operations fail rather than producing a placeholder zero value.

## Declarative i32 entry

Assign `entryCandidates` to a pattern-bound list on the file's PSL node
rule. On an entry declaration's rule use `entryI32` to bind the expression
to lower. Mark other permitted declarations with `entryPassive = True` only
when they require no execution. Exactly one candidate must supply the entry.
Selection runs after semantic validation and before expression optimization.

A flat numeric constructor can declare `value = i32Literal n` with
`effects = []` and the normal provenance pragmas. The backend checks that
`n` is exactly representable as a signed 32-bit integer before emitting MIR.

A rule may also provide `entryCstr` for the C string ABI. An optional
`entryCstrConstructor` string limits that rule to the named constructor.
When no string entry matches, the driver's existing entry lowering applies.
A flat text constructor can declare `value = cstrQuoted text` and
`effects = []` with provenance. It emits a NUL-terminated value in read-only
MIR data. The intrinsic decodes basic escapes and rejects interpolation.

For a flat expression constructor, use `nativeI32 = i32Binary op lhs rhs
[AddTag, SubTag, MulTag]` or `nativeI32 = i32Negation op operand NegTag`
with provenance. The tags are language AST constructors. The backend
recursively lowers operands, requires i32 values, and emits wrapping
addition, subtraction, multiplication or negation. Other tags remain
unsupported by that equation.

Use `nativeCompare = i32Comparison op lhs rhs [EqTag, NeTag, LtTag, LeTag, GtTag, GeTag]`
for signed i32 comparisons producing a boolean. If the same rule declares
`nativeI32`, the operands must match and the operator tags must be distinct.

Use `nativeSelect = selectSingletons condition thenStatements elseStatements`
for an expression whose branches each contain one statement. The condition
lowers to a boolean. Branches must lower to the same type and run in separate
MIR blocks. A statement can expose its expression through `value = valueOf expr`
and `effects = []`.

## Extending a language

When the CLI generates a native backend, it copies an optional adjacent
`<Grammar>.native.h` to `tools/native_adapter.h` and includes it in the
standalone driver. Define `PARSLEY_NATIVE_RESULT` (a nextc type),
`PARSLEY_NATIVE_RETURN` (the CLI spelling shown in help), and
`PARSLEY_NATIVE_OPTIONS` (a pointer to the module's backend options).
The hook receives the original language's root after validation and optional
AST optimization. For a complete file the hook can select an entry declaration.
An adapter can also define `PARSLEY_NATIVE_CHECK` as a function taking the
module's root, a `psl_sink` and its user pointer. The standalone driver invokes
it after PSL analysis and before optimization. Return a negative result for
an analysis failure or emit an error through the sink to reject the source.

~~~c
#include "language_backend.h"
static bool lower(language_backend_context *cx, const language_node *n,
                  cg_value_t *out, void *user) {
    cg_builder_t *b = language_backend_builder(cx);
    /* Inspect the language AST; use cg_build_* for operations, storage, calls
       and CFG. Use language_backend_node(cx, child) to lower a child at the
       current insertion point. user can supply bindings and symbol tables. */
    (void)b; (void)n; (void)out; (void)user;
    return false; /* decline: use inferred lowering */
}
language_backend_options opt = {0};
opt.lower = lower;
cg_diag_t diag = {0};
cg_module_t *mir = language_backend_lower(root, "entry", CG_T_F64,
                                        NULL, 0, &opt, &diag);
if (mir) {
    /* cg_compile_to_object / cg_compile_to_asm / cg_link_image /
       cg_jit_create all operate on this same real nextc MIR. */
    cg_module_destroy(mir);
}
~~~

A hook returning true owns lowering that node and supplies an SSA value.
CG_INVALID indicates failure. A hook returning false declines it. Generated
lowering enforces entry result type, constructor arity, non-null child nodes,
work and depth limits. Hook code owns the correctness of its language semantics
and of custom control-flow construction. Defaults are depth 128 and 1,000,000
node lowering steps; maximum configurable depth is 512. Native hook code must
cooperate with limits.

MIR passes are named entries with signature
bool run(cg_module_t *, cg_diag_t *, void *). Set options.passes/pass_count.
They run in order after lowering and before the mandatory MIR verifier.
Failure destroys the partial module. Successful calls transfer module ownership
to the caller. nextc's own machine/IR optimization profiles run afterward.

Generated shared libraries export the cg_* API as well as module-prefixed
lowering APIs, so plugins can build MIR and release owned modules/buffers.
Static consumers define PARSLEY_STATIC. The snapshot's generic cg_* ABI is
common to generated modules; do not mix different nextc snapshots in one
statically linked application. C23 extensions can also be compiled directly
into the generated project; no dynamic plugin loader is required.

The upstream resource files are preserved. Emission adds Windows CRT's
standard-library compatibility macro to C sources and export/import annotations
to cg_* header declarations. This does not change nextc's machine algorithms.