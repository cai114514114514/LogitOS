/* SPDX-License-Identifier: MIT */
/* This harness deliberately links no Value, bytecode compiler, VM or VM heap.
 * It exercises the real native CLI driver after a lexical allocation failure,
 * proving that native compiler state no longer depends on VM initialization. */
#include "frontend/lexer.h"
#include "include/project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *refuse_allocation(void *pointer, size_t size)
{
    (void)pointer;
    (void)size;
    return NULL;
}

int main(int argc, char **argv)
{
    AsLexError error;
    int count = 99;
    Token *tokens = as_lex_source("value = 1\n", &count, &error, refuse_allocation);
    if (tokens || count != 0 || !error.out_of_memory || !error.message[0]) {
        fputs("native lexical allocation failure was not reported\n", stderr);
        free(tokens);
        return 2;
    }
    tokens = as_lex_source("value = 1\n", &count, &error, NULL);
    if (!tokens || count < 2 || error.out_of_memory || error.message[0]) {
        fputs("a previous lexical failure poisoned the next request\n", stderr);
        free(tokens);
        return 2;
    }
    free(tokens);
    int result = as_typed_command(argc, argv);
    return result < 0 ? 2 : result;
}
