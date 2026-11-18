module;
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <iomanip>
#include <limits>

export module parsley.optimizer;
import parsley.ir;
import parsley.psl;
import parsley.c23_text;
import parsley.optimizer_runtime;

export namespace parsley::optimizer {
namespace detail {
using E = psl::PslExpr;
using P = psl::PslPattern;
struct Ctor {
  std::string name, type, tag;
  std::vector<ir::Type> fields;
  const psl::PslNodeRule *rule = nullptr;
  const E *value = nullptr;
  const E *sem_type = nullptr;
  bool eligible = false, literal = false, exhaustive = false;
  std::string reason;
  std::map<std::string, std::size_t> vars;
};
inline std::string upper(std::string s) {
  for(char& c:s)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
inline std::string lit(std::string_view s) {return c23_text::literal(s);}
inline int primitive(const ir::Type& t) {
  if(t.kind!=ir::Type::Kind::Named)return 0;
  if(t.name=="Double")return 1;
  if(t.name=="Bool")return 2;
  if(t.name=="Text")return 3;
  return 0;
}
inline bool empty_effects(const E *e) {
  return e && e->kind==E::Kind::List && e->args.empty();
}
inline bool valid_expr(const E *e,const Ctor& c,int depth=0) {
  if(!e || depth>64)return false;
  switch(e->kind) {
    case E::Kind::Number: case E::Kind::Bool: case E::Kind::String:return true;
    case E::Kind::Var:{
      auto it=c.vars.find(e->name);
      return it!=c.vars.end() && primitive(c.fields[it->second]);
    }
    case E::Kind::FieldAccess:
      return e->name=="value" && e->args.size()==1 && e->args[0] &&
        e->args[0]->kind==E::Kind::Var && c.vars.contains(e->args[0]->name) &&
        c.fields[c.vars.at(e->args[0]->name)].kind==ir::Type::Kind::Named &&
        !primitive(c.fields[c.vars.at(e->args[0]->name)]) &&
        c.fields[c.vars.at(e->args[0]->name)].name!="Int";
    case E::Kind::Call:
      return e->name=="valueOf" && e->args.size()==1 && e->args[0] &&
        e->args[0]->kind==E::Kind::Var && c.vars.contains(e->args[0]->name) &&
        c.fields[c.vars.at(e->args[0]->name)].kind==ir::Type::Kind::Named &&
        !primitive(c.fields[c.vars.at(e->args[0]->name)]) &&
        c.fields[c.vars.at(e->args[0]->name)].name!="Int";
    case E::Kind::UnaryOp:
      if(e->args.size()!=1 || (e->name!="-" && e->name!="not"))return false;
      break;
    case E::Kind::BinOp:{
      static const std::set<std::string> ops={"+","-","*","/","==","/=","<","<=",">",">=","&&","||"};
      if(e->args.size()!=2 || !ops.contains(e->name))return false;
      break;
    }
    case E::Kind::IfThenElse:if(e->args.size()!=3)return false;break;
    default:return false;
  }
  for(const auto& a:e->args)if(!valid_expr(a.get(),c,depth+1))return false;
  return true;
}
inline std::string type_key(const E *e) {
  if(!e)return "untyped";
  if((e->kind==E::Kind::Ctor || e->kind==E::Kind::Var) && e->args.empty())
    return "named:"+e->name;
  return {};
}
inline std::vector<Ctor> derive(const ir::Grammar& g,const psl::PslModule& sem,
                                const std::string& up,psl::PslStatus minimum) {
  std::vector<Ctor> out;
  std::set<std::string> tags;
  for(const auto& t:g.gTypes)for(const auto& cd:t.constructors){
    if(cd.name=="True" || cd.name=="False")continue;
    Ctor c;c.name=cd.name;c.type=t.name;
    std::string identifier=cd.name;
    for(char& ch:identifier)if(!std::isalnum(static_cast<unsigned char>(ch)) && ch!='_')ch='_';
    std::string base=up+"_NODE_"+upper(identifier);c.tag=base;
    for(int suffix=2;tags.contains(c.tag);++suffix)c.tag=base+"_"+std::to_string(suffix);
    tags.insert(c.tag);
    if(cd.is_record)for(const auto& f:cd.record_fields)c.fields.push_back(f.type);
    else c.fields=cd.positional_fields;
    c.reason="no denotational value equation";
    /* PSL uses the first matching rule. An earlier general/nested rule must
       never be bypassed by inference from a later, simpler constructor rule. */
    for(const auto& r:sem.node_rules){
      if(!r.pattern)continue;
      if(r.pattern->kind==P::Kind::Ctor && r.pattern->name!=c.name)continue;
      if(r.pattern->kind==P::Kind::TypedVar && r.pattern->type_name!=c.type)continue;
      c.rule=&r;break;
    }
    if(!c.rule){out.push_back(std::move(c));continue;}
    const auto& r=*c.rule;
    bool flat=r.pattern->kind==P::Kind::Ctor && r.pattern->args.size()==c.fields.size();
    if(flat)for(std::size_t i=0;i<r.pattern->args.size();++i){
      const auto& p=r.pattern->args[i];
      if(!p || (p->kind!=P::Kind::Var && p->kind!=P::Kind::Wildcard && p->kind!=P::Kind::TypedVar)){flat=false;break;}
      if(p->kind==P::Kind::TypedVar &&
         (c.fields[i].kind!=ir::Type::Kind::Named || primitive(c.fields[i]) ||
          c.fields[i].name=="Int" || p->type_name!=c.fields[i].name)){flat=false;break;}
      if(p->kind!=P::Kind::Wildcard && !c.vars.emplace(p->name,i).second){flat=false;break;}
    }
    c.exhaustive=flat;
    bool effects=false,other=false,duplicate=false;
    for(const auto& st:r.stmts){
      if(st.kind!=psl::PslNodeStmt::Kind::AssignAttr){other=true;continue;}
      if(st.target_var=="value"){if(c.value)duplicate=true;c.value=st.expr.get();}
      else if(st.target_var=="effects"){if(effects)duplicate=true;effects=empty_effects(st.expr.get());if(!effects)other=true;}
      else if(st.target_var=="type"){if(c.sem_type)duplicate=true;c.sem_type=st.expr.get();}
      else other=true;
    }
    if(!c.value)c.reason="no denotational value equation";
    else if(!flat)c.reason="first applicable rule has a non-flat or overlapping pattern";
    else if(t.is_foreign || !t.type_params.empty())c.reason="foreign/polymorphic AST representation";
    else if(!r.provenance.has_status || !r.provenance.has_confidence || r.provenance.source_name.empty())
      c.reason="value equation needs SOURCE, STATUS and CONFIDENCE provenance";
    else if(static_cast<int>(r.provenance.status)<static_cast<int>(minimum))
      c.reason="below requested semantic status";
    else if(other || duplicate)c.reason="effectful, duplicate or additional semantic attributes/statements";
    else if(!effects)c.reason="local effects are not explicitly empty";
    else if(!valid_expr(c.value,c))c.reason="value expression needs a user analysis/pass";
    else{
      c.eligible=true;c.reason="partial evaluation of declared value and child effects";
      c.literal=c.fields.size()==1 && primitive(c.fields[0]) && c.value->kind==E::Kind::Var &&
        c.vars.contains(c.value->name) && c.vars.at(c.value->name)==0;
    }
    out.push_back(std::move(c));
  }
  return out;
}
inline void substitute(std::string& s,std::string_view from,std::string_view to) {
  std::size_t p=0;while((p=s.find(from,p))!=std::string::npos){s.replace(p,from.size(),to);p+=to.size();}
}
inline std::string emit_expr(std::ostringstream& o,const E *e,const Ctor& c,int& seq,
                             const std::string& mod) {
  std::string v="v"+std::to_string(++seq);
  auto child=[&](const E *a){return "n->children["+std::to_string(c.vars.at(a->name))+"]";};
  switch(e->kind){
    case E::Kind::Number:
      o<<"  Fact "<<v<<"=o_num("<<std::setprecision(17)<<e->number_value<<");\n";return v;
    case E::Kind::Bool:o<<"  Fact "<<v<<"=o_bool("<<(e->bool_value?1:0)<<");\n";return v;
    case E::Kind::String:o<<"  Fact "<<v<<"=o_text(psl_str_lit("<<lit(e->name)<<"));\n";return v;
    case E::Kind::Var:
      o<<"  Fact "<<v<<"="<<mod<<"_opt_fact_of(cx,"<<child(e)<<");\n";return v;
    case E::Kind::Call:case E::Kind::FieldAccess:
      o<<"  Fact "<<v<<"="<<mod<<"_opt_fact_of(cx,"<<child(e->args[0].get())<<");\n";return v;
    case E::Kind::UnaryOp:{
      auto a=emit_expr(o,e->args[0].get(),c,seq,mod);
      o<<"  Fact "<<v<<"=o_unary("<<(e->name=="not"?1:0)<<","<<a<<");\n";return v;
    }
    case E::Kind::IfThenElse:{
      auto cond=emit_expr(o,e->args[0].get(),c,seq,mod);
      o<<"  Fact "<<v<<"=o_unknown(NULL);\n  if("<<cond<<".pure && "<<cond<<".kind==2){\n  if("<<cond<<".boolean){\n";
      auto yes=emit_expr(o,e->args[1].get(),c,seq,mod);o<<"  "<<v<<"="<<yes<<";\n  }else{\n";
      auto no=emit_expr(o,e->args[2].get(),c,seq,mod);o<<"  "<<v<<"="<<no<<";\n  }}\n";return v;
    }
    case E::Kind::BinOp:{
      auto a=emit_expr(o,e->args[0].get(),c,seq,mod);
      if(e->name=="&&" || e->name=="||"){
        bool is_and=e->name=="&&";
        o<<"  Fact "<<v<<"=o_unknown(NULL);\n  if("<<a<<".pure && "<<a<<".kind==2){\n"
         <<"  if("<<(is_and?"!":"")<<a<<".boolean) "<<v<<"=o_bool("<<(is_and?0:1)<<");\n  else{\n";
        auto b=emit_expr(o,e->args[1].get(),c,seq,mod);
        o<<"  "<<v<<"="<<b<<";\n  if("<<v<<".kind!=2 && "<<v<<".kind) "<<v<<"=o_unknown(NULL);\n  }}\n";
      }else{
        auto b=emit_expr(o,e->args[1].get(),c,seq,mod);
        static const std::vector<std::string> ops={"+","-","*","/","==","/=","<","<=",">",">="};
        auto it=std::find(ops.begin(),ops.end(),e->name);
        o<<"  Fact "<<v<<"=o_binary("<<(it-ops.begin())<<","<<a<<","<<b<<");\n";
      }
      return v;
    }
    default:o<<"  Fact "<<v<<"=o_unknown(NULL);\n";return v;
  }
}
inline std::string header(const std::string& m,const std::string& up) {
  std::string h=R"H(/* Generated semantic optimizer and extensible pass ABI (ISO C23). */
#ifndef @U@_OPT_H
#define @U@_OPT_H
#include "@m@_ast.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct @m@_opt_context @m@_opt_context;
/* kind: 0 unknown, 1 finite Double, 2 Bool, 3 Text.
   origin is an evaluation-equivalent node, even when value/effects are unknown. */
typedef struct @m@_opt_fact {
  int kind, pure;
  double number;
  int boolean;
  psl_str text;
  const @m@_node *origin;
} @m@_opt_fact;
typedef const @m@_node *(*@m@_opt_visit)(@m@_opt_context *,const @m@_node *,void *);
typedef const @m@_node *(*@m@_opt_run)(@m@_opt_context *,const @m@_node *,void *);
typedef struct @m@_opt_pass {
  const char *name;
  @m@_opt_run run;
  void *user;
} @m@_opt_pass;
typedef void (*@m@_opt_trace)(void *,const char *,const @m@_node *,const @m@_node *,size_t);
#define @U@_OPT_DISABLE_INFERENCE 1u
typedef struct @m@_opt_options {
  size_t max_steps, max_depth, max_iterations;
  @m@_opt_trace trace;
  void *trace_user;
  unsigned flags; /* bit 0: disable inferred transformations */
} @m@_opt_options;
typedef struct @m@_opt_result {
  const @m@_node *root;
  /* 0 success, 1 invalid input, 2 work/depth/convergence limit,
     3 arena allocation failure, 4 user pass failure. */
  int status;
  size_t steps, rewrites, iterations;
} @m@_opt_result;
PARSLEY_API @m@_opt_result @m@_optimize(psl_arena *,const @m@_node *,
  const @m@_opt_options *,const @m@_opt_pass *,size_t);
PARSLEY_API @m@_opt_fact @m@_opt_fact_of(@m@_opt_context *,const @m@_node *);
PARSLEY_API const @m@_node *@m@_opt_walk(@m@_opt_context *,const @m@_node *,@m@_opt_visit,void *);
PARSLEY_API const @m@_node *@m@_opt_identity(@m@_opt_context *,const @m@_node *,void *);
PARSLEY_API const @m@_node *@m@_opt_make(@m@_opt_context *,@m@_node_tag,
  const @m@_node *const *,size_t,double,int,psl_str,psl_span);
PARSLEY_API void @m@_opt_fail(@m@_opt_context *,int);
#ifdef __cplusplus
}
#endif
#endif
)H";
  substitute(h,"@m@",m);substitute(h,"@U@",up);return h;
}
inline std::string source(const std::vector<Ctor>& cs,const std::string& m,const std::string& up) {
  std::ostringstream o;
  o<<"/* Generated from PSL denotations; do not edit. */\n#include \""<<m<<"_opt.h\"\n#include <math.h>\n#include <string.h>\n";
  o<<"static void "<<m<<"_opt_set_names("<<m<<"_node *n);\n";
  std::string rt(optimizer_runtime::source);substitute(rt,"@m@",m);substitute(rt,"@U@",up);o<<rt;
  std::map<std::string,int> types;int next=5;
  for(const auto& c:cs)if(!types.contains(c.type))types[c.type]=next++;
  o<<"static int o_type(const N *n){if(!n)return 0;switch(n->tag){\n"
    <<"case "<<up<<"_NODE_NUMBER:return 1;case "<<up<<"_NODE_BOOL:return 2;\n"
    <<"case "<<up<<"_NODE_STRING:return 3;case "<<up<<"_NODE_LIST:return 4;\n";
  for(const auto& c:cs)o<<"case "<<c.tag<<":return "<<types[c.type]<<";\n";
  o<<"default:return 0;}}\n";
  o<<"static int o_valid(const N *n){\nif(!n || n->size<sizeof(*n) || n->child_count>SIZE_MAX/sizeof(const N *) || "
    <<"(n->child_count && !n->children) || (n->text_val.len && !n->text_val.ptr))return 0;\nswitch(n->tag){\n"
    <<"case "<<up<<"_NODE_NUMBER:case "<<up<<"_NODE_BOOL:case "<<up<<"_NODE_STRING:return n->child_count==0;\n"
    <<"case "<<up<<"_NODE_LIST:return 1;case "<<up<<"_NODE_UNKNOWN:return n->child_count==0;\n";
  for(const auto& c:cs){
    o<<"case "<<c.tag<<":if(n->child_count!="<<c.fields.size()<<"u)return 0;\n";
    for(std::size_t i=0;i<c.fields.size();++i){
      const auto& t=c.fields[i];int ty=primitive(t);
      if(t.kind==ir::Type::Kind::List)ty=4;
      if(t.kind==ir::Type::Kind::Named && t.name=="Int")ty=1;
      if(!ty && types.contains(t.name))ty=types[t.name];
      o<<"if(!n->children["<<i<<"])return 0;\n";
      if(ty)o<<"if(o_type(n->children["<<i<<"])!="<<ty<<")return 0;\n";
    }
    o<<"return 1;\n";
  }
  o<<"default:return 0;}}\n";
  o<<"static void "<<m<<"_opt_set_names(N *n){switch(n->tag){\n";
  for(const auto& c:cs)o<<"case "<<c.tag<<":n->ctor_name=psl_str_lit("<<lit(c.name)<<");n->type_name=psl_str_lit("<<lit(c.type)<<");break;\n";
  o<<"default:break;}}\n";
  o<<"static const char *o_sem_type(Ctx *cx,const N *n){\n"
    <<"if(!o_enter(cx))return NULL;const char *s=NULL;switch(n->tag){\n";
  for(const auto& c:cs){
    o<<"case "<<c.tag<<":";
    auto key=type_key(c.sem_type);
    if((!c.rule || c.exhaustive) && !key.empty())o<<"s="<<lit(key)<<";";
    else if(c.exhaustive && c.sem_type && c.sem_type->kind==E::Kind::Call && c.sem_type->name=="typeOf" &&
        c.sem_type->args.size()==1 && c.sem_type->args[0]->kind==E::Kind::Var &&
        c.vars.contains(c.sem_type->args[0]->name))
      o<<"s=o_sem_type(cx,n->children["<<c.vars.at(c.sem_type->args[0]->name)<<"]);";
    o<<"break;\n";
  }
  o<<"default:break;}--cx->depth;return s;}\n";
  o<<"static int o_compatible(Ctx *cx,const N *a,const N *b){\n"
    <<"if(o_type(a)!=o_type(b))return 0;const char *ta=o_sem_type(cx,a),*tb=o_sem_type(cx,b);\n"
    <<"return ta && tb && !strcmp(ta,tb);}\n";
  o<<"static Fact o_equation(Ctx *cx,const N *n){(void)cx;switch(n->tag){\n"
    <<"case "<<up<<"_NODE_NUMBER:{Fact f=o_num(n->num_val);f.origin=n;return f;}\n"
    <<"case "<<up<<"_NODE_BOOL:{Fact f=o_bool(n->bool_val);f.origin=n;return f;}\n"
    <<"case "<<up<<"_NODE_STRING:{Fact f=o_text(n->text_val);f.origin=n;return f;}\n";
  for(const auto& c:cs)if(c.eligible){
    o<<"case "<<c.tag<<":{\n";
    int seq=0;auto result=emit_expr(o,c.value,c,seq,m);
    o<<"return "<<result<<";}\n";
  }
  o<<"default:return o_unknown(n);}}\n";
  o<<"static const N *o_reify(Ctx *cx,const N *n,Fact f){\n"
    <<"if(!f.pure || !f.kind)return n;\nconst char *type=o_sem_type(cx,n);if(!type)return n;\n";
  for(const auto& c:cs)if(c.literal && !type_key(c.sem_type).empty()){
    o<<"if(o_type(n)=="<<types[c.type]<<" && f.kind=="<<primitive(c.fields[0])<<" && !strcmp(type,"<<lit(type_key(c.sem_type))<<")){\n";
    o<<"if(n->tag=="<<c.tag<<")return n;\n"
      <<"const N *value="<<m<<"_opt_make(cx,("<<m<<"_node_tag)(f.kind==1 ? "<<up<<"_NODE_NUMBER : (f.kind==2 ? "<<up<<"_NODE_BOOL : "<<up<<"_NODE_STRING)),NULL,0,f.number,f.boolean,f.text,n->span);\n"
      <<"if(!value)return n;\nconst N *children[]={value};\nconst N *out="<<m<<"_opt_make(cx,"<<c.tag<<",children,1,0,0,psl_str_lit(\"\"),n->span);\nreturn out ? out : n;}\n";
  }
  o<<"return n;}\n";
  o<<"const N *"<<m<<"_opt_identity(Ctx *cx,const N *n,void *user){(void)cx;(void)user;return n;}\n";
  return o.str();
}
} // namespace detail

inline std::vector<psl::PslGeneratedFile> generate(const ir::Grammar& grammar,
    const psl::PslModule& sem,const psl::PslCodegenOptions& options={}) {
  const auto m=c23_text::module_stem(options.module_override.empty()?grammar.module_name:options.module_override);
  const auto up=detail::upper(m);
  const auto cs=detail::derive(grammar,sem,up,options.min_status);
  std::ostringstream report;
  report<<"# Inferred optimization plan\n\nTarget: ISO C23. Pass ABI: 1.\n\n"
    <<"Value equations describe complete evaluation results; empty local effects are explicit.\n"
    <<"Unknown equations/effects remain opaque. Custom passes extend the pipeline.\n\n";
  for(const auto& c:cs){
    report<<"- "<<c.name<<": "<<(c.eligible?"inferred":"opaque")<<" — "<<c.reason;
    if(c.literal)report<<"; literal reification";
    report<<"\n";
  }
  report<<"\nBuilt-in passes: memoized bottom-up partial evaluation, constant propagation/folding, "
    <<"semantic projections and known-condition selection. No arithmetic/name heuristics.\n"
    <<"See docs/OPTIMIZER.md in the Parsley source distribution for contracts and pass examples.\n";
  report<<"\nPublic ABI: "<<m<<"_opt.h. Call "<<m
    <<"_optimize(&arena, root, NULL, NULL, 0) after semantic validation.\n"
    <<"Register named callbacks with "<<m<<"_opt_pass; callbacks can use "
    <<m<<"_opt_walk, "<<m<<"_opt_fact_of and "<<m<<"_opt_make.\n"
    <<"Defaults: 1000000 steps, depth 128, 16 rounds. Input is immutable; keep its arena alive.\n"
    <<"Status: 0 success, 1 invalid input, 2 limit, 3 arena failure, 4 user pass failure.\n";
  std::ostringstream json;
  json<<"{\"schema\":1,\"pass_abi\":1,\"target\":\"c23\",\"module\":"<<std::quoted(m)
      <<",\"constructors\":[";
  bool first=true;
  for(const auto& c:cs){
    if(!first)json<<",";
    first=false;
    json<<"{\"name\":"<<std::quoted(c.name)<<",\"type\":"<<std::quoted(c.type)
        <<",\"inferred\":"<<(c.eligible?"true":"false")
        <<",\"literal\":"<<(c.literal?"true":"false")
        <<",\"reason\":"<<std::quoted(c.reason)<<"}";
  }
  json<<"]}\n";
  return {{m+"_opt.h",detail::header(m,up)},{m+"_opt.c",detail::source(cs,m,up)},
          {m+"_opt_plan.md",report.str()},{m+"_opt_plan.json",json.str()}};
}
} // namespace parsley::optimizer