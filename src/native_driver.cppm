module;
#include <string>
#include <string_view>
export module parsley.native_driver;
import parsley.version;
export namespace parsley::native_driver {
inline std::string generate(std::string_view name, bool custom){
 std::string text=R"DRV(/* Generated standalone language compiler backed by nextc. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../@m@_parse.h"
#include "../@m@_sem.h"
#include "../@m@_opt.h"
#include "../@m@_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifndef PARSLEY_NATIVE_RESULT
#define PARSLEY_NATIVE_RESULT CG_T_F64
#define PARSLEY_NATIVE_RETURN "f64"
#define PARSLEY_NATIVE_OPTIONS NULL
#endif
static void usage(FILE *out){
 fputs("Usage: @m@_compiler SOURCE [options]\n"
       "  -h, --help                 Show this help\n"
       "  -V, --version              Show the Parsley generator version\n"
       "  --run                      Execute with the Windows JIT\n"
       "  --emit FORMAT              mir, asm, object (default), exe, or flat\n"
       "  -o FILE                    Required for object, exe and flat output\n"
       "  --return TYPE              f64, bool, i32, i64, or cstr (default: " PARSLEY_NATIVE_RETURN ")\n"
       "  --os TARGET                windows, linux, or baremetal\n"
       "  -O0, -O1                   Select nextc optimization level\n"
       "  --no-ast-opt               Skip AST optimization\n",out);
}
static int errors;
static void diagnostic(void *user,const psl_diag *diag){
 if(!diag)return;
 if(diag->severity==PSL_SEV_ERROR)++errors;
 if(user && diag->def){
   const char *path=*(const char *const *)user;
   const psl_str code=diag->def->code;
   fprintf(stderr,"%s:%" PRIu32 ":%" PRIu32 ": %.*s\n",path,diag->span.line,diag->span.col,
           (int)code.len,code.ptr);
 }
}
static char *read_source(const char *path,size_t *length){
 FILE *f=fopen(path,"rb");if(!f)return NULL;
 if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long n=ftell(f);
 if(n<0 || n>64L*1024L*1024L || fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
 char *s=malloc((size_t)n+1u);if(!s){fclose(f);return NULL;}
 if(fread(s,1,(size_t)n,f)!=(size_t)n){free(s);fclose(f);return NULL;}
 fclose(f);s[n]=0;*length=(size_t)n;return s;
}
static int write_output(const char *path,const cg_buffer_t *buffer){
 FILE *f=path ? fopen(path,"wb") : stdout;if(!f)return 0;
 int ok=fwrite(buffer->bytes.data,1,buffer->bytes.count,f)==buffer->bytes.count;
 if(path){if(fclose(f))ok=0;}else if(fflush(f))ok=0;
 return ok;
}
static int add_main(cg_module_t *module,cg_type_t result){
 cg_symbol_t entry=cg_module_symbol(module,"language_main");
 cg_func_t *main_function=cg_module_add_function(module,"main",CG_T_I32,NULL,0,false,CG_BIND_GLOBAL);
 if(!main_function)return 0;
 cg_builder_t b;cg_builder_init(&b,module,main_function);
 cg_value_t value=cg_build_call(&b,entry,result,NULL,0,false);
 if(result==CG_T_F64)value=cg_build_cast(&b,CG_OP_FPTOSI,CG_T_I32,value);
 else if(result==CG_T_I1)value=cg_build_cast(&b,CG_OP_ZEXT,CG_T_I32,value);
 else if(result==CG_T_I64)value=cg_build_cast(&b,CG_OP_TRUNC,CG_T_I32,value);
 else if(result!=CG_T_I32)return 0;
 cg_build_ret(&b,value);return cg_module_ok(module);
}
int main(int argc,char **argv){
 const char *input=NULL,*output=NULL,*emit="object";
 int run=0,ast_opt=1;cg_type_t result=PARSLEY_NATIVE_RESULT;
 cg_options_t options=cg_default_options();
 for(int i=1;i<argc;++i){
   if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")){usage(stdout);return 0;}
   if(!strcmp(argv[i],"--version") || !strcmp(argv[i],"-V")){puts("@m@_compiler (Parsley @version@)");return 0;}
   if(!strcmp(argv[i],"--run"))run=1;
   else if(!strcmp(argv[i],"--no-ast-opt"))ast_opt=0;
   else if(!strcmp(argv[i],"-O0")){options.opt_level=0;options.optimize_ir=false;}
   else if(!strcmp(argv[i],"-O1")){options.opt_level=1;options.optimize_ir=true;}
   else if((!strcmp(argv[i],"-o") || !strcmp(argv[i],"--emit") || !strcmp(argv[i],"--return") || !strcmp(argv[i],"--os")) && i+1<argc){
     const char *key=argv[i],*value=argv[++i];
     if(!strcmp(key,"-o"))output=value;
     else if(!strcmp(key,"--emit"))emit=value;
     else if(!strcmp(key,"--return")){
       if(!strcmp(value,"f64"))result=CG_T_F64;
       else if(!strcmp(value,"bool"))result=CG_T_I1;
       else if(!strcmp(value,"i32"))result=CG_T_I32;
       else if(!strcmp(value,"i64"))result=CG_T_I64;
       else if(!strcmp(value,"cstr"))result=CG_T_PTR;
       else{fprintf(stderr,"unknown return type\n");return 2;}
     }else{
       if(!strcmp(value,"windows"))options.os=CG_OS_WINDOWS;
       else if(!strcmp(value,"linux"))options.os=CG_OS_LINUX;
       else if(!strcmp(value,"baremetal"))options.os=CG_OS_BAREMETAL;
       else{fprintf(stderr,"unknown target OS\n");return 2;}
     }
   }else if(argv[i][0]=='-'){fprintf(stderr,"unknown or incomplete option: %s\n",argv[i]);return 2;}
   else if(input){fprintf(stderr,"only one input file is supported\n");return 2;}
   else input=argv[i];
 }
 if(!input){usage(stderr);return 2;}
 if(strcmp(emit,"mir") && strcmp(emit,"asm") && strcmp(emit,"object") && strcmp(emit,"exe") && strcmp(emit,"flat")){
   fprintf(stderr,"unknown emission format\n");return 2;
 }
 if(!run && result==CG_T_PTR && (!strcmp(emit,"exe") || !strcmp(emit,"flat"))){
   fprintf(stderr,"cstr results require --run or a caller that consumes the pointer ABI\n");return 2;
 }
 if(!strcmp(emit,"exe") && options.os!=CG_OS_WINDOWS){fprintf(stderr,"PE output requires the Windows ABI\n");return 2;}
 if(!run && !output && strcmp(emit,"mir") && strcmp(emit,"asm")){
   fprintf(stderr,"binary output requires -o FILE\n");return 2;
 }
 size_t length=0;char *source=read_source(input,&length);
 unsigned char *parse_memory=malloc(16u*1024u*1024u),*analysis_memory=malloc(16u*1024u*1024u),*opt_memory=malloc(16u*1024u*1024u);
 if(!source || !parse_memory || !analysis_memory || !opt_memory){fprintf(stderr,"cannot read source or allocate frontend arenas\n");free(source);free(parse_memory);free(analysis_memory);free(opt_memory);return 1;}
 psl_arena arena,analysis,optimized;
 psl_arena_init(&arena,parse_memory,16u*1024u*1024u);psl_arena_init(&analysis,analysis_memory,16u*1024u*1024u);psl_arena_init(&optimized,opt_memory,16u*1024u*1024u);
 const @m@_node *root=NULL;psl_diag parse_error={0};cg_module_t *module=NULL;cg_jit_t *jit=NULL;cg_buffer_t bytes={0};cg_diag_t diag={0};int status=1;
 if(@m@_parse(&arena,psl_str_make(source,length),&root,&parse_error)<0){
   if(parse_error.message.ptr)
     fprintf(stderr,"%s:%" PRIu32 ":%" PRIu32 ": %.*s\n", input,parse_error.span.line,
             parse_error.span.col,(int)parse_error.message.len,parse_error.message.ptr);
   else fprintf(stderr,"source parse failed\n");
   goto cleanup;
 }
 @m@_ctx *semantic=@m@_ctx_new(&analysis,NULL);
 if(!semantic || @m@_analyze(semantic,root,diagnostic,NULL)<0 || errors){fprintf(stderr,"semantic validation failed (%d errors)\n",errors);goto cleanup;}
#ifdef PARSLEY_NATIVE_CHECK
 if(PARSLEY_NATIVE_CHECK(root,diagnostic,&input)<0 || errors){
   fprintf(stderr,"project semantic validation failed (%d errors)\n",errors);
   goto cleanup;
 }
#endif
 const @m@_node *entry=NULL;
 const int selected=@m@_native_entry(semantic,result==CG_T_I32 ? 1 : result==CG_T_PTR ? 2 : 0,&entry);
 if(selected<0){fprintf(stderr,"missing, ambiguous or unsupported declarative native entry\n");goto cleanup;}
 if(selected>0)root=entry;
 if(ast_opt){
   @m@_opt_result opt=@m@_optimize(&optimized,root,NULL,NULL,0);
   if(opt.status){fprintf(stderr,"AST optimization failed: %d\n",opt.status);goto cleanup;}root=opt.root;
 }
 module=@m@_backend_lower(root,"language_main",result,NULL,0,PARSLEY_NATIVE_OPTIONS,&diag);
 if(!module)goto cleanup;
 if(run){
   if(options.os!=CG_OS_WINDOWS){cg_diag_error(&diag,"JIT execution is available only for the Windows host");goto cleanup;}
   jit=cg_jit_create(module,&options,NULL,&diag);if(!jit)goto cleanup;
   void *address=cg_jit_symbol(jit,"language_main");if(!address){cg_diag_error(&diag,"entry symbol not found");goto cleanup;}
   if(result==CG_T_F64){double (*f)(void);_Static_assert(sizeof(f)==sizeof(address),"function address width");memcpy(&f,&address,sizeof(f));printf("%.17g\n",f());}
   else if(result==CG_T_I1){bool (*f)(void);memcpy(&f,&address,sizeof(f));printf("%d\n",(int)f());}
   else if(result==CG_T_I32){int32_t (*f)(void);memcpy(&f,&address,sizeof(f));printf("%" PRId32 "\n",f());}
   else if(result==CG_T_PTR){
     const char *(*f)(void);
     memcpy(&f,&address,sizeof(f));
     const char *value=f();
     if(!value){cg_diag_error(&diag,"cstr entry returned null");goto cleanup;}
     puts(value);
   }
   else{int64_t (*f)(void);memcpy(&f,&address,sizeof(f));printf("%" PRId64 "\n",f());}
   status=0;goto cleanup;
 }
 bool ok=false;
 if(!strcmp(emit,"mir"))ok=cg_print_module(module,&bytes);
 else if(!strcmp(emit,"asm"))ok=cg_compile_to_asm(module,&options,&bytes,&diag);
 else if(!strcmp(emit,"object"))ok=cg_compile_to_object(module,&options,&bytes,&diag);
 else{
   if(!add_main(module,result)){cg_diag_error(&diag,"cannot build executable entry adapter");goto cleanup;}
   cg_image_options_t image={0};image.kind=!strcmp(emit,"exe") ? CG_IMAGE_PE : CG_IMAGE_FLAT;image.entry="main";
   const char *dlls[]={"kernel32.dll"};image.dlls=dlls;image.dll_count=1;
   cg_module_t *modules[]={module};ok=cg_link_image(modules,1,&options,&image,&bytes,&diag);
 }
 if(ok && write_output(output,&bytes))status=0;
 else if(!diag.error_count)cg_diag_error(&diag,"cannot emit or write output");
cleanup:
 if(diag.length)fprintf(stderr,"%s",diag.text);
 cg_jit_destroy(jit);cg_module_destroy(module);cg_buffer_free(&bytes);
 free(source);free(parse_memory);free(analysis_memory);free(opt_memory);return status;
}
)DRV";
 std::size_t p=0;while((p=text.find("@m@",p))!=std::string::npos){text.replace(p,3,name);p+=name.size();}
 text.replace(text.find("@version@"),9,parsley::VERSION_STRING);
 if(custom)text.insert(text.find("#ifndef PARSLEY_NATIVE_RESULT"), "#include \"native_adapter.h\"\n");
 return text;
}
}