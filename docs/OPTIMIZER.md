# Semantic optimizer — C23, pass ABI 1

An adjacent .psem causes parsley build to emit <module>_opt.h, <module>_opt.c
and Markdown/JSON inference plans. All generated build systems include the
optimizer. The generator derives transformations from the AST and node
equations, independently of constructor names. No handwritten rewrite language
is required. Parser-only grammars have no implicit evaluation semantics.

## Describe evaluation, not optimization rules

The value attribute specifies the **complete observable evaluation result**.
effects = [] asserts no additional observable effects of the local operation.
Dependencies and evaluation order are defined by the value expression.
Child effects still propagate; an empty local list does not make a child pure.
An operation with I/O, mutable reads, observable allocation identities, exceptions
or other observable behavior must describe those effects. Missing or nonempty
effects remain opaque. These are language-author contracts.

~~~haskell
{-# SOURCE spec "Arithmetic semantics" #-}
{-# STATUS reviewed #-}
{-# CONFIDENCE 1.0 #-}
node Literal n where
  type = Number
  value = n
  effects = []

{-# SOURCE spec "Arithmetic semantics" #-}
{-# STATUS reviewed #-}
{-# CONFIDENCE 1.0 #-}
node Combine lhs rhs where
  type = Number
  value = lhs.value + rhs.value
  effects = []

{-# SOURCE spec "Conditional semantics" #-}
{-# STATUS reviewed #-}
{-# CONFIDENCE 1.0 #-}
node Select condition yes no where
  type = Number
  value = if condition.value then yes.value else no.value
  effects = []
~~~

Given matching ADTs, inference discovers literal representations, propagates
constants, folds computations and selects known branches. A projection such as
value = child.value can remove a wrapper while retaining the child/effects.
SOURCE, STATUS and CONFIDENCE are required for participating equations.
--min-status applies. The first potentially applicable node rule is authoritative;
inference never bypasses an earlier general/nested rule.

## Delivered analysis domain

Finite C23 Double, Bool and byte-preserving Text; flat monomorphic constructor
patterns; primitive fields and child .value; + - * /, unary minus, comparisons,
not, &&, || and if/then/else. Boolean and conditional evaluation is lazy.
Reification/projection preserves both the grammar ADT and declared semantic
type. Named semantic types are compared conservatively by spelling;
type = typeOf child is followed. Unknown dependent types block replacement.

There are no arithmetic/name heuristics, fast-math reassociations or unrestricted
identities such as x + 0. Signed zero is retained. Zero division and nonfinite
results remain unfolded. This domain assumes ordinary C23 floating-point
evaluation without observable floating environment. Alternative rounding,
floating exception flags, arbitrary precision, overloaded arithmetic and integer
overflow semantics require user analysis/passes.

Int arithmetic stays opaque: the PSL adapter currently stores numbers as
double although the parser preserves exact Int64. Lists, bindings, general calls,
foreign/polymorphic ASTs, extra semantic attributes, scope/unification/require
statements and unsupported equations remain opaque and are reported in the plan.
This release does not claim SSA, interprocedural propagation, alias analysis,
loop optimization or general theorem proving.

## Use and extend the generated pipeline

Run semantic validation successfully on the original tree first. Optimization
does not erase diagnostics or replace the checker.

~~~c
#include "language_opt.h"
language_opt_result r =
    language_optimize(&output_arena, root, NULL, NULL, 0);
if (!r.status) root = r.root;
~~~

The AST is immutable. Changed paths are copied to the output arena; unchanged
subtrees/text can reference the input. Keep both arenas alive. A single arena
is supported. Passes must not mutate nodes or rewind the arena. Rewritten
expressions retain source spans.

Status: 0 success, 1 invalid arguments/tree, 2 work/depth/convergence limit,
3 allocation failure, 4 user-pass failure. Failure returns the original root
and restores arena.used; arena.error retains allocation failure information.
Well-addressed cycles/invalid arities/tags are checked; invalid raw pointers are
outside the C ABI. Defaults: 1,000,000 steps, depth 128, 16 rounds. Options
must have positive limits; maximum configurable depth is 512. Set options.flags
to LANGUAGE_OPT_DISABLE_INFERENCE (using the generated module prefix) to run
only custom transformations while retaining shape validation and limits.

Scratch tables grow geometrically from the back of the caller's arena and
are reclaimed between phases; output grows from the front. No heap or external
runtime is needed. Even an unchanged tree needs transient arena capacity.
Analysis and traversal memoize shared DAG nodes. Steps, built-in rewrites,
iterations and a per-pass trace make work observable.

A pass receives the same AST/context as inference. It can query
language_opt_fact_of, create checked nodes through language_opt_make and
use the memoized postorder walker:

~~~c
static const language_node *visit(language_opt_context *cx,
                                  const language_node *n, void *user) {
    /* Query facts; return n unchanged or build a correct replacement. */
    (void)cx; (void)user;
    return n;
}
static const language_node *run(language_opt_context *cx,
                                const language_node *root, void *user) {
    return language_opt_walk(cx, root, visit, user);
}
language_opt_pass passes[] = {{"my-pass", run, user_data}};
language_opt_result r =
    language_optimize(&output_arena, root, NULL, passes, 1);
~~~

Scheduling is deterministic: inferred pass, custom passes in array order,
then another round if the root changed. Analysis is invalidated between every
phase, so later inference sees custom changes. Return the same pointer for no
change. Fresh equivalent trees still count as changes; nonconvergence fails at
the round limit. NULL fails a pass; language_opt_fail supplies an explicit status.
Custom outputs are validated for shape/cycles. Custom semantic/effect/type
preservation is the pass author's responsibility.

Facts: kind 0 unknown, 1 Double, 2 Bool, 3 Text; pure marks an effect-free known
value. Origin can identify an evaluation-equivalent subtree even when its value
is unknown. Unknown does not prove purity or totality. language_opt_make copies
child arrays/text; factory allocations cannot retain temporary callback buffers.
Arbitrary native pass loops are not bounded without cooperation with the API.

The JSON plan uses schema version 1. The immutable AST and pass ABI provide
the boundary for future nextc integration. This release does not copy, invoke
or generate the nextc machine backend.