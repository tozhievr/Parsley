const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
function harness(options={}) {
  const commands = new Map(), providers = {}, calls = [], messages = [], processCalls=[], spawnCalls=[], updates=[], configurationListeners=[], output=[];
  const settings=options.settings||{};
  const disposable = () => ({dispose(){}});
  class Uri { constructor(value){this.fsPath=value;this.scheme='file';} toString(){return 'file:///'+this.fsPath.replace(/\\/g,'/');} static file(p){return new Uri(p);} static parse(p){return new Uri(p.replace(/^file:\/\/\//,''));} }
  class Emitter { constructor(){this.event=()=>disposable();}fire(){}dispose(){} }
  class Range {constructor(...args){this.args=args;}}
  const doc = {uri:Uri.file(options.docPath||path.resolve(__dirname,'../../../tests/fixtures/Calc.parsley')), languageId:'parsley', fileName:'Calc.parsley', version:1,isDirty:false,getText:()=> 'module Calc (program) where\nprogram :: Text\nprogram = "x"\nexample "x" : "x" parses-to "x"\n',save:async()=>true};
  const vscode = {
    Uri,EventEmitter:Emitter,TreeItem:class {},Range,SemanticTokensLegend:class {},ConfigurationTarget:{Workspace:2,WorkspaceFolder:3},
    TreeItemCollapsibleState:{None:0,Collapsed:1,Expanded:2},StatusBarAlignment:{Left:1},
    CodeActionKind:{QuickFix:1,Refactor:2,Source:3},
    window:{createOutputChannel:()=>({append(value){output.push(value);},appendLine(value){output.push(value);},show(){},dispose(){}}),createStatusBarItem:()=>({show(){},dispose(){}}),registerTreeDataProvider:disposable,onDidChangeActiveTextEditor:disposable,
      showQuickPick:async items=>items.find(item=>item.profile===options.profileChoice),showInformationMessage:async m=>{messages.push(m);},showWarningMessage:async m=>{messages.push(m);},showErrorMessage:async m=>{messages.push(m);}},
    workspace:{workspaceFolders:(options.roots||[]).map((value,index)=>({uri:Uri.file(value),name:path.basename(value),index})),textDocuments:[],
      getWorkspaceFolder:(uri)=>vscode.workspace.workspaceFolders.find(folder=>uri && (uri.fsPath===folder.uri.fsPath || uri.fsPath.startsWith(folder.uri.fsPath+path.sep))),
      getConfiguration:(_section,uri)=>{const scoped=options.getSettings?options.getSettings(uri):settings;return {
        get:(key)=>Object.hasOwn(scoped,key)?scoped[key]:require('../package.json').contributes.configuration.properties['parsley.'+key]?.default,
        update:async(key,value,target)=>{scoped[key]=value;updates.push([key,value,target,uri]);}
      };},findFiles:async()=>[],openTextDocument:async()=>doc,
      onDidChangeConfiguration:fn=>{configurationListeners.push(fn);return disposable();},
      onDidOpenTextDocument:disposable,onDidChangeTextDocument:disposable,onDidSaveTextDocument:disposable,onDidCloseTextDocument:disposable,onWillSaveTextDocument:disposable},
    languages:{createDiagnosticCollection:()=>({set(){},delete(){},clear(){calls.push(["clear-diagnostics"]);},dispose(){}})},
    commands:{registerCommand:(name,fn)=>{commands.set(name,fn);return disposable();},executeCommand:async(name,...args)=>{calls.push([name,...args]);if(commands.has(name))return commands.get(name)(...args);}},
    debug:{registerDebugConfigurationProvider:disposable,registerDebugAdapterDescriptorFactory:disposable,startDebugging:async(_folder,config)=>calls.push(['debug',config])}
  };
  for(const name of ['registerHoverProvider','registerDefinitionProvider','registerReferenceProvider','registerDocumentSymbolProvider','registerDocumentSemanticTokensProvider','registerDocumentFormattingEditProvider','registerCompletionItemProvider','registerCodeActionsProvider','registerCodeLensProvider']) vscode.languages[name]=(_selector,provider)=>{providers[name]=provider;return disposable();};
  const child = {execFile:(exe,args,opts,callback)=>{processCalls.push({exe,args,opts});calls.push(['cli',...args]);callback(null,'OK','');},spawn:(exe,args,opts)=>{spawnCalls.push({exe,args,opts});if(options.spawn)return options.spawn(exe,args,opts);throw new Error('unexpected process spawn');}};
  const module = {exports:{}};
  const source = fs.readFileSync(path.join(__dirname,'../out/extension.js'),'utf8');
  const context = {module,exports:module.exports,require:(name)=>name==='vscode'?vscode:name==='child_process'?child:require(name),process,Buffer,setTimeout,clearTimeout,console};
  vm.runInNewContext(source+'\nmodule.exports.testHooks={ParsleyLspClient,ParsleyLspPool,coerceUriArg,parseLocalGrammarStructure,buildArguments,testArguments,processSettings,expandSetting,resolveExecutablePath};',context);
  const clientClass = module.exports.testHooks.ParsleyLspClient;
  clientClass.prototype.sendRequest = async(method,params)=>{calls.push(['lsp',method,params]);return {passed:false,message:'expected AST differs'};};
  module.exports.activate({subscriptions:[]});
  return {commands,providers,calls,messages,doc,vscode,clientClass,processCalls,spawnCalls,updates,output,settings,hooks:module.exports.testHooks,change:key=>configurationListeners.forEach(fn=>fn({affectsConfiguration:name=>name==="parsley"||key===name||key.startsWith(name+".")}))};
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
test('profiles configure native build argv, paths, compiler, environment and process limits',async()=>{
  const root=path.resolve(__dirname,'workspace with spaces'), docPath=path.join(root,'Example.parsley');
  const h=harness({roots:[root],docPath,settings:{
    activeProfile:'native',
    profiles:{native:{outputDir:'${workspaceFolder}/gen/${fileBasenameNoExtension};literal',
      compileCompiler:true,compileShared:false,minStatus:'reviewed',emitIrJson:false,emitAgentDocs:true,
      cCompiler:'tools/cc custom.exe',timeoutMs:4567,environment:{PARSLEY_TEST_CONFIG:'controlled-test-value'}}},
    executablePath:'"${workspaceFolder}/bin/parsley custom.exe"','cli.arguments':['--verbose']
  }});
  await h.commands.get('parsley.buildC23')(h.doc.uri);
  assert.equal(h.processCalls.length,1,h.messages.join('\n'));
  const c=h.processCalls[0];
  assert.equal(c.exe,path.join(root,'bin','parsley custom.exe'));
  assert.equal(c.opts.cwd,root);assert.equal(c.opts.timeout,4567);assert.equal(c.opts.shell,false);
  assert.equal(c.opts.env.PARSLEY_CC,path.join(root,'tools','cc custom.exe'));
  assert.equal(c.opts.env.PARSLEY_TEST_CONFIG,'controlled-test-value');
  for(const flag of ['--emit-nextc','--compile-compiler','--no-compile-shared','--emit-agent-docs'])assert.ok(c.args.includes(flag),flag);
  assert.ok(!c.args.includes('--emit-ir-json'));
  assert.equal(c.args[c.args.indexOf('-o')+1],path.join(root,'gen','Example;literal'));
  assert.equal(c.args[c.args.indexOf('--min-status')+1],'reviewed');
  assert.equal(c.args[0],'--verbose');
});
test('native command forces compiler generation and legacy output/fuzz settings still apply',async()=>{
  const h=harness({settings:{'c23.outputDir':'legacy output',fuzzCount:0}});
  await h.commands.get('parsley.buildCompiler')(h.doc.uri);
  const args=h.processCalls[0].args;
  assert.ok(args.includes('--emit-nextc')&&args.includes('--compile-compiler'));
  assert.ok(args[args.indexOf('-o')+1].endsWith('legacy output'));
  await h.commands.get('parsley.test')(h.doc.uri);
  const test=h.processCalls[1].args;
  assert.equal(test[test.indexOf('--fuzz')+1],'0');
});
test('fuzz seeds preserve all uint64 digits and invalid configuration prevents process execution',async()=>{
  const seed='18446744073709551615';
  const h=harness({settings:{'test.seed':seed,'test.fuzzCount':31}});
  await h.commands.get('parsley.test')(h.doc.uri);
  assert.ok(h.processCalls[0].args.includes(seed));
  h.settings['test.seed']='18446744073709551616';
  await h.commands.get('parsley.test')(h.doc.uri);
  assert.equal(h.processCalls.length,1);
  assert.ok(h.messages.some(message=>message.includes('unsigned 64-bit')));
  h.settings.activeProfile='missing';
  await h.commands.get('parsley.buildC23')(h.doc.uri);
  assert.equal(h.processCalls.length,1);
});
test('multi-root CLI settings are resolved for the requested resource',async()=>{
  const one=path.resolve(__dirname,'folder one'),two=path.resolve(__dirname,'folder two');
  const first={executablePath:'one-tool.exe'},second={executablePath:'two-tool.exe','cli.timeoutMs':9000};
  const h=harness({roots:[one,two],getSettings:uri=>uri&&uri.fsPath.startsWith(two)?second:first});
  await h.commands.get('parsley.check')(h.vscode.Uri.file(path.join(two,'B.parsley')));
  assert.equal(h.processCalls[0].exe,'two-tool.exe');
  assert.equal(h.processCalls[0].opts.cwd,two);
  assert.equal(h.processCalls[0].opts.timeout,9000);
});
test('profile selection saves a folder setting and effective configuration hides environment values',async()=>{
  const root=path.resolve(__dirname,'profile folder');
  const h=harness({roots:[root],docPath:path.join(root,'A.parsley'),profileChoice:'release',
    settings:{profiles:{release:{compileShared:false}},environment:{PARSLEY_TEST_CONFIG:'secret-test-value'}}});
  await h.commands.get('parsley.selectProfile')(h.doc.uri);
  assert.equal(h.updates[0][0],'activeProfile');assert.equal(h.updates[0][1],'release');
  assert.equal(h.updates[0][2],h.vscode.ConfigurationTarget.WorkspaceFolder);
  h.commands.get('parsley.showConfiguration')(h.doc.uri);
  const text=h.output.join('\n');
  assert.ok(text.includes('PARSLEY_TEST_CONFIG'));assert.ok(!text.includes('secret-test-value'));
});
test('formatting, CodeLens and LSP switches avoid disabled work',async()=>{
  const h=harness({settings:{'lsp.enabled':false,'formatting.enabled':false,'codeLens.enabled':false}});
  assert.equal(await new h.clientClass({delete(){}},{show(){}},()=>{}).ensureStarted(),false);
  assert.equal(h.spawnCalls.length,0);
  assert.equal((await h.providers.registerCodeLensProvider.provideCodeLenses(h.doc)).length,0);
  assert.equal((await h.providers.registerDocumentFormattingEditProvider.provideDocumentFormattingEdits(h.doc)).length,0);
});
test('configuration updates clear diagnostics and restart existing LSP sessions',async()=>{
  const h=harness();let restarted=0;
  h.clientClass.prototype.restart=async()=>{++restarted;};
  h.change('parsley.activeProfile');await Promise.resolve();
  assert.equal(restarted,1);
  assert.ok(h.calls.some(call=>call[0]==='clear-diagnostics'));
});
test('environment deletion and invalid argument arrays are checked before execution',async()=>{
  const saved=process.env.PARSLEY_TEST_REMOVE;process.env.PARSLEY_TEST_REMOVE='controlled';
  try{
    const h=harness({settings:{environment:{PARSLEY_TEST_REMOVE:null}}});
    await h.commands.get('parsley.check')(h.doc.uri);
    assert.equal(h.processCalls[0].opts.env.PARSLEY_TEST_REMOVE,undefined);
    h.settings['build.extraArgs']=['bad\0argument'];
    await h.commands.get('parsley.buildC23')(h.doc.uri);
    assert.equal(h.processCalls.length,1);
  }finally{if(saved===undefined)delete process.env.PARSLEY_TEST_REMOVE;else process.env.PARSLEY_TEST_REMOVE=saved;}
});
test('independent LSP sessions start with each folder executable, environment and root URI',async()=>{
  const one=path.resolve(__dirname,'lsp one'),two=path.resolve(__dirname,'lsp two');
  const settings1={executablePath:'lsp-one.exe',environment:{PARSLEY_TEST_LSP:'one'}};
  const settings2={executablePath:'lsp-two.exe',environment:{PARSLEY_TEST_LSP:'two'},'lsp.arguments':['--no-color']};
  const initialized=[];
  const h=harness({roots:[one,two],getSettings:uri=>uri&&uri.fsPath.startsWith(two)?settings2:settings1,
    spawn:(exe,args,opts)=>{
      const proc=new EventEmitter();proc.stdout=new EventEmitter();proc.stderr=new EventEmitter();proc.kill=()=>true;
      proc.stdin={writable:true,write(frame){
        const message=JSON.parse(frame.slice(frame.indexOf('\r\n\r\n')+4));
        if(message.method==='initialize'){
          initialized.push({exe,args,opts,params:message.params});
          const body=JSON.stringify({jsonrpc:'2.0',id:message.id,result:{capabilities:{}}});
          queueMicrotask(()=>proc.stdout.emit('data',Buffer.from('Content-Length: '+Buffer.byteLength(body)+'\r\n\r\n'+body)));
        }
      }};
      return proc;
    }});
  const pool=new h.hooks.ParsleyLspPool({delete(){}},{show(){}},()=>{});
  const a=pool.connection(h.vscode.Uri.file(path.join(one,'A.parsley')));
  const b=pool.connection(h.vscode.Uri.file(path.join(two,'B.parsley')));
  assert.notEqual(a,b);assert.equal(pool.connection(h.vscode.Uri.file(path.join(two,'C.parsley'))),b);
  try{
    assert.equal(await a.ensureStarted(),true);assert.equal(await b.ensureStarted(),true);
    assert.equal(initialized[0].exe,'lsp-one.exe');assert.equal(initialized[1].exe,'lsp-two.exe');
    assert.equal(initialized[1].opts.env.PARSLEY_TEST_LSP,'two');assert.equal(initialized[1].opts.cwd,two);
    assert.ok(initialized[1].args.includes('--no-color'));
    assert.equal(initialized[1].params.rootUri,h.vscode.Uri.file(two).toString());
  }finally{pool.dispose();}
});

test('project commands from components use the configured entry file', async () => {
  const root = path.resolve(__dirname, 'Kotlin project');
  const entry = path.join(root, 'Kotlin.parsley');
  const h = harness({roots:[root], docPath:path.join(root, 'Kotlin', 'Syntax.parsley'),
    settings:{'project.entry':'Kotlin.parsley'}});
  await h.commands.get('parsley.check')(h.doc.uri);
  await h.commands.get('parsley.buildC23')(h.doc.uri);
  assert.deepEqual(h.processCalls.map(call => call.args[1]), [entry, entry]);
});

test('module graph opens a DOT document from the language server', async () => {
  const h = harness();
  const dot = 'digraph modules { "Entry" -> "Piece"; }';
  const opened = [];
  h.clientClass.prototype.didOpen=()=>{};
  h.clientClass.prototype.sendRequest=async method => {
    assert.equal(method, 'parsley/moduleGraph');
    return dot;
  };
  h.vscode.workspace.openTextDocument=async value => value?.content ? value : h.doc;
  h.vscode.window.showTextDocument=async value => opened.push(value);
  await h.commands.get('parsley.showModuleGraph')(h.doc.uri);
  assert.equal(opened[0].content, dot);
  assert.equal(opened[0].language, 'dot');
});