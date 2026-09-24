/* sort [-bcfnrsu] [-t SEP] [-k KEY]... [-o FILE] [FILE...]
 *
 * KEY is POSIX's F1[.C1][bfnr][,F2[.C2][bfnr]]. Keys compare in order; lines
 * whose keys are all equal fall back to a whole-line byte comparison unless
 * -s (stable) or -u, exactly as GNU does -- which is why `sort -k2` and
 * `sort -s -k2` differ on ties, and why both are here.
 *
 * -n compares the leading number of a key as a DECIMAL STRING, digit by
 * digit, never through a double: 12345678901234567890 and ...891 must not
 * compare equal because a float rounded them together.
 *
 * The sort itself is a merge sort, so equal elements keep their input order
 * -- the property -s promises and the control breaks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct key { int f1, c1, f2, c2; int b, f, n, r; int any_opt; };
static struct key keys[16];
static int nkeys;
static int g_b, g_f, g_n, g_r, g_u, g_s, g_c;
static int sep = -1;                   /* -t; -1 = blank-separated fields */

struct line { const char *s; int n; };

static int blank(int c) { return c == ' ' || c == '\t'; }

/* [*ks, *ke) of the key in line l. */
static void key_span(const struct key *k, const struct line *l, const char **ks, const char **ke)
{
    const char *p = l->s, *end = l->s + l->n;
    /* field f starts where? */
    const char *fs = p;
    for (int f = 1; f < k->f1 && fs < end; f++) {
        if (sep >= 0) { while (fs < end && *fs != sep) fs++; if (fs < end) fs++; }
        else { while (fs < end && blank(*fs)) fs++; while (fs < end && !blank(*fs)) fs++; }
    }
    if (k->b || (!k->any_opt && g_b)) while (fs < end && blank(*fs)) fs++;
    const char *s = fs + (k->c1 > 0 ? k->c1 - 1 : 0);
    if (s > end) s = end;
    const char *e = end;
    if (k->f2 > 0) {
        const char *fe = p;
        for (int f = 1; f < k->f2 && fe < end; f++) {
            if (sep >= 0) { while (fe < end && *fe != sep) fe++; if (fe < end) fe++; }
            else { while (fe < end && blank(*fe)) fe++; while (fe < end && !blank(*fe)) fe++; }
        }
        if (k->c2 > 0) {
            const char *q = fe;
            if (k->b || (!k->any_opt && g_b)) while (q < end && blank(*q)) q++;
            e = q + k->c2;
            if (e > end) e = end;
        } else {
            /* to the end of field f2 */
            e = fe;
            if (sep >= 0) { while (e < end && *e != sep) e++; }
            else { while (e < end && blank(*e)) e++; while (e < end && !blank(*e)) e++; }
        }
    }
    if (e < s) e = s;
    *ks = s; *ke = e;
}

static int numcmp(const char *a, const char *ae, const char *b, const char *be)
{
    while (a < ae && blank(*a)) a++;
    while (b < be && blank(*b)) b++;
    int na = a < ae && *a == '-', nb = b < be && *b == '-';
    if (na) a++;
    if (nb) b++;
    const char *ai = a, *bi = b;
    while (ai < ae && *ai >= '0' && *ai <= '9') ai++;
    while (bi < be && *bi >= '0' && *bi <= '9') bi++;
    /* strip leading zeros of the integer part */
    const char *a0 = a, *b0 = b;
    while (a0 < ai && *a0 == '0') a0++;
    while (b0 < bi && *b0 == '0') b0++;
    int la = (int)(ai - a0), lb = (int)(bi - b0);
    /* fraction */
    const char *af = ai, *bf = bi, *afe = ai, *bfe = bi;
    if (af < ae && *af == '.') { af++; afe = af; while (afe < ae && *afe >= '0' && *afe <= '9') afe++; } else af = afe = ai;
    if (bf < be && *bf == '.') { bf++; bfe = bf; while (bfe < be && *bfe >= '0' && *bfe <= '9') bfe++; } else bf = bfe = bi;
    int zero_a = la == 0, zero_b = lb == 0;
    for (const char *q = af; q < afe; q++) if (*q != '0') zero_a = 0;
    for (const char *q = bf; q < bfe; q++) if (*q != '0') zero_b = 0;
    if (zero_a) na = 0;
    if (zero_b) nb = 0;
    if (na != nb) return na ? -1 : 1;
    int r;
    if (la != lb) r = la < lb ? -1 : 1;
    else {
        r = memcmp(a0, b0, (size_t)la);
        if (!r) {
            while (af < afe || bf < bfe) {
                int ca = af < afe ? *af++ : '0', cb = bf < bfe ? *bf++ : '0';
                if (ca != cb) { r = ca < cb ? -1 : 1; break; }
            }
        } else r = r < 0 ? -1 : 1;
    }
    return na ? -r : r;
}

static int fold(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int strcmp_span(const char *a, const char *ae, const char *b, const char *be, int f)
{
    while (a < ae && b < be) {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (f) { ca = fold(ca); cb = fold(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return (a < ae) - (b < be);
}

static int keycmp(const struct line *x, const struct line *y)
{
    if (nkeys == 0) {
        struct key whole = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
        const char *xs = x->s, *xe = x->s + x->n, *ys = y->s, *ye = y->s + y->n;
        (void)whole;
        if (g_b) { while (xs < xe && blank(*xs)) xs++; while (ys < ye && blank(*ys)) ys++; }
        int r = g_n ? numcmp(xs, xe, ys, ye) : strcmp_span(xs, xe, ys, ye, g_f);
        return g_r ? -r : r;
    }
    for (int i = 0; i < nkeys; i++) {
        const struct key *k = &keys[i];
        const char *xs, *xe, *ys, *ye;
        key_span(k, x, &xs, &xe);
        key_span(k, y, &ys, &ye);
        int n = k->any_opt ? k->n : g_n, f = k->any_opt ? k->f : g_f, rv = k->any_opt ? k->r : g_r;
        int r = n ? numcmp(xs, xe, ys, ye) : strcmp_span(xs, xe, ys, ye, f);
        if (r) return rv ? -r : r;
    }
    return 0;
}

static int cmp(const struct line *x, const struct line *y)
{
    int r = keycmp(x, y);
    if (r || g_s || g_u) return r;
    /* last resort: the whole line, bytewise (reversed by -r) */
    r = strcmp_span(x->s, x->s + x->n, y->s, y->s + y->n, 0);
    return g_r ? -r : r;
}

static void msort(struct line *a, struct line *tmp, long n)
{
    if (n < 2) return;
#ifdef TEXTUTIL_NEGCTL
    /* THE CONTROL: an UNSTABLE sort (selection by swapping) -- equal keys come
     * out in whatever order the swaps leave them, which -s forbids. */
    (void)tmp;
    for (long i = 0; i < n; i++) {
        long m = i;
        for (long j = i + 1; j < n; j++) if (cmp(&a[j], &a[m]) < 0) m = j;
        struct line t = a[i]; a[i] = a[m]; a[m] = t;
    }
    for (long i = 0; i + 1 < n; i += 2)
        if (!cmp(&a[i], &a[i + 1])) { struct line t = a[i]; a[i] = a[i + 1]; a[i + 1] = t; }
    return;
#endif
    long h = n / 2;
    msort(a, tmp, h);
    msort(a + h, tmp, n - h);
    long i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = cmp(&a[j], &a[i]) < 0 ? a[j++] : a[i++];
    while (i < h) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, sizeof *a * (size_t)n);
}

static int parse_key(const char *s)
{
    struct key k = {0};
    char *e;
    k.f1 = (int)strtol(s, &e, 10);
    if (k.f1 < 1) return -1;
    if (*e == '.') k.c1 = (int)strtol(e + 1, &e, 10);
    for (; *e && *e != ','; e++) {
        if (*e == 'b') k.b = 1; else if (*e == 'f') k.f = 1;
        else if (*e == 'n') k.n = 1; else if (*e == 'r') k.r = 1;
        else return -1;
        k.any_opt = 1;
    }
    if (*e == ',') {
        k.f2 = (int)strtol(e + 1, &e, 10);
        if (k.f2 < 1) return -1;
        if (*e == '.') k.c2 = (int)strtol(e + 1, &e, 10);
        for (; *e; e++) {
            if (*e == 'b') k.b = 1; else if (*e == 'f') k.f = 1;
            else if (*e == 'n') k.n = 1; else if (*e == 'r') k.r = 1;
            else return -1;
            k.any_opt = 1;
        }
    }
    if (nkeys == 16) return -1;
    keys[nkeys++] = k;
    return 0;
}

static struct line *lines;
static long nlines, caplines;

static void slurp(FILE *f)
{
    char *l = 0; size_t c = 0; ssize_t n;
    while ((n = getline(&l, &c, f)) >= 0) {
        if (n && l[n - 1] == '\n') n--;
        if (nlines == caplines) {
            caplines = caplines ? caplines * 2 : 1024;
            lines = realloc(lines, sizeof *lines * (size_t)caplines);
            if (!lines) { fputs("sort: out of memory\n", stderr); exit(2); }
        }
        char *s = malloc((size_t)n + 1);
        if (!s) { fputs("sort: out of memory\n", stderr); exit(2); }
        memcpy(s, l, (size_t)n); s[n] = 0;
        lines[nlines].s = s; lines[nlines].n = (int)n; nlines++;
    }
    free(l);
}

int main(int argc, char **argv)
{
    const char *outfile = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'b': g_b = 1; break;
            case 'f': g_f = 1; break;
            case 'n': g_n = 1; break;
            case 'r': g_r = 1; break;
            case 'u': g_u = 1; break;
            case 's': g_s = 1; break;
            case 'c': g_c = 1; break;
            case 't': {
                const char *v = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!v || !v[0] || v[1]) { fputs("sort: the delimiter must be a single character\n", stderr); return 2; }
                sep = (unsigned char)v[0]; goto next;
            }
            case 'k': {
                const char *v = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!v || parse_key(v)) { fprintf(stderr, "sort: invalid key: %s\n", v ? v : ""); return 2; }
                goto next;
            }
            case 'o': {
                const char *v = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!v) return 2;
                outfile = v; goto next;
            }
            default: fprintf(stderr, "sort: invalid option -- '%c'\n", *f); return 2;
            }
        }
    next:;
    }
    if (i == argc) slurp(stdin);
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-")) { slurp(stdin); continue; }
        FILE *f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "sort: cannot read: %s: No such file or directory\n", argv[i]); return 2; }
        slurp(f); fclose(f);
    }
    if (g_c) {
        for (long k = 1; k < nlines; k++) {
            int r = cmp(&lines[k - 1], &lines[k]);
            if (r > 0 || (g_u && r == 0)) {
                fprintf(stderr, "sort: -:%ld: disorder: %s\n", k + 1, lines[k].s);
                return 1;
            }
        }
        return 0;
    }
    struct line *tmp = malloc(sizeof *tmp * (size_t)(nlines ? nlines : 1));
    if (!tmp) { fputs("sort: out of memory\n", stderr); return 2; }
    msort(lines, tmp, nlines);
    FILE *o = stdout;
    if (outfile && !(o = fopen(outfile, "w"))) { fprintf(stderr, "sort: open failed: %s\n", outfile); return 2; }
    for (long k = 0; k < nlines; k++) {
        if (g_u && k && !keycmp(&lines[k - 1], &lines[k])) continue;
        fwrite(lines[k].s, 1, (size_t)lines[k].n, o);
        fputc('\n', o);
    }
    if (o != stdout) fclose(o);
    return 0;
}
