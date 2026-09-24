/* uniq [-cdui] [-f N] [-s N] [INPUT [OUTPUT]]
 *
 * Adjacent lines compare after skipping N blank-separated fields (-f) and
 * then N characters (-s), case-folded with -i. -c prefixes GNU's "%7d " --
 * POSIX leaves the width open and BSD prints "%4d "; GNU's is what scripts
 * that cut the count out by column were written against. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int o_c, o_d, o_u, o_i, o_f, o_s;

static const char *skip(const char *s)
{
    for (int f = 0; f < o_f; f++) {
        while (*s == ' ' || *s == '\t') s++;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    for (int c = 0; c < o_s && *s; c++) s++;
    return s;
}

static int same(const char *a, const char *b)
{
    a = skip(a); b = skip(b);
#ifdef TEXTUTIL_NEGCTL
    return !strncmp(a, b, 1);                   /* THE CONTROL: first byte only */
#endif
    if (!o_i) return !strcmp(a, b);
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static void emit(FILE *o, const char *l, long n)
{
    if ((o_d && n < 2) || (o_u && n > 1)) return;
    if (o_c) fprintf(o, "%7ld %s\n", n, l);
    else fprintf(o, "%s\n", l);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'c') o_c = 1; else if (*f == 'd') o_d = 1;
            else if (*f == 'u') o_u = 1; else if (*f == 'i') o_i = 1;
            else if (*f == 'f' || *f == 's') {
                const char *v = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!v) { fputs("uniq: option requires an argument\n", stderr); return 1; }
                if (*f == 'f') o_f = atoi(v); else o_s = atoi(v);
                goto next;
            } else { fprintf(stderr, "uniq: invalid option -- '%c'\n", *f); return 1; }
        }
    next:;
    }
    FILE *in = stdin, *out = stdout;
    if (i < argc && strcmp(argv[i], "-")) {
        in = fopen(argv[i], "r");
        if (!in) { fprintf(stderr, "uniq: %s: No such file or directory\n", argv[i]); return 1; }
    }
    if (i + 1 < argc && !(out = fopen(argv[i + 1], "w"))) { fprintf(stderr, "uniq: %s: cannot open\n", argv[i + 1]); return 1; }
    char *prev = 0, *l = 0; size_t pc = 0, lc = 0; ssize_t n; long count = 0;
    while ((n = getline(&l, &lc, in)) >= 0) {
        if (n && l[n - 1] == '\n') l[n - 1] = 0;
        if (prev && same(prev, l)) { count++; continue; }
        if (prev) emit(out, prev, count);
        size_t len = strlen(l) + 1;
        if (len > pc) { pc = len * 2; prev = realloc(prev, pc); if (!prev) return 1; }
        memcpy(prev, l, len);
        count = 1;
    }
    if (prev && count) emit(out, prev, count);
    return 0;
}
