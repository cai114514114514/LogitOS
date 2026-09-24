/* cut -b LIST | -c LIST | -f LIST [-d DELIM] [-s] [FILE...]
 *
 * LIST is N, N-M, N-, -M, comma-separated, in any order: cut prints the
 * selected positions in INPUT order, once each, however the list was
 * written. -c counts bytes, as both GNU and BSD cut do in the C locale. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXPOS 4096
static unsigned char sel[MAXPOS + 2];
static int open_end;                  /* "N-": everything from N on */
static int mode;                      /* 'b', 'c', 'f' */
static int delim = '\t', o_s;

static int parse_list(const char *s)
{
    while (*s) {
        long a = 1, b;
        if (*s == '-') { s++; b = strtol(s, (char **)&s, 10); }
        else {
            a = strtol(s, (char **)&s, 10);
            if (a < 1) return -1;
            if (*s == '-') {
                s++;
                if (*s >= '0' && *s <= '9') b = strtol(s, (char **)&s, 10);
                else { b = MAXPOS; if (!open_end || a < open_end) open_end = (int)a; }
            } else b = a;
        }
        if (b < a) return -1;
        if (b > MAXPOS) b = MAXPOS;
        for (long k = a; k <= b; k++) sel[k] = 1;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return 0;
}

static int selected(long k) { return (k <= MAXPOS && sel[k]) || (open_end && k >= open_end); }

static void cut_line(const char *l, long n)
{
    if (mode != 'f') {
        for (long k = 0; k < n; k++) if (selected(k + 1)) putchar(l[k]);
        putchar('\n');
        return;
    }
    if (!memchr(l, delim, (size_t)n)) {
        if (!o_s) { fwrite(l, 1, (size_t)n, stdout); putchar('\n'); }
        return;
    }
    long field = 1, start = 0;
    int printed = 0;
    for (long k = 0; k <= n; k++) {
        if (k == n || l[k] == delim) {
            if (selected(field)) {
#ifndef TEXTUTIL_NEGCTL
                if (printed) putchar(delim);    /* THE CONTROL drops the separators */
#endif
                fwrite(l + start, 1, (size_t)(k - start), stdout);
                printed = 1;
            }
            field++;
            start = k + 1;
        }
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    int i = 1;
    const char *list = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'b' || *f == 'c' || *f == 'f' || *f == 'd') {
                const char *v = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!v) { fputs("cut: option requires an argument\n", stderr); return 1; }
                if (*f == 'd') { if (!v[0] || v[1]) { fputs("cut: the delimiter must be a single character\n", stderr); return 1; } delim = (unsigned char)v[0]; }
                else { mode = *f; list = v; }
                goto next;
            } else if (*f == 's') o_s = 1;
            else if (*f == 'n') {}
            else { fprintf(stderr, "cut: invalid option -- '%c'\n", *f); return 1; }
        }
    next:;
    }
    if (!mode || !list) { fputs("cut: you must specify a list of bytes, characters, or fields\n", stderr); return 1; }
    if (parse_list(list)) { fprintf(stderr, "cut: invalid list: %s\n", list); return 1; }
    int rc = 0;
    char *l = 0; size_t c = 0; ssize_t n;
    for (int k = i; k <= argc; k++) {
        FILE *f;
        if (k == argc) { if (i != argc) break; f = stdin; }
        else f = strcmp(argv[k], "-") ? fopen(argv[k], "r") : stdin;
        if (!f) { fprintf(stderr, "cut: %s: No such file or directory\n", argv[k]); rc = 1; continue; }
        while ((n = getline(&l, &c, f)) >= 0) {
            if (n && l[n - 1] == '\n') n--;
            cut_line(l, n);
        }
        if (f != stdin) fclose(f);
    }
    free(l);
    return rc;
}
