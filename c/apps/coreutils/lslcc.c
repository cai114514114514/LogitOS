/* Guest compiler for typed LSL source. The file format is still OLS-IR v1,
 * so existing runtime validation and immutable pipeline loading remain shared. */
#include "openlogit_lsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_SOURCE_BYTES (128 * 1024)

static char *read_source(const char *path, size_t *source_bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    char *source = malloc(MAX_SOURCE_BYTES + 1);
    if (!source) {
        fclose(file);
        return NULL;
    }
    *source_bytes = fread(source, 1, MAX_SOURCE_BYTES + 1, file);
    int failed = ferror(file) || *source_bytes > MAX_SOURCE_BYTES;
    fclose(file);
    if (failed) {
        fprintf(stderr, "lslcc: cannot read program (limit 128 KiB)\n");
        free(source);
        return NULL;
    }
    return source;
}

static int write_program(const char *path, const struct ol_shader_program *program)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 0;
    }
    int complete = fwrite(program, 1, sizeof *program, file) == sizeof *program;
    if (fclose(file) != 0)
        complete = 0;
    if (!complete)
        fprintf(stderr, "lslcc: could not write complete bytecode\n");
    return complete;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: lslcc input.lsl output.olsb\n");
        return 2;
    }
    size_t source_bytes = 0;
    char *source = read_source(argv[1], &source_bytes);
    if (!source)
        return 1;

    struct ol_lsl_shader *shader = NULL;
    struct ol_shader_error error = {0};
    int status = ol_lsl_compile(source, source_bytes, &shader, &error);
    free(source);

    /* One write keeps the summary intact when guest kernel diagnostics share
     * the serial stream; fragmented printf output can interleave mid-sentence. */
    char message[512];
    int length;
    if (status != OL_OK) {
        length = snprintf(message, sizeof message, "%s:%u: %s\n",
                          argv[1], error.line, error.message);
        if (length > 0 && length < (int)sizeof message)
            write(2, message, length);
        return 1;
    }
    const struct ol_shader_program *program = ol_lsl_ir(shader);
    int complete = write_program(argv[2], program);
    if (complete) {
        length = snprintf(message, sizeof message, "LSL v1: %u instructions, %s stage\n",
                          program->count, program->stage == LSL_VERTEX ? "vertex" : "fragment");
        complete = length > 0 && length < (int)sizeof message && write(1, message, length) == length;
    }
    ol_lsl_destroy(shader);
    return complete ? 0 : 1;
}
