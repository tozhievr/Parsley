"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const cp = __importStar(require("child_process"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
// ---------------------------------------------------------------------------
// Constants & Shared Types
// ---------------------------------------------------------------------------
const EXTENSION_VERSION = String(require("../package.json").version);
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
function isParsleyDoc(doc) {
    return Boolean(doc && (doc.languageId === "parsley" || doc.languageId === "parsley-psem"));
}
function resolveSiblingGrammarUri(uri) {
    if (uri.scheme === "file" && uri.fsPath.endsWith(".psem")) {
        const candidate = uri.fsPath.slice(0, -5) + ".parsley";
        if (fs.existsSync(candidate)) {
            return vscode.Uri.file(candidate);
        }
    }
    return uri;
}
// ---------------------------------------------------------------------------
// Output Channels & Configuration Helpers
// ---------------------------------------------------------------------------
let cliOutput;
let lspTraceOutput;
function getConfig(uri) {
    return vscode.workspace.getConfiguration("parsley", uri);
}
function resolveExecutablePath(documentUri) {
    let configured = (getConfig(documentUri).get("executablePath") || "parsley").trim();
    if ((configured.startsWith('"') && configured.endsWith('"')) || (configured.startsWith("'") && configured.endsWith("'")))
        configured = configured.slice(1, -1);
    if (configured.startsWith("~/") || configured.startsWith("~\\"))
        configured = path.join(require("os").homedir(), configured.slice(2));
    if (configured !== "parsley" && /[\\/]/.test(configured) && !path.isAbsolute(configured))
        configured = path.resolve(getWorkspaceRoot(documentUri), configured);
    if (configured && configured !== "parsley") {
        return configured;
    }
    const candidates = [];
    const exeName = process.platform === "win32" ? "parsley.exe" : "parsley";
    if (documentUri) {
        const folder = vscode.workspace.getWorkspaceFolder(documentUri);
        if (folder) {
            candidates.push(path.join(folder.uri.fsPath, "build", exeName), path.join(folder.uri.fsPath, "build", "Release", exeName), path.join(folder.uri.fsPath, "build", "Debug", exeName));
        }
    }
    for (const folder of vscode.workspace.workspaceFolders || []) {
        candidates.push(path.join(folder.uri.fsPath, "build", exeName), path.join(folder.uri.fsPath, "build", "Release", exeName), path.join(folder.uri.fsPath, "build", "Debug", exeName));
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
    const first = vscode.workspace.workspaceFolders?.[0];
    return first ? first.uri.fsPath : process.cwd();
}
function runCliCommand(args, cwd, uri) {
    const exe = resolveExecutablePath(uri);
    return new Promise((resolve) => {
        cp.execFile(exe, args, { cwd, maxBuffer: 8 * 1024 * 1024, timeout: 120000, windowsHide: true }, (err, stdout, stderr) => {
            if (err && typeof err.code === "string" && err.code === "ENOENT") {
                resolve({
                    code: 127,
                    stdout: stdout || "",
                    stderr: `Could not launch '${exe}'. Configure 'parsley.executablePath' in Settings.`,
                });
                return;
            }
            const exitCode = err && typeof err.code === "number"
                ? err.code
                : err
                    ? 1
                    : 0;
            resolve({ code: exitCode, stdout: stdout || "", stderr: stderr || "" });
        });
    });
}
// ---------------------------------------------------------------------------
// Lightweight Local Parsley Parser (for TreeViews, Fallback Outline & Debug)
// ---------------------------------------------------------------------------
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
            result.tokens.push({ kind, name, pattern: rest, type, line: i, mode: indent > 0 ? currentMode : undefined });
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
        const exParsesMatch = trimmed.match(/^example\s+"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s+parses-to\s+(.+)$/);
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
        const exFailsMatch = trimmed.match(/^example\s+"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s+fails-at\s+(.+)$/);
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
                }
                else {
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
                    }
                    else {
                        ruleMap.set(name, { name, line: i });
                    }
                }
            }
        }
    }
    result.rules = Array.from(ruleMap.values());
    return result;
}
function parseLocalPsemStructure(text) {
    const lines = text.split(/\r?\n/);
    const result = {
        moduleName: "(anonymous)",
        imports: [],
        pragmas: [],
        types: [],
        diags: [],
        nodes: [],
        flows: [],
        analyses: [],
        lints: [],
        examples: [],
        profiles: [],
    };
    let pendingStatus;
    let currentExample;
    for (let i = 0; i < lines.length; i++) {
        const raw = lines[i];
        const trimmed = raw.trim();
        if (!trimmed || trimmed.startsWith("--")) {
            continue;
        }
        const indent = raw.length - raw.trimStart().length;
        if (indent === 0) {
            currentExample = undefined;
        }
        const pragmaMatch = trimmed.match(/^\{-#\s*(.+?)\s*#-\}/);
        if (pragmaMatch) {
            result.pragmas.push({ text: pragmaMatch[1], line: i });
            const statusM = pragmaMatch[1].match(/\bSTATUS\s+(draft|reviewed|verified)\b/);
            if (statusM) {
                pendingStatus = statusM[1];
            }
            continue;
        }
        const semMatch = trimmed.match(/^semantics\s+([A-Z][A-Za-z0-9_'.]*)\s+where\b/);
        if (semMatch) {
            result.moduleName = semMatch[1];
            pendingStatus = undefined;
            continue;
        }
        const impMatch = trimmed.match(/^import\s+(?:qualified\s+)?([A-Z][A-Za-z0-9_'.]*)/);
        if (impMatch) {
            result.imports.push(impMatch[1]);
            continue;
        }
        const typeMatch = trimmed.match(/^(type|tycon|kind|builtin|coerce|overload)\s+([A-Za-z_][A-Za-z0-9_']*)(.*)$/);
        if (typeMatch) {
            result.types.push({ kind: typeMatch[1], name: typeMatch[2], detail: typeMatch[3].trim(), line: i });
            pendingStatus = undefined;
            continue;
        }
        const diagMatch = trimmed.match(/^diag\s+([A-Z][A-Za-z0-9_']*)\s*::\s*(Error|Warning|Note)\s+"((?:[^"\\]|\\.)*)"/);
        if (diagMatch) {
            result.diags.push({ code: diagMatch[1], severity: diagMatch[2], message: diagMatch[3], line: i });
            pendingStatus = undefined;
            continue;
        }
        const nodeMatch = trimmed.match(/^node\s+(.+?)\s+where\b/);
        if (nodeMatch) {
            result.nodes.push({ pattern: nodeMatch[1].trim(), line: i });
            pendingStatus = undefined;
            continue;
        }
        const flowMatch = trimmed.match(/^flow\s+([A-Z_][A-Za-z0-9_']*)\b/);
        if (flowMatch) {
            result.flows.push({ ctor: flowMatch[1], line: i });
            pendingStatus = undefined;
            continue;
        }
        const analysisMatch = trimmed.match(/^analysis\s+([A-Za-z_][A-Za-z0-9_']*)\s*::\s*(.+)$/);
        if (analysisMatch) {
            result.analyses.push({ name: analysisMatch[1], signature: analysisMatch[2].trim(), line: i });
            pendingStatus = undefined;
            continue;
        }
        const lintMatch = trimmed.match(/^lint\s+([a-z_][A-Za-z0-9_']*)\s*::\s*(.+)$/);
        if (lintMatch) {
            result.lints.push({
                name: lintMatch[1],
                severity: lintMatch[2].trim(),
                status: pendingStatus,
                line: i,
            });
            pendingStatus = undefined;
            continue;
        }
        const exMatch = trimmed.match(/^example\s+([a-z_][A-Za-z0-9_']*)\b/);
        if (exMatch) {
            currentExample = { lintName: exMatch[1], fires: [], silent: [], line: i };
            result.examples.push(currentExample);
            pendingStatus = undefined;
            continue;
        }
        if (currentExample && indent > 0) {
            const firesM = trimmed.match(/^fires\s+"((?:[^"\\]|\\.)*)"/);
            if (firesM) {
                currentExample.fires.push(unescapeParsleyString(firesM[1]));
                continue;
            }
            const silentM = trimmed.match(/^silent\s+"((?:[^"\\]|\\.)*)"/);
            if (silentM) {
                currentExample.silent.push(unescapeParsleyString(silentM[1]));
                continue;
            }
        }
        const profMatch = trimmed.match(/^profile\s+([a-z_][A-Za-z0-9_']*)\s*=\s*(.+)$/);
        if (profMatch) {
            result.profiles.push({ name: profMatch[1], rules: profMatch[2].trim(), line: i });
            pendingStatus = undefined;
            continue;
        }
    }
    return result;
}
function unescapeParsleyString(s) {
    return s
        .replace(/\\n/g, "\n")
        .replace(/\\r/g, "\r")
        .replace(/\\t/g, "\t")
        .replace(/\\"/g, '"')
        .replace(/\\\\/g, "\\");
}
function toVscodeRange(r) {
    if (!r || !r.start || !r.end) {
        return new vscode.Range(0, 0, 0, 0);
    }
    return new vscode.Range(Math.max(0, r.start.line), Math.max(0, r.start.character), Math.max(0, r.end.line), Math.max(0, r.end.character));
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
    return map[lspKind] ?? vscode.SymbolKind.Function;
}
class ParsleyLspClient {
    proc;
    buffer = Buffer.alloc(0);
    nextId = 1;
    pending = new Map();
    initialized = false;
    initPromise;
    capabilities = {};
    openedDocs = new Map();
    statusBar;
    diagnostics;
    disposed = false;
    onDiagnosticsUpdated;
    constructor(diagnostics, statusBar, onDiagnosticsUpdated) {
        this.diagnostics = diagnostics;
        this.statusBar = statusBar;
        this.onDiagnosticsUpdated = onDiagnosticsUpdated;
    }
    getSemanticLegend() {
        const sem = this.capabilities.semanticTokensProvider;
        const tokenTypes = sem?.legend?.tokenTypes?.length
            ? sem.legend.tokenTypes
            : DEFAULT_TOKEN_TYPES;
        const tokenModifiers = sem?.legend?.tokenModifiers?.length
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
            }
            catch {
                return false;
            }
        }
        const starting = this.startInternal();
        this.initPromise = starting;
        try {
            await starting;
            return this.initialized;
        }
        catch (err) {
            this.logTrace(`LSP start failed: ${err.message}`);
            return false;
        }
        finally {
            if (this.initPromise === starting)
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
            windowsHide: true,
        });
        this.proc = proc;
        proc.stdout.on("data", (chunk) => { if (this.proc === proc)
            this.handleStdout(chunk); });
        proc.stderr.on("data", (chunk) => {
            const msg = chunk.toString("utf8").trim();
            if (msg) {
                this.logTrace(`[stderr] ${msg}`);
            }
        });
        proc.on("error", (err) => {
            this.logTrace(`[error] ${err.message}`);
            this.handleExit(proc, -1);
        });
        proc.on("exit", (code) => {
            this.logTrace(`[exit] code=${code}`);
            this.handleExit(proc, code ?? -1);
        });
        const rootUri = vscode.workspace.workspaceFolders?.[0]?.uri.toString() ?? null;
        const initResult = (await this.sendRequestRaw("initialize", {
            processId: process.pid,
            clientInfo: { name: "parsley-vscode", version: EXTENSION_VERSION },
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
        }));
        if (this.proc !== proc || !initResult?.capabilities)
            throw new Error("Invalid or interrupted LSP initialization");
        this.capabilities = initResult.capabilities;
        this.initialized = true;
        this.sendNotificationRaw("initialized", {});
        this.statusBar.text = "$(check) Parsley LSP";
        this.statusBar.tooltip = `Parsley Language Server running (${exe})`;
        this.statusBar.show();
        for (const doc of vscode.workspace.textDocuments) {
            if (isParsleyDoc(doc)) {
                this.didOpen(doc);
            }
        }
    }
    handleExit(proc, code) {
        if (this.proc !== proc)
            return;
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
        this.initPromise = undefined;
        if (!this.proc) {
            return;
        }
        const p = this.proc;
        this.handleExit(p, 0);
        try {
            if (p.stdin.writable) {
                const id = this.nextId++;
                const payload = JSON.stringify({ jsonrpc: "2.0", id, method: "shutdown", params: null });
                this.writeFrameTo(p, payload);
                const exitMsg = JSON.stringify({ jsonrpc: "2.0", method: "exit", params: null });
                this.writeFrameTo(p, exitMsg);
            }
        }
        catch {
            // ignore shutdown write errors
        }
        setTimeout(() => {
            try {
                p.kill();
            }
            catch {
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
            }
            catch (err) {
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
        }
        catch {
            // ignore
        }
    }
    async sendRequest(method, params, timeoutMs = 8000) {
        const ready = await this.ensureStarted();
        if (!ready) {
            return null;
        }
        try {
            return (await this.sendRequestRaw(method, params, timeoutMs));
        }
        catch (err) {
            this.logTrace(`[req error] ${method}: ${err.message}`);
            return null;
        }
    }
    didOpen(doc) {
        if (!isParsleyDoc(doc)) {
            return;
        }
        const uri = doc.uri.toString();
        void this.ensureStarted().then((ok) => {
            if (ok && !doc.isClosed && this.openedDocs.get(uri) !== doc.version) {
                this.openedDocs.set(uri, doc.version);
                this.sendNotificationRaw("textDocument/didOpen", {
                    textDocument: {
                        uri,
                        languageId: doc.languageId,
                        version: doc.version,
                        text: doc.getText(),
                    },
                });
            }
        });
    }
    didChange(doc) {
        if (!isParsleyDoc(doc)) {
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
        if (!isParsleyDoc(doc)) {
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
            }
            catch (err) {
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
            }
            else {
                req.resolve(msg.result);
            }
            return;
        }
        if (msg.method === "textDocument/publishDiagnostics" && msg.params) {
            const params = msg.params;
            const uri = vscode.Uri.parse(params.uri);
            const vsDiags = (params.diagnostics || []).map((d) => {
                const sev = d.severity === 2
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
        this.description = options?.description;
        this.tooltip = options?.tooltip;
        this.contextValue = options?.contextValue;
        if (options?.icon) {
            this.iconPath = new vscode.ThemeIcon(options.icon);
        }
        if (options?.uri && typeof options.line === "number") {
            this.command = {
                command: "parsley.revealLine",
                title: "Reveal in Grammar",
                arguments: [options.uri, options.line],
            };
        }
        this.children = options?.children;
    }
    children;
}
class GrammarExplorerProvider {
    _onDidChangeTreeData = new vscode.EventEmitter();
    onDidChangeTreeData = this._onDidChangeTreeData.event;
    client;
    constructor(client) {
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
            return [new GrammarTreeItem("Open a .parsley or .psem file to inspect", vscode.TreeItemCollapsibleState.None, { icon: "info" })];
        }
        if (doc.languageId === "parsley-psem") {
            return buildPsemExplorerItems(doc);
        }
        const parsed = parseLocalGrammarStructure(doc.getText());
        const rootItems = [];
        // Module item
        const modDesc = parsed.exports.length ? `(${parsed.exports.join(", ")})` : "";
        const pragmaChildren = parsed.pragmas.map((p) => new GrammarTreeItem(`{-# ${p.text} #-}`, vscode.TreeItemCollapsibleState.None, {
            icon: "symbol-property",
            line: p.line,
            uri: doc.uri,
        }));
        rootItems.push(new GrammarTreeItem(`Module: ${parsed.moduleName}`, pragmaChildren.length ? vscode.TreeItemCollapsibleState.Expanded : vscode.TreeItemCollapsibleState.None, {
            description: modDesc,
            icon: "symbol-namespace",
            children: pragmaChildren,
        }));
        // Rules with types & FIRST/FOLLOW fetched in parallel from LSP hover when available
        const hoverResults = await Promise.all(parsed.rules.map((r) => fetchRuleHoverDetails(this.client, doc, r.line, r.name)));
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
                ruleMeta.push(new GrammarTreeItem(`Strategy: ${hoverInfo.strategy}`, vscode.TreeItemCollapsibleState.None, { icon: "compass" }));
            }
            if (hoverInfo.nullable !== undefined) {
                ruleMeta.push(new GrammarTreeItem(`Nullable: ${hoverInfo.nullable}`, vscode.TreeItemCollapsibleState.None, { icon: "symbol-boolean" }));
            }
            if (hoverInfo.first) {
                ruleMeta.push(new GrammarTreeItem(`FIRST: ${hoverInfo.first}`, vscode.TreeItemCollapsibleState.None, { icon: "arrow-right" }));
            }
            if (hoverInfo.follow) {
                ruleMeta.push(new GrammarTreeItem(`FOLLOW: ${hoverInfo.follow}`, vscode.TreeItemCollapsibleState.None, { icon: "arrow-left" }));
            }
            ruleChildren.push(new GrammarTreeItem(r.name, ruleMeta.length ? vscode.TreeItemCollapsibleState.Collapsed : vscode.TreeItemCollapsibleState.None, {
                description: typeDesc,
                tooltip: `${r.name} ${typeDesc}`,
                icon: "symbol-function",
                line: r.line,
                uri: doc.uri,
                children: ruleMeta,
            }));
        }
        rootItems.push(new GrammarTreeItem(`Rules (${ruleChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
            icon: "symbol-method",
            children: ruleChildren,
        }));
        // Tokens & Skips
        const tokChildren = parsed.tokens.map((t) => new GrammarTreeItem(t.name, vscode.TreeItemCollapsibleState.None, {
            description: `${t.kind}${t.mode ? ` [${t.mode}]` : ""}${t.type ? ` :: ${t.type}` : ""}`,
            tooltip: `${t.kind} ${t.name} = ${t.pattern}`,
            icon: t.kind === "skip" ? "eye-closed" : "symbol-key",
            line: t.line,
            uri: doc.uri,
        }));
        rootItems.push(new GrammarTreeItem(`Tokens & Skips (${tokChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
            icon: "symbol-variable",
            children: tokChildren,
        }));
        // Modes
        if (parsed.modes.length > 0) {
            const modeChildren = parsed.modes.map((m) => new GrammarTreeItem(m.name, vscode.TreeItemCollapsibleState.None, {
                icon: "layers",
                line: m.line,
                uri: doc.uri,
            }));
            rootItems.push(new GrammarTreeItem(`Lexer Modes (${modeChildren.length})`, vscode.TreeItemCollapsibleState.Collapsed, {
                icon: "layers",
                children: modeChildren,
            }));
        }
        // Data Types (ADTs)
        const dataChildren = parsed.dataTypes.map((d) => {
            const ctorItems = d.ctors.map((c) => new GrammarTreeItem(c, vscode.TreeItemCollapsibleState.None, {
                icon: "symbol-enum-member",
                line: d.line,
                uri: doc.uri,
            }));
            return new GrammarTreeItem(d.name, ctorItems.length ? vscode.TreeItemCollapsibleState.Collapsed : vscode.TreeItemCollapsibleState.None, {
                description: d.kind,
                icon: "symbol-enum",
                line: d.line,
                uri: doc.uri,
                children: ctorItems,
            });
        });
        rootItems.push(new GrammarTreeItem(`Data Types (${dataChildren.length})`, vscode.TreeItemCollapsibleState.Expanded, {
            icon: "symbol-struct",
            children: dataChildren,
        }));
        return rootItems;
    }
}
function buildPsemExplorerItems(doc) {
    const psem = parseLocalPsemStructure(doc.getText());
    const rootItems = [];
    const pragmaChildren = psem.pragmas.map((p) => new GrammarTreeItem(`{-# ${p.text} #-}`, vscode.TreeItemCollapsibleState.None, {
        icon: "symbol-property",
        line: p.line,
        uri: doc.uri,
    }));
    rootItems.push(new GrammarTreeItem(`Semantics: ${psem.moduleName}`, pragmaChildren.length ? vscode.TreeItemCollapsibleState.Expanded : vscode.TreeItemCollapsibleState.None, {
        description: psem.imports.length ? `imports: ${psem.imports.join(", ")}` : "",
        icon: "symbol-namespace",
        children: pragmaChildren,
    }));
    if (psem.types.length > 0) {
        rootItems.push(new GrammarTreeItem(`Types & Builtins (${psem.types.length})`, vscode.TreeItemCollapsibleState.Collapsed, {
            icon: "symbol-struct",
            children: psem.types.map((t) => new GrammarTreeItem(`${t.kind} ${t.name}`, vscode.TreeItemCollapsibleState.None, {
                description: t.detail,
                icon: "symbol-type-parameter",
                line: t.line,
                uri: doc.uri,
            })),
        }));
    }
    if (psem.diags.length > 0) {
        rootItems.push(new GrammarTreeItem(`Diagnostics (${psem.diags.length})`, vscode.TreeItemCollapsibleState.Collapsed, {
            icon: "warning",
            children: psem.diags.map((d) => new GrammarTreeItem(d.code, vscode.TreeItemCollapsibleState.None, {
                description: `${d.severity}: ${d.message}`,
                icon: d.severity === "Error" ? "error" : "warning",
                line: d.line,
                uri: doc.uri,
            })),
        }));
    }
    rootItems.push(new GrammarTreeItem(`Node Rules (${psem.nodes.length})`, vscode.TreeItemCollapsibleState.Expanded, {
        icon: "symbol-method",
        children: psem.nodes.map((n) => new GrammarTreeItem(n.pattern, vscode.TreeItemCollapsibleState.None, {
            icon: "symbol-function",
            line: n.line,
            uri: doc.uri,
        })),
    }));
    if (psem.flows.length > 0 || psem.analyses.length > 0) {
        const flowItems = [
            ...psem.flows.map((f) => new GrammarTreeItem(`flow ${f.ctor}`, vscode.TreeItemCollapsibleState.None, {
                icon: "git-branch",
                line: f.line,
                uri: doc.uri,
            })),
            ...psem.analyses.map((a) => new GrammarTreeItem(`analysis ${a.name}`, vscode.TreeItemCollapsibleState.None, {
                description: `:: ${a.signature}`,
                icon: "pulse",
                line: a.line,
                uri: doc.uri,
            })),
        ];
        rootItems.push(new GrammarTreeItem(`Flow & Analysis (${flowItems.length})`, vscode.TreeItemCollapsibleState.Expanded, {
            icon: "graph",
            children: flowItems,
        }));
    }
    rootItems.push(new GrammarTreeItem(`Lint Rules (${psem.lints.length})`, vscode.TreeItemCollapsibleState.Expanded, {
        icon: "shield",
        children: psem.lints.map((l) => new GrammarTreeItem(l.name, vscode.TreeItemCollapsibleState.None, {
            description: `${l.severity}${l.status ? ` [${l.status}]` : ""}`,
            icon: "checklist",
            line: l.line,
            uri: doc.uri,
        })),
    }));
    if (psem.profiles.length > 0) {
        rootItems.push(new GrammarTreeItem(`Profiles (${psem.profiles.length})`, vscode.TreeItemCollapsibleState.Collapsed, {
            icon: "layers",
            children: psem.profiles.map((p) => new GrammarTreeItem(p.name, vscode.TreeItemCollapsibleState.None, {
                description: p.rules,
                icon: "list-selection",
                line: p.line,
                uri: doc.uri,
            })),
        }));
    }
    return rootItems;
}
class ExampleTreeItem extends vscode.TreeItem {
    example;
    uri;
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
    _onDidChangeTreeData = new vscode.EventEmitter();
    onDidChangeTreeData = this._onDidChangeTreeData.event;
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
        if (doc.languageId === "parsley-psem") {
            const psem = parseLocalPsemStructure(doc.getText());
            const items = [];
            for (const ex of psem.examples) {
                for (const f of ex.fires) {
                    items.push(new ExampleTreeItem({ name: `${ex.lintName} (fires)`, input: f, kind: "fails-at", expected: ex.lintName, line: ex.line }, doc.uri));
                }
                for (const s of ex.silent) {
                    items.push(new ExampleTreeItem({ name: `${ex.lintName} (silent)`, input: s, kind: "parses-to", expected: "clean", line: ex.line }, doc.uri));
                }
            }
            return items;
        }
        const parsed = parseLocalGrammarStructure(doc.getText());
        return parsed.examples.map((ex) => new ExampleTreeItem(ex, doc.uri));
    }
}
async function fetchRuleHoverDetails(client, doc, line, ruleName) {
    const lineText = doc.lineAt(line).text;
    const charIdx = Math.max(0, lineText.indexOf(ruleName));
    const hover = await client.sendRequest("textDocument/hover", { textDocument: { uri: doc.uri.toString() }, position: { line, character: charIdx } }, 1500);
    if (!hover || !hover.contents) {
        return {};
    }
    const md = typeof hover.contents === "string" ? hover.contents : hover.contents.value || "";
    const typeMatch = md.match(/::\s*([^\n`]+)/);
    const stratMatch = md.match(/\*\*(?:Strategy|Analysis mode)\*\*:\s*`([^`]+)`/);
    const nullMatch = md.match(/\*\*Nullable\*\*:\s*`([^`]+)`/);
    const firstMatch = md.match(/\*\*FIRST\*\*:\s*`?(\{[^}]*\})`?/);
    const followMatch = md.match(/\*\*FOLLOW\*\*:\s*`?(\{[^}]*\})`?/);
    return {
        typeSignature: typeMatch?.[1]?.trim(),
        strategy: stratMatch?.[1]?.trim(),
        nullable: nullMatch?.[1]?.trim(),
        first: firstMatch?.[1]?.replace(/`/g, "").trim(),
        follow: followMatch?.[1]?.replace(/`/g, "").trim(),
    };
}
function coerceUriArg(uriArg) {
    if (uriArg instanceof vscode.Uri) {
        return uriArg;
    }
    if (uriArg && typeof uriArg === "object" && "uri" in uriArg)
        return coerceUriArg(uriArg.uri);
    if (typeof uriArg === "string" && uriArg.length > 0) {
        return uriArg.includes("://") ? vscode.Uri.parse(uriArg) : vscode.Uri.file(uriArg);
    }
    return undefined;
}
function getActiveParsleyDocument() {
    const active = vscode.window.activeTextEditor?.document;
    if (isParsleyDoc(active)) {
        return active;
    }
    return vscode.workspace.textDocuments.find((d) => isParsleyDoc(d));
}
// ---------------------------------------------------------------------------
// Live Grammar Playground Webview
// ---------------------------------------------------------------------------
class PlaygroundPanelManager {
    panel;
    targetUri;
    currentInput = "1 + 2 * 3";
    client;
    debounceTimer;
    constructor(client) {
        this.client = client;
    }
    async open(uri, initialInput) {
        const target = uri ?? getActiveParsleyDocument()?.uri;
        const doc = target ? await vscode.workspace.openTextDocument(resolveSiblingGrammarUri(target)) : undefined;
        if (!doc) {
            vscode.window.showWarningMessage("Open a .parsley file to use the Parsley Live Playground.");
            return;
        }
        this.targetUri = doc.uri;
        const parsed = parseLocalGrammarStructure(doc.getText());
        if (initialInput !== undefined) {
            this.currentInput = initialInput;
        }
        else if (parsed.examples.length > 0 && this.currentInput === "1 + 2 * 3") {
            this.currentInput = parsed.examples[0].input;
        }
        if (this.panel) {
            this.panel.reveal(vscode.ViewColumn.Beside);
        }
        else {
            this.panel = vscode.window.createWebviewPanel("parsleyPlayground", `Parsley Playground: ${path.basename(doc.fileName)}`, vscode.ViewColumn.Beside, { enableScripts: true, retainContextWhenHidden: true });
            this.panel.onDidDispose(() => {
                this.panel = undefined;
            });
            this.panel.webview.onDidReceiveMessage(async (msg) => {
                if (msg.type === "eval") {
                    this.currentInput = String(msg.input ?? "");
                    await this.evaluateAndPost();
                }
                else if (msg.type === "debug") {
                    if (this.targetUri) {
                        await vscode.commands.executeCommand("parsley.debugExample", {
                            uri: this.targetUri,
                            input: String(msg.input ?? this.currentInput),
                        });
                    }
                }
                else if (msg.type === "railroad") {
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
            this.panel?.webview.postMessage({
                type: "examples",
                examples: parsed.examples,
                moduleName: parsed.moduleName,
            });
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
                        message: "Language server did not return a response for `parsley/evalInput`. Verify `parsley.executablePath` points to a Parsley build with LSP playground support.",
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
        const examplesJson = JSON.stringify(parsed.examples).replace(/</g, "\\u003c").replace(/>/g, "\\u003e");
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
    .title {
      font-size: 15px;
      font-weight: 600;
    }
    .subtitle {
      font-size: 12px;
      color: var(--vscode-descriptionForeground);
    }
    .actions {
      display: flex;
      gap: 8px;
      align-items: center;
    }
    button {
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
      border: none;
      padding: 5px 10px;
      border-radius: 3px;
      cursor: pointer;
      font-size: 12px;
    }
    button:hover {
      background: var(--vscode-button-hoverBackground);
    }
    button.secondary {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
    }
    button.secondary:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }
    .badge {
      padding: 3px 8px;
      border-radius: 10px;
      font-size: 11px;
      font-weight: 600;
    }
    .badge.ok {
      background: rgba(34, 197, 94, 0.2);
      color: #4ade80;
    }
    .badge.err {
      background: rgba(239, 68, 68, 0.2);
      color: #f87171;
    }
    .examples-bar {
      display: flex;
      flex-wrap: wrap;
      gap: 6px;
      margin-bottom: 10px;
      align-items: center;
    }
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
    .section-body {
      padding: 10px;
      overflow-x: auto;
    }
    pre {
      margin: 0;
      font-family: var(--vscode-editor-font-family);
      font-size: 12.5px;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .error-box {
      border-left: 3px solid #f87171;
      padding-left: 10px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
    }
    th, td {
      text-align: left;
      padding: 4px 8px;
      border-bottom: 1px solid var(--vscode-panel-border);
      font-family: var(--vscode-editor-font-family);
    }
    th {
      color: var(--vscode-descriptionForeground);
      font-weight: 600;
    }
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
    panel;
    targetUri;
    client;
    lastSvg = "";
    constructor(client) {
        this.client = client;
    }
    async show(uri) {
        const target = uri ?? getActiveParsleyDocument()?.uri;
        const doc = target ? await vscode.workspace.openTextDocument(resolveSiblingGrammarUri(target)) : undefined;
        if (!doc) {
            vscode.window.showWarningMessage("Open a .parsley file to display its Railroad diagram.");
            return;
        }
        this.targetUri = doc.uri;
        if (this.panel) {
            this.panel.reveal(vscode.ViewColumn.Beside);
        }
        else {
            this.panel = vscode.window.createWebviewPanel("parsleyRailroad", `Railroad: ${path.basename(doc.fileName)}`, vscode.ViewColumn.Beside, { enableScripts: true, retainContextWhenHidden: true });
            this.panel.onDidDispose(() => {
                this.panel = undefined;
            });
            this.panel.webview.onDidReceiveMessage(async (msg) => {
                if (msg.type === "exportSvg" && this.lastSvg) {
                    const saveUri = await vscode.window.showSaveDialog({
                        defaultUri: vscode.Uri.file(path.join(path.dirname(this.targetUri?.fsPath || "."), `${path.basename(this.targetUri?.fsPath || "grammar", ".parsley")}.railroad.svg`)),
                        filters: { "SVG Images": ["svg"] },
                    });
                    if (saveUri) {
                        fs.writeFileSync(saveUri.fsPath, this.lastSvg, "utf8");
                        vscode.window.showInformationMessage(`Saved Railroad SVG to ${saveUri.fsPath}`);
                    }
                }
                else if (msg.type === "refresh" && this.targetUri) {
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
        if (lspRes?.svg) {
            svg = lspRes.svg;
        }
        else if (uri.scheme === "file") {
            const cli = await runCliCommand(["graph", uri.fsPath, "--format", "svg"], getWorkspaceRoot(uri), uri);
            if (cli.code === 0 && cli.stdout.includes("<svg")) {
                svg = cli.stdout;
            }
            else {
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
      margin: 0;
      padding: 0;
      width: 100%;
      height: 100%;
      overflow: hidden;
      background: var(--vscode-editor-background);
      color: var(--vscode-editor-foreground);
      font-family: var(--vscode-font-family);
      display: flex;
      flex-direction: column;
    }
    .toolbar {
      display: flex;
      gap: 8px;
      padding: 8px 12px;
      border-bottom: 1px solid var(--vscode-panel-border);
      align-items: center;
      z-index: 2;
    }
    button {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
      border: none;
      padding: 4px 10px;
      border-radius: 3px;
      cursor: pointer;
      font-size: 12px;
    }
    button:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }
    .zoom-lbl {
      font-size: 12px;
      color: var(--vscode-descriptionForeground);
      margin-left: auto;
    }
    #viewport {
      flex: 1;
      overflow: hidden;
      cursor: grab;
      position: relative;
      background: #f8fafc;
    }
    #viewport:active { cursor: grabbing; }
    #stage {
      transform-origin: 0 0;
      position: absolute;
      top: 0;
      left: 0;
      padding: 16px;
    }
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
  <div id="viewport">
    <div id="stage">${svg}</div>
  </div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const viewport = document.getElementById('viewport');
    const stage = document.getElementById('stage');
    const zoomLbl = document.getElementById('zoomLbl');
    let scale = 1;
    let tx = 16;
    let ty = 16;
    let dragging = false;
    let sx = 0, sy = 0;

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
      const factor = e.deltaY < 0 ? 1.1 : 0.9;
      scale = Math.max(0.2, Math.min(4, scale * factor));
      apply();
    }, { passive: false });

    viewport.addEventListener('mousedown', (e) => {
      dragging = true;
      sx = e.clientX - tx;
      sy = e.clientY - ty;
    });
    window.addEventListener('mousemove', (e) => {
      if (!dragging) return;
      tx = e.clientX - sx;
      ty = e.clientY - sy;
      apply();
    });
    window.addEventListener('mouseup', () => { dragging = false; });
  </script>
</body>
</html>`;
    }
}
class ParsleyInlineDebugAdapter {
    sendEmitter = new vscode.EventEmitter();
    onDidSendMessage = this.sendEmitter.event;
    seq = 1;
    client;
    programPath = "";
    programUri;
    inputText = "";
    startRule = "";
    stopOnEntry = true;
    breakpoints = new Set();
    steps = [];
    stepIdx = 0;
    evalResult = null;
    structure;
    ruleFirstFollow = new Map();
    constructor(client) {
        this.client = client;
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
        const req = msg;
        if (req.type !== "request") {
            return;
        }
        switch (req.command) {
            case "initialize":
                this.sendResponse(req, {
                    supportsConfigurationDoneRequest: true,
                    supportsEvaluateForHovers: true,
                    supportsStepBack: false,
                    supportsRestartRequest: false,
                });
                this.sendEvent("initialized");
                break;
            case "launch":
                void this.handleLaunch(req);
                break;
            case "setBreakpoints": {
                const args = req.arguments || {};
                const bps = args.breakpoints || [];
                this.breakpoints.clear();
                const verified = bps.map((b) => {
                    this.breakpoints.add(b.line);
                    return { verified: true, line: b.line };
                });
                this.sendResponse(req, { breakpoints: verified });
                break;
            }
            case "configurationDone":
                this.sendResponse(req);
                this.advanceInitial();
                break;
            case "threads":
                this.sendResponse(req, { threads: [{ id: 1, name: "Grammar Derivation" }] });
                break;
            case "stackTrace":
                this.handleStackTrace(req);
                break;
            case "scopes":
                this.sendResponse(req, {
                    scopes: [
                        { name: "Parser State", variablesReference: 1001, expensive: false },
                        { name: "Token Stream", variablesReference: 1002, expensive: false },
                        { name: "AST / Output", variablesReference: 1003, expensive: false },
                        { name: "FIRST / FOLLOW", variablesReference: 1004, expensive: false },
                    ],
                });
                break;
            case "variables":
                this.handleVariables(req);
                break;
            case "evaluate":
                void this.handleEvaluate(req);
                break;
            case "continue":
                this.sendResponse(req, { allThreadsContinued: true });
                this.runUntilBreakpointOrEnd();
                break;
            case "next":
            case "stepIn":
                this.sendResponse(req);
                if (this.stepIdx + 1 < this.steps.length) {
                    this.stepIdx++;
                    this.sendEvent("stopped", { reason: "step", threadId: 1, allThreadsStopped: true });
                }
                else {
                    this.finishSession();
                }
                break;
            case "stepOut":
                this.sendResponse(req);
                this.finishSession();
                break;
            case "disconnect":
            case "terminate":
                this.sendResponse(req);
                this.sendEvent("terminated");
                break;
            default:
                this.sendResponse(req);
                break;
        }
    }
    async handleLaunch(req) {
        const args = req.arguments || {};
        this.programPath = String(args.program || "");
        this.inputText = String(args.input ?? "");
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
                    this.structure.rules[0]?.name ||
                    "program";
        }
        this.evalResult = await this.client.sendRequest("parsley/evalInput", {
            uri: this.programUri.toString(),
            input: this.inputText,
            ...(this.startRule ? { startRule: this.startRule } : {}),
        });
        for (const r of this.structure.rules.slice(0, 20)) {
            const info = await fetchRuleHoverDetails(this.client, doc, r.line, r.name);
            this.ruleFirstFollow.set(r.name, info);
        }
        this.buildTraceSteps(doc.getText());
        this.sendResponse(req);
        this.sendEvent("output", {
            category: "console",
            output: `[parsley-debug] Loaded ${path.basename(this.programPath)} (startRule=${this.startRule}), input=${JSON.stringify(this.inputText)}\n`,
        });
    }
    buildTraceSteps(sourceText) {
        const lines = sourceText.split(/\r?\n/);
        const ruleLineMap = new Map();
        for (const r of this.structure?.rules || []) {
            ruleLineMap.set(r.name, r.line + 1);
        }
        const startLine = ruleLineMap.get(this.startRule) || 1;
        const tokens = this.evalResult?.tokens || [];
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
        // Map token steps through matching rules in the grammar
        for (let i = 0; i < tokens.length; i++) {
            const tok = tokens[i];
            let matchedRule = this.startRule;
            let matchedLine = startLine;
            for (const r of this.structure?.rules || []) {
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
        if (this.evalResult?.ok) {
            this.steps.push({
                ruleName: this.startRule,
                line: startLine,
                column: 1,
                note: `Accept AST: ${this.evalResult.formattedAst || "(ok)"}`,
                tokenIndex: Math.max(0, tokens.length - 1),
                cursorLine: tokens.length ? tokens[tokens.length - 1].line : 1,
                cursorCol: tokens.length ? tokens[tokens.length - 1].col : 1,
            });
        }
        else if (this.evalResult?.error) {
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
        const summary = this.evalResult?.ok
            ? `Parsed OK -> ${this.evalResult.formattedAst}`
            : `Parse Error -> ${this.evalResult?.error?.message || "failed"}`;
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
        const ref = Number(req.arguments?.variablesReference || 0);
        const step = this.steps[this.stepIdx] || this.steps[0];
        const tokens = this.evalResult?.tokens || [];
        const curTok = tokens[step?.tokenIndex ?? 0];
        if (ref === 1001) {
            this.sendResponse(req, {
                variables: [
                    { name: "startRule", value: this.startRule, type: "Rule", variablesReference: 0 },
                    { name: "activeRule", value: step?.ruleName || this.startRule, type: "Rule", variablesReference: 0 },
                    { name: "input", value: JSON.stringify(this.inputText), type: "Text", variablesReference: 0 },
                    { name: "cursorLine", value: String(step?.cursorLine ?? 1), type: "Int", variablesReference: 0 },
                    { name: "cursorCol", value: String(step?.cursorCol ?? 1), type: "Int", variablesReference: 0 },
                    {
                        name: "currentToken",
                        value: curTok ? `${curTok.id} (${JSON.stringify(curTok.lexeme)})` : "<eof>",
                        type: "Token",
                        variablesReference: 0,
                    },
                    { name: "status", value: this.evalResult?.ok ? "OK" : "Error", type: "Status", variablesReference: 0 },
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
            if (this.evalResult?.ok) {
                vars.push({
                    name: "formattedAst",
                    value: this.evalResult.formattedAst || "",
                    type: "AST",
                    variablesReference: 0,
                });
                if (this.evalResult.astJson !== undefined) {
                    vars.push({
                        name: "astJson",
                        value: typeof this.evalResult.astJson === "string"
                            ? this.evalResult.astJson
                            : JSON.stringify(this.evalResult.astJson),
                        type: "JSON",
                        variablesReference: 0,
                    });
                }
            }
            else {
                const e = this.evalResult?.error;
                vars.push({ name: "errorLine", value: String(e?.line ?? 1), type: "Int", variablesReference: 0 });
                vars.push({ name: "errorCol", value: String(e?.col ?? 1), type: "Int", variablesReference: 0 });
                vars.push({ name: "expected", value: e?.expected || "", type: "Text", variablesReference: 0 });
                vars.push({ name: "found", value: e?.found || "", type: "Text", variablesReference: 0 });
                vars.push({ name: "message", value: e?.message || "Parse failed", type: "Text", variablesReference: 0 });
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
        const expr = String(req.arguments?.expression || "").trim();
        if (!this.programUri) {
            this.sendResponse(req, { result: "No active grammar.", variablesReference: 0 });
            return;
        }
        const sample = expr.startsWith(":") ? expr.slice(1).trim() : expr;
        if (expr === ":tokens") {
            const list = (this.evalResult?.tokens || [])
                .map((t, i) => `[${i}] ${t.id} ${JSON.stringify(t.lexeme)} (${t.line}:${t.col})`)
                .join("\n");
            this.sendResponse(req, { result: list || "(no tokens)", variablesReference: 0 });
            return;
        }
        const res = await this.client.sendRequest("parsley/evalInput", {
            uri: this.programUri.toString(),
            input: sample,
        });
        if (!res) {
            this.sendResponse(req, { result: "LSP did not return an evaluation result.", variablesReference: 0 });
            return;
        }
        const out = res.ok
            ? `OK: ${res.formattedAst}`
            : `Error (${res.error?.line ?? 1}:${res.error?.col ?? 1}): ${res.error?.message || "parse error"}`;
        this.sendResponse(req, { result: out, variablesReference: 0 });
    }
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
        treeViews.explorer?.refresh();
        treeViews.examples?.refresh();
    });
    const explorerProvider = new GrammarExplorerProvider(client);
    const examplesProvider = new ExamplesViewProvider();
    treeViews.explorer = explorerProvider;
    treeViews.examples = examplesProvider;
    const playground = new PlaygroundPanelManager(client);
    const railroad = new RailroadPanelManager(client);
    const selector = [
        { language: "parsley", scheme: "file" },
        { language: "parsley-psem", scheme: "file" },
    ];
    const legend = client.getSemanticLegend();
    void vscode.workspace.findFiles("**/*.{parsley,psem}", "**/build/**", 1).then((found) => {
        void vscode.commands.executeCommand("setContext", "workspaceHasParsleyFiles", found.length > 0);
    });
    context.subscriptions.push(cliOutput, lspTraceOutput, diagnostics, statusBar, client, vscode.window.registerTreeDataProvider("parsleyGrammarExplorer", explorerProvider), vscode.window.registerTreeDataProvider("parsleyExamplesView", examplesProvider),
    // Document Sync
    vscode.workspace.onDidOpenTextDocument((doc) => {
        if (isParsleyDoc(doc)) {
            client.didOpen(doc);
            explorerProvider.refresh();
            examplesProvider.refresh();
            void vscode.commands.executeCommand("setContext", "workspaceHasParsleyFiles", true);
        }
    }), vscode.workspace.onDidChangeTextDocument((e) => {
        if (isParsleyDoc(e.document)) {
            client.didChange(e.document);
            playground.notifyDocumentChanged(e.document);
            explorerProvider.refresh();
            examplesProvider.refresh();
        }
    }), vscode.workspace.onDidSaveTextDocument((doc) => {
        if (isParsleyDoc(doc)) {
            void railroad.refreshIfOpen(doc.uri);
        }
    }), vscode.workspace.onDidCloseTextDocument((doc) => {
        if (isParsleyDoc(doc)) {
            client.didClose(doc);
        }
    }), vscode.window.onDidChangeActiveTextEditor((ed) => {
        if (isParsleyDoc(ed?.document)) {
            explorerProvider.refresh();
            examplesProvider.refresh();
        }
    }), vscode.workspace.onWillSaveTextDocument((e) => {
        if (isParsleyDoc(e.document) && getConfig().get("autoFormatOnSave")) {
            e.waitUntil(vscode.commands.executeCommand("vscode.executeFormatDocumentProvider", e.document.uri, { tabSize: 2, insertSpaces: true }).then((edits) => edits || []));
        }
    }),
    // Hover
    vscode.languages.registerHoverProvider(selector, {
        async provideHover(doc, pos) {
            const res = await client.sendRequest("textDocument/hover", { textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } });
            if (!res || !res.contents) {
                return undefined;
            }
            const md = typeof res.contents === "string"
                ? new vscode.MarkdownString(res.contents)
                : new vscode.MarkdownString(res.contents.value || "");
            return new vscode.Hover(md, res.range ? toVscodeRange(res.range) : undefined);
        },
    }),
    // Definition
    vscode.languages.registerDefinitionProvider(selector, {
        async provideDefinition(doc, pos) {
            const res = await client.sendRequest("textDocument/definition", { textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } });
            if (!res) {
                return undefined;
            }
            const arr = Array.isArray(res) ? res : [res];
            return arr.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), toVscodeRange(l.range)));
        },
    }),
    // References
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
    // Document Symbols (Outline & Breadcrumbs)
    vscode.languages.registerDocumentSymbolProvider(selector, {
        async provideDocumentSymbols(doc) {
            const res = await client.sendRequest("textDocument/documentSymbol", { textDocument: { uri: doc.uri.toString() } });
            if (Array.isArray(res) && res.length > 0) {
                const convert = (s) => {
                    const sym = new vscode.DocumentSymbol(s.name, s.detail || "", toVscodeSymbolKind(s.kind), toVscodeRange(s.range), toVscodeRange(s.selectionRange || s.range));
                    if (Array.isArray(s.children)) {
                        sym.children = s.children.map(convert);
                    }
                    return sym;
                };
                return res.map(convert);
            }
            // Fallback local outline
            return buildLocalDocumentSymbols(doc);
        },
    }),
    // Semantic Tokens
    vscode.languages.registerDocumentSemanticTokensProvider(selector, {
        async provideDocumentSemanticTokens(doc) {
            const res = await client.sendRequest("textDocument/semanticTokens/full", { textDocument: { uri: doc.uri.toString() } });
            if (!res || !Array.isArray(res.data)) {
                return undefined;
            }
            return new vscode.SemanticTokens(new Uint32Array(res.data));
        },
    }, legend),
    // Formatting (Ormolu style)
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
    // Completion
    vscode.languages.registerCompletionItemProvider(selector, {
        async provideCompletionItems(doc, pos) {
            const res = await client.sendRequest("textDocument/completion", { textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } });
            const rawItems = Array.isArray(res) ? res : res?.items;
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
    }, ".", ":", "<", "`"),
    // Code Actions (Quick Fixes + Explain Error)
    vscode.languages.registerCodeActionsProvider(selector, {
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
                    const kind = item.kind === "refactor"
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
                                if (dc.textDocument?.uri && Array.isArray(dc.edits)) {
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
            // Always offer Explain Error for any diagnostic code on range
            const seenCodes = new Set();
            for (const diag of ctx.diagnostics) {
                const code = diag.code ? String(diag.code) : "";
                if (code && !seenCodes.has(code)) {
                    seenCodes.add(code);
                    const explainAction = new vscode.CodeAction(`Parsley: Explain Error ${code}`, vscode.CodeActionKind.QuickFix);
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
    // CodeLens (Run Examples, Debug, Railroad, Build C23)
    vscode.languages.registerCodeLensProvider(selector, {
        async provideCodeLenses(doc) {
            const res = await client.sendRequest("textDocument/codeLens", { textDocument: { uri: doc.uri.toString() } });
            const lenses = [];
            if (Array.isArray(res) && res.length > 0) {
                for (const cl of res) {
                    if (cl.command) {
                        lenses.push(new vscode.CodeLens(toVscodeRange(cl.range), {
                            title: cl.command.title,
                            command: cl.command.command,
                            arguments: cl.command.arguments,
                        }));
                    }
                }
                return lenses;
            }
            // Fallback CodeLenses from local structure
            const parsed = parseLocalGrammarStructure(doc.getText());
            for (let i = 0; i < doc.lineCount; i++) {
                if (/^\s*module\b/.test(doc.lineAt(i).text)) {
                    const r = new vscode.Range(i, 0, i, 6);
                    lenses.push(new vscode.CodeLens(r, { title: "$(play) Playground", command: "parsley.openPlayground", arguments: [doc.uri] }));
                    lenses.push(new vscode.CodeLens(r, { title: "$(type-hierarchy) Railroad", command: "parsley.showRailroad", arguments: [doc.uri] }));
                    lenses.push(new vscode.CodeLens(r, { title: "$(tools) Build C23", command: "parsley.buildC23", arguments: [doc.uri] }));
                    break;
                }
            }
            for (const ex of parsed.examples) {
                const r = new vscode.Range(ex.line, 0, ex.line, 7);
                lenses.push(new vscode.CodeLens(r, {
                    title: `$(play) Run "${ex.name}"`,
                    command: "parsley.runSingleExample",
                    arguments: [doc.uri, ex],
                }));
                lenses.push(new vscode.CodeLens(r, {
                    title: "$(debug-alt) Debug",
                    command: "parsley.debugExample",
                    arguments: [{ uri: doc.uri, input: ex.input }],
                }));
            }
            return lenses;
        },
    }),
    // Inline Grammar Debugger
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
                    config.input = parsed.examples[0]?.input ?? "1 + 2 * 3";
                    config.stopOnEntry = true;
                }
            }
            if (!config.program) {
                return vscode.window.showInformationMessage("Open a .parsley file to debug.").then(() => undefined);
            }
            return config;
        },
    }), vscode.debug.registerDebugAdapterDescriptorFactory("parsley-debug", {
        createDebugAdapterDescriptor() {
            return new vscode.DebugAdapterInlineImplementation(new ParsleyInlineDebugAdapter(client));
        },
    }),
    // Commands
    vscode.commands.registerCommand("parsley.check", async (uriArg) => {
        const uri = coerceUriArg(uriArg) ?? getActiveParsleyDocument()?.uri;
        if (!uri) {
            vscode.window.showWarningMessage("Open a .parsley file to check.");
            return;
        }
        await saveIfDirty(uri);
        cliOutput.show(true);
        cliOutput.appendLine(`> parsley check ${uri.fsPath}`);
        const res = await runCliCommand(["check", uri.fsPath], getWorkspaceRoot(uri), uri);
        if (res.stdout)
            cliOutput.append(res.stdout);
        if (res.stderr)
            cliOutput.append(res.stderr);
        if (res.code === 0) {
            vscode.window.showInformationMessage(res.stdout.trim() || "Parsley grammar check passed.");
        }
        else {
            vscode.window.showErrorMessage(`Parsley check failed (exit ${res.code}). See Output > Parsley.`);
        }
    }), vscode.commands.registerCommand("parsley.test", async (uriArg) => {
        const uri = coerceUriArg(uriArg) ?? getActiveParsleyDocument()?.uri;
        if (!uri) {
            vscode.window.showWarningMessage("Open a .parsley file to run tests.");
            return;
        }
        await saveIfDirty(uri);
        const fuzz = getConfig().get("fuzzCount") ?? 50;
        const args = ["test", uri.fsPath];
        if (fuzz > 0) {
            args.push("--fuzz", String(fuzz));
        }
        cliOutput.show(true);
        cliOutput.appendLine(`> parsley ${args.join(" ")}`);
        const res = await runCliCommand(args, getWorkspaceRoot(uri), uri);
        if (res.stdout)
            cliOutput.append(res.stdout);
        if (res.stderr)
            cliOutput.append(res.stderr);
        if (res.code === 0) {
            vscode.window.showInformationMessage(`Parsley tests passed (${path.basename(uri.fsPath)}).`);
        }
        else {
            vscode.window.showErrorMessage(`Parsley tests failed. See Output > Parsley.`);
        }
    }), vscode.commands.registerCommand("parsley.buildC23", async (uriArg) => {
        const rawUri = coerceUriArg(uriArg) ?? getActiveParsleyDocument()?.uri;
        if (!rawUri) {
            vscode.window.showWarningMessage("Open a .parsley or .psem file to build C23 parser.");
            return;
        }
        await saveIfDirty(rawUri);
        const uri = resolveSiblingGrammarUri(rawUri);
        await saveIfDirty(uri);
        const configuredOut = getConfig().get("c23.outputDir") || "./generated_parser";
        const root = getWorkspaceRoot(uri);
        const outDir = path.isAbsolute(configuredOut) ? configuredOut : path.resolve(root, configuredOut);
        cliOutput.show(true);
        cliOutput.appendLine(`> parsley build ${uri.fsPath} -o ${outDir} --emit-ir-json`);
        const res = await runCliCommand(["build", uri.fsPath, "-o", outDir, "--emit-ir-json"], root, uri);
        if (res.stdout)
            cliOutput.append(res.stdout);
        if (res.stderr)
            cliOutput.append(res.stderr);
        if (res.code === 0) {
            const choice = await vscode.window.showInformationMessage(`ISO C23 parser built in ${outDir}`, "Reveal Folder");
            if (choice === "Reveal Folder") {
                void vscode.commands.executeCommand("revealFileInOS", vscode.Uri.file(outDir));
            }
        }
        else {
            vscode.window.showErrorMessage("Parsley C23 build failed. See Output > Parsley.");
        }
    }), vscode.commands.registerCommand("parsley.format", async (uriArg) => {
        const target = coerceUriArg(uriArg);
        const ed = target ? await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(target)) : vscode.window.activeTextEditor;
        if (!ed || !isParsleyDoc(ed.document)) {
            vscode.window.showWarningMessage("Open a .parsley or .psem file to format.");
            return;
        }
        const edits = await vscode.commands.executeCommand("vscode.executeFormatDocumentProvider", ed.document.uri, { tabSize: 2, insertSpaces: true });
        if (edits && edits.length > 0) {
            const wsEdit = new vscode.WorkspaceEdit();
            wsEdit.set(ed.document.uri, edits);
            await vscode.workspace.applyEdit(wsEdit);
            return;
        }
        await saveIfDirty(ed.document.uri);
        const cli = await runCliCommand(["fmt", ed.document.fileName], getWorkspaceRoot(ed.document.uri), ed.document.uri);
        if (cli.code === 0 && cli.stdout) {
            const fullRange = new vscode.Range(ed.document.positionAt(0), ed.document.positionAt(ed.document.getText().length));
            const wsEdit = new vscode.WorkspaceEdit();
            wsEdit.replace(ed.document.uri, fullRange, cli.stdout);
            await vscode.workspace.applyEdit(wsEdit);
        }
        else {
            vscode.window.showErrorMessage(cli.stderr || "Formatting failed.");
        }
    }), vscode.commands.registerCommand("parsley.showRailroad", async (uriArg) => {
        await railroad.show(coerceUriArg(uriArg));
    }), vscode.commands.registerCommand("parsley.openPlayground", async (uriArg) => {
        await playground.open(coerceUriArg(uriArg));
    }), vscode.commands.registerCommand("parsley.explainError", async (codeArg) => {
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
    }), vscode.commands.registerCommand("parsley.debugExample", async (arg, _exName, exInput) => {
        let uri;
        let input;
        if (arg instanceof ExampleTreeItem) {
            uri = arg.uri;
            input = arg.example.input;
        }
        else if (typeof arg === "string") {
            uri = coerceUriArg(arg);
            if (typeof exInput === "string") {
                input = exInput;
            }
        }
        else if (arg instanceof vscode.Uri) {
            uri = arg;
        }
        else if (arg && typeof arg === "object") {
            const obj = arg;
            uri = coerceUriArg(obj.uri);
            if (typeof obj.input === "string")
                input = obj.input;
        }
        const target = uri ?? getActiveParsleyDocument()?.uri;
        const doc = target ? await vscode.workspace.openTextDocument(resolveSiblingGrammarUri(target)) : undefined;
        if (!doc) {
            vscode.window.showWarningMessage("Open a .parsley file to debug.");
            return;
        }
        const parsed = parseLocalGrammarStructure(doc.getText());
        if (input === undefined) {
            input = await vscode.window.showInputBox({
                prompt: "Enter sample input to trace with the Parsley Grammar Debugger",
                value: parsed.examples[0]?.input ?? "1 + 2 * 3",
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
            startRule: parsed.exports[0] || parsed.rules[0]?.name || "",
            stopOnEntry: true,
        });
    }), vscode.commands.registerCommand("parsley.runSingleExample", async (arg1, arg2) => {
        const uri = arg1 instanceof ExampleTreeItem ? arg1.uri : coerceUriArg(arg1);
        if (!uri)
            return;
        const doc = await vscode.workspace.openTextDocument(uri);
        const ex = arg1 instanceof ExampleTreeItem ? arg1.example : typeof arg2 === "string"
            ? parseLocalGrammarStructure(doc.getText()).examples.find(item => item.name === arg2)
            : arg2;
        if (!ex) {
            vscode.window.showWarningMessage("Select a grammar or PSL example.");
            return;
        }
        client.didOpen(doc);
        client.didChange(doc);
        const name = doc.languageId === "parsley-psem" ? ex.name.replace(/ \((fires|silent)\)$/, "") : ex.name;
        const res = await client.sendRequest("parsley/runExample", { uri: uri.toString(), name });
        cliOutput.show(true);
        if (!res) {
            await vscode.commands.executeCommand("parsley.test", uri);
            return;
        }
        const message = `[${res.passed ? "PASS" : "FAIL"}] ${name}: ${res.message}`;
        cliOutput.appendLine(message);
        if (res.passed)
            vscode.window.showInformationMessage(message);
        else
            vscode.window.showErrorMessage(message);
    }), vscode.commands.registerCommand("parsley.restartLsp", async () => {
        await client.restart();
        vscode.window.showInformationMessage("Parsley Language Server restarted.");
    }), vscode.commands.registerCommand("parsley.refreshViews", () => {
        explorerProvider.refresh();
        examplesProvider.refresh();
    }), vscode.commands.registerCommand("parsley.revealLine", async (uri, line) => {
        const doc = await vscode.workspace.openTextDocument(uri);
        const ed = await vscode.window.showTextDocument(doc);
        const pos = new vscode.Position(Math.max(0, line), 0);
        ed.selection = new vscode.Selection(pos, pos);
        ed.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
    }));
    // Start LSP immediately if a .parsley file is already open
    const openParsley = getActiveParsleyDocument();
    if (openParsley) {
        client.didOpen(openParsley);
    }
}
function deactivate() {
    // Disposables registered on context handle shutdown cleanly.
}
// ---------------------------------------------------------------------------
// Utility Helpers
// ---------------------------------------------------------------------------
async function saveIfDirty(uri) {
    const stem = uri.fsPath.replace(/\.(parsley|psem)$/, "");
    for (const doc of vscode.workspace.textDocuments) {
        if ((doc.uri.toString() === uri.toString() || (doc.uri.scheme === "file" && doc.uri.fsPath.replace(/\.(parsley|psem)$/, "") === stem)) && doc.isDirty) {
            if (!await doc.save())
                throw new Error(`Could not save ${doc.fileName}; command cancelled.`);
        }
    }
}
function escapeHtml(s) {
    return s
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}
function buildLocalDocumentSymbols(doc) {
    if (doc.languageId === "parsley-psem") {
        const psem = parseLocalPsemStructure(doc.getText());
        const symbols = [];
        for (const d of psem.diags) {
            const r = doc.lineAt(d.line).range;
            symbols.push(new vscode.DocumentSymbol(d.code, `${d.severity}: ${d.message}`, vscode.SymbolKind.Constant, r, r));
        }
        for (const n of psem.nodes) {
            const r = doc.lineAt(n.line).range;
            symbols.push(new vscode.DocumentSymbol(`node ${n.pattern}`, "PSL Node Rule", vscode.SymbolKind.Function, r, r));
        }
        for (const f of psem.flows) {
            const r = doc.lineAt(f.line).range;
            symbols.push(new vscode.DocumentSymbol(`flow ${f.ctor}`, "CFG Edge", vscode.SymbolKind.Event, r, r));
        }
        for (const a of psem.analyses) {
            const r = doc.lineAt(a.line).range;
            symbols.push(new vscode.DocumentSymbol(a.name, a.signature, vscode.SymbolKind.Property, r, r));
        }
        for (const l of psem.lints) {
            const r = doc.lineAt(l.line).range;
            symbols.push(new vscode.DocumentSymbol(l.name, l.severity, vscode.SymbolKind.Method, r, r));
        }
        for (const p of psem.profiles) {
            const r = doc.lineAt(p.line).range;
            symbols.push(new vscode.DocumentSymbol(`profile ${p.name}`, p.rules, vscode.SymbolKind.Array, r, r));
        }
        return symbols;
    }
    const parsed = parseLocalGrammarStructure(doc.getText());
    const symbols = [];
    for (const t of parsed.tokens) {
        const r = doc.lineAt(t.line).range;
        symbols.push(new vscode.DocumentSymbol(t.name, `${t.kind}${t.type ? ` :: ${t.type}` : ""}`, vscode.SymbolKind.Variable, r, r));
    }
    for (const d of parsed.dataTypes) {
        const r = doc.lineAt(d.line).range;
        const sym = new vscode.DocumentSymbol(d.name, d.kind, vscode.SymbolKind.Enum, r, r);
        sym.children = d.ctors.map((c) => new vscode.DocumentSymbol(c, d.name, vscode.SymbolKind.EnumMember, r, r));
        symbols.push(sym);
    }
    for (const rule of parsed.rules) {
        const r = doc.lineAt(rule.line).range;
        symbols.push(new vscode.DocumentSymbol(rule.name, rule.signature || "Rule", vscode.SymbolKind.Function, r, r));
    }
    for (const ex of parsed.examples) {
        const r = doc.lineAt(ex.line).range;
        symbols.push(new vscode.DocumentSymbol(`example "${ex.name}"`, ex.kind, vscode.SymbolKind.Event, r, r));
    }
    return symbols;
}
function buildFallbackCompletions(doc) {
    if (doc.languageId === "parsley-psem") {
        const psem = parseLocalPsemStructure(doc.getText());
        const items = [];
        for (const l of psem.lints) {
            const it = new vscode.CompletionItem(l.name, vscode.CompletionItemKind.Method);
            it.detail = `lint :: ${l.severity}`;
            items.push(it);
        }
        for (const d of psem.diags) {
            const it = new vscode.CompletionItem(d.code, vscode.CompletionItemKind.Constant);
            it.detail = `${d.severity}: ${d.message}`;
            items.push(it);
        }
        for (const kw of [
            "semantics", "where", "import", "source", "manual", "rev",
            "type", "tycon", "kind", "builtin", "coerce", "overload",
            "diag", "node", "flow", "analysis", "lint", "example", "profile", "project",
            "match", "when", "say", "fix", "fires", "silent", "gen", "kill",
            "declare", "lookup", "scope", "require", "unify", "fresh", "mono", "generalize", "orFail",
            "typeOf", "arity", "useCount", "constFold", "nameSpan", "reachable", "dominates",
            "before", "after", "notMember", "isPrefixOf", "replace",
            "Forward", "Backward", "Must", "May", "Error", "Warning", "Note",
            "Safe", "chain", "branch", "loop", "jump", "jumpOut", "jumpTo",
        ]) {
            items.push(new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword));
        }
        return items;
    }
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
//# sourceMappingURL=extension.js.map