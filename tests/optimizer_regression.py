"""Compile and execute the generated C23 optimizer and pass ABI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cli", required=True)
parser.add_argument("--cc", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
math_libraries = ["-lm"] if os.name != "nt" else []

def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            encoding="utf-8", timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix="parsley optimizer ") as tmp:
    out = Path(tmp)
    run([args.cli, "build", root/"tests/fixtures/Optimizer.parsley", "-o", out, "--no-compile-shared"])
    plan = (out/"optimizer_opt_plan.md").read_text(encoding="utf-8")
    assert "Combine: inferred" in plan and "Trace: opaque" in plan and "Opaque: opaque" in plan, plan
    manifest = json.loads((out/"optimizer_opt_plan.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == 1 and manifest["pass_abi"] == 1 and manifest["target"] == "c23"
    consumer = out/"consumer.c"
    consumer.write_text(r'''
#include "optimizer_parse.h"
#include "optimizer_opt.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned char input_mem[4u*1024u*1024u], output_mem[8u*1024u*1024u];
static psl_arena input, output;
#define REQUIRE(X) do { if(!(X)){fprintf(stderr,"optimizer assertion at line %d: %s\n",__LINE__,#X);return 1;} }while(0)
static const optimizer_node *parse(const char *source){
  psl_arena_init(&input,input_mem,sizeof(input_mem));
  psl_arena_init(&output,output_mem,sizeof(output_mem));
  const optimizer_node *root=NULL;psl_diag err={0};
  if(optimizer_parse(&input,psl_str_make(source,strlen(source)),&root,&err)<0)return NULL;
  return root;
}
static int scalar(const optimizer_node *n,double value){
  return n && n->tag==OPTIMIZER_NODE_SCALAR && n->child_count==1 && n->children[0]->num_val==value;
}
static size_t custom_visits, trace_events;
static const optimizer_node *extend(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  (void)user;
  if(n->tag==OPTIMIZER_NODE_OPAQUE){++custom_visits;return n->children[0];}
  if(n->tag==OPTIMIZER_NODE_SCALAR){
    optimizer_opt_fact fact=optimizer_opt_fact_of(cx,n);
    if(fact.kind!=1)optimizer_opt_fail(cx,4);
  }
  return n;
}
static const optimizer_node *custom(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  return optimizer_opt_walk(cx,n,extend,user);
}
static const optimizer_node *fail_pass(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  (void)cx;(void)n;(void)user;return NULL;
}
static const optimizer_node *grow(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  (void)user;
  const optimizer_node *children[]={n};
  return optimizer_opt_make(cx,OPTIMIZER_NODE_OPAQUE,children,1,0,0,psl_str_lit(""),n->span);
}
static const optimizer_node *count_visit(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  (void)cx;(void)user;if(n->tag==OPTIMIZER_NODE_OPAQUE)++custom_visits;return n;
}
static const optimizer_node *count_pass(optimizer_opt_context *cx,const optimizer_node *n,void *user){
  return optimizer_opt_walk(cx,n,count_visit,user);
}
static void trace(void *user,const char *name,const optimizer_node *before,const optimizer_node *after,size_t round){
  (void)user;(void)round;if(name && before && after)++trace_events;
}
static unsigned random_state=42u;
static unsigned random_value(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static optimizer_node *make_node(optimizer_node_tag tag,const optimizer_node **children,size_t count){
  psl_span span={0};
  optimizer_node *n=optimizer_new_node(&input,tag,psl_str_lit(""),psl_str_lit("Expr"),span,count);
  if(n && count)memcpy(n->children,children,count*sizeof(*children));
  return n;
}
static const optimizer_node *sample(unsigned depth){
  psl_span span={0};
  unsigned choice=random_value()%9u;
  if(!depth || choice<2u){
    const optimizer_node *children[]={optimizer_new_num_node(&input,(double)(random_value()%11u)-5.0,span)};
    return make_node(OPTIMIZER_NODE_SCALAR,children,1);
  }
  if(choice==2u){
    const optimizer_node *b[]={optimizer_new_bool_node(&input,(int)(random_value()%2u),span)};
    const optimizer_node *children[]={make_node(OPTIMIZER_NODE_LOGICAL,b,1),sample(depth-1),sample(depth-1)};
    return make_node(OPTIMIZER_NODE_BRANCH,children,3);
  }
  if(choice==3u || choice==4u){
    const optimizer_node *children[]={sample(depth-1),sample(depth-1)};
    return make_node(choice==3u ? OPTIMIZER_NODE_COMBINE : OPTIMIZER_NODE_DIVIDE,children,2);
  }
  const optimizer_node *children[]={sample(depth-1)};
  optimizer_node_tag tag=choice==5u ? OPTIMIZER_NODE_TRACE :
    (choice==6u ? OPTIMIZER_NODE_FORWARD : (choice==7u ? OPTIMIZER_NODE_RETYPE : OPTIMIZER_NODE_OPAQUE));
  return make_node(tag,children,1);
}
static double evaluate(const optimizer_node *n,uint64_t *effects){
  if(n->tag==OPTIMIZER_NODE_SCALAR)return n->children[0]->num_val;
  if(n->tag==OPTIMIZER_NODE_LOGICAL)return (double)n->children[0]->bool_val;
  if(n->tag==OPTIMIZER_NODE_BRANCH){
    double condition=evaluate(n->children[0],effects);
    return evaluate(n->children[condition!=0.0 ? 1 : 2],effects);
  }
  double a=evaluate(n->children[0],effects);
  if(n->tag==OPTIMIZER_NODE_COMBINE || n->tag==OPTIMIZER_NODE_DIVIDE){
    double b=evaluate(n->children[1],effects);
    return n->tag==OPTIMIZER_NODE_COMBINE ? a+b : a/b;
  }
  if(n->tag==OPTIMIZER_NODE_TRACE){
    unsigned char bytes[sizeof(a)];memcpy(bytes,&a,sizeof(a));
    for(size_t i=0;i<sizeof(a);++i)*effects=(*effects^bytes[i])*UINT64_C(1099511628211);
    *effects^=UINT64_C(0x1234);
  }
  return a;
}
int main(void){
  const optimizer_node *n=parse("add 2 add 3 4");REQUIRE(n);
  const optimizer_node *original_right=n->children[1];
  optimizer_opt_result r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && scalar(r.root,9) && r.rewrites>=2);
  REQUIRE(n->tag==OPTIMIZER_NODE_COMBINE && n->children[1]==original_right);
  size_t mark=output.used;
  optimizer_opt_result again=optimizer_optimize(&output,r.root,NULL,NULL,0);
  REQUIRE(!again.status && again.root==r.root && !again.rewrites);

  n=parse("if true add 2 3 trace 9");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && scalar(r.root,5));
  n=parse("if false 1 trace 9");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && r.root==n->children[2] && r.root->tag==OPTIMIZER_NODE_TRACE);
  n=parse("add trace 1 2");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && r.root==n);
  n=parse("div 1 0");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && r.root==n);
  n=parse("retype 1");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && r.root==n);
  n=parse("forward retype 1");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && r.root==n->children[0]);

  n=parse("add opaque 1 2");REQUIRE(n);
  custom_visits=trace_events=0;
  optimizer_opt_pass pass={"custom-denotation",custom,NULL};
  optimizer_opt_options opts={100000u,128u,16u,trace,NULL,0};
  r=optimizer_optimize(&output,n,&opts,&pass,1);
  REQUIRE(!r.status && scalar(r.root,3) && custom_visits==1 && trace_events>=4);

  n=parse("add 2 3");REQUIRE(n);mark=output.used;
  pass.run=fail_pass;
  r=optimizer_optimize(&output,n,NULL,&pass,1);
  REQUIRE(r.status==4 && r.root==n && output.used==mark && !r.rewrites);
  pass.run=grow;opts.max_iterations=2;
  r=optimizer_optimize(&output,n,&opts,&pass,1);
  REQUIRE(r.status==2 && r.root==n && output.used==mark);
  opts.max_steps=1;
  r=optimizer_optimize(&output,n,&opts,NULL,0);
  REQUIRE(r.status==2 && r.root==n && output.used==mark);
  opts.max_steps=100000u;opts.max_depth=2;
  r=optimizer_optimize(&output,n,&opts,NULL,0);
  REQUIRE(r.status==2 && r.root==n && output.used==mark);
  unsigned char tiny[8];psl_arena small;psl_arena_init(&small,tiny,sizeof(tiny));
  r=optimizer_optimize(&small,n,NULL,NULL,0);
  REQUIRE(r.status==3 && r.root==n && small.used==0 && small.error);
  r=optimizer_optimize(NULL,n,NULL,NULL,0);REQUIRE(r.status==1 && r.root==n);

  n=parse("opaque 1");REQUIRE(n);
  optimizer_node dag={0};dag.size=sizeof(dag);dag.tag=OPTIMIZER_NODE_COMBINE;
  const optimizer_node *shared[]={n,n};dag.children=shared;dag.child_count=2;
  custom_visits=0;pass.run=count_pass;
  r=optimizer_optimize(&output,&dag,NULL,&pass,1);
  REQUIRE(!r.status && r.root==&dag && custom_visits==1);

  optimizer_node cycle={0};cycle.size=sizeof(cycle);cycle.tag=OPTIMIZER_NODE_FORWARD;
  const optimizer_node *cyclic_children[]={&cycle};cycle.children=cyclic_children;cycle.child_count=1;
  mark=output.used;r=optimizer_optimize(&output,&cycle,NULL,NULL,0);
  REQUIRE(r.status==1 && r.root==&cycle && output.used==mark);
  cycle.child_count=0;
  r=optimizer_optimize(&output,&cycle,NULL,NULL,0);REQUIRE(r.status==1);

  n=parse("add 0 0");REQUIRE(n);
  optimizer_node *left=(optimizer_node *)n->children[0]->children[0];
  optimizer_node *right=(optimizer_node *)n->children[1]->children[0];
  left->num_val=-0.0;right->num_val=-0.0;
  r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && scalar(r.root,0) && signbit(r.root->children[0]->num_val));
  psl_arena_init(&output,output_mem,sizeof(output_mem));
  left->num_val=NAN;
  r=optimizer_optimize(&output,n,NULL,NULL,0);REQUIRE(!r.status && r.root==n);
  n=parse("both false trace true");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && r.root->tag==OPTIMIZER_NODE_LOGICAL && !r.root->children[0]->bool_val);
  n=parse("either true trace false");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && r.root->tag==OPTIMIZER_NODE_LOGICAL && r.root->children[0]->bool_val);
  n=parse("not eq text red text blue");REQUIRE(n);
  r=optimizer_optimize(&output,n,NULL,NULL,0);
  REQUIRE(!r.status && r.root->tag==OPTIMIZER_NODE_LOGICAL && r.root->children[0]->bool_val);
  n=parse("add 2 3");REQUIRE(n);
  opts=(optimizer_opt_options){100000u,128u,16u,NULL,NULL,OPTIMIZER_OPT_DISABLE_INFERENCE};
  r=optimizer_optimize(&output,n,&opts,NULL,0);
  REQUIRE(!r.status && r.root==n && !output.used);
  for(unsigned trial=0;trial<1000u;++trial){
    psl_arena_init(&input,input_mem,sizeof(input_mem));
    psl_arena_init(&output,output_mem,sizeof(output_mem));
    n=sample(6);REQUIRE(n && !input.error);
    uint64_t before_effects=UINT64_C(1469598103934665603), after_effects=before_effects;
    double before=evaluate(n,&before_effects);
    r=optimizer_optimize(&output,n,NULL,NULL,0);
    REQUIRE(!r.status);
    double after=evaluate(r.root,&after_effects);
    REQUIRE(before_effects==after_effects);
    REQUIRE((isnan(before) && isnan(after)) || !memcmp(&before,&after,sizeof(before)));
  }
  size_t previous_steps=0;
  for(size_t width=1000;width<=2000;width*=2){
    psl_arena_init(&input,input_mem,sizeof(input_mem));
    psl_arena_init(&output,output_mem,sizeof(output_mem));
    psl_span span={0};
    optimizer_node *list=optimizer_new_list_node(&input,width,span);REQUIRE(list);
    for(size_t i=0;i<width;++i){
      const optimizer_node *children[]={optimizer_new_num_node(&input,(double)i,span)};
      list->children[i]=make_node(OPTIMIZER_NODE_SCALAR,children,1);
    }
    REQUIRE(!input.error);
    r=optimizer_optimize(&output,list,NULL,NULL,0);
    REQUIRE(!r.status && r.root==list && !output.used);
    if(previous_steps)REQUIRE(r.steps<previous_steps*3u);
    previous_steps=r.steps;
  }
  optimizer_node foreign={0};foreign.size=sizeof(foreign);foreign.tag=OPTIMIZER_NODE_UNKNOWN;
  r=optimizer_optimize(&output,&foreign,NULL,NULL,0);
  REQUIRE(!r.status && r.root==&foreign);
  puts("1000 semantic-equivalence cases, effects, custom passes, DAG sharing and bounded failures passed");
  return 0;
}
''', encoding="utf-8")
    sources = [out/"parser.c", out/"optimizer_parse.c", out/"optimizer_sem.c",
               out/"optimizer_opt.c", consumer]
    exe = out/"optimizer-test.exe"
    run([args.cc, "-std=c23", "-Wall", "-Wextra", "-pedantic", "-Werror",
         "-DPARSLEY_STATIC", *sources, "-o", exe, *math_libraries])
    print(run([exe]).stdout, end="")
    # Changing the denotation changes the generated optimization, without any
    # optimizer rule or constructor-name change.
    original_grammar = (root/"tests/fixtures/Optimizer.parsley").read_text(encoding="utf-8")
    original_sem = (root/"tests/fixtures/Optimizer.psem").read_text(encoding="utf-8")

    def variant(name, semantics, expectation, inferred):
        directory = out/name
        directory.mkdir()
        grammar = directory/"Optimizer.parsley"
        grammar.write_text(original_grammar, encoding="utf-8")
        (directory/"Optimizer.psem").write_text(semantics, encoding="utf-8")
        generated = directory/"generated"
        run([args.cli, "build", grammar, "-o", generated, "--no-compile-shared"])
        manifest = json.loads((generated/"optimizer_opt_plan.json").read_text(encoding="utf-8"))
        row = next(c for c in manifest["constructors"] if c["name"] == "Combine")
        assert row["inferred"] == inferred, row
        check = generated/"check.c"
        check.write_text('''
#include "optimizer_parse.h"
#include "optimizer_opt.h"
static unsigned char memory[1024*1024], output[1024*1024];
int main(void){
  psl_arena arena, dst;psl_arena_init(&arena,memory,sizeof(memory));psl_arena_init(&dst,output,sizeof(output));
  const optimizer_node *n=0;psl_diag error={0};
  if(optimizer_parse(&arena,psl_str_lit("add 2 3"),&n,&error)<0)return 1;
  optimizer_opt_result r=optimizer_optimize(&dst,n,0,0,0);
  if(r.status)return 2;
  return (''' + expectation + ''') ? 0 : 3;
}
''', encoding="utf-8")
        exe = generated/"check.exe"
        run([args.cc, "-std=c23", "-Wall", "-Wextra", "-pedantic", "-Werror",
             "-DPARSLEY_STATIC", generated/"parser.c", generated/"optimizer_parse.c",
             generated/"optimizer_opt.c", check, "-o", exe, *math_libraries])
        run([exe])

    variant("changed-denotation", original_sem.replace("l.value + r.value", "l.value - r.value"),
            "r.root->tag==OPTIMIZER_NODE_SCALAR && r.root->children[0]->num_val==-1", True)
    variant("unknown-effects", original_sem.replace("value = l.value + r.value\n  effects = []",
                                                   "value = l.value + r.value"),
            "r.root==n", False)
    prefix = '''node _ where
  type = Number
  value = 0
  effects = []

{-# SOURCE language "first-match semantics" #-}
{-# STATUS reviewed #-}
{-# CONFIDENCE 1.0 #-}
'''
    variant("first-matching-rule", original_sem.replace("node Scalar n where",
                                                       prefix+"node Scalar n where"),
            "r.root==n", False)
    print("denotation changes, unknown effects and first-rule precedence passed")
    variant("typed-child", original_sem.replace("node Combine l r where", "node Combine (l :: Expr) r where"),
            "r.root->tag==OPTIMIZER_NODE_SCALAR && r.root->children[0]->num_val==5", True)
    variant("typed-primitive-never-matches", original_sem.replace("node Scalar n where", "node Scalar (n :: Expr) where"),
            "r.root==n", True)
