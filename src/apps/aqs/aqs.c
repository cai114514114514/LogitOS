#include "aqs.h"
#include <stdio.h>      /* FILE, fopen/fread/fclose, stdin */
#include <stdlib.h>     /* malloc/realloc/free */
#include <string.h>     /* strcmp (-c mode) */

/* /bin/aqs : run a script file (aqs foo.aqs) or, with no argument, read a whole
 * program from stdin (so `echo 'print(6*7)' | aqs` works). The interpreter core
 * is portable; this file is the Aqua/host entry that does the file I/O. */

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

    /* Compile-only mode: `aqs -c in.aqs -o out.la` writes a .la (LAQ1 header +
     * serialized bytecode). aqs_compile gives a standalone ObjFn whose ->module is
     * the throwaway __main__ (aqs_dump never serializes ->module). aqs.c builds into
     * both the host aqsc and on-Aqua /bin/aqs, so both get this mode. */
    if (argc == 5 && strcmp(argv[1], "-c") == 0 && strcmp(argv[3], "-o") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { aqs_emit_cstr("aqs: cannot open "); aqs_emit_cstr(argv[2]); aqs_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { aqs_emit_cstr("aqs: out of memory\n"); return 1; }
        ObjFn *fn = aqs_compile(src);
        free(src);
        if (!fn) { aqs_emit_cstr("aqs: "); aqs_emit_cstr(aqs_err); aqs_emit_cstr("\n"); aqs_free_objects(); return 1; }
        FILE *out = fopen(argv[4], "wb");
        if (!out) { aqs_emit_cstr("aqs: cannot create "); aqs_emit_cstr(argv[4]); aqs_emit_cstr("\n"); aqs_free_objects(); return 1; }
        int rc = aqs_dump(fn, out);
        fclose(out);
        if (rc) { aqs_emit_cstr("aqs: "); aqs_emit_cstr(aqs_err); aqs_emit_cstr("\n"); }
        aqs_free_objects();
        return rc;
    }

    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { aqs_emit_cstr("aqs: cannot open "); aqs_emit_cstr(argv[1]); aqs_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
    } else {
        src = slurp(stdin);
    }
    if (!src) { aqs_emit_cstr("aqs: out of memory\n"); return 1; }

    int rc = aqs_interpret(src);
    if (rc) { aqs_emit_cstr("aqs: "); aqs_emit_cstr(aqs_err); aqs_emit_cstr("\n"); }
    free(src);
    aqs_free_objects();
    return rc;
}
