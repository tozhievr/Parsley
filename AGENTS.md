Current implementation contracts: [docs/STATUS.md](docs/STATUS.md); byte regex dialect: [docs/REGEX.md](docs/REGEX.md). Only C23 is a code-generation target. The following specification is subject to the documented implementation limits.

# AGENTS.md — Parsley & PSL Guide for AI Coding Agents

This document is the authoritative technical reference for autonomous and assistant AI coding agents authoring, extending, debugging, and compiling Parsley grammars (`.parsley`) and Parsley Semantic Layer specifications (`.psem`).

---

## 1. System Overview & Architecture

Parsley is a high-performance language development toolkit inspired by Haskell syntax and semantics, designed from first principles to compile into **pure, zero-dependency ISO C23** frontends, static analyzers, and shared libraries.

The language architecture is strictly bifurcated into two mutually exclusive, complementary module types:
1. **Grammar Modules (`<Name>.parsley`)**: Defines the surface syntax, lexical modes, AST algebraic data types (`data`), precedence tables (`operators`), and combinator rules.
2. **Semantic Layer Modules (`<Name>.psem`)**: Defines the static semantics, scope rules, type systems (Hindley-Milner type inference), control flow graphs (`flow`), dataflow analyses (`analysis`), and linter rules (`lint`).

```
                +---------------------+
                | Grammar: <Mod>.parsley |
                +---------------------+
                           |
            [Lexer / Layout Virtual Braces]
                           |
                   [Surface Parser]
                           |
             [Name Resolution & Scopes]
                           |
            [Type Checking (HM Inferred)]
                           |
                [Desugar & Monomorphize]
                           |
                 [Core Grammar IR]
                           |
            +--------------+--------------+
            |                             |
      [Grammar Analysis]           +---------------+
   (FIRST/FOLLOW/LeftFactor)       | <Mod>.psem   |
            |                      +---------------+
            |                             |
            |                    [PSL 5-Pass Plan]
            |                    1. Scopes & Decl
            |                    2. Symbol Lookup
            |                    3. HM Unification
            |                    4. CFG Dataflow
            |                    5. Lint Matcher
            |                             |
            +--------------+--------------+
                           |
                 [ISO C23 Codegen]
                           |
      +--------------------+--------------------+
      |                    |                    |
   parser.h             parser.c         Multi-Build Project
   <mod>_ast.h          <mod>_sem.c       - CMakeLists.txt
   <mod>_sem.h          test_parser.c     - meson.build
   <mod>_diag.h                           - Makefile, xmake.lua
                                          - Bazel, pkg-config
                                          - <mod>_parser.dll/.so
```

---

## 2. Parsley Grammar DSL Specification (`.parsley`)

### 2.1 Module Header & Pragmas
Every grammar file begins with an optional pragma list and a mandatory module declaration:
```haskell
{-# START program #-}
{-# MODE PEG #-}
module Calc (program) where

import Parsley.Prelude (sepBy, many, some, optional, between, eof)
```

Supported pragmas:
- `{-# START <rule> #-}`: Overrides the entry point rule.
- `{-# MODE LL1|LLk|LALR|PEG|GLL #-}`: Selects grammar-analysis constraints/labels; the generated runtime remains ordered recursive descent/Pratt parsing.
- `{-# NOFACTOR #-}`: Suppresses automatic left-factoring for ambiguity inspection (`E0214`).

### 2.2 Lexical Syntax (`skip`, `token`, `keywords`, `mode`)
- **`skip <name> = /regex/`**: Discarded trivia (whitespace, comments).
- **`token <name> = /regex/ [:: Type]`**: Named token. Default type is `Text`, overrideable with `:: Double`, `:: Int`, etc.
- **`keywords = ["...", "..."]`**: Reserved keywords. Win over regex identifiers on equal length.
- **Anonymous String Literals**: Literals like `"+"` or `"let"` automatically become lexer tokens with priority above general regexes.
- **Lexer Modes (Push/Pop)**:
  ```haskell
  mode string where
    token chars  = /[^"]+/
    token escape = /[\\][nrt"]/
    token close  = "\"" -> pop

  token open = "\"" -> push string
  ```

### 2.3 Algebraic Data Types (`data`)
AST nodes are declared directly in the grammar. They are compiled into idiomatic C23 tagged unions (`enum` tag + `union` payload):
```haskell
data Op = Add | Sub | Mul | Div | Pow

data Expr
  = Num  Double
  | Var  Text
  | Neg  Expr
  | Bin  Op Expr Expr
  | Call Text [Expr]
  | Let  Text Expr Expr
```

### 2.4 Syntactic Rules & Combinators
Rules use Applicative and Alternative style:
- `f <$> a <*> b`: Map constructor `f` over rule results `a` and `b`.
- `p *> q`: Sequence `p` then `q`, keeping `q`'s result.
- `p <* q`: Sequence `p` then `q`, keeping `p`'s result.
- `p <|> q`: Alternative choice (ordered under PEG).
- `many p`: Zero or more repetitions (yields `[a]`).
- `some p`: One or more repetitions (yields `[a]`).
- `optional p`: Zero or one repetition (yields `Maybe a`).
- `sepBy p sep`: Elements separated by `sep` (yields `[a]`).
- `sepEndBy p sep`: Elements separated and optionally terminated by `sep`.
- `between open close p`: Encloses `p` between `open` and `close`.
- `eof`: Asserts end of file.
- `lookahead p`: Asserts `p` matches without consuming input.
- `notFollowedBy p`: Negative lookahead without consuming input.
- `p <?> "label"`: Custom expected error label.
- `recover p sync`: Error recovery synchronizing on `sync` token.

### 2.5 Precedence Tables (`operators`)
Declarative precedence climbing replaces manual recursive grammar stratification:
```haskell
expr :: Expr
expr = operators atom
  [ infixl 6 ["+" => Bin Add, "-" => Bin Sub]
  , infixl 7 ["*" => Bin Mul, "/" => Bin Div]
  , infixr 8 ["^" => Bin Pow]
  , prefix  9 ["-" => Neg]
  ]
```

### 2.6 ApplicativeDo & `context do`
```haskell
letExpr :: Expr
letExpr = do
  "let"
  x    <- ident
  "="
  e    <- expr
  "in"
  body <- expr
  pure (Let x e body)
```
- By default, `do` blocks are desugared to Applicative sequences.
- Variable bindings used dynamically in later parsers require `context do`:
  ```haskell
  htmlTag :: Node
  htmlTag = context do
    name <- "<" *> ident <* ">"
    body <- many node
    "</" *> exactly name <* ">"
    pure (Elem name body)
  ```

### 2.7 In-Grammar Test Assertions (`example`)
Every grammar supports compile-time self-testing:
```haskell
example "arithmetic" : "1 + 2 * 3" parses-to Bin Add (Num 1) (Bin Mul (Num 2) (Num 3))
example "syntax-err" : "1 +" fails-at (1, 4) expecting "expression"
```

---

## 3. Parsley Semantic Layer Specification (`.psem`)

The PSL module (`<Mod>.psem`) describes the static semantics of a grammar. It resides adjacent to `<Mod>.parsley`.

### 3.1 Provenance Pragmas
All semantic rules must document provenance and verification level:
- `{-# LANGUAGE Sem1 #-}`: Declares PSL language version.
- `{-# SOURCE ref "Section 4.2" #-}`: Reference to documentation or specification.
- `{-# STATUS draft|reviewed|verified #-}`: Rule maturity status.
  - `draft`: Experimental, severity capped at Warning.
  - `reviewed`: Reviewed by human or domain expert.
  - `verified`: Formally verified against test corpora.
- `{-# CONFIDENCE 0.95 #-}`: Confidence score in `[0.0, 1.0]`.
- `{-# LOCKED #-}`: Prevents automatic LLM overwrites.
- `{-# SUPERSEDES oldRule BY "reason" #-}`: Rule evolution tracking.

### 3.2 Types, Kinds & Built-ins
```haskell
type Number = Double
type Boolean = Bool

kind Variable where shadowing = Warn
kind Function

builtin sin :: Function (Number -> Number)
builtin pi  :: Variable Number
```

### 3.3 Diagnostic Catalog (`diag`)
Standardized error codes and interpolation templates:
```haskell
diag E001 :: Error "Undefined variable `{name}`"
diag E002 :: Error "Undefined function `{name}`"
diag E003 :: Error "Function `{name}` expects {expected} args, got {actual}"
diag E004 :: Error "Type mismatch: expected {expected}, got {actual}"
diag W001 :: Warning "Variable `{name}` is never read"
```

### 3.4 Node Equations (`node`)
Runs in the `Sem` effect monad:
```haskell
node Num _ where
  type = Number

node Var x where
  sym  <- lookup Variable x `orFail` E001 { name = x }
  type =  typeOf sym

node Let x e body where
  scope body $ declare Variable x (typeOf e)
  type = typeOf body

node Bin op l r where
  require (typeOf l == Number && typeOf r == Number) `orFail` E004
  type = Number
```

### 3.5 Hindley-Milner Type Inference
Polymorphism and unification:
- `a <- fresh`: Allocates a fresh type variable at the current rank level.
- `unify a b orFail E`: Unifies type `a` with `b` via Union-Find; emits `E` on failure.
- `mono t`: Enforces monomorphic instantiation.
- `generalize t`: Generalizes free variables deeper than current rank level.

```haskell
node Lam x body where
  a <- fresh
  scope body $ declare Variable x (mono a)
  type = Fn a (typeOf body)

node App f arg where
  r <- fresh
  unify (typeOf f) (Fn (typeOf arg) r) `orFail` E004
  type = r
```

### 3.6 Control Flow & Dataflow (`flow` & `analysis`)
```haskell
flow Block ss     = chain ss
flow If c t e     = branch c [t, e]
flow While c body = loop c body
flow Return e     = jump e Exit

analysis Assigned :: Forward, Must Symbol
  gen  (Assign x _)       = [x]
  gen  (Decl x (Just _))  = [x]

analysis Live :: Backward, May Symbol
  gen  (Var x)            = [x]
  kill (Assign x _)       = [x]
```

### 3.7 Lint Rules & Autofixes (`lint` & `example`)
Every lint rule **must** be accompanied by an `example` block containing both `fires` and `silent` cases:
```haskell
lint unusedBinding :: Warning, Style
  match (Let x _ _)
  when  useCount x == 0 && not ("_" `isPrefixOf` x)
  say   "Variable `{x}` is never used"
  fix   "Prefix with underscore" Safe (replace (nameSpan x) ("_" <> x))

example unusedBinding
  fires  "let x = 1 in 2"
  silent "let x = 1 in x + 1"
  silent "let _x = 1 in 2"
```

### 3.8 Multi-Unit Projects (`project`)
```haskell
project where
  unit     Module
  imports  (Import path names) = unit (resolve path)
  exports  (Export names)      = publish names
  prelude  = "std/prelude"
  search   = [".", "lib"]
  cycles   = Forbidden
```

### 3.9 Lock Files (`<Mod>.psem.lock`)
Parsley computes a cryptographic hash of all AST constructor signatures from `<Mod>.parsley`. If grammar AST constructors change without updating `.psem`, compilation halts with `PSL-LOCK-MISMATCH`.

---

## 4. CLI Workflow & Tooling Reference

The `parsley` binary provides an all-in-one command-line interface:

| Command | Usage | Purpose |
|---|---|---|
| `build` | `parsley build <file> [-o dir] [--no-compile-shared]` | Compiles grammar/semantics into C23 project and shared library (`.dll`/`.so`/`.dylib`). |
| `check` | `parsley check <file>` | Validates syntax, imports, HM types, and PSL attributes without code generation. |
| `test` | `parsley test <file> [--fuzz N]` | Runs in-grammar `example` assertions and property-guided grammar fuzzer. |
| `explain` | `parsley explain <CODE>` | Explains diagnostic codes (supported catalog entries such as `E0001`, `E0600`, `PSL-*`) with bad/good examples and fixes. |
| `fmt` | `parsley fmt <file> [--write\|--check]` | Canonical grammar formatter and PSL whitespace normalizer. |
| `graph` | `parsley graph <file> [--format dot\|svg] [-o file]` | Generates railroad syntax diagrams or Graphviz DOT graphs. |
| `lsp` | `parsley lsp` | Starts the stdio JSON-RPC 2.0 Language Server Protocol daemon. |
| `agent-docs`| `parsley agent-docs [--format markdown\|json]` | Emits machine-readable AST schemas and combinator signatures for AI agents. |
| `pkg` | `parsley pkg <list\|show\|init> [name]` | Manages standard library modules (`Json`, `Sql`, `Python`, `Swift`, etc.). |

---

## 5. Generated ISO C23 Architecture & ABI

The generated C23 codebase adheres to strict engineering constraints:
1. **ISO C23 Standard Compliance**: Compiled with `-std=c23 -Wall -Wextra -pedantic` with zero warnings.
2. **Checked ownership**: Parser allocations use a checked, bounded transaction and are released by `parsley_result_free`; PSL nodes/types use the caller-supplied `psl_arena`. Allocation/resource failures are explicit. See `docs/STATUS.md` for codes and limits.
3. **Tagged Unions for AST**: Every `data` constructor compiles to an `enum tag` and corresponding `union payload`.

### Public C23 ABI Interface (`parser.h`)
```c
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(PARSLEY_STATIC)
  #ifdef PARSLEY_BUILDING_DLL
    #define PARSLEY_API __declspec(dllexport)
  #else
    #define PARSLEY_API __declspec(dllimport)
  #endif
#else
  #define PARSLEY_API __attribute__((visibility("default")))
#endif

PARSLEY_API ParsleyResult parsley_parse(const char* input);
PARSLEY_API ParsleyResult parsley_parse_n(const char* input, size_t len);
PARSLEY_API void          parsley_result_free(ParsleyResult* result);
PARSLEY_API int           parsley_format_ast(const ParsleyResult* result, char* buf, size_t buf_size);
PARSLEY_API int           parsley_run_examples(void);

#ifdef __cplusplus
}
#endif
```

### Semantic Layer C23 ABI (`<mod>_sem.h`)
```c
typedef struct psl_arena psl_arena;
typedef struct psl_diag  psl_diag;
typedef void (*psl_sink)(void *user, const psl_diag *diag);

typedef struct calc_ctx calc_ctx;
typedef struct calc_options {
  int min_status;      /* 0: draft, 1: reviewed, 2: verified */
  uint64_t profile;    /* Bitmask of active lint profiles */
} calc_options;

calc_ctx* calc_ctx_new(psl_arena* arena, const calc_options* opt);
int       calc_analyze(calc_ctx* cx, const Expr* root, psl_sink sink, void* user);
int       calc_psl_selftest(void);
```

### Generated Multi-Build-System Artifacts
Running `parsley build <file> -o <dir>` emits:
- `parser.h` & `include/<mod>_parser.h`
- `parser.c` & `test_parser.c`
- `CMakeLists.txt` (with `SHARED`, `STATIC`, and `ALIAS` targets, plus CTest)
- `meson.build` (with `shared_library`, `static_library`, and `declare_dependency`)
- `Makefile` (portable POSIX C23 Makefile)
- `xmake.lua` (XMake C23 project)
- `BUILD.bazel` & `MODULE.bazel` (Bazel rules)
- `<mod>_parser.pc` (pkg-config descriptor)
- Compiled shared library (`<mod>_parser.dll` / `lib<mod>_parser.so` / `lib<mod>_parser.dylib`)

---

## 6. Best Practices & Rules for AI Agents

1. **Always Verify with Both `check` and `test`**:
   - Run `parsley check <file.parsley>` to catch syntax and HM type errors.
   - Run `parsley test <file.parsley> --fuzz 30` to verify all `example` assertions and exercise the sampled inputs (fuzzing does not prove absence of all parser faults).
2. **Never Edit Generated C23 Code Manually**:
   - Modify the source `.parsley` or `.psem` files and let `parsley build` re-generate the C23 targets.
3. **Maintain Strict Parity Between `.parsley` and `.psem`**:
   - Constructor names and arities used in `node` equations or `lint` match patterns must exactly match the `data` declarations in the imported `.parsley` grammar.
4. **Always Provide Mandatory `example` Blocks for Lints**:
   - Every `lint` must include both `fires` (positive assertion) and `silent` (negative assertion), without colons.
5. **Always Set Provenance Pragmas**:
   - Annotate rules with `{-# SOURCE ... #-}`, `{-# STATUS ... #-}`, and `{-# CONFIDENCE ... #-}`.
6. **Use Layout Formatting**:
   - Format grammar and semantic modules using `parsley fmt <file> --write` to maintain clean, unified diffs.

## 7. Semantic optimization generation (2.1+)

See [docs/OPTIMIZER.md](docs/OPTIMIZER.md) for the authoritative denotation,
effects, ownership and extensible pass ABI contracts. With PSL, build generates
<module>_opt.h/.c and Markdown/JSON inference plans and links the layer into all
generated projects. Automatic inference uses value equations, explicit empty
local effects and the grammar AST, rather than constructor-name heuristics.
Unknown semantics remain opaque; native custom passes can extend the analysis.
Always validate the original program before optimizing. Never assume that a
type equation alone proves a runtime operation pure or gives its evaluation.