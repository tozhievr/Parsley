module;
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
export module parsley.native_backend;
import parsley.ir;
import parsley.psl;
import parsley.optimizer;
import parsley.c23_text;
import parsley.nextc_bundle;
import parsley.native_driver;

export namespace parsley::native_backend {
namespace detail {
using E=psl::PslExpr;
using Ctor=optimizer::detail::Ctor;
inline void replace(std::string& text,std::string_view from,std::string_view to){
  std::size_t p=0;while((p=text.find(from,p))!=std::string::npos){text.replace(p,from.size(),to);p+=to.size();}
}
inline std::string expr(std::ostringstream& o,const E *e,const Ctor& c,int& seq,const std::string& m){
  const std::string v="v"+std::to_string(++seq);
  auto child=[&](const E *var){return "n->children["+std::to_string(c.vars.at(var->name))+"]";};
  auto need=[&](const std::string& a){o<<"if("<<a<<"==CG_INVALID)return CG_INVALID;\n";};
  switch(e->kind){
    case E::Kind::Number:o<<"cg_value_t "<<v<<"=cg_build_fconst(&cx->builder,CG_T_F64,"<<std::setprecision(17)<<e->number_value<<");\n";break;
    case E::Kind::Bool:o<<"cg_value_t "<<v<<"=cg_build_iconst(&cx->builder,CG_T_I1,"<<(e->bool_value?1:0)<<");\n";break;
    case E::Kind::Var:{
      int kind=optimizer::detail::primitive(c.fields[c.vars.at(e->name)]);
      if(kind==1)o<<"cg_value_t "<<v<<"=cg_build_fconst(&cx->builder,CG_T_F64,"<<child(e)<<"->num_val);\n";
      else if(kind==2)o<<"cg_value_t "<<v<<"=cg_build_iconst(&cx->builder,CG_T_I1,"<<child(e)<<"->bool_val);\n";
      else o<<"cg_value_t "<<v<<"=b_error(cx,\"Text requires a language lowering hook\");\n";
      break;
    }
    case E::Kind::Call:case E::Kind::FieldAccess:
      o<<"cg_value_t "<<v<<"="<<m<<"_backend_node(cx,"<<child(e->args[0].get())<<");\n";break;
    case E::Kind::UnaryOp:{
      auto a=expr(o,e->args[0].get(),c,seq,m);need(a);
      if(e->name=="-")o<<"if(cg_value_type(cx->builder.func,"<<a<<")!=CG_T_F64)return b_error(cx,\"minus expects f64\");\n"
        <<"cg_value_t "<<v<<"=cg_build_fneg(&cx->builder,"<<a<<");\n";
      else o<<"if(cg_value_type(cx->builder.func,"<<a<<")!=CG_T_I1)return b_error(cx,\"not expects bool\");\n"
        <<"cg_value_t "<<v<<"=cg_build_icmp(&cx->builder,CG_ICMP_EQ,"<<a<<",cg_build_iconst(&cx->builder,CG_T_I1,0));\n";
      break;
    }
    case E::Kind::IfThenElse:case E::Kind::BinOp:{
      bool conditional=e->kind==E::Kind::IfThenElse;
      bool lazy=conditional || e->name=="&&" || e->name=="||";
      auto a=expr(o,e->args[0].get(),c,seq,m);need(a);
      if(lazy){
        o<<"if(cg_value_type(cx->builder.func,"<<a<<")!=CG_T_I1)return b_error(cx,\"condition expects bool\");\n"
         <<"cg_block_t yes"<<v<<"=cg_builder_new_block(&cx->builder),no"<<v<<"=cg_builder_new_block(&cx->builder),join"<<v<<"=cg_builder_new_block(&cx->builder);\n"
         <<"cg_build_condbr(&cx->builder,"<<a<<",yes"<<v<<",no"<<v<<");\ncg_builder_set_block(&cx->builder,yes"<<v<<");\n";
        std::string y,z;
        if(conditional || e->name=="&&")y=expr(o,e->args[1].get(),c,seq,m);
        else{y="yesvalue"+v;o<<"cg_value_t "<<y<<"=cg_build_iconst(&cx->builder,CG_T_I1,1);\n";}
        need(y);
        o<<"cg_block_t endyes"<<v<<"=cx->builder.block;\ncg_build_br(&cx->builder,join"<<v<<");\ncg_builder_set_block(&cx->builder,no"<<v<<");\n";
        if(conditional)z=expr(o,e->args[2].get(),c,seq,m);
        else if(e->name=="||")z=expr(o,e->args[1].get(),c,seq,m);
        else{z="novalue"+v;o<<"cg_value_t "<<z<<"=cg_build_iconst(&cx->builder,CG_T_I1,0);\n";}
        need(z);
        o<<"if(cg_value_type(cx->builder.func,"<<y<<")!=cg_value_type(cx->builder.func,"<<z<<"))return b_error(cx,\"branch types differ\");\n"
         <<"cg_block_t endno"<<v<<"=cx->builder.block;\ncg_build_br(&cx->builder,join"<<v<<");\ncg_builder_set_block(&cx->builder,join"<<v<<");\n"
         <<"cg_value_t "<<v<<"=cg_build_phi(&cx->builder,cg_value_type(cx->builder.func,"<<y<<"));\n"
         <<"if(!cg_phi_add_incoming(cx->builder.func,"<<v<<","<<y<<",endyes"<<v<<") || !cg_phi_add_incoming(cx->builder.func,"<<v<<","<<z<<",endno"<<v<<"))return b_error(cx,\"cannot build phi\");\n";
      }else{
        auto b=expr(o,e->args[1].get(),c,seq,m);need(b);
        o<<"if(cg_value_type(cx->builder.func,"<<a<<")!=cg_value_type(cx->builder.func,"<<b<<"))return b_error(cx,\"operand types differ\");\n";
        const std::map<std::string,std::string> arith={{"+","FADD"},{"-","FSUB"},{"*","FMUL"},{"/","FDIV"}};
        if(arith.contains(e->name)){
          o<<"if(cg_value_type(cx->builder.func,"<<a<<")!=CG_T_F64)return b_error(cx,\"arithmetic expects f64\");\n"
           <<"cg_value_t "<<v<<"=cg_build_binary(&cx->builder,CG_OP_"<<arith.at(e->name)<<","<<a<<","<<b<<");\n";
        }else{
          const std::map<std::string,std::string> cmp={{"==","OEQ"},{"/=","UNE"},{"<","OLT"},{"<=","OLE"},{">","OGT"},{">=","OGE"}};
          o<<"cg_value_t "<<v<<"=CG_INVALID;\nif(cg_value_type(cx->builder.func,"<<a<<")==CG_T_F64)"
           <<v<<"=cg_build_fcmp(&cx->builder,CG_FCMP_"<<cmp.at(e->name)<<","<<a<<","<<b<<");\n";
          if(e->name=="==" || e->name=="/=")o<<"else if(cg_value_type(cx->builder.func,"<<a<<")==CG_T_I1)"
            <<v<<"=cg_build_icmp(&cx->builder,CG_ICMP_"<<(e->name=="=="?"EQ":"NE")<<","<<a<<","<<b<<");\n";
          o<<"else return b_error(cx,\"comparison requires scalar language lowering\");\n";
        }
      }
      break;
    }
    default:o<<"cg_value_t "<<v<<"=b_error(cx,\"unsupported semantic expression\");\n";break;
  }
  need(v);return v;
}
inline std::string bundle_file(const nextc_bundle::File& file){
  if(file.path.ends_with(".c"))return "#define _CRT_SECURE_NO_WARNINGS 1\n"+std::string(file.content);
  if(!file.path.ends_with(".h"))return std::string(file.content);
  std::string out=R"API(#ifndef PARSLEY_API
#if defined(PARSLEY_STATIC)
#define PARSLEY_API
#elif defined(_WIN32)
#if defined(PARSLEY_BUILDING_DLL)
#define PARSLEY_API __declspec(dllexport)
#else
#define PARSLEY_API __declspec(dllimport)
#endif
#else
#define PARSLEY_API
#endif
#endif
)API";
  std::istringstream input{std::string(file.content)};std::string line;
  while(std::getline(input,line)){
    auto paren=line.find('(');
    if(paren!=std::string::npos && !line.starts_with("static ") && !line.starts_with("typedef ") &&
       !line.starts_with("#") && !line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))){
      auto begin=paren;
      while(begin && (std::isalnum(static_cast<unsigned char>(line[begin-1])) || line[begin-1]=='_'))--begin;
      if(std::string_view(line).substr(begin,paren-begin).starts_with("cg_"))out+="PARSLEY_API ";
    }
    out+=line;out+='\n';
  }
  return out;
}
inline std::string header(std::string m,std::string up){
  std::string h=R"NB(/* Generated C23 frontend-to-nextc lowering ABI 1. */
#ifndef @U@_BACKEND_H
#define @U@_BACKEND_H
#include "@m@_ast.h"
#ifdef __cplusplus
extern "C" {
#endif
#ifndef PARSLEY_NEXTC_PUBLIC_API
#define PARSLEY_NEXTC_PUBLIC_API
#include "nextc/cg.h"
#endif
typedef struct @m@_backend_context @m@_backend_context;
/* true: handled, false: use inferred lowering. CG_INVALID means failure.
   Use backend_node to recursively lower child expressions in the current block. */
typedef bool (*@m@_lower_hook)(@m@_backend_context *,const @m@_node *,cg_value_t *,void *);
typedef struct @m@_mir_pass {
  const char *name;
  bool (*run)(cg_module_t *,cg_diag_t *,void *);
  void *user;
} @m@_mir_pass;
typedef struct @m@_backend_options {
  size_t max_depth, max_steps; /* zero selects defaults: 128, 1000000 */
  @m@_lower_hook lower;
  void *user;
  const @m@_mir_pass *passes;
  size_t pass_count;
} @m@_backend_options;
PARSLEY_API cg_builder_t *@m@_backend_builder(@m@_backend_context *);
PARSLEY_API cg_diag_t *@m@_backend_diagnostic(@m@_backend_context *);
PARSLEY_API cg_value_t @m@_backend_node(@m@_backend_context *,const @m@_node *);
PARSLEY_API cg_module_t *@m@_backend_lower(const @m@_node *,const char *,
  cg_type_t,const cg_type_t *,uint32_t,const @m@_backend_options *,cg_diag_t *);
#ifdef __cplusplus
}
#endif
#endif
)NB";
  replace(h,"@m@",m);replace(h,"@U@",up);return h;
}
inline const E *literal_operand(const Ctor& c, psl::PslStatus minimum,
                                std::string_view intrinsic, int primitive_kind) {
  if (!c.rule || !c.exhaustive || !c.value || c.value->kind != E::Kind::Call ||
      c.value->name != intrinsic || c.value->args.size() != 1) return nullptr;
  const E *arg = c.value->args[0].get();
  if (!arg || arg->kind != E::Kind::Var || !c.vars.contains(arg->name) ||
      optimizer::detail::primitive(c.fields[c.vars.at(arg->name)]) != primitive_kind) return nullptr;
  const auto& provenance = c.rule->provenance;
  if (!provenance.has_status || !provenance.has_confidence || provenance.source_name.empty() ||
      static_cast<int>(provenance.status) < static_cast<int>(minimum)) return nullptr;
  bool pure = false;
  for (const auto& st : c.rule->stmts) {
    if (st.kind != psl::PslNodeStmt::Kind::AssignAttr) return nullptr;
    if (st.target_var == "effects") {
      if (pure || !optimizer::detail::empty_effects(st.expr.get())) return nullptr;
      pure = true;
    } else if (st.target_var == "value") {
      if (st.expr.get() != c.value) return nullptr;
    } else if (st.target_var != "type") return nullptr;
  }
  return pure ? arg : nullptr;
}

// A native arithmetic equation identifies the AST operands and names of
// the operator constructors. It does not infer operator meaning from names.
inline bool emit_i32_arithmetic(std::ostringstream& out, const Ctor& c,
                                const std::string& module, psl::PslStatus minimum) {
  if (!c.rule || !c.exhaustive) return false;
  const auto& provenance = c.rule->provenance;
  if (!provenance.has_status || !provenance.has_confidence || provenance.source_name.empty() ||
      static_cast<int>(provenance.status) < static_cast<int>(minimum)) return false;
  const E *equation = nullptr;
  const E *comparison = nullptr;
  for (const auto& st : c.rule->stmts) {
    if (st.kind != psl::PslNodeStmt::Kind::AssignAttr) continue;
    if (st.target_var == "nativeI32") {
      if (equation) return false;
      equation = st.expr.get();
    } else if (st.target_var == "nativeCompare") {
      if (comparison) return false;
      comparison = st.expr.get();
    }
  }
  const bool comparison_only = !equation && comparison;
  if (comparison_only) equation = comparison;
  if (!equation || equation->kind != E::Kind::Call) return false;
  const bool unary = equation->name == "i32Negation" && equation->args.size() == 3;
  const bool binary = equation->name == "i32Binary" && equation->args.size() == 4;
  if (!unary && !binary && !comparison_only) return false;
  if (comparison && (unary || comparison->kind != E::Kind::Call ||
                     comparison->name != "i32Comparison" || comparison->args.size() != 4)) return false;
  const std::size_t operands = unary ? 2 : 3;
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < operands; ++i) {
    const auto& arg = equation->args[i];
    if (!arg || arg->kind != E::Kind::Var || !c.vars.contains(arg->name)) return false;
    indices.push_back(c.vars.at(arg->name));
  }
  const auto& operations = equation->args[operands];
  if (!operations) return false;
  std::vector<std::string> names;
  auto add_name = [&](const E *e) {
    if (!e || e->kind != E::Kind::Ctor || !e->args.empty() ||
        std::find(names.begin(), names.end(), e->name) != names.end()) return false;
    names.push_back(e->name);
    return true;
  };
  if (unary) {
    if (!add_name(operations.get())) return false;
  } else {
    if (!comparison_only && (operations->kind != E::Kind::List || operations->args.size() != 3)) return false;
    if (!comparison_only)
      for (const auto& op : operations->args) if (!add_name(op.get())) return false;
  }

  const auto arithmetic_count = names.size();
  if (comparison) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
      const auto& arg = comparison->args[i];
      if (!arg || arg->kind != E::Kind::Var || arg->name != equation->args[i]->name) return false;
    }
    const auto& predicates = comparison->args[3];
    if (!predicates || predicates->kind != E::Kind::List || predicates->args.size() != 6) return false;
    for (const auto& predicate : predicates->args) if (!add_name(predicate.get())) return false;
  }

  out << "case " << c.tag << ":{\n"
      << "const N *op=n->children[" << indices[0] << "];\n"
      << "if(op->child_count)return b_error(cx,\"invalid i32 operator\");\n"
      << "unsigned operation;\n";
  for (std::size_t i = 0; i < names.size(); ++i) {
    out << (i ? "else if" : "if") << "(psl_str_eq(op->ctor_name,psl_str_lit("
        << c23_text::literal(names[i]) << ")))operation=" << i << "u;\n";
  }
  out << "else return b_error(cx,\"no declarative i32 lowering for this operator\");\n"
      << "cg_value_t left=" << module << "_backend_node(cx,n->children[" << indices[1] << "]);\n"
      << "if(left==CG_INVALID)return CG_INVALID;\n"
      << "if(cg_value_type(cx->builder.func,left)!=CG_T_I32)return b_error(cx,\"i32 operand required\");\n";
  if (unary) {
    out << "(void)operation;return cg_build_binary(&cx->builder,CG_OP_SUB,"
        << "cg_build_iconst(&cx->builder,CG_T_I32,0),left);}\n";
  } else {
    out << "cg_value_t right=" << module << "_backend_node(cx,n->children[" << indices[2] << "]);\n"
        << "if(right==CG_INVALID)return CG_INVALID;\n"
        << "if(cg_value_type(cx->builder.func,right)!=CG_T_I32)return b_error(cx,\"i32 operand required\");\n"
        << "switch(operation){\n";
    static constexpr std::string_view opcodes[] = {"CG_OP_ADD", "CG_OP_SUB", "CG_OP_MUL"};
    static constexpr std::string_view predicates[] = {
      "CG_ICMP_EQ", "CG_ICMP_NE", "CG_ICMP_SLT", "CG_ICMP_SLE", "CG_ICMP_SGT", "CG_ICMP_SGE"
    };
    for (std::size_t i = 0; i < arithmetic_count; ++i)
      out << "case " << i << ":return cg_build_binary(&cx->builder," << opcodes[i] << ",left,right);\n";
    for (std::size_t i = arithmetic_count; i < names.size(); ++i)
      out << "case " << i << ":return cg_build_icmp(&cx->builder," << predicates[i - arithmetic_count] << ",left,right);\n";
    out << "default:return b_error(cx,\"invalid i32 operation\");}}\n";
  }
  return true;
}

inline bool emit_singleton_select(std::ostringstream& out, const Ctor& c,
                                  const std::string& m, psl::PslStatus minimum) {
  if (!c.rule || !c.exhaustive) return false;
  const auto& provenance = c.rule->provenance;
  if (!provenance.has_status || !provenance.has_confidence || provenance.source_name.empty() ||
      static_cast<int>(provenance.status) < static_cast<int>(minimum)) return false;
  const E *equation = nullptr;
  for (const auto& st : c.rule->stmts) {
    if (st.kind != psl::PslNodeStmt::Kind::AssignAttr || st.target_var != "nativeSelect") continue;
    if (equation) return false;
    equation = st.expr.get();
  }
  if (!equation || equation->kind != E::Kind::Call || equation->name != "selectSingletons" ||
      equation->args.size() != 3) return false;
  std::vector<std::size_t> fields;
  for (const auto& arg : equation->args) {
    if (!arg || arg->kind != E::Kind::Var || !c.vars.contains(arg->name)) return false;
    fields.push_back(c.vars.at(arg->name));
  }

  const auto list = optimizer::detail::upper(m) + "_NODE_LIST";
  out << "case " << c.tag << ":{\n"
      << "const N *yes=n->children[" << fields[1] << "],*no=n->children[" << fields[2] << "];\n"
      << "if(yes->tag!=" << list << " || no->tag!=" << list
      << " || yes->child_count!=1 || no->child_count!=1 || !yes->children || !no->children)"
      << "return b_error(cx,\"selectSingletons requires one statement in each branch\");\n"
      << "cg_value_t condition=" << m << "_backend_node(cx,n->children[" << fields[0] << "]);\n"
      << "if(condition==CG_INVALID)return CG_INVALID;\n"
      << "if(cg_value_type(cx->builder.func,condition)!=CG_T_I1)return b_error(cx,\"condition expects bool\");\n"
      << "cg_block_t then_block=cg_builder_new_block(&cx->builder),"
      << "else_block=cg_builder_new_block(&cx->builder),join=cg_builder_new_block(&cx->builder);\n"
      << "cg_build_condbr(&cx->builder,condition,then_block,else_block);\n"
      << "cg_builder_set_block(&cx->builder,then_block);\n"
      << "cg_value_t then_value=" << m << "_backend_node(cx,yes->children[0]);\n"
      << "if(then_value==CG_INVALID)return CG_INVALID;\n"
      << "cg_block_t then_end=cx->builder.block;\ncg_build_br(&cx->builder,join);\n"
      << "cg_builder_set_block(&cx->builder,else_block);\n"
      << "cg_value_t else_value=" << m << "_backend_node(cx,no->children[0]);\n"
      << "if(else_value==CG_INVALID)return CG_INVALID;\n"
      << "cg_type_t type=cg_value_type(cx->builder.func,then_value);\n"
      << "if(type!=cg_value_type(cx->builder.func,else_value))return b_error(cx,\"branch types differ\");\n"
      << "cg_block_t else_end=cx->builder.block;\ncg_build_br(&cx->builder,join);\n"
      << "cg_builder_set_block(&cx->builder,join);\ncg_value_t result=cg_build_phi(&cx->builder,type);\n"
      << "if(!cg_phi_add_incoming(cx->builder.func,result,then_value,then_end) || "
      << "!cg_phi_add_incoming(cx->builder.func,result,else_value,else_end))"
      << "return b_error(cx,\"cannot build phi\");\nreturn result;}\n";
  return true;
}

inline std::string source(const std::vector<Ctor>& ctors,const std::string& m,
                          psl::PslStatus minimum){
  std::ostringstream o;
  o<<"#include \""<<m<<"_backend.h\"\n#include <string.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
   <<"struct "<<m<<"_backend_context {cg_builder_t builder;cg_diag_t *diag;"<<m<<"_backend_options options;size_t depth,steps;};\n"
   <<"typedef "<<m<<"_backend_context Ctx;\ntypedef "<<m<<"_node N;\n"
   <<"static cg_value_t b_error(Ctx *cx,const char *message){cg_diag_error(cx->diag,\"%s\",message);return CG_INVALID;}\n"
   <<"cg_builder_t *"<<m<<"_backend_builder(Ctx *cx){return cx ? &cx->builder : NULL;}\n"
   <<"cg_diag_t *"<<m<<"_backend_diagnostic(Ctx *cx){return cx ? cx->diag : NULL;}\n"
   ;
  if (std::any_of(ctors.begin(), ctors.end(), [&](const Ctor& c) {
        return literal_operand(c, minimum, "cstrQuoted", 3) != nullptr;
      })) {
    o << R"CSTR(static cg_value_t b_quoted_cstr(Ctx *cx, psl_str text) {
  if(!text.ptr || text.len<2 || text.len>UINT32_MAX ||
     text.ptr[0]!='"' || text.ptr[text.len-1]!='"')
    return b_error(cx,"cstrQuoted requires a quoted string");
  char *bytes=malloc(text.len);
  if(!bytes)return b_error(cx,"cannot allocate string");
  size_t size=0;
  bool valid=true;
  for(size_t i=1;i<text.len-1 && valid;++i){
    char ch=text.ptr[i];
    if(ch=='\\'){
      if(++i>=text.len-1){valid=false;break;}
      ch=text.ptr[i];
      switch(ch){
        case 'n':ch='\n';break;
        case 'r':ch='\r';break;
        case 't':ch='\t';break;
        case 'b':ch='\b';break;
        case '"':case '\'':case '\\':case '$':break;
        default:valid=false;break;
      }
    }else if(ch=='$' && i+1<text.len-1){
      const unsigned char next=(unsigned char)text.ptr[i+1];
      valid=next!='{' && next!='_' && next<128 &&
            !(next>='a' && next<='z') && !(next>='A' && next<='Z');
    }else if(ch=='\0' || ch=='\r' || ch=='\n' || ch=='"')valid=false;
    if(valid)bytes[size++]=ch;
  }
  if(!valid){free(bytes);return b_error(cx,"unsupported cstrQuoted escape or interpolation");}
  bytes[size++]='\0';
  char name[48];
  snprintf(name,sizeof(name),"parsley_cstr_%u",(unsigned)cx->builder.module->symbols.count);
  const cg_symbol_t symbol=cg_module_add_data(cx->builder.module,name,(uint32_t)size,1,
                                             bytes,CG_DATA_RO,CG_BIND_LOCAL);
  free(bytes);
  return symbol==CG_INVALID ? CG_INVALID : cg_build_symaddr(&cx->builder,symbol);
}
)CSTR";
  }
  o<<"static cg_value_t b_infer(Ctx *cx,const N *n){(void)cx;switch(n->tag){\n";
  for(const auto& c:ctors) {
    if (const E *arg = literal_operand(c, minimum, "i32Literal", 1)) {
      o << "case " << c.tag << ":{\n"
        << "const N *field=n->children[" << c.vars.at(arg->name) << "];\n"
        << "if(field->tag!=" << optimizer::detail::upper(m) << "_NODE_NUMBER)"
        << "return b_error(cx,\"i32Literal requires a numeric field\");\n"
        << "const double value=field->num_val;\n"
        << "if(!(value>=INT32_MIN && value<=INT32_MAX) || value!=(double)(int32_t)value)"
        << "return b_error(cx,\"i32Literal requires an exact signed 32-bit integer\");\n"
        << "return cg_build_iconst(&cx->builder,CG_T_I32,(int32_t)value);}\n";
      continue;
    }
    if (const E *arg = literal_operand(c, minimum, "cstrQuoted", 3)) {
      o << "case " << c.tag << ":{\n"
        << "const N *field=n->children[" << c.vars.at(arg->name) << "];\n"
        << "if(field->tag!=" << optimizer::detail::upper(m) << "_NODE_STRING)"
        << "return b_error(cx,\"cstrQuoted requires a text field\");\n"
        << "return b_quoted_cstr(cx,field->text_val);}\n";
      continue;
    }
    if (emit_i32_arithmetic(o, c, m, minimum) || emit_singleton_select(o, c, m, minimum)) continue;
    if(!c.eligible)continue;
    o<<"case "<<c.tag<<":{\n";int seq=0;auto result=expr(o,c.value,c,seq,m);o<<"return "<<result<<";}\n";
  }
  o<<"default:return b_error(cx,\"constructor needs a language lowering hook\");}}\n";
  o<<"cg_value_t "<<m<<"_backend_node(Ctx *cx,const N *n){\n"
   <<"if(!cx)return CG_INVALID;\nif(!n || n->size<sizeof(*n) || (n->child_count && !n->children))return b_error(cx,\"invalid AST node\");\n"
   <<"if(cx->depth>=cx->options.max_depth || cx->steps++>=cx->options.max_steps)return b_error(cx,\"lowering resource limit\");\n"
   <<"switch(n->tag){\n";
  for(const auto& c:ctors)o<<"case "<<c.tag<<":if(n->child_count!="<<c.fields.size()<<"u)return b_error(cx,\"invalid constructor arity\");break;\n";
  o<<"default:break;}\nif(n->child_count>cx->options.max_steps)return b_error(cx,\"child budget exceeded\");\nfor(size_t i=0;i<n->child_count;++i)if(!n->children[i] || n->children[i]->size<sizeof(N))return b_error(cx,\"invalid child node\");\n++cx->depth;\ncg_value_t value=CG_INVALID;\n"
   <<"if(!cx->options.lower || !cx->options.lower(cx,n,&value,cx->options.user))value=b_infer(cx,n);\n"
   <<"--cx->depth;\nif(value==CG_INVALID && !cx->diag->error_count)b_error(cx,\"language hook failed\");\nreturn value;}\n";
  o<<"cg_module_t *"<<m<<"_backend_lower(const N *root,const char *symbol,cg_type_t result,const cg_type_t *params,uint32_t count,const "<<m<<"_backend_options *options,cg_diag_t *diag){\n"
   <<"if(!diag)return NULL;\nif(!root || !symbol || !*symbol || (count && !params) || result==CG_T_VOID || result>=CG_T_COUNT){cg_diag_error(diag,\"invalid lowering arguments\");return NULL;}\n"
   <<"Ctx cx={0};cx.diag=diag;if(options)cx.options=*options;\n"
   <<"if(!cx.options.max_depth)cx.options.max_depth=128;if(!cx.options.max_steps)cx.options.max_steps=1000000;\n"
   <<"if(cx.options.max_depth>512 || (cx.options.pass_count && !cx.options.passes)){cg_diag_error(diag,\"invalid lowering options\");return NULL;}\n"
   <<"cg_module_t *module=cg_module_create();if(!module){cg_diag_error(diag,\"cannot allocate MIR module\");return NULL;}\n"
   <<"cg_func_t *function=cg_module_add_function(module,symbol,result,params,count,false,CG_BIND_GLOBAL);\n"
   <<"if(!function){cg_diag_error(diag,\"cannot allocate MIR function\");cg_module_destroy(module);return NULL;}\n"
   <<"cg_builder_init(&cx.builder,module,function);\ncg_value_t value="<<m<<"_backend_node(&cx,root);\n"
   <<"if(value==CG_INVALID || diag->error_count || cg_value_type(function,value)!=result){if(!diag->error_count)cg_diag_error(diag,\"entry result type differs from declared return type\");cg_module_destroy(module);return NULL;}\n"
   <<"cg_build_ret(&cx.builder,value);\nfor(size_t i=0;i<cx.options.pass_count;++i){const "<<m<<"_mir_pass *pass=&cx.options.passes[i];\n"
   <<"if(!pass->run || !pass->name || !pass->run(module,diag,pass->user)){if(!diag->error_count)cg_diag_error(diag,\"MIR extension pass failed\");cg_module_destroy(module);return NULL;}}\n"
   <<"if(!cg_module_ok(module) || diag->error_count || !cg_verify_module(module,diag)){cg_module_destroy(module);return NULL;}\nreturn module;}\n";
  return o.str();
}
} // namespace detail
inline std::vector<psl::PslGeneratedFile> generate(const ir::Grammar& g,const psl::PslModule& sem,const psl::PslCodegenOptions& options, const std::optional<std::string>& adapter){
  const std::string m=c23_text::module_stem(options.module_override.empty()?g.module_name:options.module_override);
  const auto up=optimizer::detail::upper(m);
  auto ctors=optimizer::detail::derive(g,sem,up,options.min_status);
  std::vector<psl::PslGeneratedFile> files={{m+"_backend.h",detail::header(m,up)},{m+"_backend.c",detail::source(ctors,m,options.min_status)}};
  files.push_back({"tools/"+m+"_compiler.c",native_driver::generate(m, adapter.has_value())});
  if(adapter)files.push_back({"tools/native_adapter.h",*adapter});
  for(const auto& file:nextc_bundle::files)files.push_back({std::string(file.path),detail::bundle_file(file)});
  return files;
}
}