#include "as.h"
#include "lexer.h"
#include <stdio.h>      /* FILE, fopen/fread/fclose, stdin */
#include <stdlib.h>     /* malloc/realloc/free */
#include <string.h>     /* strcmp (-c mode) */

/* /bin/as : run a script file (as foo.as) or, with no argument, read a whole
 * program from stdin (so `echo 'print(6*7)' | as` works). The interpreter core
 * is portable; this file is the Aether/host entry that does the file I/O. */

static char *slurp(FILE *f)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 4096 + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) return NULL; }
        size_t r = fread(buf + len, 1, 4096, f);
        len += r;
        if (r < 4096) break;       /* short read == EOF (our fread only stops short at EOF) */
    }
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv)
{
    char *src;

    /* Compile-only mode: `as -c in.as -o out.la` writes a .la (LAQ1 header +
     * serialized bytecode). as_compile gives a standalone ObjFn whose ->module is
     * the throwaway __main__ (as_dump never serializes ->module). as.c builds into
     * both the host asc and on-Aether /bin/as, so both get this mode. */
    if (argc == 5 && strcmp(argv[1], "-c") == 0 && strcmp(argv[3], "-o") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }
        ObjFn *fn = as_compile(src);
        free(src);
        if (!fn) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); as_free_objects(); return 1; }
        FILE *out = fopen(argv[4], "wb");
        if (!out) { as_emit_cstr("as: cannot create "); as_emit_cstr(argv[4]); as_emit_cstr("\n"); as_free_objects(); return 1; }
        int rc = as_dump(fn, out);
        fclose(out);
        if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); }
        as_free_objects();
        return rc;
    }

    /* Token-dump mode (M21-P3 S1 fixpoint harness): `as -lex file.as` prints one
     * "type line len checksum" per token -- compared byte-for-byte against the
     * self-hosted lexer (lib/aslex.as) over the whole corpus. */
    if (argc == 3 && strcmp(argv[1], "-lex") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }
        int count = 0;
        Token *toks = as_lex(src, &count);
        if (!toks) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); free(src); return 1; }
        for (int i = 0; i < count; i++) {
            unsigned sum = 0;
            for (int k = 0; k < toks[i].len; k++) sum = (sum + (unsigned char)toks[i].start[k]) % 9973;
            printf("%d %d %d %u\n", (int)toks[i].type, toks[i].line, toks[i].len, sum);
        }
        free(toks);
        free(src);
        return 0;
    }

    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[1]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        as_set_args(argc - 1, argv + 1);    /* args() = [script, script-args...] */
    } else {
        src = slurp(stdin);
    }
    if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }

    int rc = as_interpret(src);
    if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); }
    free(src);
    as_free_objects();
    return rc;
}
