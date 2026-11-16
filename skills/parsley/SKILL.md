---
name: parsley
description: Step-by-step instructions, reference patterns, common pitfalls, and operational workflows for authoring, testing, and building Parsley grammars (.parsley) and Parsley Semantic Layer specifications (.psem). Use when creating new language grammars, defining ASTs, adding static semantics, configuring linters, or compiling ISO C17 parser libraries.
---

# Authoring Parsley Grammars & PSL Semantics

Use `docs/STATUS.md` and `docs/REGEX.md` for current implementation limits. Only C17 is generated. `MODE` controls analysis constraints; execution uses ordered recursive descent/Pratt parsing. Parser results own checked allocations; only PSL data uses the caller's arena. Fuzz samples do not prove universal parser safety.

This skill guides AI agents through designing, developing, testing, and compiling domain-specific or general-purpose programming languages using the **Parsley** DSL ecosystem.

---

## 1. Core Workflow for Grammar Development (`.parsley`)

When creating a new grammar `<Name>.parsley`:

### Step 1: Define Module Header & Analysis Profile
```haskell
{-# START program #-}
{-# MODE PEG #-}
module MyLang (program) where

import Parsley.Prelude (sepBy, sepEndBy, many, some, optional, between, eof)
```
- Use `{-# MODE PEG #-}` to select PEG-oriented analysis checks; it does not change the C17 runtime algorithm.
- Always import `eof` from `Parsley.Prelude` to ensure the top-level rule consumes the entire input.

### Step 2: Define Lexical Rules
```haskell
-- Skipped tokens (whitespace, comments)
skip ws          = /[[:space:]]+/
skip lineComment = /\/\/[^\n]*/

-- Typed lexical tokens
token numberLit = /[0-9]+([.][0-9]+)?/   :: Double
token stringLit = /"([^"\\]|\\.)*"/      :: Text
token ident     = /[a-zA-Z_][a-zA-Z0-9_]*/ :: Text

-- Reserved keywords (automatically prioritized over ident)
keywords = ["let", "in", "if", "then", "else", "true", "false"]
```

### Step 3: Define AST Algebraic Data Types (`data`)
Structure the AST cleanly. In the generated ISO C17 code, each `data` declaration becomes a tagged union:
```haskell
data Op = Add | Sub | Mul | Div | Eq

data Expr
  = Num Double
  | Str Text
  | Bool Bool
  | Var Text
  | Bin Op Expr Expr
  | If Expr Expr Expr
  | Let Text Expr Expr
```

### Step 4: Define Precedence Tables & Syntactic Rules
Use `operators` for arithmetic and logical expressions rather than manual stratification:
```haskell
program :: Expr
program = expr <* eof

expr :: Expr
expr = operators atom
  [ infixl 1 ["==" => Bin Eq]
  , infixl 2 ["+"  => Bin Add, "-" => Bin Sub]
  , infixl 3 ["*"  => Bin Mul, "/" => Bin Div]
  ]

atom :: Expr
atom
  =  Num  <$> numberLit
  |  Str  <$> stringLit
  |  "true"  *> pure (Bool True)
  |  "false" *> pure (Bool False)
  |  Var  <$> ident
  |  "(" *> expr <* ")"
  |  letExpr

letExpr :: Expr
letExpr = do
  "let"
  x <- ident
  "="
  v <- expr
  "in"
  body <- expr
  pure (Let x v body)
```

### Step 5: Add In-Grammar Test Assertions
Always include at least 3–5 positive parse assertions and 2 negative syntax error assertions:
```haskell
example "addition"   : "1 + 2 * 3" parses-to Bin Add (Num 1) (Bin Mul (Num 2) (Num 3))
example "let-expr"   : "let x = 1 in x + 2" parses-to Let "x" (Num 1) (Bin Add (Var "x") (Num 2))
example "err-syntax" : "1 +" fails-at (1, 4) expecting "expression"
```

### Step 6: Verify Grammar
Run the CLI toolchain to validate:
```bash
# Check syntax and Hindley-Milner type inference
parsley check MyLang.parsley

# Run in-grammar example assertions and fuzzing
parsley test MyLang.parsley --fuzz 50

# Format canonically
parsley fmt MyLang.parsley --write
```

---

## 2. Core Workflow for Static Semantics (`.psem`)

Static semantics are defined in `<Name>.psem` in the same directory as `<Name>.parsley`.

### Step 1: Module Header & Provenance Pragmas
```haskell
{-# LANGUAGE Sem1 #-}
{-# SOURCE ref "MyLang Specification 1.0" #-}
{-# STATUS verified #-}
{-# CONFIDENCE 0.98 #-}

semantics MyLang where

import MyLang
import Parsley.Sem.Prelude

source ref = manual "MyLang Language Specification" rev "1.0"
```

### Step 2: Types, Kinds & Diagnostic Catalog
```haskell
type Number = Double
type Boolean = Bool

kind Variable where shadowing = Warn

diag E001 :: Error "Undefined variable `{name}`"
diag E002 :: Error "Operand type mismatch: expected {expected}, got {actual}"
diag W001 :: Warning "Variable `{name}` is never used"
```

### Step 3: Node Equations (`node`)
Specify symbol declarations, scope boundaries, and type unification:
```haskell
node Num _ where
  type = Number

node Bool _ where
  type = Boolean

node Var x where
  sym  <- lookup Variable x `orFail` E001 { name = x }
  type =  typeOf sym

node Let x v body where
  scope body $ declare Variable x (typeOf v)
  type = typeOf body

node Bin op l r where
  require (typeOf l == Number && typeOf r == Number)
    `orFail` E002 { expected = "Number", actual = show (typeOf l) }
  type = Number
```

### Step 4: Control Flow & Dataflow (`flow` & `analysis`)
```haskell
flow If c t e = branch c [t, e]

analysis Initialized :: Forward, Must Symbol
  gen  (Let x _ _) = [x]

analysis Live :: Backward, May Symbol
  gen  (Var x)     = [x]
  kill (Let x _ _) = [x]
```

### Step 5: Linter Rules with Mandatory Self-Tests (`lint`)
Every `lint` rule **must** have an `example` block containing both `fires` and `silent` cases:
```haskell
lint unusedVariable :: Warning, Style
  match (Let x _ _)
  when  useCount x == 0 && not ("_" `isPrefixOf` x)
  say   "Variable `{x}` is declared but never read"
  fix   "Prefix with underscore" Safe (replace (nameSpan x) ("_" <> x))

example unusedVariable
  fires  "let x = 1 in 2"
  silent "let x = 1 in x + 1"
  silent "let _unused = 1 in 2"

profile default = [unusedVariable]
```

### Step 6: Validate Semantics
```bash
parsley check MyLang.psem
```

---

## 3. Compiling to ISO C17 (`parsley build`)

To compile the grammar and static semantic layer into production-ready ISO C17:

```bash
# Builds complete multi-build-system C17 project and compiles shared library
parsley build MyLang.parsley -o ./out_mylang
```

This generates:
- `parser.h` & `include/mylang_parser.h`: Public C17 interface with tagged unions.
- `parser.c`: Maximal-munch lexer and recursive-descent parser.
- `mylang_sem.h` & `mylang_sem.c`: 5-pass static semantic analyzer.
- `test_parser.c`: Standalone C17 test runner.
- `CMakeLists.txt`, `meson.build`, `Makefile`, `xmake.lua`, `BUILD.bazel`.
- Compiled dynamic library (`mylang_parser.dll` / `libmylang_parser.so` / `libmylang_parser.dylib`).

---

## 4. Common Pitfalls & Solutions

| Pitfall | Cause | Solution |
|---|---|---|
| `E0214: ambiguous alternatives` | Overlapping prefix in two rules (e.g. `Call` vs `Var` in `atom`). | Parsley auto-factors by default. Do not add `{-# NOFACTOR #-}` unless debugging. |
| `E0500: direct left recursion` | Rule calls itself directly at the start in LL mode. | Use `operators` table for expression trees or use PEG mode (`{-# MODE PEG #-}`). |
| `E0301: missing type signature` | Exported rule has no explicit `:: Type` signature. | Add top-level signature, e.g. `expr :: Expr`. |
| `E0310: monadic binding in ApplicativeDo` | Non-`context` `do` block uses a bound variable in subsequent parser. | Use `context do` instead of `do` for dynamic grammar dependencies. |
| `PSL-EXAMPLE-MISSING` | A `lint` rule is missing a corresponding `example` block. | Add `example <lintName>` with both `fires` and `silent` cases. |
| `PSL-LOCK-MISMATCH` | AST constructors in `.parsley` were modified without updating `.psem.lock`. | Update `.psem` to match new AST constructors and rebuild lock file. |
| `PSL-CYCLE` | Attribute cycle detected in `node` equations. | Ensure synthesized attributes (`type = ...`) depend only on children or inherited attributes. |

---

## 5. Reference Implementation: Mini Language

### `Mini.parsley`
```haskell
{-# MODE PEG #-}
module Mini (program) where

import Parsley.Prelude (sepBy, many, eof)

skip ws = /[[:space:]]+/

token numberLit = /[0-9]+/                 :: Double
token ident     = /[a-zA-Z_][a-zA-Z0-9_]*/ :: Text

keywords = ["if", "else", "while", "return", "var"]

data Stmt
  = VarDecl Text Expr
  | Assign Text Expr
  | IfStmt Expr [Stmt] [Stmt]
  | WhileStmt Expr [Stmt]
  | ReturnStmt Expr

data Expr
  = Num Double
  | Var Text
  | Add Expr Expr

program :: [Stmt]
program = many stmt <* eof

stmt :: Stmt
stmt
  =  (do "var"; x <- ident; "="; e <- expr; ";"; pure (VarDecl x e))
  |  (do x <- ident; "="; e <- expr; ";"; pure (Assign x e))
  |  (do "if"; "("; c <- expr; ")"; "{"; t <- many stmt; "}"; "else"; "{"; e <- many stmt; "}"; pure (IfStmt c t e))
  |  (do "while"; "("; c <- expr; ")"; "{"; b <- many stmt; "}"; pure (WhileStmt c b))
  |  (do "return"; e <- expr; ";"; pure (ReturnStmt e))

expr :: Expr
expr = operators atom
  [ infixl 1 ["+" => Add] ]

atom :: Expr
atom
  =  Num <$> numberLit
  |  Var <$> ident
  |  "(" *> expr <* ")"

example "mini-assign" : "var x = 10; x = x + 1;" parses-to [VarDecl "x" (Num 10), Assign "x" (Add (Var "x") (Num 1))]
example "mini-err"    : "var x =" fails-at (1, 10) expecting "expression"
```

### `Mini.psem`
```haskell
{-# LANGUAGE Sem1 #-}
{-# STATUS verified #-}
{-# CONFIDENCE 1.0 #-}

semantics Mini where

import Mini
import Parsley.Sem.Prelude

type Number = Double
kind Variable

diag E001 :: Error "Undefined variable `{name}`"
diag W001 :: Warning "Unreachable code"

node Num _ where
  type = Number

node Var x where
  sym  <- lookup Variable x `orFail` E001 { name = x }
  type = typeOf sym

node Add l r where
  type = Number

node VarDecl x e where
  declare Variable x (typeOf e)
  type = Number

node Assign x e where
  lookup Variable x `orFail` E001 { name = x }
  type = Number

flow IfStmt c t e    = branch c [t, e]
flow WhileStmt c b   = loop c b
flow ReturnStmt e    = jump e Exit

lint unreachableStmt :: Warning
  match (s :: Stmt)
  when  not (reachable s)
  say   "Unreachable statement"

example unreachableStmt
  fires  "return 1; var x = 2;"
  silent "var x = 1; return x;"

profile default = [unreachableStmt]
```
