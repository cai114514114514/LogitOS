/* SPDX-License-Identifier: MIT */
/* Exercise the real scanner in both modes; recovery must preserve ordinary
 * token meaning and release partial allocations on every allocator failure. */
#include "frontend/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, reports, allocations, fail_after = -1;

static void require(int valid, const char *name)
{
    if (!valid) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
    checks++;
}

static void report(void *context, Token site, const char *message)
{
    const char *source = context;
    require(site.start >= source && site.start + site.len <= source + strlen(source),
            "error source range");
    require(site.type == T_ERROR && site.len > 0 && message[0], "error token metadata");
    reports++;
}

static void *allocate(void *pointer, size_t bytes)
{
    if (allocations++ == fail_after) {
        return NULL;
    }
    return realloc(pointer, bytes);
}

int main(void)
{
    const char *valid[] = {
        "def main() -> None:\n    value = [1,\n             2]\n    print(value)\n",
        "text = 'first\nsecond'\nformatted = f\"{1 + 2}\"\n",
        "struct Point:\n    x: f64\n    y: f64\n",
    };
    AsLexError error;
    for (unsigned sample = 0; sample < sizeof valid / sizeof *valid; sample++) {
        int strict_count, recovered_count;
        Token *strict = as_lex_source(valid[sample], &strict_count, &error, NULL);
        reports = 0;
        Token *recovered = as_lex_recover(valid[sample], &recovered_count, &error,
                                         report, (void *)valid[sample], NULL);
        require(strict && recovered && strict_count == recovered_count && reports == 0,
                "valid source mode parity");
        for (int index = 0; index < strict_count; index++) {
            require(strict[index].type == recovered[index].type &&
                        strict[index].start == recovered[index].start &&
                        strict[index].len == recovered[index].len &&
                        strict[index].line == recovered[index].line,
                    "valid token parity");
        }
        free(strict);
        free(recovered);
    }

    const char *broken = "a = 0x\nb = @\nc = 'unfinished\nlater = 1\n";
    int count;
    Token *strict = as_lex_source(broken, &count, &error, NULL);
    require(!strict && count == 0 && error.message[0], "strict mode still rejects errors");
    reports = 0;
    Token *tokens = as_lex_recover(broken, &count, &error, report, (void *)broken, NULL);
    require(tokens && count && tokens[count - 1].type == T_EOF,
            "recovery keeps token stream");
    int errors = 0, later = 0;
    for (int index = 0; index < count; index++) {
        errors += tokens[index].type == T_ERROR;
        later += tokens[index].type == T_IDENT && tokens[index].len == 5 &&
                 !memcmp(tokens[index].start, "later", 5) && tokens[index].line == 4;
    }
    require(reports == 3 && errors == 3 && later == 1, "errors preserve following declaration");
    free(tokens);

    char large[4096];
    strcpy(large, broken);
    for (int index = 0; index < 100; index++) {
        strcat(large, "value = 1\n");
    }
    allocations = 0;
    tokens = as_lex_recover(large, &count, &error, report, large, allocate);
    require(tokens != NULL, "allocator baseline");
    int total = allocations;
    free(tokens);
    for (fail_after = 0; fail_after < total; fail_after++) {
        allocations = 0;
        count = 99;
        tokens = as_lex_recover(large, &count, &error, report, large, allocate);
        require(!tokens && count == 0 && error.out_of_memory,
                "allocation failure remains fatal");
    }
    printf("PASS lexical recovery: %d checks\n", checks);
    return 0;
}
