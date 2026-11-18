"""End-to-end regression cases from the November 2026 source audits."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument('--cli', required=True)
parser.add_argument('--cc', required=True)
options, remaining = parser.parse_known_args()
CLI = str(Path(options.cli).resolve())
CC = options.cc
ROOT = Path(__file__).resolve().parents[1]

class AuditRegression(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='parsley audit ')
        self.dir = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()
    def run_cli(self, *args, ok=True):
        result = subprocess.run([CLI, *map(str,args)], capture_output=True, text=True, encoding='utf-8', timeout=30)
        if ok: self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        else: self.assertNotEqual(result.returncode, 0, result.stdout+result.stderr)
        return result
    def grammar(self, text, name='Audit.parsley'):
        path = self.dir/name
        path.write_text(text,encoding='utf-8')
        return path
    def build(self, grammar, sub='generated'):
        out = self.dir/sub
        self.run_cli('build',grammar,'-o',out,'--no-compile-shared')
        return out
    def compile(self, sources, out='test.exe'):
        exe=self.dir/out
        command=[CC,'-std=c23','-Wall','-Wextra','-pedantic','-Werror','-DPARSLEY_STATIC',*map(str,sources),'-o',str(exe)]
        if os.name!='nt': command.append('-lm')
        if os.environ.get('PARSLEY_SANITIZE_GENERATED')=='1': command += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        result=subprocess.run(command,capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return exe
    def execute(self, exe):
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
    def test_cli_counts_and_invalid_regex(self):
        path=self.grammar('module Audit (program) where\nprogram :: Text\nprogram = "x"\n')
        for value in ['abc','-1','+1','1.5','18446744073709551616','1000001']:
            with self.subTest(value=value):
                r=self.run_cli('test',path,'--fuzz',value,ok=False)
                self.assertEqual(r.returncode,2)
        for pattern in ['^x','$','\\b','(?=x)','a**','a*?','[z-a]','[[:word:]]','[а-я]','a{99999999999}','(x','x)','a{3,2}','a{,5}']:
            with self.subTest(pattern=pattern):
                p=self.grammar('module Audit (program) where\ntoken value = /'+pattern+'/\nprogram :: Text\nprogram = value\n')
                r=self.run_cli('check',p,ok=False)
                self.assertIn('error',r.stderr)
    def test_bom_start_and_empty(self):
        p=self.grammar('\ufeffmodule Audit (program) where\nprogram :: Text\nprogram = "x"\n')
        self.run_cli('check',p)
        p.write_text('{-# START absent #-}\nmodule Audit (program) where\nprogram :: Text\nprogram = "x"\n',encoding='utf-8')
        self.assertIn('START',self.run_cli('check',p,ok=False).stderr)
        p.write_text('',encoding='utf-8');self.run_cli('check',p,ok=False)
    def test_broken_lock_and_package_name(self):
        for suffix in ['parsley','psem']:shutil.copyfile(ROOT/'tests/fixtures'/('Calc.'+suffix),self.dir/('Calc.'+suffix))
        (self.dir/'Calc.psem.lock').write_text('invalid lock',encoding='utf-8')
        self.assertIn('PSL-LOCK-FORMAT',self.run_cli('check',self.dir/'Calc.psem',ok=False).stderr)
        for name in ['../escape','my-lang','1abc','a b']:
            self.run_cli('pkg','init',name,self.dir/'package',ok=False)
        self.run_cli('pkg','init','Example',self.dir/'package')
        manifest=self.dir/'package/parsley.toml';before=manifest.read_bytes()
        self.run_cli('pkg','init','Example',self.dir/'package',ok=False)
        self.assertEqual(before,manifest.read_bytes())
    def test_int64_and_namespaced_abi(self):
        p=self.grammar('module AuditInt (program) where\nimport Parsley.Prelude (eof)\ntoken number = /-?[0-9]+/ :: Int\ndata Ast = Num Int\nprogram :: Ast\nprogram = Num <$> number <* eof\n')
        one=self.build(p,'one')
        p=self.grammar('module AuditOther (program) where\nprogram :: Text\nprogram = "x"\n')
        two=self.build(p,'two')
        consumer=self.dir/'consumer.c'
        consumer.write_text(f'''#define PARSLEY_NO_COMPAT 1
#include "{(one/'parser.h').as_posix()}"
#include "{(two/'parser.h').as_posix()}"
#include <string.h>
#include <stdint.h>
int main(void) {{
  auditint_ParsleyResult r=auditint_parser_parse("9007199254740993");
  if(!r.ok || r.root->as.num.f0 != INT64_C(9007199254740993))return 1;
  char b[100];if(auditint_parser_format_ast(&r,b,sizeof(b))<0 || strcmp(b,"Num 9007199254740993"))return 2;
  auditint_parser_result_free(&r);auditint_parser_result_free(&r);
  r=auditint_parser_parse("9223372036854775807");if(!r.ok || r.root->as.num.f0!=INT64_MAX)return 3;auditint_parser_result_free(&r);
  r=auditint_parser_parse("-9223372036854775808");if(!r.ok || r.root->as.num.f0!=INT64_MIN)return 4;auditint_parser_result_free(&r);
  r=auditint_parser_parse("9223372036854775808");if(r.ok || r.error.code!=3)return 5;auditint_parser_result_free(&r);
  auditother_ParsleyResult s=auditother_parser_parse("x");if(!s.ok)return 6;auditother_parser_result_free(&s);return 0;
}}
''',encoding='utf-8')
        self.execute(self.compile([one/'parser.c',two/'parser.c',consumer]))
    def test_allocation_failure_cleanup(self):
        p=self.grammar('module AuditInt (program) where\nimport Parsley.Prelude (eof)\ntoken number = /[0-9]+/ :: Int\ndata Ast = Num Int\nprogram :: Ast\nprogram = Num <$> number <* eof\n')
        out=self.build(p)
        fault=self.dir/'fault.c'
        fault.write_text('''#include <stdlib.h>
#include <stddef.h>
static size_t calls,fail_at,live;
static void* fault_malloc(size_t n){if(++calls==fail_at)return NULL;void* p=malloc(n);if(p)++live;return p;}
static void* fault_realloc(void* p,size_t n){if(++calls==fail_at)return NULL;return realloc(p,n);}
static void fault_free(void* p){if(p)--live;free(p);}
#define PARSLEY_MALLOC fault_malloc
#define PARSLEY_REALLOC fault_realloc
#define PARSLEY_FREE fault_free
#include "'''+(out/'parser.c').as_posix()+'''"
int main(void){for(size_t i=1;i<80;++i){calls=0;fail_at=i;ParsleyResult r=parsley_parse("42");if(!r.ok&&r.error.code!=1)return 1;parsley_result_free(&r);if(live)return 2;}return 0;}
''',encoding='utf-8')
        self.execute(self.compile([fault]))
    def test_utf8_c_literals_and_large_examples(self):
        word='é1??('
        p=self.grammar('module Audit (program) where\nprogram :: Text\nprogram = "'+word+'"\nexample "тест1" : "'+word+'" parses-to "'+word+'"\n')
        out=self.build(p)
        self.execute(self.compile([out/'parser.c',out/'test_parser.c']))
        words=['word']*700
        p=self.grammar('module Audit (program) where\nimport Parsley.Prelude (many, eof)\nskip ws = /[ ]+/\ntoken word = /[a-z]+/\ndata Ast = Words [Text]\nprogram :: Ast\nprogram = Words <$> many word <* eof\nexample "long" : "'+' '.join(words)+'" parses-to Words ['+', '.join('"word"' for _ in words)+']\n')
        out=self.build(p,'large')
        self.execute(self.compile([out/'parser.c',out/'test_parser.c']))
    def test_deep_input_has_controlled_error(self):
        p=self.grammar('module Audit (program) where\nimport Parsley.Prelude (eof)\nprogram :: Unit\nprogram = nested <* eof\nnested :: Unit\nnested = "(" *> nested <* ")" <|> pure ()\n')
        out=self.build(p)
        consumer=self.dir/'depth.c'
        consumer.write_text('#include "'+(out/'parser.h').as_posix()+'"\n#include <string.h>\nint main(void){char input[20001];memset(input,\' (\',0);memset(input,40,10000);memset(input+10000,41,10000);input[20000]=0;ParsleyResult r=parsley_parse(input);int bad=r.ok||r.error.code!=2;parsley_result_free(&r);return bad;}\n'.replace("memset(input,' (',0);",''),encoding='utf-8')
        self.execute(self.compile([out/'parser.c',consumer]))
    def test_psl_adapter_uses_grammar_ast(self):
        for suffix in ['parsley','psem']:
            shutil.copyfile(ROOT/'tests/fixtures'/('Calc.'+suffix),self.dir/('Calc.'+suffix))
        out=self.build(self.dir/'Calc.parsley')
        consumer=self.dir/'semantic.c'
        consumer.write_text('#include "'+(out/'calc_parse.h').as_posix()+'"\n#include "'+(out/'calc_sem.h').as_posix()+'''"
static unsigned char memory[1024*1024];
static void sink(void* user,const psl_diag* d){if(d->def&&psl_str_eq(d->def->rule_name,psl_str_lit("divByConstZero")))++*(int*)user;}
int main(void){
  psl_arena arena;psl_arena_init(&arena,memory,sizeof(memory));
  const calc_node* root=0;psl_diag error;
  if(calc_parse(&arena,psl_str_lit("1 / 0"),&root,&error)||!root||root->tag!=CALC_NODE_BIN)return 1;
  calc_ctx* ctx=calc_ctx_new(&arena,0);int found=0;
  if(!ctx||calc_analyze(ctx,root,sink,&found)||found!=1)return 2;
  psl_arena_init(&arena,memory,sizeof(memory));
  if(calc_parse(&arena,psl_str_lit("1 +"),&root,&error)==0||root)return 3;
  unsigned char tiny[8];psl_arena_init(&arena,tiny,sizeof(tiny));
  if(calc_parse(&arena,psl_str_lit("1"),&root,&error)!=-2||!arena.error)return 4;
  return 0;
}
''',encoding='utf-8')
        self.execute(self.compile([out/'parser.c',out/'calc_parse.c',out/'calc_sem.c',consumer]))
    def lsp(self,messages, prefix=b''):
        data=prefix
        for item in messages:
            body=json.dumps(item,ensure_ascii=False).encode('utf-8');data+=f'Content-Length: {len(body)}\r\n\r\n'.encode()+body
        r=subprocess.run([CLI,'lsp'],input=data,capture_output=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stderr)
        result=[];data=r.stdout
        while data:
            header,data=data.split(b'\r\n\r\n',1);size=int(header.split(b':',1)[1]);result.append(json.loads(data[:size]));data=data[size:]
        return result
    def test_lsp_zero_frame_unicode_and_json(self):
        source='module Audit (program) where\nprogram :: Text\nprogram = "é🙂" *> missing\n'
        uri=(self.dir/'Audit.parsley').as_uri()
        messages=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{}},{'jsonrpc':'2.0','method':'textDocument/didOpen','params':{'textDocument':{'uri':uri,'text':source}}}]
        output=self.lsp(messages,b'Content-Length: 0\r\n\r\n')
        self.assertTrue(any(item.get('error',{}).get('code')==-32700 for item in output))
        diagnostics=[d for item in output if item.get('method')=='textDocument/publishDiagnostics' for d in item['params']['diagnostics']]
        expected=len(source.splitlines()[2].split('missing')[0].encode('utf-16-le'))//2
        self.assertTrue(any(d['range']['start']['character']==expected for d in diagnostics),diagnostics)
        output=self.lsp([{'jsonrpc':'2.0','id':2,'method':'initialize','params':{}}],b'Content-Length: 3\r\n\r\n{xx')
        self.assertIn('error',output[0]);self.assertEqual(output[-1]['id'],2)

    def test_module_lsp_live_overlay_and_navigation(self):
        entry = self.dir / 'Entry.parsley'
        piece = self.dir / 'Piece.parsley'
        source = 'module Entry (start) where\nimport Piece\nstart :: Text\nstart = piece\n'
        component = 'module Piece where\npiece :: Text\npiece = "disk"\n'
        entry.write_text(source, encoding='utf-8')
        piece.write_text(component, encoding='utf-8')
        root, dependency = entry.as_uri(), piece.as_uri()
        def message(method, params, ident=None):
            msg = {'jsonrpc': '2.0', 'method': method, 'params': params}
            if ident is not None:
                msg['id'] = ident
            return msg
        output = self.lsp([
            message('initialize', {}, 1),
            message('textDocument/didOpen', {'textDocument': {'uri': root, 'text': source}}),
            message('textDocument/definition', {'textDocument': {'uri': root},
                    'position': {'line': 3, 'character': 10}}, 2),
            message('parsley/moduleGraph', {'uri': root}, 3),
            message('textDocument/didOpen', {'textDocument': {'uri': dependency,
                    'text': component.replace('piece', 'renamed')}}),
            message('textDocument/didClose', {'textDocument': {'uri': dependency}}),
        ])
        definition = next(item['result'] for item in output if item.get('id') == 2)
        self.assertTrue(definition['uri'].endswith('/Piece.parsley'), definition)
        graph = next(item['result'] for item in output if item.get('id') == 3)
        self.assertIn('"Entry" -> "Piece"', graph)
        diagnostics = [item['params']['diagnostics'] for item in output
                       if item.get('method') == 'textDocument/publishDiagnostics'
                       and item['params']['uri'] == root]
        self.assertFalse(diagnostics[0], diagnostics)
        self.assertTrue(any(d['code'] == 'E0200' for group in diagnostics for d in group), diagnostics)
        self.assertFalse(diagnostics[-1], diagnostics)

if __name__=='__main__':
    unittest.main(argv=[sys.argv[0],*remaining])