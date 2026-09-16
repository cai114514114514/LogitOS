/* Independent frontend oracle: compare complete token bytes, not a checksum.
 * This links lexer.c alone and neither initializes nor executes a VM. */
#include "frontend/lexer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) {
        return 2;
    }
    long size = ftell(input);
    if (size < 0 || fseek(input, 0, SEEK_SET)) {
        fclose(input);
        return 2;
    }
    char *source = malloc((size_t)size + 1);
    if (!source || fread(source, 1, (size_t)size, input) != (size_t)size) {
        free(source);
        fclose(input);
        return 2;
    }
    fclose(input);
    source[size] = 0;

    AsLexError error = {0};
    int count = 0;
    Token *tokens = as_lex_source(source, &count, &error, NULL);
    if (!tokens) {
        fprintf(stderr, "%s\n", error.message);
        free(source);
        return 1;
    }
    for (int index = 0; index < count; index++) {
        Token *token = &tokens[index];
        printf("%d %d %d ", token->type, token->line, token->len);
        for (int byte = 0; byte < token->len; byte++) {
            printf("%02x", (unsigned char)token->start[byte]);
        }
        putchar('\n');
    }
    free(tokens);
    free(source);
    return 0;
}
