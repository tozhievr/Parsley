const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
function harness() {
  const commands = new Map(), providers = {}, calls = [], messages = [];
  const disposable = () => ({dispose(){}});
  class Uri { constructor(value){this.fsPath=value;this.scheme='file';} toString(){return 'file:///'+this.fsPath.replace(/\\/g,'/');} static file(p){return new Uri(p);} static parse(p){return new Uri(p.replace(/^file:\/\/\//,''));} }
  class Emitter { constructor(){this.event=()=>disposable();}fire(){}dispose(){} }
  class Range {constructor(...args){this.args=args;}}
  const doc = {uri:Uri.file(path.resolve(__dirname,'../../../tests/fixtures/Calc.parsley')), languageId:'parsley', fileName:'Calc.parsley', version:1,isDirty:false,getText:()=> 'module Calc (program) where\nprogram :: Text\nprogram = "x"\nexample "x" : "x" parses-to "x"\n',save:async()=>true};
  const vscode = {
    Uri,EventEmitter:Emitter,TreeItem:class {},Range,SemanticTokensLegend:class {},
    TreeItemCollapsibleState:{None:0,Collapsed:1,Expanded:2},StatusBarAlignment:{Left:1},
    CodeActionKind:{QuickFix:1,Refactor:2,Source:3},
    window:{createOutputChannel:()=>({append(){},appendLine(){},show(){},dispose(){}}),createStatusBarItem:()=>({show(){},dispose(){}}),registerTreeDataProvider:disposable,onDidChangeActiveTextEditor:disposable,
      showInformationMessage:async m=>{messages.push(m);},showWarningMessage:async m=>{messages.push(m);},showErrorMessage:async m=>{messages.push(m);}},
    workspace:{workspaceFolders:[],textDocuments:[],getWorkspaceFolder:()=>undefined,getConfiguration:()=>({get:(key)=>key==='executablePath'?'parsley':undefined}),findFiles:async()=>[],openTextDocument:async()=>doc,
      onDidOpenTextDocument:disposable,onDidChangeTextDocument:disposable,onDidSaveTextDocument:disposable,onDidCloseTextDocument:disposable,onWillSaveTextDocument:disposable},
    languages:{createDiagnosticCollection:()=>({set(){},delete(){},dispose(){}})},
    commands:{registerCommand:(name,fn)=>{commands.set(name,fn);return disposable();},executeCommand:async(name,...args)=>{calls.push([name,...args]);if(commands.has(name))return commands.get(name)(...args);}},
    debug:{registerDebugConfigurationProvider:disposable,registerDebugAdapterDescriptorFactory:disposable,startDebugging:async(_folder,config)=>calls.push(['debug',config])}
  };
  for(const name of ['registerHoverProvider','registerDefinitionProvider','registerReferenceProvider','registerDocumentSymbolProvider','registerDocumentSemanticTokensProvider','registerDocumentFormattingEditProvider','registerCompletionItemProvider','registerCodeActionsProvider','registerCodeLensProvider']) vscode.languages[name]=(_selector,provider)=>{providers[name]=provider;return disposable();};
  const child = {execFile:(_exe,args,_opts,callback)=>{calls.push(['cli',...args]);callback(null,'OK','');},spawn:()=>{throw new Error('unexpected process spawn');}};
  const module = {exports:{}};
  const source = fs.readFileSync(path.join(__dirname,'../out/extension.js'),'utf8');
  const context = {module,exports:module.exports,require:(name)=>name==='vscode'?vscode:name==='child_process'?child:require(name),process,Buffer,setTimeout,clearTimeout,console};
  vm.runInNewContext(source+'\nmodule.exports.testHooks={ParsleyLspClient,coerceUriArg,parseLocalGrammarStructure};',context);
  const clientClass = module.exports.testHooks.ParsleyLspClient;
  clientClass.prototype.sendRequest = async(method,params)=>{calls.push(['lsp',method,params]);return {passed:false,message:'expected AST differs'};};
  module.exports.activate({subscriptions:[]});
  return {commands,providers,calls,messages,doc,vscode,clientClass,hooks:module.exports.testHooks};
}
test('activation registers every manifest command with real API names',()=>{
  const h=harness();const manifest=require('../package.json');
  for(const command of manifest.contributes.commands)assert.ok(h.commands.has(command.command),command.command);
  assert.ok(h.providers.registerCodeActionsProvider);
});
test('check/test/build dispatch executable argv for URI and tree items',async()=>{
  const h=harness();
  await h.commands.get('parsley.check')({uri:h.doc.uri});await h.commands.get('parsley.test')(h.doc.uri);await h.commands.get('parsley.buildC23')(h.doc.uri);
  for(const command of ['check','test','build'])assert.ok(h.calls.some(c=>c[0]==='cli'&&c[1]===command));
});
test('single example accepts LSP string arguments and validates expected AST',async()=>{
  const h=harness();h.clientClass.prototype.didOpen=()=>{};h.clientClass.prototype.didChange=()=>{};
  await h.commands.get('parsley.runSingleExample')(h.doc.uri.toString(),'x');
  assert.ok(h.calls.some(c=>c[0]==='lsp'&&c[1]==='parsley/runExample'&&c[2].name==='x'));
  assert.ok(h.messages.some(m=>m.includes('[FAIL]')));
});
test('debug command accepts a Uri directly',async()=>{
  const h=harness();h.vscode.window.showInputBox=async()=> 'x';await h.commands.get('parsley.debugExample')(h.doc.uri);
  assert.ok(h.calls.some(c=>c[0]==='debug'&&c[1].program===h.doc.fileName));
});
test('an old process exit cannot clear the replacement LSP process',()=>{
  const h=harness();const client=new h.clientClass({delete(){}},{show(){}},()=>{});const old={},current={};client.proc=current;client.initialized=true;
  client.handleExit(old,0);assert.equal(client.proc,current);assert.equal(client.initialized,true);
});