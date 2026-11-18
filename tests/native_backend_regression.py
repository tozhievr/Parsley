"""Windows end-to-end checks for the generated frontend and the real nextc backend."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument("--cli",required=True)
p.add_argument("--cc",required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
def run(cmd,code=0):
    r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,encoding="utf-8",timeout=120)
    assert r.returncode==code, (cmd,r.returncode,r.stdout,r.stderr)
    return r.stdout

manifest=json.loads((root/"resources/nextc/UPSTREAM.json").read_text(encoding="utf-8"))
for name,digest in manifest["sha256_lf"].items():
    assert hashlib.sha256((root/"resources/nextc"/name).read_bytes().replace(b"\r\n",b"\n")).hexdigest()==digest,name

with tempfile.TemporaryDirectory(prefix="parsley native ") as tmp:
    d=Path(tmp)
    run([a.cli,"build",root/"tests/fixtures/Optimizer.parsley","-o",d,
         "--emit-nextc","--compile-compiler","--no-compile-shared"])
    compiler=d/"optimizer_compiler.exe"
    source=d/"input.lang"
    source.write_text("add 2 add 3 4",encoding="utf-8")
    assert run([compiler,source,"--run","--no-ast-opt"]).strip()=="9"
    assert run([compiler,source,"--run"]).strip()=="9"
    assert "fadd" in run([compiler,source,"--emit","mir","--no-ast-opt"])
    assert run([compiler,source,"--emit","asm"]).strip()
    obj=d/"program.obj"
    run([compiler,source,"--emit","object","-o",obj])
    assert obj.read_bytes()[:2]==b"\x64\x86"
    pe=d/"program.exe"
    run([compiler,source,"--emit","exe","-o",pe])
    assert pe.read_bytes()[:2]==b"MZ"
    run([pe],code=9)
    raw=d/"program.bin"
    run([compiler,source,"--emit","flat","-o",raw])
    assert len(raw.read_bytes())>8
    source.write_text("not false",encoding="utf-8")
    assert run([compiler,source,"--run","--return","bool"]).strip()=="1"
    source.write_text("trace 1",encoding="utf-8")
    run([compiler,source,"--run"],code=1)
    consumer=d/"hooks.c"
    consumer.write_text(r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include "optimizer_backend.h"
#include "optimizer_parse.h"
#include <stdio.h>
#include <string.h>
static unsigned char memory[4*1024*1024];
static int effects,passes;
#define CHECK(X) do{if(!(X)){fprintf(stderr,"line %d: %s\n%s",__LINE__,#X,diag.text);return 1;}}while(0)
static double effect(double x){++effects;return x;}
static void *resolve(void *user,const char *name){
 (void)user;if(strcmp(name,"effect"))return NULL;
 double (*f)(double)=effect;void *p=NULL;memcpy(&p,&f,sizeof(p));return p;
}
static bool interpret_call(void *user,const char *name,const uint64_t *args,uint32_t count,cg_type_t type,uint64_t *out){
 (void)user;(void)type;if(strcmp(name,"effect") || count!=1)return false;
 ++effects;*out=args[0];return true;
}
static bool lower(optimizer_backend_context *cx,const optimizer_node *node,cg_value_t *out,void *user){
 (void)user;cg_builder_t *b=optimizer_backend_builder(cx);
 if(node->tag==OPTIMIZER_NODE_OPAQUE){*out=cg_func_param(b->func,0);return true;}
 if(node->tag==OPTIMIZER_NODE_TRACE){
   cg_value_t arg=optimizer_backend_node(cx,node->children[0]);
   if(arg==CG_INVALID){*out=CG_INVALID;return true;}
   cg_symbol_t fn=cg_module_declare_function(b->module,"effect");
   *out=cg_build_call(b,fn,CG_T_F64,&arg,1,false);return true;
 }
 return false;
}
static bool pass(cg_module_t *module,cg_diag_t *diag,void *user){
 (void)module;(void)diag;(void)user;++passes;return true;
}
static bool broken(cg_module_t *module,cg_diag_t *diag,void *user){
 (void)diag;(void)user;
 return cg_module_add_function(module,"missing_return",CG_T_F64,NULL,0,false,CG_BIND_LOCAL)!=NULL;
}
int main(void){
 cg_diag_t diag={0};psl_arena arena;psl_arena_init(&arena,memory,sizeof(memory));
 const optimizer_node *node=NULL;psl_diag error={0};
 CHECK(optimizer_parse(&arena,psl_str_lit("if eq opaque 0 0 trace 10 trace 20"),&node,&error)>=0);
 optimizer_mir_pass extension={"count",pass,NULL};
 optimizer_backend_options options={0};options.lower=lower;options.passes=&extension;options.pass_count=1;
 cg_type_t params[]={CG_T_F64};
 cg_module_t *module=optimizer_backend_lower(node,"entry",CG_T_F64,params,1,&options,&diag);
 CHECK(module && passes==1);
 cg_options_t target=cg_default_options();target.opt_level=0;target.optimize_ir=false;
 cg_jit_host_t host={NULL,resolve};cg_interp_host_t interp={NULL,interpret_call};
 cg_jit_t *jit=cg_jit_create(module,&target,&host,&diag);CHECK(jit);
 void *address=cg_jit_symbol(jit,"entry");double (*fn)(double);memcpy(&fn,&address,sizeof(fn));
 for(int i=0;i<100;++i){
   double x=(double)i,expected=i==0 ? 10.0 : 20.0;
   uint64_t bits=0,value=0;memcpy(&bits,&x,sizeof(bits));effects=0;
   CHECK(cg_interp_run(module,cg_module_symbol(module,"entry"),&bits,1,&value,&interp,&diag));
   double interpreted=0;memcpy(&interpreted,&value,sizeof(interpreted));CHECK(interpreted==expected && effects==1);
   effects=0;CHECK(fn(x)==expected && effects==1);
 }
 cg_jit_destroy(jit);cg_module_destroy(module);
 extension.run=broken;diag=(cg_diag_t){0};
 module=optimizer_backend_lower(node,"entry",CG_T_F64,params,1,&options,&diag);
 CHECK(!module && diag.error_count);
 options.passes=NULL;options.pass_count=0;options.max_depth=1;diag=(cg_diag_t){0};
 module=optimizer_backend_lower(node,"entry",CG_T_F64,params,1,&options,&diag);
 CHECK(!module && diag.error_count);
 puts("native lowering hooks, MIR passes/verifier and 100 interpreter/JIT value/effect comparisons passed");
 return 0;
}
''',encoding="utf-8")
    sources=[d/"parser.c",d/"optimizer_parse.c",d/"optimizer_backend.c",*sorted((d/"nextc").rglob("*.c")),consumer]
    exe=d/"hooks.exe"
    run([a.cc,"-std=c23","-O1","-Wall","-Wextra","-pedantic","-Werror","-DPARSLEY_STATIC",
         "-I"+str(d),*sources,"-o",exe])
    print(run([exe]),end="")
    print("generated compiler: JIT, MIR, assembly, COFF, PE execution and flat image passed")