"use strict";

Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const path = require("path");

// ---------------------------------------------------------------------------
// Constants & Shared Types
// ---------------------------------------------------------------------------

const DEFAULT_TOKEN_TYPES = [
  "namespace",
  "keyword",
  "type",
  "enumMember",
  "function",
  "variable",
  "operator",
  "regexp",
  "string",
  "number",
  "comment",
  "macro",
];

const DEFAULT_TOKEN_MODIFIERS = ["declaration", "definition", "readonly"];

// ---------------------------------------------------------------------------
// Output Channels & Configuration Helpers
// ---------------------------------------------------------------------------

let cliOutput;
let lspTraceOutput;

function getConfig() {
  return vscode.workspace.getConfiguration("parsley");
}

function resolveExecutablePath(documentUri) {
  const configured = (getConfig().get("executablePath") || "parsley").trim();
  if (configured && configured !== "parsley") {
    return configured;
  }

  const candidates = [];
  const exeName = process.platform === "win32" ? "parsley.exe" : "parsley";

  if (documentUri) {
    const folder = vscode.workspace.getWorkspaceFolder(documentUri);
    if (folder) {
      candidates.push(
        path.join(folder.uri.fsPath, "build", exeName),
        path.join(folder.uri.fsPath, "build", "Release", exeName),
        path.join(folder.uri.fsPath, "build", "Debug", exeName)
      );
    }
  }

  for (const folder of vscode.workspace.workspaceFolders || []) {
    candidates.push(
      path.join(folder.uri.fsPath, "build", exeName),
      path.join(folder.uri.fsPath, "build", "Release", exeName),
      path.join(folder.uri.fsPath, "build", "Debug", exeName)
    );
  }

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return configured || "parsley";
}

function getWorkspaceRoot(uri) {
  if (uri) {
    const folder = vscode.workspace.getWorkspaceFolder(uri);
    if (folder) {
      return folder.uri.fsPath;
    }
    if (uri.scheme === "file") {
      return path.dirname(uri.fsPath);
    }
  }
  const first = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  return first ? first.uri.fsPath : process.cwd();
}

function runCliCommand(args, cwd, uri) {
  const exe = resolveExecutablePath(uri);
  return new Promise((resolve) => {
    cp.execFile(exe, args, { cwd, maxBuffer: 8 * 1024 * 1024 }, (err, stdout, stderr) => {
      if (err && typeof err.code === "string" && err.code === "ENOENT") {
        resolve({
          code: 127,
          stdout: stdout || "",
          stderr: `Could not launch '${exe}'. Configure 'parsley.executablePath' in Settings.`,
        });
        return;
      }
      const exitCode =
        err && typeof err.code === "number" ? err.code : err ? 1 : 0;
      resolve({ code: exitCode, stdout: stdout || "", stderr: stderr || "" });
    });
  });
}

// ---------------------------------------------------------------------------
// Lightweight Local Parsley Parser (for TreeViews, Fallback Outline & Debug)
// ---------------------------------------------------------------------------

function unescapeParsleyString(s) {
  return s
    .replace(/\\n/g, "\n")
    .replace(/\\r/g, "\r")
    .replace(/\\t/g, "\t")
    .replace(/\\"/g, '"')
    .replace(/\\\\/g, "\\");
}

function parseLocalGrammarStructure(text) {
  const lines = text.split(/\r?\n/);
  const result = {
    moduleName: "(anonymous)",
    exports: [],
    pragmas: [],
    tokens: [],
    modes: [],
    dataTypes: [],
    rules: [],
    examples: [],
  };

  const ruleMap = new Map();
  let currentMode;
  let currentData;

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    const trimmed = raw.trim();
    if (!trimmed || trimmed.startsWith("--")) {
      continue;
    }

    const indent = raw.length - raw.trimStart().length;
    if (indent === 0) {
      currentMode = undefined;
      if (!trimmed.startsWith("|") && !trimmed.startsWith("=")) {
        currentData = undefined;
      }
    }

    const pragmaMatch = trimmed.match(/^\{-#\s*(.+?)\s*#-\}/);
    if (pragmaMatch) {
      result.pragmas.push({ text: pragmaMatch[1], line: i });
      continue;
    }

    const modMatch = trimmed.match(/^module\s+([A-Z][A-Za-z0-9_'.]*)(?:\s*\(([^)]*)\))?\s+where\b/);
    if (modMatch) {
      result.moduleName = modMatch[1];
      if (modMatch[2]) {
        result.exports = modMatch[2]
          .split(",")
          .map((s) => s.trim())
          .filter(Boolean);
      }
      continue;
    }

    const modeMatch = trimmed.match(/^mode\s+([a-z_][A-Za-z0-9_']*)\s+where\b/);
    if (modeMatch) {
      currentMode = modeMatch[1];
      result.modes.push({ name: currentMode, line: i });
      continue;
    }

    const tokMatch = trimmed.match(/^(token|skip)\s+([a-z_][A-Za-z0-9_']*)\s*=\s*(.+)$/);
    if (tokMatch) {
      const kind = tokMatch[1];
      const name = tokMatch[2];
      let rest = tokMatch[3].trim();
      let type;
      const typeIdx = rest.lastIndexOf("::");
      if (typeIdx !== -1) {
        type = rest.slice(typeIdx + 2).trim();
        rest = rest.slice(0, typeIdx).trim();
      }
      result.tokens.push({
        kind,
        name,
        pattern: rest,
        type,
        line: i,
        mode: indent > 0 ? currentMode : undefined,
      });
      continue;
    }

    const dataMatch = trimmed.match(/^(foreign\s+data|newtype|data|type)\s+([A-Z][A-Za-z0-9_']*)(.*)$/);
    if (dataMatch) {
      const kind = dataMatch[1];
      const name = dataMatch[2];
      const rest = dataMatch[3].trim();
      currentData = { kind, name, ctors: [], line: i };
      if (rest.startsWith("=")) {
        const rhs = rest.slice(1).trim();
        for (const part of rhs.split("|")) {
          const ctorMatch = part.trim().match(/^([A-Z][A-Za-z0-9_']*)/);
          if (ctorMatch) {
            currentData.ctors.push(ctorMatch[1]);
          }
        }
      }
      result.dataTypes.push(currentData);
      continue;
    }

    if (currentData && (trimmed.startsWith("=") || trimmed.startsWith("|"))) {
      const rhs = trimmed.slice(1).trim();
      for (const part of rhs.split("|")) {
        const ctorMatch = part.trim().match(/^([A-Z][A-Za-z0-9_']*)/);
        if (ctorMatch) {
          currentData.ctors.push(ctorMatch[1]);
        }
      }
      continue;
    }

    const exParsesMatch = trimmed.match(
      /^example\s+"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s+parses-to\s+(.+)$/
    );
    if (exParsesMatch) {
      result.examples.push({
        name: exParsesMatch[1],
        input: unescapeParsleyString(exParsesMatch[2]),
        kind: "parses-to",
        expected: exParsesMatch[3].trim(),
        line: i,
      });
      continue;
    }

    const exFailsMatch = trimmed.match(
      /^example\s+"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s+fails-at\s+(.+)$/
    );
    if (exFailsMatch) {
      result.examples.push({
        name: exFailsMatch[1],
        input: unescapeParsleyString(exFailsMatch[2]),
        kind: "fails-at",
        expected: exFailsMatch[3].trim(),
        line: i,
      });
      continue;
    }

    if (indent === 0) {
      const sigMatch = trimmed.match(/^([a-z_][A-Za-z0-9_']*)\s*::\s*(.+)$/);
      if (sigMatch) {
        const name = sigMatch[1];
        const existing = ruleMap.get(name);
        if (existing) {
          existing.signature = sigMatch[2].trim();
        } else {
          ruleMap.set(name, { name, signature: sigMatch[2].trim(), line: i });
        }
        continue;
      }

      const defMatch = trimmed.match(/^([a-z_][A-Za-z0-9_']*)(?:\s+[a-z_][A-Za-z0-9_']*)*\s*(?:=|$)/);
      if (defMatch) {
        const name = defMatch[1];
        if (!["import", "keywords", "example", "mode", "token", "skip", "data", "newtype", "type", "foreign", "module"].includes(name)) {
          const existing = ruleMap.get(name);
          if (existing) {
            existing.line = i;
          } else {
            ruleMap.set(name, { name, line: i });
          }
        }
      }
    }
  }

  result.rules = Array.from(ruleMap.values());
  return result;
}

function toVscodeRange(r) {
  if (!r || !r.start || !r.end) {
    return new vscode.Range(0, 0, 0, 0);
  }
  return new vscode.Range(
    Math.max(0, r.start.line),
    Math.max(0, r.start.character),
    Math.max(0, r.end.line),
    Math.max(0, r.end.character)
  );
}

function toLspRange(r) {
  return {
    start: { line: r.start.line, character: r.start.character },
    end: { line: r.end.line, character: r.end.character },
  };
}

function toVscodeSymbolKind(lspKind) {
  const map = {
    1: vscode.SymbolKind.File,
    2: vscode.SymbolKind.Module,
    3: vscode.SymbolKind.Namespace,
    4: vscode.SymbolKind.Package,
    5: vscode.SymbolKind.Class,
    6: vscode.SymbolKind.Method,
    7: vscode.SymbolKind.Property,
    8: vscode.SymbolKind.Field,
    9: vscode.SymbolKind.Constructor,
    10: vscode.SymbolKind.Enum,
    11: vscode.SymbolKind.Interface,
    12: vscode.SymbolKind.Function,
    13: vscode.SymbolKind.Variable,
    14: vscode.SymbolKind.Constant,
    15: vscode.SymbolKind.String,
    16: vscode.SymbolKind.Number,
    22: vscode.SymbolKind.EnumMember,
    23: vscode.SymbolKind.Struct,
  };
  return map[lspKind] !== undefined ? map[lspKind] : vscode.SymbolKind.Function;
}

// ---------------------------------------------------------------------------
// Zero-Dependency Stdio JSON-RPC 2.0 LSP Client
// ---------------------------------------------------------------------------

class ParsleyLspClient {
  constructor(diagnostics, statusBar, onDiagnosticsUpdated) {
    this.proc = undefined;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.initialized = false;
    this.initPromise = undefined;
    this.capabilities = {};
    this.openedDocs = new Map();
    this.statusBar = statusBar;
    this.diagnostics = diagnostics;
    this.disposed = false;
    this.onDiagnosticsUpdated = onDiagnosticsUpdated;
  }

  getSemanticLegend() {
    const sem = this.capabilities.semanticTokensProvider;
    const tokenTypes =
      sem && sem.legend && Array.isArray(sem.legend.tokenTypes) && sem.legend.tokenTypes.length
        ? sem.legend.tokenTypes
        : DEFAULT_TOKEN_TYPES;
    const tokenModifiers =
      sem && sem.legend && Array.isArray(sem.legend.tokenModifiers) && sem.legend.tokenModifiers.length
        ? sem.legend.tokenModifiers
        : DEFAULT_TOKEN_MODIFIERS;
    return new vscode.SemanticTokensLegend(tokenTypes, tokenModifiers);
  }

  async ensureStarted() {
    if (this.disposed) {
      return false;
    }
    if (this.proc && this.initialized) {
      return true;
    }
    if (this.initPromise) {
      try {
        await this.initPromise;
        return this.initialized;
      } catch {
        return false;
      }
    }

    this.initPromise = this.startInternal();
    try {
      await this.initPromise;
      return this.initialized;
    } catch (err) {
      this.logTrace(`LSP start failed: ${err.message}`);
      return false;
    } finally {
      this.initPromise = undefined;
    }
  }

  async startInternal() {
    const exe = resolveExecutablePath();
    this.statusBar.text = "$(sync~spin) Parsley LSP";
    this.statusBar.tooltip = `Starting ${exe} lsp...`;
    this.statusBar.show();

    this.buffer = Buffer.alloc(0);
    this.initialized = false;

    const proc = cp.spawn(exe, ["lsp"], {
      cwd: getWorkspaceRoot(),
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.proc = proc;

    proc.stdout.on("data", (chunk) => this.handleStdout(chunk));
    proc.stderr.on("data", (chunk) => {
      const msg = chunk.toString("utf8").trim();
      if (msg) {
        this.logTrace(`[stderr] ${msg}`);
      }
    });

    proc.on("error", (err) => {
      this.logTrace(`[error] ${err.message}`);
      this.handleExit(-1);
    });

    proc.on("exit", (code) => {
      this.logTrace(`[exit] code=${code}`);
      this.handleExit(code === null ? -1 : code);
    });

    const firstFolder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    const rootUri = firstFolder ? firstFolder.uri.toString() : null;
    const initResult = await this.sendRequestRaw("initialize", {
      processId: process.pid,
      clientInfo: { name: "parsley-vscode", version: "1.3.0" },
      rootUri,
      capabilities: {
        textDocument: {
          synchronization: { dynamicRegistration: false, didSave: true },
          hover: { contentFormat: ["markdown", "plaintext"] },
          definition: { linkSupport: false },
          references: {},
          documentSymbol: { hierarchicalDocumentSymbolSupport: true },
          formatting: {},
          completion: { completionItem: { snippetSupport: true } },
          codeAction: {
            codeActionLiteralSupport: {
              codeActionKind: { valueSet: ["quickfix", "refactor", "source"] },
            },
          },
          codeLens: {},
          semanticTokens: {
            requests: { full: true },
            tokenTypes: DEFAULT_TOKEN_TYPES,
            tokenModifiers: DEFAULT_TOKEN_MODIFIERS,
            formats: ["relative"],
          },
          publishDiagnostics: {},
        },
      },
    });

    this.capabilities = (initResult && initResult.capabilities) || {};
    this.initialized = true;
    this.sendNotificationRaw("initialized", {});

    this.statusBar.text = "$(check) Parsley LSP";
    this.statusBar.tooltip = `Parsley Language Server running (${exe})`;
    this.statusBar.show();

    for (const doc of vscode.workspace.textDocuments) {
      if (doc.languageId === "parsley") {
        this.didOpen(doc);
      }
    }
  }

  handleExit(code) {
    this.proc = undefined;
    this.initialized = false;
    this.openedDocs.clear();
    for (const [, req] of this.pending) {
      clearTimeout(req.timer);
      req.reject(new Error(`Parsley LSP exited (code ${code})`));
    }
    this.pending.clear();
    if (!this.disposed) {
      this.statusBar.text = "$(warning) Parsley LSP";
      this.statusBar.tooltip = "Parsley LSP stopped. Click to restart.";
      this.statusBar.command = "parsley.restartLsp";
      this.statusBar.show();
    }
  }

  async restart() {
    await this.stop();
    await this.ensureStarted();
  }

  async stop() {
    if (!this.proc) {
      return;
    }
    const p = this.proc;
    this.proc = undefined;
    this.initialized = false;
    try {
      if (p.stdin.writable) {
        const id = this.nextId++;
        const payload = JSON.stringify({ jsonrpc: "2.0", id, method: "shutdown", params: null });
        this.writeFrameTo(p, payload);
        const exitMsg = JSON.stringify({ jsonrpc: "2.0", method: "exit", params: null });
        this.writeFrameTo(p, exitMsg);
      }
    } catch {
      // ignore shutdown write errors
    }
    setTimeout(() => {
      try {
        p.kill();
      } catch {
        // ignore
      }
    }, 300);
  }

  dispose() {
    this.disposed = true;
    void this.stop();
  }

  logTrace(line) {
    const mode = getConfig().get("trace.server") || "off";
    if (mode !== "off") {
      lspTraceOutput.appendLine(line);
    }
  }

  writeFrameTo(proc, body) {
    const bytes = Buffer.byteLength(body, "utf8");
    const header = `Content-Length: ${bytes}\r\n\r\n`;
    proc.stdin.write(header + body, "utf8");
  }

  sendRequestRaw(method, params, timeoutMs = 8000) {
    if (!this.proc || !this.proc.stdin.writable) {
      return Promise.reject(new Error("Parsley LSP is not running"));
    }
    const id = this.nextId++;
    const payload = JSON.stringify({ jsonrpc: "2.0", id, method, params });
    this.logTrace(`--> [${id}] ${method}`);
    if (getConfig().get("trace.server") === "verbose") {
      this.logTrace(payload);
    }

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`LSP request '${method}' timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, method, timer });
      try {
        this.writeFrameTo(this.proc, payload);
      } catch (err) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(err);
      }
    });
  }

  sendNotificationRaw(method, params) {
    if (!this.proc || !this.proc.stdin.writable) {
      return;
    }
    const payload = JSON.stringify({ jsonrpc: "2.0", method, params });
    this.logTrace(`--> [notif] ${method}`);
    try {
      this.writeFrameTo(this.proc, payload);
    } catch {
      // ignore
    }
  }

  async sendRequest(method, params, timeoutMs = 8000) {
    const ready = await this.ensureStarted();
    if (!ready) {
      return null;
    }
    try {
      return await this.sendRequestRaw(method, params, timeoutMs);
    } catch (err) {
      this.logTrace(`[req error] ${method}: ${err.message}`);
      return null;
    }
  }

  didOpen(doc) {
    if (doc.languageId !== "parsley") {
      return;
    }
    const uri = doc.uri.toString();
    this.openedDocs.set(uri, doc.version);
    void this.ensureStarted().then((ok) => {
      if (ok) {
        this.sendNotificationRaw("textDocument/didOpen", {
          textDocument: {
            uri,
            languageId: "parsley",
            version: doc.version,
            text: doc.getText(),
          },
        });
      }
    });
  }

  didChange(doc) {
    if (doc.languageId !== "parsley") {
      return;
    }
    const uri = doc.uri.toString();
    if (!this.openedDocs.has(uri)) {
      this.didOpen(doc);
      return;
    }
    this.openedDocs.set(uri, doc.version);
    if (this.initialized) {
      this.sendNotificationRaw("textDocument/didChange", {
        textDocument: { uri, version: doc.version },
        contentChanges: [{ text: doc.getText() }],
      });
    }
  }

  didClose(doc) {
    if (doc.languageId !== "parsley") {
      return;
    }
    const uri = doc.uri.toString();
    this.openedDocs.delete(uri);
    this.diagnostics.delete(doc.uri);
    if (this.initialized) {
      this.sendNotificationRaw("textDocument/didClose", {
        textDocument: { uri },
      });
    }
  }

  handleStdout(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (true) {
      let headerEnd = this.buffer.indexOf("\r\n\r\n");
      let sepLen = 4;
      if (headerEnd === -1) {
        headerEnd = this.buffer.indexOf("\n\n");
        sepLen = 2;
      }
      if (headerEnd === -1) {
        return;
      }

      const headerStr = this.buffer.subarray(0, headerEnd).toString("ascii");
      const match = headerStr.match(/Content-Length:\s*(\d+)/i);
      if (!match) {
        this.buffer = this.buffer.subarray(headerEnd + sepLen);
        continue;
      }

      const contentLength = parseInt(match[1], 10);
      const totalLen = headerEnd + sepLen + contentLength;
      if (this.buffer.length < totalLen) {
        return;
      }

      const jsonBytes = this.buffer.subarray(headerEnd + sepLen, totalLen).toString("utf8");
      this.buffer = this.buffer.subarray(totalLen);

      try {
        const msg = JSON.parse(jsonBytes);
        this.dispatchMessage(msg);
      } catch (err) {
        this.logTrace(`[parse error] ${err.message}`);
      }
    }
  }

  dispatchMessage(msg) {
    if (typeof msg.id === "number" && this.pending.has(msg.id)) {
      const req = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      clearTimeout(req.timer);
      this.logTrace(`<-- [${msg.id}] ${req.method}`);
      if (msg.error) {
        req.reject(new Error(msg.error.message || "LSP error"));
      } else {
        req.resolve(msg.result);
      }
      return;
    }

    if (msg.method === "textDocument/publishDiagnostics" && msg.params) {
      const params = msg.params;
      const uri = vscode.Uri.parse(params.uri);
      const vsDiags = (params.diagnostics || []).map((d) => {
        const sev =
          d.severity === 2
            ? vscode.DiagnosticSeverity.Warning
            : d.severity === 3
              ? vscode.DiagnosticSeverity.Information
              : d.severity === 4
                ? vscode.DiagnosticSeverity.Hint
                : vscode.DiagnosticSeverity.Error;
        const diag = new vscode.Diagnostic(toVscodeRange(d.range), d.message, sev);
        if (d.code !== undefined) {
          diag.code = String(d.code);
        }
        diag.source = d.source || "parsley";
        return diag;
      });
      this.diagnostics.set(uri, vsDiags);
      this.onDiagnosticsUpdated(uri);
    }
  }
}

// ---------------------------------------------------------------------------
// TreeViews: GrammarExplorerProvider & ExamplesViewProvider
// ---------------------------------------------------------------------------

class GrammarTreeItem extends vscode.TreeItem {
  constructor(label, collapsibleState, options) {
    super(label, collapsibleState);
    if (options) {
      this.description = options.description;
      this.tooltip = options.tooltip;
      this.contextValue = options.contextValue;
      if (options.icon) {
        this.iconPath = new vscode.ThemeIcon(options.icon);
      }
      if (options.uri && typeof options.line === "number") {
        this.command = {
          command: "parsley.revealLine",
          title: "Reveal in Grammar",
          arguments: [options.uri, options.line],
        };
      }
      this.children = options.children;
    }
  }
}

class GrammarExplorerProvider {
  constructor(client) {
    this._onDidChangeTreeData = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    this.client = client;
  }

  refresh() {
    this._onDidChangeTreeData.fire(undefined);
  }

  getTreeItem(element) {
    return element;
  }

  async getChildren(element) {
    if (element) {
      return element.children || [];
    }

    const doc = getActiveParsleyDocument();
    if (!doc) {
      return [
        new GrammarTreeItem(
          "Open a .parsley file to inspect grammar",
          vscode.TreeItemCollapsibleState.None,
          { icon: "info" }
        ),
      ];
    }

    const parsed = parseLocalGrammarStructure(doc.getText());
    const rootItems = [];

    const modDesc = parsed.exports.length ? `(${parsed.exports.join(", ")})` : "";
    const pragmaChildren = parsed.pragmas.map(
      (p) =>
        new GrammarTreeItem(`{-# ${p.text} #-}`, vscode.TreeItemCollapsibleState.None, {
          icon: "symbol-property",
          line: p.line,
          uri: doc.uri,
        })
    );
    rootItems.push(
      new GrammarTreeItem(
        `Module: ${parsed.moduleName}`,
        pragmaChildren.length
          ? vscode.TreeItemCollapsibleState.Expanded
          : vscode.TreeItemCollapsibleState.None,
        {
          description: modDesc,
          icon: "symbol-namespace",
          children: pragmaChildren,
        }
      )
    );

    const hoverResults = await Promise.all(
      parsed.rules.map((r) => fetchRuleHoverDetails(this.client, doc, r.line, r.name))
    );
    const ruleChildren = [];
    for (let idx = 0; idx < parsed.rules.length; idx++) {
      const r = parsed.rules[idx];
      const hoverInfo = hoverResults[idx] || {};
      let typeDesc = r.signature ? `:: ${r.signature}` : "";
      const ruleMeta = [];
      if (!typeDesc && hoverInfo.typeSignature) {
        typeDesc = `:: ${hoverInfo.typeSignature}`;
      }
      if (hoverInfo.strategy) {
        ruleMeta.push(
          new GrammarTreeItem(`Strategy: ${hoverInfo.strategy}`, vscode.TreeItemCollapsibleState.None, {
            icon: "compass",
          })
        );
      }
      if (hoverInfo.nullable !== undefined) {
        ruleMeta.push(
          new GrammarTreeItem(`Nullable: ${hoverInfo.nullable}`, vscode.TreeItemCollapsibleState.None, {
            icon: "symbol-boolean",
          })
        );
      }
      if (hoverInfo.first) {
        ruleMeta.push(
          new GrammarTreeItem(`FIRST: ${hoverInfo.first}`, vscode.TreeItemCollapsibleState.None, {
            icon: "arrow-right",
          })
        );
      }
      if (hoverInfo.follow) {
        ruleMeta.push(
          new GrammarTreeItem(`FOLLOW: ${hoverInfo.follow}`, vscode.TreeItemCollapsibleState.None, {
            icon: "arrow-left",
          })
        );
      }

      ruleChildren.push(
        new GrammarTreeItem(
          r.name,
          ruleMeta.length
            ? vscode.TreeItemCollapsibleState.Collapsed
            : vscode.TreeItemCollapsibleState.None,
          {
            description: typeDesc,
            tooltip: `${r.name} ${typeDesc}`,
            icon: "symbol-function",
            line: r.line,
            uri: doc.uri,
            children: ruleMeta,
          }
        )
      );
    }

    rootItems.push(
      new GrammarTreeItem(`Rules (${ruleChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
        icon: "symbol-method",
        children: ruleChildren,
      })
    );

    const tokChildren = parsed.tokens.map(
      (t) =>
        new GrammarTreeItem(t.name, vscode.TreeItemCollapsibleState.None, {
          description: `${t.kind}${t.mode ? ` [${t.mode}]` : ""}${t.type ? ` :: ${t.type}` : ""}`,
          tooltip: `${t.kind} ${t.name} = ${t.pattern}`,
          icon: t.kind === "skip" ? "eye-closed" : "symbol-key",
          line: t.line,
          uri: doc.uri,
        })
    );
    rootItems.push(
      new GrammarTreeItem(`Tokens & Skips (${tokChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
        icon: "symbol-variable",
        children: tokChildren,
      })
    );

    if (parsed.modes.length > 0) {
      const modeChildren = parsed.modes.map(
        (m) =>
          new GrammarTreeItem(m.name, vscode.TreeItemCollapsibleState.None, {
            icon: "layers",
            line: m.line,
            uri: doc.uri,
          })
      );
      rootItems.push(
        new GrammarTreeItem(`Lexer Modes (${modeChildren.length})`, vscode.TreeItemCollapsibleState.Collapsed, {
          icon: "layers",
          children: modeChildren,
        })
      );
    }

    const dataChildren = parsed.dataTypes.map((d) => {
      const ctorItems = d.ctors.map(
        (c) =>
          new GrammarTreeItem(c, vscode.TreeItemCollapsibleState.None, {
            icon: "symbol-enum-member",
            line: d.line,
            uri: doc.uri,
          })
      );
      return new GrammarTreeItem(
        d.name,
        ctorItems.length
          ? vscode.TreeItemCollapsibleState.Collapsed
          : vscode.TreeItemCollapsibleState.None,
        {
          description: d.kind,
          icon: "symbol-enum",
          line: d.line,
          uri: doc.uri,
          children: ctorItems,
        }
      );
    });
    rootItems.push(
      new GrammarTreeItem(`Data Types (${dataChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
        icon: "symbol-struct",
        children: dataChildren,
      })
    );

    return rootItems;
  }
}

class ExampleTreeItem extends vscode.TreeItem {
  constructor(example, uri) {
    super(example.name, vscode.TreeItemCollapsibleState.None);
    this.example = example;
    this.uri = uri;
    this.description = `${example.kind}: ${JSON.stringify(example.input)}`;
    this.tooltip = `example "${example.name}" : ${JSON.stringify(example.input)} ${example.kind} ${example.expected}`;
    this.contextValue = "parsleyExample";
    this.iconPath = new vscode.ThemeIcon(example.kind === "parses-to" ? "beaker" : "error");
    this.command = {
      command: "parsley.revealLine",
      title: "Jump to Example",
      arguments: [uri, example.line],
    };
  }
}

class ExamplesViewProvider {
  constructor() {
    this._onDidChangeTreeData = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._onDidChangeTreeData.event;
  }

  refresh() {
    this._onDidChangeTreeData.fire(undefined);
  }

  getTreeItem(element) {
    return element;
  }

  getChildren() {
    const doc = getActiveParsleyDocument();
    if (!doc) {
      return [];
    }
    const parsed = parseLocalGrammarStructure(doc.getText());
    return parsed.examples.map((ex) => new ExampleTreeItem(ex, doc.uri));
  }
}

async function fetchRuleHoverDetails(client, doc, line, ruleName) {
  const lineText = doc.lineAt(line).text;
  const charIdx = Math.max(0, lineText.indexOf(ruleName));
  const hover = await client.sendRequest(
    "textDocument/hover",
    { textDocument: { uri: doc.uri.toString() }, position: { line, character: charIdx } },
    1500
  );
  if (!hover || !hover.contents) {
    return {};
  }
  const md = typeof hover.contents === "string" ? hover.contents : hover.contents.value || "";
  const typeMatch = md.match(/::\s*([^\n`]+)/);
  const stratMatch = md.match(/\*\*Strategy\*\*:\s*`([^`]+)`/);
  const nullMatch = md.match(/\*\*Nullable\*\*:\s*`([^`]+)`/);
  const firstMatch = md.match(/\*\*FIRST\*\*:\s*`?(\{[^}]*\})`?/);
  const followMatch = md.match(/\*\*FOLLOW\*\*:\s*`?(\{[^}]*\})`?/);

  return {
    typeSignature: typeMatch && typeMatch[1] ? typeMatch[1].trim() : undefined,
    strategy: stratMatch && stratMatch[1] ? stratMatch[1].trim() : undefined,
    nullable: nullMatch && nullMatch[1] ? nullMatch[1].trim() : undefined,
    first: firstMatch && firstMatch[1] ? firstMatch[1].replace(/`/g, "").trim() : undefined,
    follow: followMatch && followMatch[1] ? followMatch[1].replace(/`/g, "").trim() : undefined,
  };
}

function coerceUriArg(uriArg) {
  if (uriArg instanceof vscode.Uri) {
    return uriArg;
  }
  if (typeof uriArg === "string" && uriArg.length > 0) {
    return uriArg.includes("://") ? vscode.Uri.parse(uriArg) : vscode.Uri.file(uriArg);
  }
  return undefined;
}

function getActiveParsleyDocument() {
  const active = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
  if (active && active.languageId === "parsley") {
    return active;
  }
  return vscode.workspace.textDocuments.find((d) => d.languageId === "parsley");
}

// ---------------------------------------------------------------------------
// Live Grammar Playground Webview
// ---------------------------------------------------------------------------

class PlaygroundPanelManager {
  constructor(client) {
    this.panel = undefined;
    this.targetUri = undefined;
    this.currentInput = "1 + 2 * 3";
    this.client = client;
    this.debounceTimer = undefined;
  }

  async open(uri, initialInput) {
    const doc = uri ? await vscode.workspace.openTextDocument(uri) : getActiveParsleyDocument();
    if (!doc) {
      vscode.window.showWarningMessage("Open a .parsley file to use the Parsley Live Playground.");
      return;
    }

    this.targetUri = doc.uri;
    const parsed = parseLocalGrammarStructure(doc.getText());
    if (initialInput !== undefined) {
      this.currentInput = initialInput;
    } else if (parsed.examples.length > 0 && this.currentInput === "1 + 2 * 3") {
      this.currentInput = parsed.examples[0].input;
    }

    if (this.panel) {
      this.panel.reveal(vscode.ViewColumn.Beside);
    } else {
      this.panel = vscode.window.createWebviewPanel(
        "parsleyPlayground",
        `Parsley Playground: ${path.basename(doc.fileName)}`,
        vscode.ViewColumn.Beside,
        { enableScripts: true, retainContextWhenHidden: true }
      );
      this.panel.onDidDispose(() => {
        this.panel = undefined;
      });
      this.panel.webview.onDidReceiveMessage(async (msg) => {
        if (msg.type === "eval") {
          this.currentInput = String(msg.input === undefined ? "" : msg.input);
          await this.evaluateAndPost();
        } else if (msg.type === "debug") {
          if (this.targetUri) {
            await vscode.commands.executeCommand("parsley.debugExample", {
              uri: this.targetUri,
              input: String(msg.input === undefined ? this.currentInput : msg.input),
            });
          }
        } else if (msg.type === "railroad") {
          await vscode.commands.executeCommand("parsley.showRailroad", this.targetUri);
        }
      });
    }

    this.panel.title = `Playground: ${path.basename(doc.fileName)}`;
    this.panel.webview.html = this.buildHtml(parsed, doc.fileName);
    await this.evaluateAndPost();
  }

  notifyDocumentChanged(doc) {
    if (!this.panel || !this.targetUri || doc.uri.toString() !== this.targetUri.toString()) {
      return;
    }
    if (this.debounceTimer) {
      clearTimeout(this.debounceTimer);
    }
    this.debounceTimer = setTimeout(() => {
      const parsed = parseLocalGrammarStructure(doc.getText());
      if (this.panel) {
        this.panel.webview.postMessage({
          type: "examples",
          examples: parsed.examples,
          moduleName: parsed.moduleName,
        });
      }
      void this.evaluateAndPost();
    }, 180);
  }

  async evaluateAndPost() {
    if (!this.panel || !this.targetUri) {
      return;
    }
    const res = await this.client.sendRequest("parsley/evalInput", {
      uri: this.targetUri.toString(),
      input: this.currentInput,
    });
    if (!res) {
      this.panel.webview.postMessage({
        type: "result",
        result: {
          ok: false,
          error: {
            line: 1,
            col: 1,
            message:
              "Language server did not return a response for `parsley/evalInput`. Verify `parsley.executablePath` points to a Parsley build with LSP playground support.",
          },
          tokens: [],
        },
      });
      return;
    }
    this.panel.webview.postMessage({ type: "result", result: res });
  }

  buildHtml(parsed, fileName) {
    const nonce = String(Date.now());
    const escapedInput = escapeHtml(this.currentInput);
    const examplesJson = JSON.stringify(parsed.examples);
    return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Parsley Playground</title>
  <style>
    body {
      font-family: var(--vscode-font-family);
      color: var(--vscode-editor-foreground);
      background: var(--vscode-editor-background);
      margin: 0;
      padding: 16px;
      line-height: 1.45;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 12px;
      border-bottom: 1px solid var(--vscode-panel-border);
      padding-bottom: 10px;
    }
    .title { font-size: 15px; font-weight: 600; }
    .subtitle { font-size: 12px; color: var(--vscode-descriptionForeground); }
    .actions { display: flex; gap: 8px; align-items: center; }
    button {
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
      border: none;
      padding: 5px 10px;
      border-radius: 3px;
      cursor: pointer;
      font-size: 12px;
    }
    button:hover { background: var(--vscode-button-hoverBackground); }
    button.secondary {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
    }
    button.secondary:hover { background: var(--vscode-button-secondaryHoverBackground); }
    .badge { padding: 3px 8px; border-radius: 10px; font-size: 11px; font-weight: 600; }
    .badge.ok { background: rgba(34, 197, 94, 0.2); color: #4ade80; }
    .badge.err { background: rgba(239, 68, 68, 0.2); color: #f87171; }
    .examples-bar { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 10px; align-items: center; }
    .chip {
      background: var(--vscode-badge-background);
      color: var(--vscode-badge-foreground);
      padding: 2px 8px;
      border-radius: 4px;
      font-size: 11px;
      cursor: pointer;
    }
    textarea {
      width: 100%;
      min-height: 92px;
      box-sizing: border-box;
      font-family: var(--vscode-editor-font-family);
      font-size: var(--vscode-editor-font-size);
      color: var(--vscode-input-foreground);
      background: var(--vscode-input-background);
      border: 1px solid var(--vscode-input-border, var(--vscode-panel-border));
      border-radius: 4px;
      padding: 8px;
      resize: vertical;
    }
    .section {
      margin-top: 14px;
      border: 1px solid var(--vscode-panel-border);
      border-radius: 4px;
      overflow: hidden;
    }
    .section-title {
      background: var(--vscode-sideBarSectionHeader-background, rgba(128,128,128,0.1));
      padding: 6px 10px;
      font-size: 12px;
      font-weight: 600;
      border-bottom: 1px solid var(--vscode-panel-border);
    }
    .section-body { padding: 10px; overflow-x: auto; }
    pre {
      margin: 0;
      font-family: var(--vscode-editor-font-family);
      font-size: 12.5px;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .error-box { border-left: 3px solid #f87171; padding-left: 10px; }
    table { width: 100%; border-collapse: collapse; font-size: 12px; }
    th, td {
      text-align: left;
      padding: 4px 8px;
      border-bottom: 1px solid var(--vscode-panel-border);
      font-family: var(--vscode-editor-font-family);
    }
    th { color: var(--vscode-descriptionForeground); font-weight: 600; }
  </style>
</head>
<body>
  <div class="header">
    <div>
      <div class="title" id="modTitle">Parsley Playground — ${escapeHtml(parsed.moduleName)}</div>
      <div class="subtitle">${escapeHtml(path.basename(fileName))}</div>
    </div>
    <div class="actions">
      <span id="statusBadge" class="badge">Evaluating...</span>
      <button id="debugBtn" class="secondary">Debug Input</button>
      <button id="railroadBtn" class="secondary">Railroad SVG</button>
    </div>
  </div>
  <div class="examples-bar" id="examplesBar"></div>
  <textarea id="inputBox" placeholder="Type sample input to parse live...">${escapedInput}</textarea>
  <div class="section">
    <div class="section-title">Parsed AST / Diagnostics</div>
    <div class="section-body" id="astOutput"><pre>Waiting for evaluation...</pre></div>
  </div>
  <div class="section" id="jsonSection" style="display:none;">
    <div class="section-title">AST JSON Structure</div>
    <div class="section-body"><pre id="jsonOutput"></pre></div>
  </div>
  <div class="section">
    <div class="section-title">Token Stream</div>
    <div class="section-body" id="tokensOutput">No tokens yet.</div>
  </div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const inputBox = document.getElementById('inputBox');
    const statusBadge = document.getElementById('statusBadge');
    const astOutput = document.getElementById('astOutput');
    const jsonSection = document.getElementById('jsonSection');
    const jsonOutput = document.getElementById('jsonOutput');
    const tokensOutput = document.getElementById('tokensOutput');
    const examplesBar = document.getElementById('examplesBar');
    const modTitle = document.getElementById('modTitle');

    let timer = null;
    function renderExamples(examples) {
      examplesBar.innerHTML = '';
      if (!examples || !examples.length) return;
      const lbl = document.createElement('span');
      lbl.className = 'subtitle';
      lbl.textContent = 'Examples:';
      examplesBar.appendChild(lbl);
      examples.forEach(ex => {
        const btn = document.createElement('button');
        btn.className = 'chip';
        btn.textContent = ex.name;
        btn.title = ex.input;
        btn.onclick = () => {
          inputBox.value = ex.input;
          vscode.postMessage({ type: 'eval', input: ex.input });
        };
        examplesBar.appendChild(btn);
      });
    }
    renderExamples(${examplesJson});

    inputBox.addEventListener('input', () => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        vscode.postMessage({ type: 'eval', input: inputBox.value });
      }, 150);
    });
    document.getElementById('debugBtn').addEventListener('click', () => {
      vscode.postMessage({ type: 'debug', input: inputBox.value });
    });
    document.getElementById('railroadBtn').addEventListener('click', () => {
      vscode.postMessage({ type: 'railroad' });
    });
    function esc(s) {
      return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
    }
    window.addEventListener('message', event => {
      const msg = event.data;
      if (msg.type === 'examples') {
        modTitle.textContent = 'Parsley Playground — ' + (msg.moduleName || '');
        renderExamples(msg.examples || []);
        return;
      }
      if (msg.type !== 'result') return;
      const r = msg.result || {};
      if (r.ok) {
        statusBadge.className = 'badge ok';
        statusBadge.textContent = 'Parsed OK';
        astOutput.innerHTML = '<pre>' + esc(r.formattedAst || '(empty)') + '</pre>';
        if (r.astJson !== undefined && r.astJson !== null) {
          jsonSection.style.display = 'block';
          jsonOutput.textContent = typeof r.astJson === 'string' ? r.astJson : JSON.stringify(r.astJson, null, 2);
        } else {
          jsonSection.style.display = 'none';
        }
      } else {
        statusBadge.className = 'badge err';
        statusBadge.textContent = 'Parse Error';
        jsonSection.style.display = 'none';
        const e = r.error || {};
        const lines = inputBox.value.split(/\\r?\\n/);
        const errLine = Math.max(1, e.line || 1);
        const errCol = Math.max(1, e.col || 1);
        const lineText = lines[errLine - 1] || '';
        const caret = ' '.repeat(Math.max(0, errCol - 1)) + '^';
        astOutput.innerHTML =
          '<div class="error-box">' +
          '<div><strong>Line ' + errLine + ', Column ' + errCol + '</strong></div>' +
          (e.expected ? '<div>Expected: <code>' + esc(e.expected) + '</code></div>' : '') +
          (e.found ? '<div>Found: <code>' + esc(e.found) + '</code></div>' : '') +
          '<div style="margin-top:4px">' + esc(e.message || 'Parse error') + '</div>' +
          '<pre style="margin-top:8px;opacity:0.9">' + esc(lineText) + '\\n' + esc(caret) + '</pre>' +
          '</div>';
      }
      const toks = r.tokens || [];
      if (!toks.length) {
        tokensOutput.textContent = 'No tokens emitted.';
      } else {
        let html = '<table><thead><tr><th>#</th><th>Token ID</th><th>Lexeme</th><th>Pos</th></tr></thead><tbody>';
        toks.forEach((t, idx) => {
          html += '<tr><td>' + (idx + 1) + '</td><td>' + esc(t.id) + '</td><td>' + esc(JSON.stringify(t.lexeme)) + '</td><td>' + t.line + ':' + t.col + '</td></tr>';
        });
        html += '</tbody></table>';
        tokensOutput.innerHTML = html;
      }
    });
  </script>
</body>
</html>`;
  }
}

// ---------------------------------------------------------------------------
// Railroad Diagram Webview
// ---------------------------------------------------------------------------

class RailroadPanelManager {
  constructor(client) {
    this.panel = undefined;
    this.targetUri = undefined;
    this.client = client;
    this.lastSvg = "";
  }

  async show(uri) {
    const doc = uri ? await vscode.workspace.openTextDocument(uri) : getActiveParsleyDocument();
    if (!doc) {
      vscode.window.showWarningMessage("Open a .parsley file to display its Railroad diagram.");
      return;
    }
    this.targetUri = doc.uri;

    if (this.panel) {
      this.panel.reveal(vscode.ViewColumn.Beside);
    } else {
      this.panel = vscode.window.createWebviewPanel(
        "parsleyRailroad",
        `Railroad: ${path.basename(doc.fileName)}`,
        vscode.ViewColumn.Beside,
        { enableScripts: true, retainContextWhenHidden: true }
      );
      this.panel.onDidDispose(() => {
        this.panel = undefined;
      });
      this.panel.webview.onDidReceiveMessage(async (msg) => {
        if (msg.type === "exportSvg" && this.lastSvg) {
          const baseDir = path.dirname((this.targetUri && this.targetUri.fsPath) || ".");
          const baseName = path.basename((this.targetUri && this.targetUri.fsPath) || "grammar", ".parsley");
          const saveUri = await vscode.window.showSaveDialog({
            defaultUri: vscode.Uri.file(path.join(baseDir, `${baseName}.railroad.svg`)),
            filters: { "SVG Images": ["svg"] },
          });
          if (saveUri) {
            fs.writeFileSync(saveUri.fsPath, this.lastSvg, "utf8");
            vscode.window.showInformationMessage(`Saved Railroad SVG to ${saveUri.fsPath}`);
          }
        } else if (msg.type === "refresh" && this.targetUri) {
          await this.renderForUri(this.targetUri);
        }
      });
    }

    this.panel.title = `Railroad: ${path.basename(doc.fileName)}`;
    await this.renderForUri(doc.uri);
  }

  async refreshIfOpen(uri) {
    if (this.panel && this.targetUri && uri.toString() === this.targetUri.toString()) {
      await this.renderForUri(uri);
    }
  }

  async renderForUri(uri) {
    if (!this.panel) {
      return;
    }
    let svg = "";
    const lspRes = await this.client.sendRequest("parsley/railroadSvg", {
      uri: uri.toString(),
    });
    if (lspRes && lspRes.svg) {
      svg = lspRes.svg;
    } else if (uri.scheme === "file") {
      const cli = await runCliCommand(["graph", uri.fsPath, "--format", "svg"], getWorkspaceRoot(uri), uri);
      if (cli.code === 0 && cli.stdout.includes("<svg")) {
        svg = cli.stdout;
      } else {
        svg = `<svg xmlns="http://www.w3.org/2000/svg" width="600" height="120"><rect width="100%" height="100%" fill="#1e293b"/><text x="20" y="55" fill="#f87171" font-family="sans-serif" font-size="14">${escapeHtml(cli.stderr || "Unable to generate Railroad SVG. Fix grammar syntax/type errors first.")}</text></svg>`;
      }
    }

    this.lastSvg = svg;
    const nonce = String(Date.now());
    this.panel.webview.html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';" />
  <style>
    html, body {
      margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden;
      background: var(--vscode-editor-background); color: var(--vscode-editor-foreground);
      font-family: var(--vscode-font-family); display: flex; flex-direction: column;
    }
    .toolbar {
      display: flex; gap: 8px; padding: 8px 12px;
      border-bottom: 1px solid var(--vscode-panel-border); align-items: center; z-index: 2;
    }
    button {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
      border: none; padding: 4px 10px; border-radius: 3px; cursor: pointer; font-size: 12px;
    }
    button:hover { background: var(--vscode-button-secondaryHoverBackground); }
    .zoom-lbl { font-size: 12px; color: var(--vscode-descriptionForeground); margin-left: auto; }
    #viewport { flex: 1; overflow: hidden; cursor: grab; position: relative; background: #f8fafc; }
    #viewport:active { cursor: grabbing; }
    #stage { transform-origin: 0 0; position: absolute; top: 0; left: 0; padding: 16px; }
  </style>
</head>
<body>
  <div class="toolbar">
    <button id="zoomIn">+ Zoom In</button>
    <button id="zoomOut">- Zoom Out</button>
    <button id="reset">Reset</button>
    <button id="refresh">Refresh</button>
    <button id="exportBtn">Export SVG</button>
    <span class="zoom-lbl" id="zoomLbl">100%</span>
  </div>
  <div id="viewport"><div id="stage">${svg}</div></div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const viewport = document.getElementById('viewport');
    const stage = document.getElementById('stage');
    const zoomLbl = document.getElementById('zoomLbl');
    let scale = 1, tx = 16, ty = 16, dragging = false, sx = 0, sy = 0;
    function apply() {
      stage.style.transform = 'translate(' + tx + 'px, ' + ty + 'px) scale(' + scale + ')';
      zoomLbl.textContent = Math.round(scale * 100) + '%';
    }
    apply();
    document.getElementById('zoomIn').onclick = () => { scale = Math.min(4, scale * 1.2); apply(); };
    document.getElementById('zoomOut').onclick = () => { scale = Math.max(0.2, scale / 1.2); apply(); };
    document.getElementById('reset').onclick = () => { scale = 1; tx = 16; ty = 16; apply(); };
    document.getElementById('refresh').onclick = () => vscode.postMessage({ type: 'refresh' });
    document.getElementById('exportBtn').onclick = () => vscode.postMessage({ type: 'exportSvg' });
    viewport.addEventListener('wheel', (e) => {
      e.preventDefault();
      scale = Math.max(0.2, Math.min(4, scale * (e.deltaY < 0 ? 1.1 : 0.9)));
      apply();
    }, { passive: false });
    viewport.addEventListener('mousedown', (e) => { dragging = true; sx = e.clientX - tx; sy = e.clientY - ty; });
    window.addEventListener('mousemove', (e) => { if (!dragging) return; tx = e.clientX - sx; ty = e.clientY - sy; apply(); });
    window.addEventListener('mouseup', () => { dragging = false; });
  </script>
</body>
</html>`;
  }
}

// ---------------------------------------------------------------------------
// Inline Grammar Debugger (`parsley-debug`)
// ---------------------------------------------------------------------------

class ParsleyInlineDebugAdapter {
  constructor(client) {
    this.sendEmitter = new vscode.EventEmitter();
    this.onDidSendMessage = this.sendEmitter.event;
    this.seq = 1;
    this.client = client;
    this.programPath = "";
    this.programUri = undefined;
    this.inputText = "";
    this.startRule = "";
    this.stopOnEntry = true;
    this.breakpoints = new Set();
    this.steps = [];
    this.stepIdx = 0;
    this.evalResult = null;
    this.structure = undefined;
    this.ruleFirstFollow = new Map();
  }

  dispose() {
    this.sendEmitter.dispose();
  }

  sendResponse(req, body, success = true, message) {
    this.sendEmitter.fire({
      seq: this.seq++,
      type: "response",
      request_seq: req.seq,
      command: req.command,
      success,
      message,
      body,
    });
  }

  sendEvent(event, body) {
    this.sendEmitter.fire({
      seq: this.seq++,
      type: "event",
      event,
      body,
    });
  }

  handleMessage(msg) {
    if (msg.type !== "request") {
      return;
    }

    switch (msg.command) {
      case "initialize":
        this.sendResponse(msg, {
          supportsConfigurationDoneRequest: true,
          supportsEvaluateForHovers: true,
          supportsStepBack: false,
          supportsRestartRequest: false,
        });
        this.sendEvent("initialized");
        break;

      case "launch":
        void this.handleLaunch(msg);
        break;

      case "setBreakpoints": {
        const args = msg.arguments || {};
        const bps = args.breakpoints || [];
        this.breakpoints.clear();
        const verified = bps.map((b) => {
          this.breakpoints.add(b.line);
          return { verified: true, line: b.line };
        });
        this.sendResponse(msg, { breakpoints: verified });
        break;
      }

      case "configurationDone":
        this.sendResponse(msg);
        this.advanceInitial();
        break;

      case "threads":
        this.sendResponse(msg, { threads: [{ id: 1, name: "Grammar Derivation" }] });
        break;

      case "stackTrace":
        this.handleStackTrace(msg);
        break;

      case "scopes":
        this.sendResponse(msg, {
          scopes: [
            { name: "Parser State", variablesReference: 1001, expensive: false },
            { name: "Token Stream", variablesReference: 1002, expensive: false },
            { name: "AST / Output", variablesReference: 1003, expensive: false },
            { name: "FIRST / FOLLOW", variablesReference: 1004, expensive: false },
          ],
        });
        break;

      case "variables":
        this.handleVariables(msg);
        break;

      case "evaluate":
        void this.handleEvaluate(msg);
        break;

      case "continue":
        this.sendResponse(msg, { allThreadsContinued: true });
        this.runUntilBreakpointOrEnd();
        break;

      case "next":
      case "stepIn":
        this.sendResponse(msg);
        if (this.stepIdx + 1 < this.steps.length) {
          this.stepIdx++;
          this.sendEvent("stopped", { reason: "step", threadId: 1, allThreadsStopped: true });
        } else {
          this.finishSession();
        }
        break;

      case "stepOut":
        this.sendResponse(msg);
        this.finishSession();
        break;

      case "disconnect":
      case "terminate":
        this.sendResponse(msg);
        this.sendEvent("terminated");
        break;

      default:
        this.sendResponse(msg);
        break;
    }
  }

  async handleLaunch(req) {
    const args = req.arguments || {};
    this.programPath = String(args.program || "");
    this.inputText = String(args.input === undefined ? "" : args.input);
    this.startRule = String(args.startRule || "");
    this.stopOnEntry = args.stopOnEntry !== false;

    if (!this.programPath || !fs.existsSync(this.programPath)) {
      this.sendResponse(req, undefined, false, `Grammar file not found: ${this.programPath}`);
      return;
    }

    this.programUri = vscode.Uri.file(this.programPath);
    const doc = await vscode.workspace.openTextDocument(this.programUri);
    this.client.didOpen(doc);
    this.structure = parseLocalGrammarStructure(doc.getText());

    if (!this.startRule) {
      this.startRule =
        this.structure.exports[0] ||
        (this.structure.rules[0] && this.structure.rules[0].name) ||
        "program";
    }

    this.evalResult = await this.client.sendRequest("parsley/evalInput", {
      uri: this.programUri.toString(),
      input: this.inputText,
      ...(this.startRule ? { startRule: this.startRule } : {}),
    });

    const ruleSubset = this.structure.rules.slice(0, 20);
    const hoverInfos = await Promise.all(
      ruleSubset.map((r) => fetchRuleHoverDetails(this.client, doc, r.line, r.name))
    );
    for (let i = 0; i < ruleSubset.length; i++) {
      this.ruleFirstFollow.set(ruleSubset[i].name, hoverInfos[i]);
    }

    this.buildTraceSteps(doc.getText());
    this.sendResponse(msgOrReq(req));
    this.sendEvent("output", {
      category: "console",
      output: `[parsley-debug] Loaded ${path.basename(this.programPath)} (startRule=${this.startRule}), input=${JSON.stringify(this.inputText)}\n`,
    });
  }

  buildTraceSteps(sourceText) {
    const lines = sourceText.split(/\r?\n/);
    const ruleLineMap = new Map();
    for (const r of (this.structure && this.structure.rules) || []) {
      ruleLineMap.set(r.name, r.line + 1);
    }

    const startLine = ruleLineMap.get(this.startRule) || 1;
    const tokens = (this.evalResult && this.evalResult.tokens) || [];
    this.steps = [
      {
        ruleName: this.startRule,
        line: startLine,
        column: 1,
        note: `Enter start rule '${this.startRule}'`,
        tokenIndex: 0,
        cursorLine: 1,
        cursorCol: 1,
      },
    ];

    for (let i = 0; i < tokens.length; i++) {
      const tok = tokens[i];
      let matchedRule = this.startRule;
      let matchedLine = startLine;
      for (const r of (this.structure && this.structure.rules) || []) {
        const ruleSrcLine = lines[r.line] || "";
        const bodySlice = lines.slice(r.line, Math.min(lines.length, r.line + 12)).join(" ");
        if (bodySlice.includes(tok.id) || bodySlice.includes(`"${tok.lexeme}"`) || ruleSrcLine.includes(tok.id)) {
          matchedRule = r.name;
          matchedLine = r.line + 1;
          break;
        }
      }
      this.steps.push({
        ruleName: matchedRule,
        line: matchedLine,
        column: 1,
        note: `Consume token ${tok.id} (${JSON.stringify(tok.lexeme)}) at ${tok.line}:${tok.col}`,
        tokenIndex: i,
        cursorLine: tok.line,
        cursorCol: tok.col,
      });
    }

    if (this.evalResult && this.evalResult.ok) {
      this.steps.push({
        ruleName: this.startRule,
        line: startLine,
        column: 1,
        note: `Accept AST: ${this.evalResult.formattedAst || "(ok)"}`,
        tokenIndex: Math.max(0, tokens.length - 1),
        cursorLine: tokens.length ? tokens[tokens.length - 1].line : 1,
        cursorCol: tokens.length ? tokens[tokens.length - 1].col : 1,
      });
    } else if (this.evalResult && this.evalResult.error) {
      this.steps.push({
        ruleName: this.startRule,
        line: startLine,
        column: 1,
        note: `Parse error at ${this.evalResult.error.line}:${this.evalResult.error.col}: ${this.evalResult.error.message || "unexpected token"}`,
        tokenIndex: Math.max(0, tokens.length - 1),
        cursorLine: this.evalResult.error.line,
        cursorCol: this.evalResult.error.col,
      });
    }
  }

  advanceInitial() {
    this.stepIdx = 0;
    if (this.stopOnEntry) {
      this.sendEvent("stopped", { reason: "entry", threadId: 1, allThreadsStopped: true });
      return;
    }
    this.runUntilBreakpointOrEnd();
  }

  runUntilBreakpointOrEnd() {
    for (let i = this.stepIdx + 1; i < this.steps.length; i++) {
      if (this.breakpoints.has(this.steps[i].line)) {
        this.stepIdx = i;
        this.sendEvent("stopped", { reason: "breakpoint", threadId: 1, allThreadsStopped: true });
        return;
      }
    }
    this.finishSession();
  }

  finishSession() {
    const summary =
      this.evalResult && this.evalResult.ok
        ? `Parsed OK -> ${this.evalResult.formattedAst}`
        : `Parse Error -> ${(this.evalResult && this.evalResult.error && this.evalResult.error.message) || "failed"}`;
    this.sendEvent("output", { category: "console", output: `[parsley-debug] ${summary}\n` });
    this.sendEvent("terminated");
  }

  handleStackTrace(req) {
    const frames = [];
    for (let i = this.stepIdx; i >= 0; i--) {
      const s = this.steps[i];
      frames.push({
        id: i + 1,
        name: `${s.ruleName} — ${s.note}`,
        source: { name: path.basename(this.programPath), path: this.programPath },
        line: s.line,
        column: s.column,
      });
    }
    this.sendResponse(req, { stackFrames: frames, totalFrames: frames.length });
  }

  handleVariables(req) {
    const ref = Number((req.arguments && req.arguments.variablesReference) || 0);
    const step = this.steps[this.stepIdx] || this.steps[0];
    const tokens = (this.evalResult && this.evalResult.tokens) || [];
    const curTok = tokens[step ? step.tokenIndex : 0];

    if (ref === 1001) {
      this.sendResponse(req, {
        variables: [
          { name: "startRule", value: this.startRule, type: "Rule", variablesReference: 0 },
          { name: "activeRule", value: (step && step.ruleName) || this.startRule, type: "Rule", variablesReference: 0 },
          { name: "input", value: JSON.stringify(this.inputText), type: "Text", variablesReference: 0 },
          { name: "cursorLine", value: String((step && step.cursorLine) || 1), type: "Int", variablesReference: 0 },
          { name: "cursorCol", value: String((step && step.cursorCol) || 1), type: "Int", variablesReference: 0 },
          {
            name: "currentToken",
            value: curTok ? `${curTok.id} (${JSON.stringify(curTok.lexeme)})` : "<eof>",
            type: "Token",
            variablesReference: 0,
          },
          { name: "status", value: this.evalResult && this.evalResult.ok ? "OK" : "Error", type: "Status", variablesReference: 0 },
        ],
      });
      return;
    }

    if (ref === 1002) {
      const vars = tokens.map((t, i) => ({
        name: `[${i}] ${t.id}`,
        value: `${JSON.stringify(t.lexeme)} @ ${t.line}:${t.col}`,
        type: "Token",
        variablesReference: 0,
      }));
      this.sendResponse(req, { variables: vars });
      return;
    }

    if (ref === 1003) {
      const vars = [];
      if (this.evalResult && this.evalResult.ok) {
        vars.push({
          name: "formattedAst",
          value: this.evalResult.formattedAst || "",
          type: "AST",
          variablesReference: 0,
        });
        if (this.evalResult.astJson !== undefined) {
          vars.push({
            name: "astJson",
            value:
              typeof this.evalResult.astJson === "string"
                ? this.evalResult.astJson
                : JSON.stringify(this.evalResult.astJson),
            type: "JSON",
            variablesReference: 0,
          });
        }
      } else {
        const e = this.evalResult && this.evalResult.error;
        vars.push({ name: "errorLine", value: String((e && e.line) || 1), type: "Int", variablesReference: 0 });
        vars.push({ name: "errorCol", value: String((e && e.col) || 1), type: "Int", variablesReference: 0 });
        vars.push({ name: "expected", value: (e && e.expected) || "", type: "Text", variablesReference: 0 });
        vars.push({ name: "found", value: (e && e.found) || "", type: "Text", variablesReference: 0 });
        vars.push({ name: "message", value: (e && e.message) || "Parse failed", type: "Text", variablesReference: 0 });
      }
      this.sendResponse(req, { variables: vars });
      return;
    }

    if (ref === 1004) {
      const vars = [];
      for (const [rname, info] of this.ruleFirstFollow.entries()) {
        vars.push({
          name: rname,
          value: `FIRST=${info.first || "{}"} | FOLLOW=${info.follow || "{}"}`,
          type: info.typeSignature || "Rule",
          variablesReference: 0,
        });
      }
      this.sendResponse(req, { variables: vars });
      return;
    }

    this.sendResponse(req, { variables: [] });
  }

  async handleEvaluate(req) {
    const expr = String((req.arguments && req.arguments.expression) || "").trim();
    if (!this.programUri) {
      this.sendResponse(req, { result: "No active grammar.", variablesReference: 0 });
      return;
    }
    if (expr === ":tokens") {
      const list = ((this.evalResult && this.evalResult.tokens) || [])
        .map((t, i) => `[${i}] ${t.id} ${JSON.stringify(t.lexeme)} (${t.line}:${t.col})`)
        .join("\n");
      this.sendResponse(req, { result: list || "(no tokens)", variablesReference: 0 });
      return;
    }
    const sample = expr.startsWith(":") ? expr.slice(1).trim() : expr;
    const res = await this.client.sendRequest("parsley/evalInput", {
      uri: this.programUri.toString(),
      input: sample,
    });
    if (!res) {
      this.sendResponse(req, { result: "LSP did not return an evaluation result.", variablesReference: 0 });
      return;
    }
    const errObj = res.error || {};
    const out = res.ok
      ? `OK: ${res.formattedAst}`
      : `Error (${errObj.line || 1}:${errObj.col || 1}): ${errObj.message || "parse error"}`;
    this.sendResponse(req, { result: out, variablesReference: 0 });
  }
}

function msgOrReq(req) {
  return req;
}

// ---------------------------------------------------------------------------
// Extension Activation & Registration
// ---------------------------------------------------------------------------

function activate(context) {
  cliOutput = vscode.window.createOutputChannel("Parsley");
  lspTraceOutput = vscode.window.createOutputChannel("Parsley Language Server");

  const diagnostics = vscode.languages.createDiagnosticCollection("parsley");
  const statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
  statusBar.text = "$(symbol-misc) Parsley";
  statusBar.tooltip = "Parsley Grammar DSL";

  const treeViews = {};
  const client = new ParsleyLspClient(diagnostics, statusBar, () => {
    if (treeViews.explorer) treeViews.explorer.refresh();
    if (treeViews.examples) treeViews.examples.refresh();
  });

  const explorerProvider = new GrammarExplorerProvider(client);
  const examplesProvider = new ExamplesViewProvider();
  treeViews.explorer = explorerProvider;
  treeViews.examples = examplesProvider;

  const playground = new PlaygroundPanelManager(client);
  const railroad = new RailroadPanelManager(client);

  const selector = { language: "parsley", scheme: "file" };
  const legend = client.getSemanticLegend();

  void vscode.workspace.findFiles("**/*.parsley", "**/build/**", 1).then((found) => {
    void vscode.commands.executeCommand("setContext", "workspaceHasParsleyFiles", found.length > 0);
  });

  context.subscriptions.push(
    cliOutput,
    lspTraceOutput,
    diagnostics,
    statusBar,
    client,
    vscode.window.registerTreeDataProvider("parsleyGrammarExplorer", explorerProvider),
    vscode.window.registerTreeDataProvider("parsleyExamplesView", examplesProvider),

    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId === "parsley") {
        client.didOpen(doc);
        explorerProvider.refresh();
        examplesProvider.refresh();
        void vscode.commands.executeCommand("setContext", "workspaceHasParsleyFiles", true);
      }
    }),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId === "parsley") {
        client.didChange(e.document);
        playground.notifyDocumentChanged(e.document);
        explorerProvider.refresh();
        examplesProvider.refresh();
      }
    }),
    vscode.workspace.onDidSaveTextDocument((doc) => {
      if (doc.languageId === "parsley") {
        void railroad.refreshIfOpen(doc.uri);
      }
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId === "parsley") {
        client.didClose(doc);
      }
    }),
    vscode.window.onDidChangeActiveTextEditor((ed) => {
      if (ed && ed.document.languageId === "parsley") {
        explorerProvider.refresh();
        examplesProvider.refresh();
      }
    }),
    vscode.workspace.onWillSaveTextDocument((e) => {
      if (e.document.languageId === "parsley" && getConfig().get("autoFormatOnSave")) {
        e.waitUntil(
          vscode.commands
            .executeCommand("vscode.executeFormatDocumentProvider", e.document.uri, {
              tabSize: 2,
              insertSpaces: true,
            })
            .then((edits) => edits || [])
        );
      }
    }),

    vscode.languages.registerHoverProvider(selector, {
      async provideHover(doc, pos) {
        const res = await client.sendRequest("textDocument/hover", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: pos.line, character: pos.character },
        });
        if (!res || !res.contents) {
          return undefined;
        }
        const md =
          typeof res.contents === "string"
            ? new vscode.MarkdownString(res.contents)
            : new vscode.MarkdownString(res.contents.value || "");
        return new vscode.Hover(md, res.range ? toVscodeRange(res.range) : undefined);
      },
    }),

    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(doc, pos) {
        const res = await client.sendRequest("textDocument/definition", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: pos.line, character: pos.character },
        });
        if (!res) {
          return undefined;
        }
        const arr = Array.isArray(res) ? res : [res];
        return arr.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), toVscodeRange(l.range)));
      },
    }),

    vscode.languages.registerReferenceProvider(selector, {
      async provideReferences(doc, pos, ctx) {
        const res = await client.sendRequest("textDocument/references", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: pos.line, character: pos.character },
          context: { includeDeclaration: ctx.includeDeclaration },
        });
        if (!res) {
          return [];
        }
        return res.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), toVscodeRange(l.range)));
      },
    }),

    vscode.languages.registerDocumentSymbolProvider(selector, {
      async provideDocumentSymbols(doc) {
        const res = await client.sendRequest("textDocument/documentSymbol", {
          textDocument: { uri: doc.uri.toString() },
        });
        if (Array.isArray(res) && res.length > 0) {
          const convert = (s) => {
            const sym = new vscode.DocumentSymbol(
              s.name,
              s.detail || "",
              toVscodeSymbolKind(s.kind),
              toVscodeRange(s.range),
              toVscodeRange(s.selectionRange || s.range)
            );
            if (Array.isArray(s.children)) {
              sym.children = s.children.map(convert);
            }
            return sym;
          };
          return res.map(convert);
        }
        return buildLocalDocumentSymbols(doc);
      },
    }),

    vscode.languages.registerDocumentSemanticTokensProvider(
      selector,
      {
        async provideDocumentSemanticTokens(doc) {
          const res = await client.sendRequest("textDocument/semanticTokens/full", {
            textDocument: { uri: doc.uri.toString() },
          });
          if (!res || !Array.isArray(res.data)) {
            return undefined;
          }
          return new vscode.SemanticTokens(new Uint32Array(res.data));
        },
      },
      legend
    ),

    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      async provideDocumentFormattingEdits(doc) {
        const res = await client.sendRequest("textDocument/formatting", {
          textDocument: { uri: doc.uri.toString() },
          options: { tabSize: 2, insertSpaces: true },
        });
        if (Array.isArray(res) && res.length > 0) {
          return res.map((e) => new vscode.TextEdit(toVscodeRange(e.range), e.newText));
        }
        return [];
      },
    }),

    vscode.languages.registerCompletionItemProvider(
      selector,
      {
        async provideCompletionItems(doc, pos) {
          const res = await client.sendRequest("textDocument/completion", {
            textDocument: { uri: doc.uri.toString() },
            position: { line: pos.line, character: pos.character },
          });
          const rawItems = Array.isArray(res) ? res : res && res.items;
          if (!Array.isArray(rawItems)) {
            return buildFallbackCompletions(doc);
          }
          return rawItems.map((it) => {
            const item = new vscode.CompletionItem(it.label, it.kind);
            item.detail = it.detail;
            if (it.documentation) {
              item.documentation =
                typeof it.documentation === "string"
                  ? it.documentation
                  : new vscode.MarkdownString(it.documentation.value);
            }
            if (it.insertText) {
              item.insertText =
                it.insertTextFormat === 2
                  ? new vscode.SnippetString(it.insertText)
                  : it.insertText;
            }
            return item;
          });
        },
      },
      ".",
      ":",
      "<",
      "`"
    ),

    vscode.languages.registerCodeActionProvider(selector, {
      async provideCodeActions(doc, range, ctx) {
        const actions = [];
        const res = await client.sendRequest("textDocument/codeAction", {
          textDocument: { uri: doc.uri.toString() },
          range: toLspRange(range),
          context: {
            diagnostics: ctx.diagnostics.map((d) => ({
              range: toLspRange(d.range),
              code: d.code,
              message: d.message,
            })),
          },
        });

        if (Array.isArray(res)) {
          for (const item of res) {
            const kind =
              item.kind === "refactor"
                ? vscode.CodeActionKind.Refactor
                : item.kind === "source"
                  ? vscode.CodeActionKind.Source
                  : vscode.CodeActionKind.QuickFix;
            const act = new vscode.CodeAction(item.title, kind);
            act.isPreferred = Boolean(item.isPreferred);
            if (item.edit) {
              const wsEdit = new vscode.WorkspaceEdit();
              if (item.edit.changes) {
                for (const [u, edits] of Object.entries(item.edit.changes)) {
                  const target = vscode.Uri.parse(u);
                  for (const ed of edits) {
                    wsEdit.replace(target, toVscodeRange(ed.range), ed.newText);
                  }
                }
              }
              if (Array.isArray(item.edit.documentChanges)) {
                for (const dc of item.edit.documentChanges) {
                  if (dc.textDocument && dc.textDocument.uri && Array.isArray(dc.edits)) {
                    const target = vscode.Uri.parse(dc.textDocument.uri);
                    for (const ed of dc.edits) {
                      wsEdit.replace(target, toVscodeRange(ed.range), ed.newText);
                    }
                  }
                }
              }
              act.edit = wsEdit;
            }
            if (item.command) {
              act.command = {
                title: item.command.title,
                command: item.command.command,
                arguments: item.command.arguments,
              };
            }
            actions.push(act);
          }
        }

        const seenCodes = new Set();
        for (const diag of ctx.diagnostics) {
          const code = diag.code ? String(diag.code) : "";
          if (code && !seenCodes.has(code)) {
            seenCodes.add(code);
            const explainAction = new vscode.CodeAction(
              `Parsley: Explain Error ${code}`,
              vscode.CodeActionKind.QuickFix
            );
            explainAction.command = {
              title: `Explain ${code}`,
              command: "parsley.explainError",
              arguments: [code],
            };
            actions.push(explainAction);
          }
        }

        return actions;
      },
    }),

    vscode.languages.registerCodeLensProvider(selector, {
      async provideCodeLenses(doc) {
        const res = await client.sendRequest("textDocument/codeLens", {
          textDocument: { uri: doc.uri.toString() },
        });

        const lenses = [];
        if (Array.isArray(res) && res.length > 0) {
          for (const cl of res) {
            if (cl.command) {
              lenses.push(
                new vscode.CodeLens(toVscodeRange(cl.range), {
                  title: cl.command.title,
                  command: cl.command.command,
                  arguments: cl.command.arguments,
                })
              );
            }
          }
          return lenses;
        }

        const parsed = parseLocalGrammarStructure(doc.getText());
        for (let i = 0; i < doc.lineCount; i++) {
          if (/^\s*module\b/.test(doc.lineAt(i).text)) {
            const r = new vscode.Range(i, 0, i, 6);
            lenses.push(new vscode.CodeLens(r, { title: "$(play) Playground", command: "parsley.openPlayground", arguments: [doc.uri] }));
            lenses.push(new vscode.CodeLens(r, { title: "$(type-hierarchy) Railroad", command: "parsley.showRailroad", arguments: [doc.uri] }));
            lenses.push(new vscode.CodeLens(r, { title: "$(tools) Build C17", command: "parsley.buildC17", arguments: [doc.uri] }));
            break;
          }
        }
        for (const ex of parsed.examples) {
          const r = new vscode.Range(ex.line, 0, ex.line, 7);
          lenses.push(
            new vscode.CodeLens(r, {
              title: `$(play) Run "${ex.name}"`,
              command: "parsley.runSingleExample",
              arguments: [doc.uri, ex],
            })
          );
          lenses.push(
            new vscode.CodeLens(r, {
              title: "$(debug-alt) Debug",
              command: "parsley.debugExample",
              arguments: [{ uri: doc.uri, input: ex.input }],
            })
          );
        }
        return lenses;
      },
    }),

    vscode.debug.registerDebugConfigurationProvider("parsley-debug", {
      resolveDebugConfiguration(_folder, config) {
        if (!config.type && !config.request && !config.name) {
          const ed = vscode.window.activeTextEditor;
          if (ed && ed.document.languageId === "parsley") {
            config.type = "parsley-debug";
            config.name = "Debug Parsley Grammar";
            config.request = "launch";
            config.program = ed.document.fileName;
            const parsed = parseLocalGrammarStructure(ed.document.getText());
            config.input = (parsed.examples[0] && parsed.examples[0].input) || "1 + 2 * 3";
            config.stopOnEntry = true;
          }
        }
        if (!config.program) {
          return vscode.window.showInformationMessage("Open a .parsley file to debug.").then(() => undefined);
        }
        return config;
      },
    }),
    vscode.debug.registerDebugAdapterDescriptorFactory("parsley-debug", {
      createDebugAdapterDescriptor() {
        return new vscode.DebugAdapterInlineImplementation(new ParsleyInlineDebugAdapter(client));
      },
    }),

    vscode.commands.registerCommand("parsley.check", async (uriArg) => {
      const activeDoc = getActiveParsleyDocument();
      const uri = coerceUriArg(uriArg) || (activeDoc && activeDoc.uri);
      if (!uri) {
        vscode.window.showWarningMessage("Open a .parsley file to check.");
        return;
      }
      await saveIfDirty(uri);
      cliOutput.show(true);
      cliOutput.appendLine(`> parsley check ${uri.fsPath}`);
      const res = await runCliCommand(["check", uri.fsPath], getWorkspaceRoot(uri), uri);
      if (res.stdout) cliOutput.append(res.stdout);
      if (res.stderr) cliOutput.append(res.stderr);
      if (res.code === 0) {
        vscode.window.showInformationMessage(res.stdout.trim() || "Parsley grammar check passed.");
      } else {
        vscode.window.showErrorMessage(`Parsley check failed (exit ${res.code}). See Output > Parsley.`);
      }
    }),

    vscode.commands.registerCommand("parsley.test", async (uriArg) => {
      const activeDoc = getActiveParsleyDocument();
      const uri = coerceUriArg(uriArg) || (activeDoc && activeDoc.uri);
      if (!uri) {
        vscode.window.showWarningMessage("Open a .parsley file to run tests.");
        return;
      }
      await saveIfDirty(uri);
      const fuzzCfg = getConfig().get("fuzzCount");
      const fuzz = fuzzCfg === undefined ? 50 : fuzzCfg;
      const args = ["test", uri.fsPath];
      if (fuzz > 0) {
        args.push("--fuzz", String(fuzz));
      }
      cliOutput.show(true);
      cliOutput.appendLine(`> parsley ${args.join(" ")}`);
      const res = await runCliCommand(args, getWorkspaceRoot(uri), uri);
      if (res.stdout) cliOutput.append(res.stdout);
      if (res.stderr) cliOutput.append(res.stderr);
      if (res.code === 0) {
        vscode.window.showInformationMessage(`Parsley tests passed (${path.basename(uri.fsPath)}).`);
      } else {
        vscode.window.showErrorMessage("Parsley tests failed. See Output > Parsley.");
      }
    }),

    vscode.commands.registerCommand("parsley.buildC17", async (uriArg) => {
      const activeDoc = getActiveParsleyDocument();
      const uri = coerceUriArg(uriArg) || (activeDoc && activeDoc.uri);
      if (!uri) {
        vscode.window.showWarningMessage("Open a .parsley file to build C17 parser.");
        return;
      }
      await saveIfDirty(uri);
      const configuredOut = getConfig().get("c17.outputDir") || "./generated_parser";
      const root = getWorkspaceRoot(uri);
      const outDir = path.isAbsolute(configuredOut) ? configuredOut : path.resolve(root, configuredOut);
      cliOutput.show(true);
      cliOutput.appendLine(`> parsley build ${uri.fsPath} -o ${outDir} --emit-ir-json`);
      const res = await runCliCommand(["build", uri.fsPath, "-o", outDir, "--emit-ir-json"], root, uri);
      if (res.stdout) cliOutput.append(res.stdout);
      if (res.stderr) cliOutput.append(res.stderr);
      if (res.code === 0) {
        const choice = await vscode.window.showInformationMessage(
          `ISO C17 parser built in ${outDir}`,
          "Reveal Folder"
        );
        if (choice === "Reveal Folder") {
          void vscode.commands.executeCommand("revealFileInOS", vscode.Uri.file(outDir));
        }
      } else {
        vscode.window.showErrorMessage("Parsley C17 build failed. See Output > Parsley.");
      }
    }),

    vscode.commands.registerCommand("parsley.format", async () => {
      const ed = vscode.window.activeTextEditor;
      if (!ed || ed.document.languageId !== "parsley") {
        vscode.window.showWarningMessage("Open a .parsley file to format.");
        return;
      }
      const edits = await vscode.commands.executeCommand(
        "vscode.executeFormatDocumentProvider",
        ed.document.uri,
        { tabSize: 2, insertSpaces: true }
      );
      if (edits && edits.length > 0) {
        const wsEdit = new vscode.WorkspaceEdit();
        wsEdit.set(ed.document.uri, edits);
        await vscode.workspace.applyEdit(wsEdit);
        return;
      }
      const cli = await runCliCommand(["fmt", ed.document.fileName], getWorkspaceRoot(ed.document.uri), ed.document.uri);
      if (cli.code === 0 && cli.stdout) {
        const fullRange = new vscode.Range(
          ed.document.positionAt(0),
          ed.document.positionAt(ed.document.getText().length)
        );
        const wsEdit = new vscode.WorkspaceEdit();
        wsEdit.replace(ed.document.uri, fullRange, cli.stdout);
        await vscode.workspace.applyEdit(wsEdit);
      } else {
        vscode.window.showErrorMessage(cli.stderr || "Formatting failed.");
      }
    }),

    vscode.commands.registerCommand("parsley.showRailroad", async (uriArg) => {
      await railroad.show(coerceUriArg(uriArg));
    }),

    vscode.commands.registerCommand("parsley.openPlayground", async (uriArg) => {
      await playground.open(coerceUriArg(uriArg));
    }),

    vscode.commands.registerCommand("parsley.explainError", async (codeArg) => {
      const known = [
        "E0001", "E0002", "E0100", "E0200", "E0214",
        "E0300", "E0301", "E0310", "E0400", "E0500",
        "E0510", "E0520", "E0600",
      ];
      let code = typeof codeArg === "string" ? codeArg : "";
      if (!code) {
        code =
          (await vscode.window.showQuickPick(known, {
            placeHolder: "Select a Parsley diagnostic code to explain (e.g. E0214, E0301)",
          })) || "";
      }
      if (!code) {
        return;
      }
      const res = await runCliCommand(["explain", code], getWorkspaceRoot());
      cliOutput.show(true);
      cliOutput.appendLine(`--- parsley explain ${code} ---`);
      cliOutput.appendLine(res.stdout || res.stderr);
      const doc = await vscode.workspace.openTextDocument({
        language: "markdown",
        content: `# Parsley Diagnostic ${code}\n\n\`\`\`text\n${(res.stdout || res.stderr).trim()}\n\`\`\`\n`,
      });
      await vscode.window.showTextDocument(doc, { preview: true, viewColumn: vscode.ViewColumn.Beside });
    }),

    vscode.commands.registerCommand("parsley.debugExample", async (arg, _exName, exInput) => {
      let uri;
      let input;

      if (arg instanceof ExampleTreeItem) {
        uri = arg.uri;
        input = arg.example.input;
      } else if (typeof arg === "string") {
        uri = coerceUriArg(arg);
        if (typeof exInput === "string") {
          input = exInput;
        }
      } else if (arg && typeof arg === "object") {
        uri = coerceUriArg(arg.uri);
        if (typeof arg.input === "string") input = arg.input;
      }

      const doc = uri ? await vscode.workspace.openTextDocument(uri) : getActiveParsleyDocument();
      if (!doc) {
        vscode.window.showWarningMessage("Open a .parsley file to debug.");
        return;
      }
      const parsed = parseLocalGrammarStructure(doc.getText());
      if (input === undefined) {
        input = await vscode.window.showInputBox({
          prompt: "Enter sample input to trace with the Parsley Grammar Debugger",
          value: (parsed.examples[0] && parsed.examples[0].input) || "1 + 2 * 3",
        });
        if (input === undefined) {
          return;
        }
      }

      const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
      await vscode.debug.startDebugging(folder, {
        type: "parsley-debug",
        request: "launch",
        name: `Debug ${path.basename(doc.fileName)}`,
        program: doc.fileName,
        input,
        startRule: parsed.exports[0] || (parsed.rules[0] && parsed.rules[0].name) || "",
        stopOnEntry: true,
      });
    }),

    vscode.commands.registerCommand("parsley.runSingleExample", async (arg1, arg2) => {
      let uri;
      let ex;
      if (arg1 instanceof ExampleTreeItem) {
        uri = arg1.uri;
        ex = arg1.example;
      } else if (arg1 instanceof vscode.Uri && arg2) {
        uri = arg1;
        ex = arg2;
      }
      if (!uri || !ex) {
        return;
      }
      const res = await client.sendRequest("parsley/evalInput", {
        uri: uri.toString(),
        input: ex.input,
      });
      cliOutput.show(true);
      if (res) {
        const passed = ex.kind === "parses-to" ? res.ok : !res.ok;
        const msg = res.ok
          ? `AST: ${res.formattedAst}`
          : `Error: ${(res.error && res.error.message) || "parse error"}`;
        cliOutput.appendLine(`[${passed ? "PASS" : "FAIL"}] example "${ex.name}" (${JSON.stringify(ex.input)}) -> ${msg}`);
        if (passed) {
          vscode.window.showInformationMessage(`[PASS] "${ex.name}" -> ${msg}`);
        } else {
          vscode.window.showErrorMessage(`[FAIL] "${ex.name}" -> ${msg}`);
        }
      } else {
        await vscode.commands.executeCommand("parsley.test", uri);
      }
    }),

    vscode.commands.registerCommand("parsley.restartLsp", async () => {
      await client.restart();
      vscode.window.showInformationMessage("Parsley Language Server restarted.");
    }),

    vscode.commands.registerCommand("parsley.refreshViews", () => {
      explorerProvider.refresh();
      examplesProvider.refresh();
    }),

    vscode.commands.registerCommand("parsley.revealLine", async (uri, line) => {
      const doc = await vscode.workspace.openTextDocument(uri);
      const ed = await vscode.window.showTextDocument(doc);
      const pos = new vscode.Position(Math.max(0, line), 0);
      ed.selection = new vscode.Selection(pos, pos);
      ed.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
    })
  );

  const openParsley = getActiveParsleyDocument();
  if (openParsley) {
    client.didOpen(openParsley);
  }
}

function deactivate() {}

// ---------------------------------------------------------------------------
// Utility Helpers
// ---------------------------------------------------------------------------

async function saveIfDirty(uri) {
  const doc = vscode.workspace.textDocuments.find((d) => d.uri.toString() === uri.toString());
  if (doc && doc.isDirty) {
    await doc.save();
  }
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function buildLocalDocumentSymbols(doc) {
  const parsed = parseLocalGrammarStructure(doc.getText());
  const symbols = [];

  for (const t of parsed.tokens) {
    const r = doc.lineAt(t.line).range;
    symbols.push(
      new vscode.DocumentSymbol(t.name, `${t.kind}${t.type ? ` :: ${t.type}` : ""}`, vscode.SymbolKind.Variable, r, r)
    );
  }
  for (const d of parsed.dataTypes) {
    const r = doc.lineAt(d.line).range;
    const sym = new vscode.DocumentSymbol(d.name, d.kind, vscode.SymbolKind.Enum, r, r);
    sym.children = d.ctors.map(
      (c) => new vscode.DocumentSymbol(c, d.name, vscode.SymbolKind.EnumMember, r, r)
    );
    symbols.push(sym);
  }
  for (const rule of parsed.rules) {
    const r = doc.lineAt(rule.line).range;
    symbols.push(
      new vscode.DocumentSymbol(rule.name, rule.signature || "Rule", vscode.SymbolKind.Function, r, r)
    );
  }
  for (const ex of parsed.examples) {
    const r = doc.lineAt(ex.line).range;
    symbols.push(
      new vscode.DocumentSymbol(`example "${ex.name}"`, ex.kind, vscode.SymbolKind.Event, r, r)
    );
  }
  return symbols;
}

function buildFallbackCompletions(doc) {
  const parsed = parseLocalGrammarStructure(doc.getText());
  const items = [];

  for (const r of parsed.rules) {
    const it = new vscode.CompletionItem(r.name, vscode.CompletionItemKind.Function);
    it.detail = r.signature || "Grammar rule";
    items.push(it);
  }
  for (const t of parsed.tokens) {
    const it = new vscode.CompletionItem(t.name, vscode.CompletionItemKind.Variable);
    it.detail = `${t.kind}${t.type ? ` :: ${t.type}` : ""}`;
    items.push(it);
  }
  for (const d of parsed.dataTypes) {
    items.push(new vscode.CompletionItem(d.name, vscode.CompletionItemKind.Struct));
    for (const c of d.ctors) {
      const ci = new vscode.CompletionItem(c, vscode.CompletionItemKind.Constructor);
      ci.detail = `Constructor of ${d.name}`;
      items.push(ci);
    }
  }
  for (const kw of [
    "module", "where", "import", "qualified", "as", "hiding",
    "token", "skip", "keywords", "mode", "push", "pop",
    "data", "newtype", "foreign", "operators", "infixl", "infixr", "infix",
    "prefix", "postfix", "mixfix", "do", "context", "pure", "exactly",
    "example", "parses-to", "fails-at", "expecting",
    "sepBy", "sepEndBy", "many", "some", "optional", "between",
    "eof", "lookahead", "notFollowedBy", "recover",
  ]) {
    items.push(new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword));
  }
  return items;
}
