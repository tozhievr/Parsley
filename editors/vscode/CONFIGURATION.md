# Parsley configuration in VS Code / Cursor (extension 2.1, compiler 2.3)

Open Parsley: Open Settings to edit settings in either editor. All Parsley
settings are resource-scoped, so workspace folders can use different tools
and build policies. Use Parsley: Select Profile to select a named profile
for the current folder; Default settings clears that override.

## Example workspace settings

~~~json
{
  "parsley.executablePath": "${workspaceFolder}/build/parsley.exe",
  "parsley.activeProfile": "native",
  "parsley.profiles": {
    "native": {
      "outputDir": "${workspaceFolder}/generated/${fileBasenameNoExtension}",
      "emitNextc": true,
      "compileCompiler": true,
      "compileShared": false,
      "minStatus": "reviewed",
      "emitIrJson": true,
      "emitAgentDocs": true,
      "cCompiler": "clang",
      "timeoutMs": 180000,
      "fuzzCount": 100,
      "fuzzSeed": "42",
      "environment": { "MY_LANGUAGE_MODE": "development" }
    },
    "frontend": {
      "emitNextc": false,
      "compileCompiler": false,
      "compileShared": true,
      "minStatus": "draft"
    }
  }
}
~~~

Precedence is VS Code/Cursor user/workspace/folder settings, then the active
named profile for supported fields. Changing the active profile does not
overwrite the other settings. Unknown profiles and invalid argument types,
limits or seed values stop the command with a configuration error.

## Build and test settings

| Setting | Default / behavior |
| --- | --- |
| parsley.build.outputDir | Empty inherits legacy c23.outputDir |
| parsley.build.emitNextc | false; generate the bundled machine backend/compiler target |
| parsley.build.compileCompiler | false; build the compiler executable, implies emitNextc |
| parsley.build.compileShared | true; build the parser/analyzer/backend DLL |
| parsley.build.minStatus | draft; applies to Check, Test and Build |
| parsley.build.emitIrJson | true |
| parsley.build.emitIrSexpr | false |
| parsley.build.emitAgentDocs | false |
| parsley.build.extraArgs | Additional build argv entries appended to managed options |
| parsley.test.fuzzCount | null inherits legacy fuzzCount (50); 0 disables fuzz samples |
| parsley.test.seed | Decimal string, default 42, full unsigned 64-bit range |

Parsley: Build Native Language Compiler (nextc) forces emitNextc and
compileCompiler for that invocation while keeping the profile's other options.
Build ISO C23 uses the configured choices. Both save dirty sibling grammar/PSL
documents before invoking the CLI. Compiler generation requires Parsley 2.2+.
See BACKEND.md for backend hooks, real machine targets and inference limits.

Profile fields are outputDir, emitNextc, compileCompiler, compileShared,
minStatus, emitIrJson, emitIrSexpr, emitAgentDocs, extraArgs, executablePath,
cCompiler, environment, timeoutMs, fuzzCount and fuzzSeed.

## Paths and process control

executablePath selects Parsley. cCompilerPath selects the C23 compiler through
PARSLEY_CC; the profile field is cCompiler. Bare executable names use PATH,
and relative paths resolve from the document's workspace folder.

Output paths, executable paths, cwd and environment values support:
workspaceFolder, fileDirname, fileBasenameNoExtension, userHome and env:NAME,
enclosed in the usual dollar-brace syntax. Unknown/unset variables are errors.
A directory such as generated/${fileBasenameNoExtension} avoids collisions
when a workspace contains several grammars.

cli.cwd defaults to ${workspaceFolder}. cli.timeoutMs defaults to 120000
(range 100..3600000), cli.maxBufferMiB to 8 (range 1..512). cli.arguments and
lsp.arguments are arrays of separate argv entries. environment is an object
of strings or null: null removes an inherited variable. Profile environment
entries override folder entries. env references in environment values read
the inherited process environment; path settings also see merged overrides.

Processes use execFile/spawn argv without a shell. Spaces and shell
metacharacters remain literal argument data. Extra arguments can override
managed CLI options; the language/compiler CLI still validates its own flags.

Parsley: Show Effective Configuration displays resolved paths, profile,
build arguments, timeout/buffer limits and environment variable names.
It does not print environment values. Changes apply to subsequent commands.

## Language server and editor features

Each workspace folder has its own LSP process using that folder's executable,
cwd, arguments and environment. Documents and requests route by resource URI.
Tool/profile/environment/LSP changes restart existing sessions automatically,
and changed editor switches refresh CodeLens/diagnostics.

- lsp.enabled: true; false keeps local editor fallbacks without launching LSP.
- lsp.requestTimeoutMs: 8000, range 100..600000.
- diagnostics.enabled: true.
- formatting.enabled: true; also gates autoFormatOnSave.
- codeLens.enabled: true.
- autoFormatOnSave: false, preserved from earlier versions.
- trace.server: off/messages/verbose, now read per LSP workspace.

Legacy c23.outputDir and fuzzCount remain supported. Use the new build/test
keys when you want profiles or distinct per-folder settings. The VSIX uses the
same extension API in VS Code and Cursor.

## Modular projects

Set parsley.project.entry to the workspace-relative entry grammar or PSL file to run Check, Test and Build from any component. Use Parsley: Show Module Graph to open the dependency graph as DOT. The live graph uses open documents and shows the paired semantic layer when it is open. CLI commands read saved files.
