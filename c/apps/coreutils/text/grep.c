/* grep [-EFivcnlLoqxwrhHs] [-e PATTERN]... [-f FILE] [PATTERN] [FILE...]
 *
 * Patterns are lre (lre.h): BRE by default, ERE with -E, fixed strings with
 * -F; leftmost-longest, linear in the line. Several -e patterns are OR'd.
 * Exit status is POSIX's: 0 a line was selected, 1 none was, 2 an error
 * (an error wins even if another file matched, unless -q already succeeded).
 *
 * Differences from GNU grep that a script might notice are deliberate and
 * few: no binary-file heuristics (a file is text), no --color, no context
 * (-A/-B/-C) yet. Each is refused by name if asked for rather than ignored. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "lre.h"

static int o_E, o_F, o_i, o_v, o_c, o_n, o_l, o_L, o_o, o_q, o_x, o_w, o_r, o_s;
static int o_H = -1;                 /* -1: decide by file count */
static char **pats;
static int npats;
static struct lre **res;
static int status = 1, errors;

static void addpat(const char *p)
{
    /* A pattern containing newlines is several patterns (POSIX). */
    const char *s = p;
    for (;;) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char *copy = malloc(n + 1);
        if (!copy) { fputs("grep: out of memory\n", stderr); exit(2); }
        memcpy(copy, s, n); copy[n] = 0;
        char **np = realloc(pats, sizeof *pats * (size_t)(npats + 1));
        if (!np) { fputs("grep: out of memory\n", stderr); exit(2); }
        pats = np; pats[npats++] = copy;
        if (!e) break;
        s = e + 1;
    }
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int wordc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* Fixed-string search for pattern k from `from`; [*s,*e) on success. */
static int fixed_find(const char *line, int n, int k, int from, int *ms, int *me)
{
    const char *p = pats[k];
    int m = (int)strlen(p);
    for (int i = from; i + m <= n; i++) {
        int j = 0;
        if (o_i) { while (j < m && lc((unsigned char)line[i + j]) == lc((unsigned char)p[j])) j++; }
        else     { while (j < m && line[i + j] == p[j]) j++; }
        if (j == m) { *ms = i; *me = i + m; return 1; }
    }
    return 0;
}

static int one_find(const char *line, int n, int k, int from, int *ms, int *me)
{
    if (o_F) return fixed_find(line, n, k, from, ms, me);
    int sub[2 * LRE_NSUB];
    if (!lre_exec(res[k], line, n, from, 0, sub)) return 0;
    *ms = sub[0]; *me = sub[1];
    return 1;
}

/* The earliest (then longest) match of any pattern at or after `from`,
 * honouring -w and -x. */
static int find(const char *line, int n, int from, int *ms, int *me)
{
    int best = 0, bs = 0, be = 0;
    for (int k = 0; k < npats; k++) {
        int f = from, s, e;
        while (f <= n && one_find(line, n, k, f, &s, &e)) {
            int ok = 1;
            if (o_x) ok = s == 0 && e == n;
            else if (o_w) ok = (s == 0 || !wordc((unsigned char)line[s - 1])) &&
                               (e == n || !wordc((unsigned char)line[e])) && e > s;
            if (ok) {
                if (!best || s < bs || (s == bs && e > be)) { best = 1; bs = s; be = e; }
                break;
            }
            f = s + 1;
            if (o_x) break;
        }
    }
    if (best) { *ms = bs; *me = be; }
    return best;
}

static void prefix(const char *name, long lineno)
{
    if (o_H > 0) printf("%s:", name);
    if (o_n) printf("%ld:", lineno);
}

static void grep_stream(FILE *f, const char *name)
{
    char *line = 0;
    size_t cap = 0;
    ssize_t len;
    long lineno = 0, count = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        int n = (int)len;
        if (n && line[n - 1] == '\n') n--;
        int s, e, hit = find(line, n, 0, &s, &e);
        if (hit == o_v) continue;
        count++;
        status = status == 2 ? 2 : 0;
        if (o_q) { free(line); exit(0); }
        if (o_l || o_L) break;
        if (o_c) continue;
        if (o_o && !o_v) {
            int from = 0;
            while (from <= n && find(line, n, from, &s, &e)) {
                if (e > s) { prefix(name, lineno); printf("%.*s\n", e - s, line + s); from = e; }
                else from = s + 1;
            }
            continue;
        }
        if (o_o) continue;
        prefix(name, lineno);
        fwrite(line, 1, (size_t)n, stdout);
        putchar('\n');
    }
    free(line);
    if (o_c) { if (o_H > 0) printf("%s:", name); printf("%ld\n", count); }
    if (o_l && count) printf("%s\n", name);
    if (o_L && !count) printf("%s\n", name);
}

static void grep_path(const char *path, int top)
{
    struct stat st;
    if (o_r && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { if (!o_s) fprintf(stderr, "grep: %s: cannot open directory\n", path); errors = 1; return; }
        struct dirent *de;
        /* Sorted, so the output order does not depend on the directory's
         * insertion order -- LogitFS lists in insertion order, a host
         * filesystem in its own. */
        char **names = 0; int nn = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char **np = realloc(names, sizeof *names * (size_t)(nn + 1));
            if (!np) break;
            names = np;
            names[nn++] = strdup(de->d_name);
        }
        closedir(d);
        for (int i = 1; i < nn; i++)
            for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
            }
        for (int i = 0; i < nn; i++) {
            char sub[1024];
            size_t l = strlen(path);
            snprintf(sub, sizeof sub, "%s%s%s", path, (l && path[l - 1] == '/') ? "" : "/", names[i]);
            free(names[i]);
            grep_path(sub, 0);
        }
        free(names);
        return;
    }
    (void)top;
    FILE *f = fopen(path, "r");
    if (!f) { if (!o_s) fprintf(stderr, "grep: %s: No such file or directory\n", path); errors = 1; return; }
    grep_stream(f, path);
    fclose(f);
}

static void usage(void)
{
    fputs("usage: grep [-EFivcnlLoqxwrhHs] [-e PATTERN]... [-f FILE] [PATTERN] [FILE...]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'E': o_E = 1; o_F = 0; break;
            case 'G': o_E = 0; o_F = 0; break;
            case 'F': o_F = 1; break;
            case 'i': o_i = 1; break;
            case 'y': o_i = 1; break;
            case 'v': o_v = 1; break;
            case 'c': o_c = 1; break;
            case 'n': o_n = 1; break;
            case 'l': o_l = 1; break;
            case 'L': o_L = 1; break;
            case 'o': o_o = 1; break;
            case 'q': o_q = 1; break;
            case 'x': o_x = 1; break;
            case 'w': o_w = 1; break;
            case 'r': case 'R': o_r = 1; break;
            case 'h': o_H = 0; break;
            case 'H': o_H = 1; break;
            case 's': o_s = 1; break;
            case 'e':
                if (f[1]) { addpat(f + 1); goto nextarg; }
                if (++i >= argc) usage();
                addpat(argv[i]); goto nextarg;
            case 'f': {
                const char *fn = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!fn) usage();
                FILE *pf = fopen(fn, "r");
                if (!pf) { fprintf(stderr, "grep: %s: No such file or directory\n", fn); return 2; }
                char *l = 0; size_t c = 0; ssize_t n;
                while ((n = getline(&l, &c, pf)) >= 0) {
                    if (n && l[n - 1] == '\n') l[n - 1] = 0;
                    addpat(l);
                }
                free(l); fclose(pf);
                goto nextarg;
            }
            case 'A': case 'B': case 'C':
                fprintf(stderr, "grep: -%c (context) is not supported\n", *f); return 2;
            default:
                fprintf(stderr, "grep: invalid option -- '%c'\n", *f); usage();
            }
        }
    nextarg:;
    }
    if (!npats) { if (i >= argc) usage(); addpat(argv[i++]); }
    if (!o_F) {
        res = calloc((size_t)npats, sizeof *res);
        for (int k = 0; k < npats; k++) {
            char err[128];
            if (lre_compile(&res[k], pats[k], (o_E ? LRE_EXTENDED : 0) | (o_i ? LRE_ICASE : 0),
                            err, sizeof err)) {
                fprintf(stderr, "grep: %s\n", err);
                return 2;
            }
        }
    }
    int nfiles = argc - i;
    if (o_H < 0) o_H = nfiles > 1 || o_r;
    if (nfiles == 0) {
        if (o_r) grep_path(".", 1);
        else grep_stream(stdin, "(standard input)");
    } else {
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "-")) grep_stream(stdin, "(standard input)");
            else grep_path(argv[i], 1);
        }
    }
    fflush(stdout);
    if (errors && !(o_q && status == 0)) return 2;
    return status;
}
