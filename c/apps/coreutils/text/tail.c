/* tail [-n [+]N | -c [+]N] [FILE...]
 *
 * The last N lines (default 10) or bytes, or from line/byte N with `+N`.
 * Several files each get a "==> name <==" header, as POSIX and both common
 * implementations print. The obsolete `tail -5` form is accepted. No -f: a
 * follow that polls is a busy loop this machine would feel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long want = 10;
static int bytes, from_start;

static int tail_stream(FILE *f)
{
    char *buf = 0; size_t cap = 0, len = 0;
    char tmp[4096]; size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (len + r > cap) { cap = (len + r) * 2; buf = realloc(buf, cap); if (!buf) return 1; }
        memcpy(buf + len, tmp, r); len += r;
    }
    size_t start = 0;
    if (bytes) {
        if (from_start) start = want > 0 ? (size_t)(want - 1) : 0;
        else start = len > (size_t)want ? len - (size_t)want : 0;
        if (start > len) start = len;
    } else if (from_start) {
        long line = 1;
        while (start < len && line < want) { if (buf[start] == '\n') line++; start++; }
    } else {
        long seen = 0;
        size_t i = len;
        /* A final line without a newline still counts as a line. */
        if (i && buf[i - 1] == '\n') i--;
        while (i > 0) {
            if (buf[i - 1] == '\n' && ++seen == want) break;
            i--;
        }
#ifdef TEXTUTIL_NEGCTL
        if (i < len && want > 0) while (i < len && buf[i] != '\n') i++;   /* THE CONTROL: one line short */
        if (i < len) i++;
#endif
        start = want ? i : len;
    }
    fwrite(buf + start, 1, len - start, stdout);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        const char *a = argv[i] + 1;
        if (*a >= '0' && *a <= '9') { want = atol(a); continue; }       /* tail -5 */
        if (*a == 'n' || *a == 'c') {
            bytes = *a == 'c';
            const char *v = a[1] ? a + 1 : (++i < argc ? argv[i] : 0);
            if (!v) { fputs("tail: option requires an argument\n", stderr); return 1; }
            if (*v == '+') { from_start = 1; v++; } else if (*v == '-') v++;
            want = atol(v);
            continue;
        }
        fprintf(stderr, "tail: invalid option -- '%c'\n", *a);
        return 1;
    }
    int nfiles = argc - i, rc = 0;
    if (!nfiles) return tail_stream(stdin);
    for (int k = i; k < argc; k++) {
        FILE *f = strcmp(argv[k], "-") ? fopen(argv[k], "r") : stdin;
        if (!f) { fprintf(stderr, "tail: cannot open '%s' for reading: No such file or directory\n", argv[k]); rc = 1; continue; }
        if (nfiles > 1) printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        rc |= tail_stream(f);
        if (f != stdin) fclose(f);
    }
    return rc;
}
