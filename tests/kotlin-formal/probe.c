#include "kotlin_parse.h"
#include "kotlin_sem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void diagnostic(void *user, const psl_diag *diag) {
    const bool located = *(const bool *)user;
    if (diag->def) {
        psl_str code = diag->def->code;
        if (located) {
            printf("%.*s %" PRIu32 " %" PRIu32 "\n", (int)code.len, code.ptr,
                   diag->span.offset, diag->span.len);
        } else {
            printf("diagnostic %.*s\n", (int)code.len, code.ptr);
        }
    }
}

int main(int argc, char **argv) {
    const bool parse_only = argc == 2 && strcmp(argv[1], "--parse-only") == 0;
    bool located = argc == 2 && strcmp(argv[1], "--diagnostic-spans") == 0;
    if (argc != 1 && !parse_only && !located) return 2;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 2;
#endif
    static char input[65536];
    static unsigned char parser_memory[4 * 1024 * 1024];
    static unsigned char semantic_memory[8 * 1024 * 1024];
    const size_t length = fread(input, 1, sizeof(input), stdin);
    if (ferror(stdin) || length == sizeof(input)) return 2;
    psl_arena parser_arena, semantic_arena;
    psl_arena_init(&parser_arena, parser_memory, sizeof(parser_memory));
    psl_arena_init(&semantic_arena, semantic_memory, sizeof(semantic_memory));
    const kotlin_node *root = NULL;
    psl_diag error = {0};
    if (kotlin_parse(&parser_arena, psl_str_make(input, length), &root, &error) < 0) {
        if (parser_arena.error || !error.def) return 2;
        puts("reject");
        fprintf(stderr, "syntax error at %" PRIu32 ":%" PRIu32 ": %.*s\n",
                error.span.line, error.span.col, (int)error.message.len, error.message.ptr);
        return 0;
    }
    puts("accept");
    if (parse_only) return 0;
    kotlin_ctx *context = kotlin_ctx_new(&semantic_arena, NULL);
    if (!context || kotlin_analyze(context, root, diagnostic, &located) < 0) return 2;
    return 0;
}
