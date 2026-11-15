# Parsley Grammar DSL — VSCode & Cursor Extension

Full IDE support for the **Parsley Grammar DSL** (`*.parsley`), compatible with both **Visual Studio Code** (`^1.85.0`) and **Cursor**, with zero external npm dependencies.

## Features

- **Syntax & Semantic Highlighting**:
  - Full TextMate grammar (`source.parsley`) for pragmas (`{-# ... #-}`), comments (`--`, `{- ... -}`), modules, imports, lexer tokens/skips/modes (`push`/`pop`), ADTs (`data`/`newtype`/`foreign data`), operator precedence tables (`operators`, `infixl`/`infixr`/`prefix`/`postfix`/`mixfix`), `do` / `context do` blocks, regex literals (`/.../`), combinators (`<$>`, `<*>`, `<|>`, `<?>`), backtick infix operators, and `example` assertions.
  - LSP semantic tokens (`textDocument/semanticTokens/full`) distinguishing rules, tokens, constructors, data types, keywords, operators, regexes, strings, numbers, comments, and pragmas.
- **Language Server Protocol (`parsley lsp`)**:
  - Real-time diagnostics (`publishDiagnostics`)
  - Hover with inferred rule types, parsing strategy, nullability, `FIRST`, and `FOLLOW` sets
  - Go to Definition & Find All References
  - Document Symbols (Outline & Breadcrumbs)
  - Ormolu-style formatting (`parsley fmt` / `textDocument/formatting`, plus optional `parsley.autoFormatOnSave`)
  - Auto-completion (`textDocument/completion`)
  - Code Actions (quick fixes for `E0301`, `E0214`, plus one-click `Explain Error`)
  - CodeLens on `module` and `example` declarations
- **Live Grammar Playground** (`Parsley: Open Live Grammar Playground`):
  - Interactive side-by-side webview evaluating sample inputs via `parsley/evalInput` with live AST, JSON view, token stream table, and line/column error carets.
- **Railroad Diagram Viewer** (`Parsley: Show Railroad Diagram`):
  - Renders railroad SVG (`parsley/railroadSvg` or `parsley graph --format svg`) with zoom, pan, refresh, and SVG export.
- **Activity Bar Grammar Views**:
  - **Grammar Structure** (`parsleyGrammarExplorer`): Rules (with types & FIRST/FOLLOW), Tokens & Skips, Lexer Modes, and Data Types.
  - **Examples & Tests** (`parsleyExamplesView`): One-click Run and Debug for every `example` assertion.
- **Inline Grammar Debugger (`parsley-debug`)**:
  - Trace rule derivations step-by-step against sample input, set line breakpoints on rules, inspect Parser State / Token Stream / AST / FIRST-FOLLOW in the Variables pane, and evaluate sample expressions live in the Debug Console.
- **ISO C17 Codegen Integration**:
  - Run `Parsley: Build ISO C17 Project & Shared Library` directly from the editor title bar, context menu, or Command Palette.

## Installation (VSCode & Cursor)

### Option 1: Symlink or Copy into Extensions Directory (Zero-Build, Immediate)

This extension ships with a pre-built, zero-dependency `out/extension.js` ready to run immediately.

**Windows (PowerShell):**

```powershell
# For VSCode:
New-Item -ItemType SymbolicLink -Path "$env:USERPROFILE\.vscode\extensions\parsley-lang" -Target "D:\Parsley\editors\vscode"
# Or copy:
Copy-Item -Recurse "D:\Parsley\editors\vscode" "$env:USERPROFILE\.vscode\extensions\parsley-lang"

# For Cursor:
New-Item -ItemType SymbolicLink -Path "$env:USERPROFILE\.cursor\extensions\parsley-lang" -Target "D:\Parsley\editors\vscode"
# Or copy:
Copy-Item -Recurse "D:\Parsley\editors\vscode" "$env:USERPROFILE\.cursor\extensions\parsley-lang"
```

**macOS / Linux:**

```bash
# VSCode
ln -s /path/to/Parsley/editors/vscode ~/.vscode/extensions/parsley-lang

# Cursor
ln -s /path/to/Parsley/editors/vscode ~/.cursor/extensions/parsley-lang
```

Reload the window (`Developer: Reload Window`) and open any `.parsley` file.

### Option 2: Package & Install `.vsix`

```bash
cd editors/vscode
npx --yes @vscode/vsce package
code --install-extension parsley-lang-1.3.0.vsix
# or for Cursor:
cursor --install-extension parsley-lang-1.3.0.vsix
```

## Configuration

| Setting | Default | Description |
| --- | --- | --- |
| `parsley.executablePath` | `"parsley"` | Path to the `parsley` binary. Auto-detects `<workspace>/build/parsley(.exe)` when left as `"parsley"`. |
| `parsley.autoFormatOnSave` | `false` | Automatically format `.parsley` files in Ormolu style on save. |
| `parsley.trace.server` | `"off"` | LSP JSON-RPC trace level (`"off"`, `"messages"`, `"verbose"`). |
| `parsley.c17.outputDir` | `"./generated_parser"` | Default output directory for `Parsley: Build ISO C17 Project & Shared Library`. |
| `parsley.fuzzCount` | `50` | Number of generated inputs for `Parsley: Run Examples & Fuzzer`. |

## Keyboard Shortcuts (when editing `.parsley`)

- `Ctrl+Alt+P C` (`Cmd+Alt+P C` on macOS): Check Grammar
- `Ctrl+Alt+P T` (`Cmd+Alt+P T` on macOS): Run Examples & Fuzzer
- `Ctrl+Alt+P B` (`Cmd+Alt+P B` on macOS): Build ISO C17 Project & Shared Library
- `Ctrl+Alt+P F` (`Cmd+Alt+P F` on macOS): Format Grammar (Ormolu Style)
- `Ctrl+Alt+P R` (`Cmd+Alt+P R` on macOS): Show Railroad Diagram
- `Ctrl+Alt+P P` (`Cmd+Alt+P P` on macOS): Open Live Grammar Playground
- `Ctrl+Alt+P D` (`Cmd+Alt+P D` on macOS): Debug Grammar with Input
