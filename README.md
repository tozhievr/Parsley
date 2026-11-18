# Parsley — Haskell-Inspired Grammar DSL & ISO C23 Parser Generator

Parsley 2.1 also generates a semantic optimizer from PSL value/effect equations,
with an extensible native pass interface. The generated C23 layer performs
constant folding/propagation, known-branch selection and semantic projections,
and reports its inference decisions. See [optimizer contracts and pass examples](docs/OPTIMIZER.md).

[English](#english) | [Русский](#русский)

---

<a name="english"></a>
## English

**Parsley** is an expressive, Haskell-inspired domain-specific language (DSL) and compiler for defining formal grammars and compiling them **exclusively into pure ISO C23** standalone parser frontends and shared/dynamic libraries (`.dll` on Windows, `.so` on Linux, `.dylib` on macOS).

With Parsley, your grammar reads like a high-level Haskell module—featuring algebraic data types (`data`), applicative combinators (`<$>`, `<*>`, `<*`, `*>`), `do`-notation, and precedence tables—while compiling directly into high-performance, zero-dependency C23 code.

---

Current implementation and ABI contracts: [STATUS.md](docs/STATUS.md); accepted regex syntax: [REGEX.md](docs/REGEX.md). Only C23 is generated. The bundled language grammars are subsets; `MODE` selects analysis checks, while the runtime uses ordered recursive descent/Pratt parsing. Regenerate headers and libraries together after the unreleased ABI changes described in STATUS.

Актуальные возможности и контракты: [STATUS.md](docs/STATUS.md), [regex-диалект](docs/REGEX.md). Единственный таргет — C23; встроенные грамматики — подмножества языков. `MODE` выбирает проверки, а не отдельный алгоритм исполнения. После изменений ABI заголовки и библиотеки нужно регенерировать вместе.

### Core Architecture & Philosophy

1. **Haskell-like Syntax & Aesthetics**:
   - Layout-sensitive syntax with significant whitespace.
   - Algebraic Data Types (`data Op = Add | Sub | Mul | Div | Pow`).
   - Automatic Anonymous Tokens: string literals (e.g. `"+"` or `"let"`) automatically become lexer tokens with higher priority than general identifiers.
   - In-grammar Test Assertions (`example "math" : "1 + 2 * 3" parses-to ...` and `example "fail" : "1 +" fails-at (1, 4) expecting "expression"`).

2. **Exclusive ISO C23 Target & Multi-Build-System Project Generation**:
   - Parsley focuses exclusively on ISO C23 (`-std=c23`).
   - `parsley build <file.parsley> -o <dir>` generates a **complete, standalone multi-build-system C23 project** ready for instant compilation or direct integration into any existing codebase:
     - `parser.h` & `include/<module>_parser.h`: Cross-platform public C interface (`PARSLEY_API` symbol export/import, tagged unions `enum` tag + `union` payload for ADTs, `ParsleyResult`, error reporting with line/column/expected tokens).
     - `parser.c`: Maximal-munch lexer with mode stack, recursive-descent parser, Pratt precedence-climbing engine, memory management (`parsley_result_free`), AST formatter (`parsley_format_ast`), and self-test harness (`parsley_run_examples`).
     - `test_parser.c`: Standalone C23 test executable calling `parsley_run_examples()` and validating parse trees.
     - `CMakeLists.txt`: Complete C23 CMake project (`SHARED` `<mod>_parser`, `STATIC` `<mod>_parser_static`, and `ALIAS` `<Mod>::parser` for `add_subdirectory`/`FetchContent`, plus CTest).
     - `meson.build`: Meson C23 project with `shared_library`, `static_library`, `declare_dependency` (`<mod>_parser_dep` for `subproject`), pkg-config generation, and tests.
     - `Makefile`: Portable C23 Makefile (`all`, `shared`, `static`, `test`, `install`, `clean`).
     - `xmake.lua`: XMake build script (`set_languages("c23")`).
     - `BUILD.bazel` & `MODULE.bazel`: Bazel build definitions (`cc_library`, `cc_binary` with `linkshared = True`, `cc_test`).
     - `<module>_parser.pc`: Standard pkg-config metadata file.
     - `README.md`: Integration guide for CMake, Meson, Make, XMake, Bazel, and pkg-config.
     - **Compiled Dynamic Library**: `<module>_parser.dll` (Windows) / `lib<module>_parser.so` (Linux) / `lib<module>_parser.dylib` (macOS) built automatically by default (pass `--no-compile-shared` to generate source project only).

3. **Public C23 ABI (`parser.h`)**:
   ```c
   #ifdef __cplusplus
   extern "C" {
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

4. **Static Grammar Analysis**:
   - Computes `NULLABLE`, `FIRST`, and `FOLLOW` sets.
   - Detects unreachable rules (`E0520`) and left recursion (`E0500` in LL analysis; mutual cycles are rejected in every profile).
   - Automated left-factoring for overlapping prefixes (e.g. `Call` vs `Var` in `atom`).
   - Conflict diagnostics with concrete counterexamples and explanations (e.g. `parsley explain E0214`).

5. **Modern C++26 Modules Compiler**:
   - Modular compiler implementation in C++26 (`.cppm`) built with Clang 23.1.3, CMake 4.5.0-rc1, and Ninja.
   - Global Module Fragment headers ensure reliable multi-platform standard library interoperability.

---

### Grammar Example (`Calc.parsley`)

```haskell
module Calc (program) where

import Parsley.Prelude (sepBy, many, eof)

-- Lexical rules
skip  ws      = /[[:space:]]+/
skip  comment = /--.*/
token number  = /[0-9]+([.][0-9]+)?/   :: Double
token ident   = /[a-z_][a-zA-Z0-9_]*/  :: Text
keywords      = ["let", "in"]

-- AST definitions (derived as C23 tagged unions)
data Op = Add | Sub | Mul | Div | Pow

data Expr
  = Num  Double
  | Var  Text
  | Neg  Expr
  | Bin  Op Expr Expr
  | Call Text [Expr]
  | Let  Text Expr Expr

-- Syntactic rules
program :: Expr
program = expr <* eof

expr :: Expr
expr = operators atom
  [ infixl 6 ["+" => Bin Add, "-" => Bin Sub]
  , infixl 7 ["*" => Bin Mul, "/" => Bin Div]
  , infixr 8 ["^" => Bin Pow]
  , prefix  9 ["-" => Neg]
  ]

atom :: Expr
atom
  =  Num  <$> number
  |  Call <$> ident <*> parens (expr `sepBy` ",")
  |  Var  <$> ident
  |  parens expr
  |  letExpr

letExpr :: Expr
letExpr = do
  "let"
  x    <- ident
  "="
  e    <- expr
  "in"
  body <- expr
  pure (Let x e body)

parens :: Rule a -> Rule a
parens p = "(" *> p <* ")"

-- In-grammar assertions & self-test suite
example "арифметика" : "1 + 2 * 3" parses-to Bin Add (Num 1) (Bin Mul (Num 2) (Num 3))
example "ошибка" : "1 +" fails-at (1, 4) expecting "expression"
```

---

### ANTLR4-Inspired Language Subsets & ISO C23 Static Analyzers

Parsley includes representative grammar subsets based on ANTLR4 grammars, each paired with an end-to-end static analyzer written in pure ISO C23 linked against the generated parser dynamic library:

1. **Python 3 (`tests/fixtures/Python.parsley` & `tests/analyzers/python_analyzer.c`)**:
   - Translated from `PythonLexer.g4` & `PythonParser.g4`.
   - Supports `import`, `from ... import`, `def` (with type annotations), `class` (with inheritance), `if/elif/else`, `while`, `for...in`, `return/pass/break/continue`, augmented assignments, precedence-climbing expressions, and left-recursive postfix expressions (`PyCall`, `PyIndex`, `PyAttr`).
   - C23 Static Analyzer computes: function/class counts, assignments, cyclomatic complexity, call graphs, lint checks (division by zero literals, empty `pass`-only functions, duplicate function definitions).

2. **Swift 5 (`tests/fixtures/Swift.parsley` & `tests/analyzers/swift_analyzer.c`)**:
   - Translated from `Swift5Lexer.g4` & `Swift5Parser.g4`.
   - Supports `import`, `struct`, `class`, `enum`, `func`, `let`, `var`, `if/else`, `guard...else`, `while`, `for...in`, `return`, and operator precedence.
   - C23 Static Analyzer computes: struct/class/func counts, `let` vs `var` immutability ratio, `guard` counts, cyclomatic complexity, and warnings (mutable vars, division by zero, duplicate declarations).

3. **Kotlin (`tests/fixtures/Kotlin.parsley` & `tests/analyzers/kotlin_analyzer.c`)**:
   - Translated from `KotlinLexer.g4` & `KotlinParser.g4`.
   - Supports `package`, `import`, `class`, `interface`, `fun` (block & expression bodies), `val`, `var`, `if/else`, `when` branches, `while`, `for...in`, and expressions.
   - C23 Static Analyzer computes: package hierarchy, import lists, interface/class/func metrics, `val` vs `var` ratios, `when` branch metrics, and lint checks (division by zero, duplicate declarations).

4. **Haskell 2010 (`tests/fixtures/Haskell.parsley` & `tests/analyzers/haskell_analyzer.c`)**:
   - Translated from `HaskellLexer.g4` & `HaskellParser.g4`.
   - Supports `module ... where`, `import` (qualified/as/hiding), `data` ADTs, `type` synonyms, `::` type signatures, function equations, `if/then/else`, `let/in`, `case/of`, and operators.
   - C23 Static Analyzer computes: ADTs, constructors, type signatures, function bindings, recursive self-call detection, missing top-level signatures, and division by zero checks.

---

### VSCode & Cursor IDE Extension (`editors/vscode/`)

Parsley comes with a full-featured, zero-external-npm-dependency extension for both **Visual Studio Code** and **Cursor**:

- **Syntax & Semantic Highlighting**:
  - TextMate grammar (`source.parsley`) covering pragmas, keywords, regex literals, strings, constructors, combinators, and indentation.
  - Full LSP 12-type / 3-modifier semantic tokens support (`textDocument/semanticTokens/full`).
- **Language Server Protocol (LSP)**:
  - Real-time diagnostic reporting on edit with inline error messages and error codes.
  - Hover provider displaying inferred rule types, FIRST/FOLLOW sets, and parsing strategy.
  - Go to Definition (`textDocument/definition`) and Find All References (`textDocument/references`).
  - Hierarchical Document Symbols (`textDocument/documentSymbol`) for outline views.
  - Ormolu-style code formatting (`textDocument/formatting`).
  - Auto-completion (`textDocument/completion`) for rules, tokens, ADT constructors, and prelude combinators.
  - Quick-fix CodeActions (`textDocument/codeAction`) for `E0301` (add missing type signature) and `E0214` (add `{-# NOFACTOR #-}`).
  - CodeLenses for building C23, opening railroad diagrams, or debugging grammar examples.
- **Interactive Webviews**:
  - **Live Grammar Playground**: test input strings against any rule in real time, view formatted ASTs, token streams, or syntax errors.
  - **Interactive Railroad Diagrams**: visualize grammar rules as SVG syntax diagrams with pan and zoom.
- **Sidebar Explorers**:
  - `Grammar Explorer`: browse rules with types and FIRST/FOLLOW sets, tokens, modes, and ADTs.
  - `Examples View`: inspect in-grammar `example` test assertions with one-click Run and Debug.
- **Inline Grammar Debugger (`parsley-debug`)**:
  - Set breakpoints on grammar rule definitions.
  - Step through rule derivations, inspect current tokens, input position (line/col), and parsed AST values live.

#### Installing the Extension

```bash
# In VS Code
code --install-extension ./parsley-lang-1.6.1.vsix --force

# In Cursor
cursor --install-extension ./parsley-lang-1.6.1.vsix --force
```
Or symlink `editors/vscode/` directly to `~/.vscode/extensions/parsley-lang` or `~/.cursor/extensions/parsley-lang`.

---

### One-Line Installation / Установка одной командой

Run from the directory containing the downloaded package. CLI version: **1.3.1**; extension version: **1.6.1**. Choose the archive for your OS and architecture.

Запускайте из каталога со скачанным пакетом. Версия CLI — **1.3.1**, расширения — **1.6.1**. Выбирайте архив под свою ОС и архитектуру.

**Windows x64 — PowerShell**. Installs for the current user and updates PATH for current and future terminals. Установка в профиль пользователя с добавлением в PATH текущего и будущих терминалов:

```powershell
$ErrorActionPreference='Stop'; $d="$env:LOCALAPPDATA\Programs\Parsley"; Expand-Archive ./parsley-1.3.1-win64.zip $d -Force; $b="$d\parsley-1.3.1-win64\bin"; $p=[Environment]::GetEnvironmentVariable('Path','User'); if (($p -split ';') -notcontains $b) { [Environment]::SetEnvironmentVariable('Path',("$p;$b".Trim(';')),'User') }; $env:Path+=";$b"; parsley --version
```

**Linux — Bash**. Installs to `/usr/local/bin` using `sudo`. Requires a Linux build; the Windows archive cannot run on Linux. Установка через `sudo`; необходим Linux-пакет:

```bash
(set -eu; d=$(mktemp -d); trap 'rm -rf -- "$d"' EXIT; tar -xzf ./parsley-1.3.1-Linux.tar.gz -C "$d"; sudo install -m 755 "$d"/parsley-1.3.1-Linux/bin/parsley /usr/local/bin/parsley; parsley --version)
```

For another version, update both the archive name and extracted directory. These commands use local archives; this checkout has no configured public download URL. The toolchain below is needed for building from source; generating shared C23 libraries also requires a C compiler.

Для другой версии замените номер в имени архива и каталога внутри него. Команды используют локальные архивы: публичный URL скачивания в этом checkout не настроен. Инструменты ниже нужны для сборки из исходников; генерация динамических библиотек C23 также требует компилятора C.

**VS Code / Cursor — Windows & Linux** (from the directory with the VSIX / из каталога с VSIX):

```sh
code --install-extension ./parsley-lang-1.6.1.vsix --force
```

For Cursor, replace `code` with `cursor`. Для Cursor замените `code` на `cursor`.

### Prerequisites & Toolchain

- **CMake**: 4.5.0-rc1 or newer (`cmake_minimum_required(VERSION 4.5.0)`)
- **Ninja**: Build system generator
- **Compiler**: Clang 23.1.3 (with C++26 modules and ISO C23 support)
- **Platforms**: Windows 11/Server (MSVC STL), Linux (libstdc++ / libc++), macOS (Apple Silicon / Intel libc++)

---

### Building Parsley Compiler

#### On Windows (Ninja + Clang 23)

```powershell
# Configure using Clang 23 and Ninja
cmake -B build -G Ninja `
  -DCMAKE_CXX_COMPILER="D:/clang-23.1.3/bin/clang++.exe" `
  -DCMAKE_C_COMPILER="D:/clang-23.1.3/bin/clang.exe" `
  -DCMAKE_BUILD_TYPE=Release

# Build all targets (parsley compiler + tests)
cmake --build build --config Release

# Run automated tests
ctest --test-dir build --output-on-failure -C Release

# Create release packages (ZIP, TGZ)
cpack --config build/CPackConfig.cmake -B dist
```

#### On Linux / macOS

```bash
# Configure with Ninja and Clang
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --parallel

# Test
ctest --test-dir build --output-on-failure

# Package
cpack --config build/CPackConfig.cmake -B dist
```

---

### CLI Reference

| Command | Contract / flags |
|---|---|
| `build <file.parsley\|file.psem> [-o dir]` | Generate C23; semantic files use the adjacent grammar. `--no-compile-shared`, `--min-status draft|reviewed|verified`, `--emit-agent-docs`, `--emit-ir-json`, `--emit-ir-sexpr` |
| `check <file.parsley\|file.psem>` | Validate grammar/PSL; accepts `--min-status` |
| `test <file.parsley\|file.psem> [--fuzz N] [--seed N]` | Grammar and PSL assertions; `N` is 0–1,000,000, default seed 42; accepts `--min-status` |
| `fmt <file> [--write\|-w\|--check\|-c]` | Print canonical grammar formatting or PSL whitespace normalization; write/check are mutually exclusive |
| `graph <file.parsley> [--format dot\|svg] [-o file]` | Grammar visualization |
| `agent-docs [--format markdown\|json] [-o path] [file.parsley]` | Agent reference/schema output |
| `explain <CODE>` | Explain a supported diagnostic code; codes are a catalog, not every number in a contiguous range |
| `lsp` | Full-document stdio JSON-RPC language server with UTF-16 positions |
| `pkg <list\|show\|init> [name] [directory]` | Inspect bundled modules or scaffold a package; refuses existing files |
| `--version`, `-V`, `version`; `--help`, `-h`, `help` | Version/help aliases |

`-b/--backend c23` and `c` select the sole code-generation backend. Compatibility values `ir-json`/`ir-sexpr` produce IR dumps, not another executable-language target. `-v/--verbose` includes analysis notes; `--no-color` is accepted and diagnostics are plain text. Exit 0 indicates success; 1 indicates a failed check/test/I/O or ordinary usage error; 2 indicates rejected bounded numeric options or a caught compiler/protocol failure. `PARSLEY_CC` is a path/name of one C compiler executable, without shell syntax or appended flags.

`program = expr <* eof` requires a complete parse. Without `eof`, a generated parser may accept a prefix; inspect `result.consumed`. Generic `parsley_*` names in examples are compatibility aliases for the module-prefixed symbols. See [STATUS.md](docs/STATUS.md) for resource/ownership rules and multiple-parser integration.

---
### Standard Library (`parsley.stdlib`)

Parsley includes built-in standard modules accessible via `import`:

- **`Parsley.Prelude`**: Primitives: `sepBy`, `sepEndBy`, `many`, `some`, `optional`, `between`, `eof`, `lookahead`, `notFollowedBy`, `recover`, `exactly`.
- **`Parsley.Ascii`** (legacy alias `Parsley.Unicode`, ASCII only): Tokens for character classes: `alpha`, `digit`, `hexDigit`, `space`, `whitespace`, `upper`, `lower`, `letter`, `alphanum`, `newline`.
- **`Parsley.Layout`**: Layout tokens: `indent`, `dedent`, `newline`, `sameIndent`, `block`.
- **`Json`**: JSON grammar subset; consult its examples for coverage.
- **`Sql`**: ANSI SQL grammar subset (SELECT, INSERT, UPDATE, DELETE).
- **`Python`**: Python 3 grammar subset derived from ANTLR4 Python3.
- **`Swift`**: Swift 5 grammar subset derived from ANTLR4 Swift5.
- **`Kotlin`**: Kotlin grammar subset derived from ANTLR4 Kotlin.
- **`Haskell`**: Haskell 2010 grammar subset derived from ANTLR4 Haskell.

---

### Semantic Versioning Workflow

The compiler version is read only from `VERSION`; CMake generates `build/generated/version.cppm` from `src/version.cppm.in`. The extension version is independent in `editors/vscode/package.json`:

```bash
# Bump patch: 0.1.0 -> 0.1.1
cmake -DBUMP=patch -P scripts/bump_semver.cmake

# Bump minor: 0.1.0 -> 0.2.0
cmake -DBUMP=minor -P scripts/bump_semver.cmake

# Bump major: 0.2.0 -> 1.0.0
cmake -DBUMP=major -P scripts/bump_semver.cmake

# Set explicit version
cmake -DSET_VERSION=1.0.0 -P scripts/bump_semver.cmake
```

---

<a name="русский"></a>
## Русский

**Parsley** — это выразительный предметно-ориентированный язык (DSL) в стиле Haskell и компилятор грамматик, ориентированный **исключительно на генерацию парсеров на чистом ISO C23** и компиляцию динамических/разделяемых библиотек (`.dll` на Windows, `.so` на Linux, `.dylib` на macOS).

Грамматика описывается как обычный Haskell-модуль: с сигнатурами, алгебраическими типами данных (`data`), каррированием, аппликативными комбинаторами (`<$>`, `<*>`, `<*`, `*>`), блоками `do` и таблицами операторов `operators`.

---

### Ключевые особенности

1. **Синтаксис Haskell**:
   - Layout вместо фигурных скобок и точек с запятой.
   - Алгебраические типы данных (`data Expr = ...`).
   - Автоматические анонимные токены: строковые литералы (`"("`, `"let"`) имеют приоритет над регулярными выражениями идентификаторов.
   - Тесты `example` прямо в грамматике для компиляционной верификации.

2. **Исключительная ориентация на ISO C23 и мульти-сборочные проекты**:
   - Компилятор генерирует исключительно переносимый ISO C23 (`-std=c23`).
   - Команда `parsley build <file.parsley> -o <dir>` создаёт **полный готовый C23-проект** для сборки и интеграции в любую экосистему:
     - `parser.h` и `include/<module>_parser.h`: публичный интерфейс с макросом экспорта `PARSLEY_API`, размеченными объединениями (`tagged unions`), структурой `ParsleyResult` и функциями `parsley_parse`, `parsley_parse_n`, `parsley_result_free`, `parsley_format_ast`, `parsley_run_examples`.
     - `parser.c`: полная реализация лексера, парсера (рекурсивный спуск + precedence climbing), управления памятью и тестов.
     - `test_parser.c`: автономный тестовый исполняемый файл C23.
     - `CMakeLists.txt`: C23 проект с целями `SHARED`, `STATIC` и `ALIAS` для бесшовного подключения через `add_subdirectory` или `FetchContent`.
     - `meson.build`: проект Meson с `shared_library`, `static_library`, зависимостью `subproject` и тестами.
     - `Makefile`: переносимый Makefile (`all`, `shared`, `static`, `test`, `install`, `clean`).
     - `xmake.lua`: файл сборки для XMake (`set_languages("c23")`).
     - `BUILD.bazel` и `MODULE.bazel`: цели для сборки в Bazel.
     - `<module>_parser.pc`: файл конфигурации `pkg-config`.
     - `README.md`: руководство по интеграции во все поддерживаемые системы сборки.
     - **Скомпилированная динамическая библиотека**: `<module>_parser.dll` (Windows) / `lib<module>_parser.so` (Linux) / `lib<module>_parser.dylib` (macOS), собираемая автоматически (флаг `--no-compile-shared` отключает автокомпиляцию).

3. **Статический анализ**:
   - Построение множеств `NULLABLE`, `FIRST` и `FOLLOW`.
   - Обнаружение мёртвых правил (`E0520`) и левой рекурсии (`E0500`).
   - Автоматический left-factoring общих префиксов.
   - Объяснение ошибок с контрпримерами (`parsley explain E0214`).

4. **Архитектура компилятора на C++26**:
   - Модули C++26 (`.cppm`) с Clang 23.1.3, CMake 4.5.0-rc1 и Ninja.
   - Использование Global Module Fragment для безопасного подключения стандартных библиотек на всех платформах.

---

### Продакшн-грамматики ANTLR4 и статические анализаторы C23

В поставку Parsley включены 4 грамматики реальных языков программирования, транслированные из ANTLR4, а также соответствующие автономные статические анализаторы на ISO C23:

1. **Python 3 (`tests/fixtures/Python.parsley` & `tests/analyzers/python_analyzer.c`)**:
   - Полноценная поддержка синтаксиса Python 3: `import`, `from ... import`, `def`, `class`, операторы ветвления и циклов, аннотации типов, операторы с приоритетами.
   - Анализатор C23: подсчёт функций, классов, присваиваний, цикломатической сложности, граф вызовов и линтинг (деление на ноль, пустые функции с `pass`, дублирующиеся имена).

2. **Swift 5 (`tests/fixtures/Swift.parsley` & `tests/analyzers/swift_analyzer.c`)**:
   - Поддержка `struct`, `class`, `enum`, `func`, `let`/`var`, `guard...else`, циклов и операторов.
   - Анализатор C23: соотношение иммутабельности `let`/`var`, анализ функций и классов, цикломатическая сложность, предупреждения линтера.

3. **Kotlin (`tests/fixtures/Kotlin.parsley` & `tests/analyzers/kotlin_analyzer.c`)**:
   - Поддержка пакетов, импортов, классов, интерфейсов, функций, веток `when`, циклов.
   - Анализатор C23: иерархия пакетов, метрики объявления классов и функций, проверка корректности выражений.

4. **Haskell 2010 (`tests/fixtures/Haskell.parsley` & `tests/analyzers/haskell_analyzer.c`)**:
   - Поддержка определений модулей, импортов, ADT-типов, уравнений функций, сигнатур типов `::`, `case/of`, `let/in`.
   - Анализатор C23: обнаружение рекурсивных функций, отсутствующих сигнатур верхнего уровня, деления на ноль.

---

### Расширение для VSCode и Cursor (`editors/vscode/`)

Полнофункциональное расширение без внешних зависимостей npm:
- **Подсветка синтаксиса**: TextMate грамматика (`source.parsley`) и семантические токены LSP (12 типов, 3 модификатора).
- **LSP-возможности**:
  - Диагностика в реальном времени.
  - Hover с выведенными типами и множествами FIRST/FOLLOW.
  - Переход к определению и поиск ссылок.
  - Иерархические символы документа (`DocumentSymbol`).
  - Форматирование по стилю Ormolu.
  - Автодополнение правил, токенов, конструкторов и ключевых слов.
  - Quick-fix исправления (`E0301`, `E0214`).
  - CodeLens для сборки, визуализации и запуска тестов.
- **Интерактивные Webview**:
  - **Live Playground**: интерактивный ввод строк с просмотром AST и ошибок.
  - **Railroad Diagrams**: интерактивные SVG-диаграммы правил грамматики.
- **Встроенный отладчик грамматик (`parsley-debug`)**:
  - Точки останова на правилах.
  - Пошаговое выполнение разбора входных данных с просмотром промежуточных токенов и AST.

---

### Установка готовых пакетов

Однострочные команды для Windows, Linux, VS Code и Cursor: [установка готовых пакетов](#one-line-installation--установка-одной-командой).

### Сборка компилятора

#### Windows (Ninja + Clang 23)

```powershell
cmake -B build -G Ninja `
  -DCMAKE_CXX_COMPILER="D:/clang-23.1.3/bin/clang++.exe" `
  -DCMAKE_C_COMPILER="D:/clang-23.1.3/bin/clang.exe" `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
cpack --config build/CPackConfig.cmake -B dist
```

#### Linux / macOS

```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -B dist
```

---

### Команды CLI

Полная таблица флагов приведена в [CLI Reference](#cli-reference). `build/check/test/fmt` работают с `.parsley` и `.psem`; `graph` требует грамматику. `test --fuzz N --seed S` воспроизводим, `fmt --write` и `--check` несовместимы. `pkg init` создаёт шаблон проекта и не перезаписывает существующие файлы; это не сетевой менеджер зависимостей.

C23 — единственный таргет. IR JSON/S-expression — форматы инспекции. `MODE` задаёт профиль анализа. Для полного разбора добавляйте `<* eof`; правила памяти, ABI и ограничений приведены в [STATUS.md](docs/STATUS.md). Версия компилятора берётся из `VERSION`, расширение версионируется независимо.

### Управление версиями (SemVer)

Файл `VERSION` — единственный источник версии компилятора. CMake генерирует `build/generated/version.cppm` из шаблона `src/version.cppm.in`; версия расширения независима:

```bash
cmake -DBUMP=patch -P scripts/bump_semver.cmake
cmake -DBUMP=minor -P scripts/bump_semver.cmake
cmake -DBUMP=major -P scripts/bump_semver.cmake
cmake -DSET_VERSION=1.0.0 -P scripts/bump_semver.cmake
```

---

### Лицензия

Parsley распространяется под лицензией MIT.
