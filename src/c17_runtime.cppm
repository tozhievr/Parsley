module;
#include <string_view>
export module parsley.c17_runtime;
export namespace parsley::c17_runtime {
inline constexpr std::string_view source = R"C17(
#include <setjmp.h>
#include <limits.h>
#include <errno.h>
#include <float.h>
#include <locale.h>
#ifndef PARSLEY_MAX_DEPTH
#define PARSLEY_MAX_DEPTH 128
#endif
#ifndef PARSLEY_MAX_BYTES
#define PARSLEY_MAX_BYTES (64u * 1024u * 1024u)
#endif
#ifndef PARSLEY_MAX_STEPS
#define PARSLEY_MAX_STEPS 10000000u
#endif
#ifndef PARSLEY_MALLOC
#define PARSLEY_MALLOC malloc
#endif
#ifndef PARSLEY_REALLOC
#define PARSLEY_REALLOC realloc
#endif
#ifndef PARSLEY_FREE
#define PARSLEY_FREE free
#endif
/* Each API invocation owns a checked allocation transaction. The TLS pointer is
   thread-local and restored on return; returned allocations become independent. */
typedef union PAllocation PAllocation;
typedef struct PAllocationScope {
  jmp_buf escape;
  PAllocation* head;
  struct PAllocationScope* previous;
  size_t bytes, steps, depth;
  int nfa_cur[4096], nfa_next[4096], nfa_seen[4096], nfa_stack[8192];
} PAllocationScope;
union PAllocation {
  struct { PAllocation *prev, *next; PAllocationScope* owner; size_t size; } info;
  max_align_t alignment;
};
static _Thread_local PAllocationScope* p_scope;
static void p_fatal(int code) {
  if (p_scope) longjmp(p_scope->escape, code);
}
static void p_step(void) {
  if (p_scope && ++p_scope->steps > PARSLEY_MAX_STEPS) p_fatal(2);
}
static void* p_malloc(size_t size) {
  size_t n = size ? size : 1;
  if (n > SIZE_MAX - sizeof(PAllocation) || (p_scope && (n > PARSLEY_MAX_BYTES || p_scope->bytes > PARSLEY_MAX_BYTES - n))) { p_fatal(2); return NULL; }
  PAllocation* h = (PAllocation*)PARSLEY_MALLOC(sizeof(PAllocation) + n);
  if (!h) { p_fatal(1); return NULL; }
  h->info.prev = NULL; h->info.next = p_scope ? p_scope->head : NULL;
  h->info.owner = p_scope; h->info.size = n;
  if (p_scope) { if (p_scope->head) p_scope->head->info.prev = h; p_scope->head = h; p_scope->bytes += n; }
  return h + 1;
}
static void p_free(void* ptr) {
  if (!ptr) return;
  PAllocation* h = ((PAllocation*)ptr)-1;
  if (h->info.owner) {
    if (h->info.prev) h->info.prev->info.next = h->info.next; else h->info.owner->head = h->info.next;
    if (h->info.next) h->info.next->info.prev = h->info.prev;
    h->info.owner->bytes -= h->info.size;
  }
  PARSLEY_FREE(h);
}
static void* p_calloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) { p_fatal(2); return NULL; }
  size_t bytes = n * size; void* ptr = p_malloc(bytes); if (ptr) memset(ptr,0,bytes); return ptr;
}
static void* p_realloc(void* ptr, size_t size) {
  if (!ptr) return p_malloc(size);
  if (!size) { p_free(ptr); return NULL; }
  PAllocation* old = ((PAllocation*)ptr)-1;
  size_t old_size = old->info.size;
  PAllocationScope* owner = old->info.owner;
  if (size > SIZE_MAX - sizeof(PAllocation) || (owner && (size > PARSLEY_MAX_BYTES || owner->bytes - old_size > PARSLEY_MAX_BYTES - size))) { p_fatal(2); return NULL; }
  PAllocation* h = (PAllocation*)PARSLEY_REALLOC(old,sizeof(PAllocation)+size);
  if (!h) { p_fatal(1); return NULL; }
  h->info.size = size;
  if (owner) {
    if (h->info.prev) h->info.prev->info.next = h; else owner->head = h;
    if (h->info.next) h->info.next->info.prev = h;
    owner->bytes = owner->bytes - old_size + size;
  }
  return h+1;
}
static PAllocationScope* p_begin_scope(void) {
  PAllocationScope* s = (PAllocationScope*)PARSLEY_MALLOC(sizeof(PAllocationScope));
  if (!s) return NULL;
  memset(s,0,sizeof(*s)); s->previous = p_scope; p_scope = s; return s;
}
static void p_end_scope(PAllocationScope* s, bool keep) {
  PAllocation* h = s->head;
  while (h) { PAllocation* next = h->info.next; if (keep) { h->info.owner = NULL; h->info.prev = NULL; h->info.next = NULL; } else PARSLEY_FREE(h); h = next; }
  p_scope = s->previous; PARSLEY_FREE(s);
}
static void p_enter(void) { p_step(); if (p_scope && ++p_scope->depth > PARSLEY_MAX_DEPTH) p_fatal(2); }
static void p_leave(void) { if (p_scope) --p_scope->depth; }
#define free p_free
#define realloc p_realloc

/* Decimal conversion is independent of the embedding host's locale. */
static size_t p_utf8_width(const char* text,size_t available) {
  if(!available)return 0;
  unsigned char c=(unsigned char)text[0];size_t n=c>=0xc2&&c<=0xdf?2u:c>=0xe0&&c<=0xef?3u:c>=0xf0&&c<=0xf4?4u:1u;
  if(n>available)return 1;
  for(size_t i=1;i<n;++i)if(((unsigned char)text[i]&0xc0)!=0x80)return 1;
  return n;
}static bool p_parse_i64(const char* text, size_t n, int64_t* out) {
  size_t i = 0; bool neg = false; uint64_t value = 0;
  if (i < n && (text[i]=='-' || text[i]=='+')) neg = text[i++]=='-';
  size_t start = i; uint64_t limit = neg ? (uint64_t)INT64_MAX+1u : (uint64_t)INT64_MAX;
  for (; i<n; ++i) { unsigned d = (unsigned)(text[i]-'0'); if (d>9 || value>(limit-d)/10u) return false; value=value*10u+d; }
  if (i==start) return false;
  *out = neg ? (value==(uint64_t)INT64_MAX+1u ? INT64_MIN : -(int64_t)value) : (int64_t)value; return true;
}
static bool p_parse_double(const char* text, size_t n, double* out) {
  size_t i=0; bool neg=false; long double value=0; int fraction=0, exponent=0; bool digits=false;
  if (i<n && (text[i]=='-' || text[i]=='+')) neg=text[i++]=='-';
  while (i<n && text[i]>='0' && text[i]<='9') { digits=true; value=value*10+(text[i++]-'0'); }
  if (i<n && text[i]=='.') { ++i; while (i<n && text[i]>='0' && text[i]<='9') { digits=true; value=value*10+(text[i++]-'0'); ++fraction; } }
  if (!digits) return false;
  if (i<n && (text[i]=='e' || text[i]=='E')) {
    ++i; bool minus=false; if (i<n && (text[i]=='-' || text[i]=='+')) minus=text[i++]=='-';
    size_t start=i; while (i<n && text[i]>='0' && text[i]<='9') { if (exponent>10000) return false; exponent=exponent*10+(text[i++]-'0'); }
    if (i==start) return false; if (minus) exponent=-exponent;
  }
  if (i!=n || exponent-fraction > 10000 || exponent-fraction < -10000) return false;
  value *= powl(10.0L,(long double)(exponent-fraction)); *out=(double)(neg ? -value : value); return isfinite(*out);
}
static void p_format_double(char* out, size_t size, double value) {
  char local[128]; snprintf(local,sizeof(local),"%.15g",value);
  const char* point=localeconv()->decimal_point; size_t plen=strlen(point); char* p = plen ? strstr(local,point) : NULL;
  if (p && strcmp(point,".")) { size_t tail=strlen(p+plen); *p='.'; memmove(p+1,p+plen,tail+1); }
  snprintf(out,size,"%s",local);
}
)C17";
}