# Contributing

The compiler uses Clang 23+, CMake 4.5+ and Ninja. Generated projects use ISO C17 only.
Configure with `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release`, then run `cmake --build build` and `ctest --test-dir build --output-on-failure`.

For the extension run `npm ci --prefix editors/vscode`, `npm run check --prefix editors/vscode`, `npm run compile --prefix editors/vscode`, and `npm test --prefix editors/vscode`. Commit the generated `out/extension.js` together with its TypeScript source. `npm run package --prefix editors/vscode` builds the installable VSIX.

On Linux/macOS add `-DPARSLEY_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug` in a separate build directory for ASan/UBSan. The audit regression suite also accepts `PARSLEY_SANITIZE_GENERATED=1` to instrument generated C17 consumers.

Add focused regression coverage for compiler/ABI/LSP changes. Keep `.parsley` and `.psem` constructors aligned, provide fires and silent examples for every lint, and verify grammar changes using both `check` and `test --fuzz 30`. Do not edit generated C17 artifacts. Compiler version comes exclusively from `VERSION`; the extension has an independent version in `editors/vscode/package.json`.