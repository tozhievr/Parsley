module;
#include <string_view>
export module parsley.optimizer_runtime;
export namespace parsley::optimizer_runtime {
inline constexpr std::string_view source = R"OPT(
typedef @m@_node N;
typedef @m@_opt_context Ctx;
typedef @m@_opt_fact Fact;
typedef struct o_cache {
  const N *key, *out;
  Fact fact;
  unsigned walking, walked, evaluating, evaluated;
  struct o_cache *next;
} o_cache;
struct @m@_opt_context {
  psl_arena *arena;
  @m@_opt_options options;
  @m@_opt_result result;
  o_cache **cache;
  size_t cache_capacity, cache_count, scratch_top;
  size_t depth;
};
static Fact o_unknown(const N *origin) {
  Fact f = {0}; f.origin = origin; return f;
}
static int o_tick(Ctx *cx) {
  if (cx->result.status) return 0;
  if (cx->result.steps >= cx->options.max_steps) { cx->result.status=2; return 0; }
  ++cx->result.steps; return 1;
}
void @m@_opt_fail(Ctx *cx, int status) {
  if (cx && !cx->result.status) cx->result.status=status ? status : 4;
}
static void *o_alloc(Ctx *cx, size_t bytes, size_t alignment) {
  if (!o_tick(cx)) return NULL;
  size_t capacity=cx->arena->cap;
  cx->arena->cap=cx->scratch_top;
  void *p=psl_arena_alloc(cx->arena,bytes,alignment);
  cx->arena->cap=capacity;
  if (!p) cx->result.status=3;
  return p;
}
static int o_valid(const N *n);
static int o_type(const N *n);
static Fact o_equation(Ctx *cx, const N *n);
static int o_compatible(Ctx *cx,const N *a,const N *b);
static const N *o_reify(Ctx *cx, const N *n, Fact f);
/* Analysis cache grows geometrically in scratch space at the back of the same
   arena. Output nodes grow from the front; scratch never escapes a phase. */
static void *o_scratch(Ctx *cx,size_t bytes,size_t align) {
  if(!o_tick(cx))return NULL;
  uintptr_t base=(uintptr_t)cx->arena->base;
  if(!base || cx->scratch_top>UINTPTR_MAX-base || bytes>cx->scratch_top){
    cx->arena->error=1;cx->result.status=3;return NULL;
  }
  uintptr_t address=(base+cx->scratch_top-bytes)&~(uintptr_t)(align-1u);
  if(address<base || address-base<cx->arena->used){
    cx->arena->error=1;cx->result.status=3;return NULL;
  }
  cx->scratch_top=(size_t)(address-base);
  void *p=(void *)address;memset(p,0,bytes);return p;
}
static void o_reset(Ctx *cx) {
  cx->cache=NULL;cx->cache_count=0;cx->cache_capacity=0;
  cx->scratch_top=cx->arena->cap;
}
static size_t o_hash(const N *n,size_t capacity) {
  uintptr_t x=(uintptr_t)n;
  x=(x>>3u)^(x>>17u);
  return (size_t)(x*(uintptr_t)UINT64_C(11400714819323198485))&(capacity-1u);
}
static o_cache *o_entry(Ctx *cx, const N *n) {
  if(!cx->cache_capacity || cx->cache_count>=cx->cache_capacity){
    size_t capacity=cx->cache_capacity ? cx->cache_capacity*2u : 64u;
    if(capacity<cx->cache_capacity || capacity>SIZE_MAX/sizeof(o_cache *)){
      cx->result.status=2;return NULL;
    }
    o_cache **table=(o_cache **)o_scratch(cx,capacity*sizeof(*table),_Alignof(o_cache *));
    if(!table)return NULL;
    for(size_t i=0;i<cx->cache_capacity;++i){
      o_cache *entry=cx->cache[i];
      while(entry){
        if(!o_tick(cx))return NULL;
        o_cache *next=entry->next;
        size_t hash=o_hash(entry->key,capacity);
        entry->next=table[hash];table[hash]=entry;entry=next;
      }
    }
    cx->cache=table;cx->cache_capacity=capacity;
  }
  size_t h=o_hash(n,cx->cache_capacity);
  for(o_cache *e=cx->cache[h];e;e=e->next){
    if(!o_tick(cx))return NULL;
    if(e->key==n)return e;
  }
  o_cache *e=(o_cache *)o_scratch(cx,sizeof(*e),_Alignof(o_cache));
  if(!e)return NULL;
  e->key=n;e->next=cx->cache[h];cx->cache[h]=e;++cx->cache_count;return e;
}
static int o_enter(Ctx *cx) {
  if(!o_tick(cx))return 0;
  if(cx->depth>=cx->options.max_depth){cx->result.status=2;return 0;}
  ++cx->depth;return 1;
}
Fact @m@_opt_fact_of(Ctx *cx, const N *n) {
  Fact f=o_unknown(n);
  if(!cx || !o_enter(cx))return f;
  if(!o_valid(n)){cx->result.status=1;--cx->depth;return f;}
  o_cache *e=o_entry(cx,n);
  if(!e){--cx->depth;return f;}
  if(e->evaluated){--cx->depth;return e->fact;}
  if(e->evaluating){cx->result.status=1;--cx->depth;return f;}
  e->evaluating=1;
  f=o_equation(cx,n);
  if(!f.origin || (f.origin!=n && !o_compatible(cx,n,f.origin)))f.origin=n;
  e->fact=f;e->evaluated=1;e->evaluating=0;
  --cx->depth;return f;
}
static Fact o_num(double v) {
  Fact f=o_unknown(NULL);
  if(isfinite(v)){f.kind=1;f.number=v;f.pure=1;}
  return f;
}
static Fact o_bool(int v) {
  Fact f=o_unknown(NULL);f.kind=2;f.boolean=!!v;f.pure=1;return f;
}
static Fact o_text(psl_str v) {
  Fact f=o_unknown(NULL);f.kind=3;f.text=v;f.pure=1;return f;
}
static Fact o_unary(int op,Fact a) {
  if(!a.pure)return o_unknown(NULL);
  if(op==0 && a.kind==1)return o_num(-a.number);
  if(op==1 && a.kind==2)return o_bool(!a.boolean);
  return o_unknown(NULL);
}
static Fact o_binary(int op,Fact a,Fact b) {
  if(!a.pure || !b.pure)return o_unknown(NULL);
  if(a.kind==1 && b.kind==1){
    switch(op){
      case 0:return o_num(a.number+b.number);
      case 1:return o_num(a.number-b.number);
      case 2:return o_num(a.number*b.number);
      case 3:return b.number!=0.0 ? o_num(a.number/b.number) : o_unknown(NULL);
      case 4:return o_bool(a.number==b.number);
      case 5:return o_bool(a.number!=b.number);
      case 6:return o_bool(a.number<b.number);
      case 7:return o_bool(a.number<=b.number);
      case 8:return o_bool(a.number>b.number);
      case 9:return o_bool(a.number>=b.number);
      default:break;
    }
  }
  if(a.kind && a.kind==b.kind && (op==4 || op==5)){
    int equal=a.kind==2 ? a.boolean==b.boolean : psl_str_eq(a.text,b.text);
    return o_bool(op==4 ? equal : !equal);
  }
  return o_unknown(NULL);
}
/* Lazy semantic evaluation is emitted as statements: only the selected branch
   contributes values/effects. No host evaluation of target programs occurs. */
static const N *o_walk(Ctx *cx,const N *n,@m@_opt_visit visit,void *user) {
  if(!o_enter(cx))return n;
  if(!o_valid(n)){cx->result.status=1;--cx->depth;return n;}
  o_cache *entry=o_entry(cx,n);
  if(!entry){--cx->depth;return n;}
  if(entry->walked){--cx->depth;return entry->out;}
  if(entry->walking){cx->result.status=1;--cx->depth;return n;}
  entry->walking=1;
  const N **children=NULL;
  const N *current=n;
  for(size_t i=0;i<n->child_count && !cx->result.status;++i){
    const N *child=o_walk(cx,n->children[i],visit,user);
    if(child!=n->children[i] && !children){
      children=(const N **)o_alloc(cx,n->child_count*sizeof(*children),_Alignof(const N *));
      if(children)memcpy(children,n->children,n->child_count*sizeof(*children));
    }
    if(children)children[i]=child;
  }
  if(children && !cx->result.status){
    N *copy=(N *)o_alloc(cx,sizeof(*copy),_Alignof(N));
    if(copy){*copy=*n;copy->children=children;current=copy;}
  }
  if(!cx->result.status){
    const N *next=visit(cx,current,user);
    if(!next){if(!cx->result.status)cx->result.status=4;}
    else if(!o_valid(next) || o_type(current)!=o_type(next))cx->result.status=1;
    else current=next;
  }
  entry->out=current;entry->walked=1;entry->walking=0;
  --cx->depth;return current;
}
const N *@m@_opt_walk(Ctx *cx,const N *n,@m@_opt_visit visit,void *user) {
  if(!cx || !visit){if(cx)@m@_opt_fail(cx,1);return n;}
  return o_walk(cx,n,visit,user);
}
static const N *o_infer(Ctx *cx,const N *n,void *user) {
  (void)user;
  Fact f=@m@_opt_fact_of(cx,n);
  const N *out=n;
  if(f.origin && f.origin!=n && o_compatible(cx,n,f.origin))out=f.origin;
  else if(f.pure && f.kind)out=o_reify(cx,n,f);
  if(out!=n && !cx->result.status)++cx->result.rewrites;
  return out;
}
const N *@m@_opt_make(Ctx *cx,@m@_node_tag tag,const N *const *children,
                     size_t count,double number,int boolean,psl_str text,psl_span span) {
  if(!cx)return NULL;
  N proto={0};
  proto.size=sizeof(proto);proto.tag=tag;proto.children=(const N **)children;proto.child_count=count;
  proto.num_val=number;proto.bool_val=boolean;proto.text_val=text;proto.span=span;
  if(!o_valid(&proto)){@m@_opt_fail(cx,1);return NULL;}
  N *n=(N *)o_alloc(cx,sizeof(*n),_Alignof(N));
  if(!n)return NULL;
  *n=proto;
  if(count){
    const N **copy=(const N **)o_alloc(cx,count*sizeof(*copy),_Alignof(const N *));
    if(!copy)return NULL;
    memcpy(copy,children,count*sizeof(*copy));n->children=copy;
  }
  /* Text is copied: user pass buffers need not outlive the callback. */
  if(text.len){
    char *copy=(char *)o_alloc(cx,text.len,1);
    if(!copy)return NULL;
    memcpy(copy,text.ptr,text.len);n->text_val.ptr=copy;
  }
  @m@_opt_set_names(n);
  return n;
}
@m@_opt_result @m@_optimize(psl_arena *arena,const N *root,
                           const @m@_opt_options *options,
                           const @m@_opt_pass *passes,size_t pass_count) {
  Ctx cx={0};
  cx.arena=arena;cx.result.root=root;
  cx.options.max_steps=1000000u;cx.options.max_depth=128u;cx.options.max_iterations=16u;
  if(options)cx.options=*options;
  if(!arena || arena->error || arena->used>arena->cap || !root ||
     (pass_count && !passes) || !cx.options.max_steps || !cx.options.max_depth ||
     cx.options.max_depth>512u || !cx.options.max_iterations || (cx.options.flags & ~1u)){
    cx.result.status=1;return cx.result;
  }
  size_t mark=arena->used;
  const N *current=root;
  int stable=0;
  /* Reference optional emitted helpers in no-equation languages too. */
  (void)o_num;(void)o_bool;(void)o_text;(void)o_unary;(void)o_binary;
  for(size_t round=0;round<cx.options.max_iterations && !cx.result.status;++round){
    const N *before=current;
    ++cx.result.iterations;
    for(size_t pi=0;pi<=pass_count && !cx.result.status;++pi){
      o_reset(&cx);
      const N *old=current;
      const char *name="inferred-semantics";
      if(!pi)current=o_walk(&cx,current,(cx.options.flags & 1u) ? @m@_opt_identity : o_infer,NULL);
      else{
        const @m@_opt_pass *p=&passes[pi-1];
        name=p->name;
        if(!p->run || !name){cx.result.status=1;break;}
        current=p->run(&cx,current,p->user);
        if(!current && !cx.result.status)cx.result.status=4;
      }
      if(cx.result.status)break;
      /* Validate custom outputs, including cycles and child shapes. */
      if(pi){
        o_reset(&cx);
        current=o_walk(&cx,current,@m@_opt_identity,NULL);
      }
      if(cx.result.status)break;
      if(cx.options.trace)cx.options.trace(cx.options.trace_user,name,old,current,round);
    }
    if(current==before){stable=1;break;}
  }
  if(!stable && !cx.result.status)cx.result.status=2;
  if(cx.result.status){arena->used=mark;cx.result.root=root;cx.result.rewrites=0;}
  else cx.result.root=current;
  return cx.result;
}
)OPT";
}