#include "kotlin_parse.h"
#include "kotlin_sem.h"

#include <stdio.h>
#include <stdlib.h>

static void diagnostic(void *user, const psl_diag *diag) {
    (void)user;
    if (diag->def) {
        psl_str code = diag->def->code;
        printf("diagnostic %.*s\n", (int)code.len, code.ptr);
    }
}

int main(void) {
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
        return 0;
    }
    puts("accept");
    kotlin_ctx *context = kotlin_ctx_new(&semantic_arena, NULL);
    if (!context || kotlin_analyze(context, root, diagnostic, NULL) < 0) return 2;
    return 0;
}
