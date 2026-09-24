/* diff [-u | -U N] [-q] FILE1 FILE2
 *
 * Line diff by Myers' O((N+M)D) algorithm, printed in the POSIX default
 * format (NaM,M / NcM / NdM with < and > lines) or unified format (-u, three
 * lines of context). Exit status 0 same, 1 different, 2 trouble.
 *
 * Two correct diffs of the same files may differ -- which of two equal-cost
 * edit scripts is chosen is an implementation detail -- so the gate checks a
 * diff by APPLYING it with patch(1) and getting the second file back, never by
 * comparing its bytes with another diff's. "\ No newline at end of file" is
 * emitted where a file's last line lacks one, so patch can reproduce that too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct file { char **l; int *len; int n; int noeol; const char *name; };

static int readf(const char *name, struct file *f)
{
    FILE *fp = strcmp(name, "-") ? fopen(name, "r") : stdin;
    if (!fp) { fprintf(stderr, "diff: %s: No such file or directory\n", name); return -1; }
    f->name = name; f->n = 0; f->l = 0; f->len = 0; f->noeol = 0;
    int cap = 0;
    char *b = 0; size_t c = 0; ssize_t k;
    while ((k = getline(&b, &c, fp)) >= 0) {
        if (f->n == cap) { cap = cap ? cap * 2 : 256; f->l = realloc(f->l, sizeof *f->l * (size_t)cap); f->len = realloc(f->len, sizeof *f->len * (size_t)cap); }
        if (k && b[k - 1] == '\n') k--; else f->noeol = 1;
        f->l[f->n] = malloc((size_t)k + 1);
        memcpy(f->l[f->n], b, (size_t)k); f->l[f->n][k] = 0;
        f->len[f->n] = (int)k;
        f->n++;
    }
    free(b);
    if (fp != stdin) fclose(fp);
    return 0;
}

static struct file A, B;
static int eq(int i, int j) { return A.len[i] == B.len[j] && !memcmp(A.l[i], B.l[j], (size_t)A.len[i]); }

/* The edit script as a sequence of ops over (i, j): 0 keep, 1 delete A[i], 2 insert B[j]. */
static char *ops; static int nops;

static void myers(void)
{
    int n = A.n, m = B.n, max = n + m;
    int *v = calloc((size_t)(2 * max + 2), sizeof *v);
    int **trace = calloc((size_t)(max + 1), sizeof *trace);
    int off = max, d, found = -1;
    for (d = 0; d <= max; d++) {
        trace[d] = malloc(sizeof(int) * (size_t)(2 * max + 2));
        memcpy(trace[d], v, sizeof(int) * (size_t)(2 * max + 2));
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
            else x = v[off + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && eq(x, y)) { x++; y++; }
            v[off + k] = x;
            if (x >= n && y >= m) { found = d; break; }
        }
        if (found >= 0) break;
    }
    /* backtrack */
    ops = malloc((size_t)(n + m + 1)); nops = 0;
    char *rev = malloc((size_t)(n + m + 1)); int nr = 0;
    int x = n, y = m;
    for (d = found; d > 0; d--) {
        int *vv = trace[d], k = x - y, pk;
        if (k == -d || (k != d && vv[off + k - 1] < vv[off + k + 1])) pk = k + 1; else pk = k - 1;
        int px = vv[off + pk], py = px - pk;
        while (x > px && y > py) { rev[nr++] = 0; x--; y--; }
        if (x == px) { rev[nr++] = 2; y--; } else { rev[nr++] = 1; x--; }
    }
    while (x > 0 && y > 0) { rev[nr++] = 0; x--; y--; }
    while (nr) ops[nops++] = rev[--nr];
    free(rev);
    for (int t = 0; t <= (found < 0 ? max : found); t++) free(trace[t]);
    free(trace); free(v);
}

static void range(int a, int b)              /* 1-based inclusive, normal format */
{
    if (a == b) printf("%d", a); else printf("%d,%d", a, b);
}

static void line_out(char tag, struct file *f, int i)
{
    printf("%c%s%s\n", tag, tag == '<' || tag == '>' ? " " : "", f->l[i]);
#ifndef TEXTUTIL_NEGCTL
    /* THE CONTROL omits this marker: patch then gives the last line a newline
     * the file never had. */
    if (i == f->n - 1 && f->noeol) puts("\\ No newline at end of file");
#endif
}

static void normal(void)
{
    int i = 0, j = 0, p = 0;
    while (p < nops) {
        if (ops[p] == 0) { i++; j++; p++; continue; }
        int i0 = i, j0 = j;
        while (p < nops && ops[p] != 0) { if (ops[p] == 1) i++; else j++; p++; }
        int dels = i - i0, ins = j - j0;
        if (dels && ins) { range(i0 + 1, i); putchar('c'); range(j0 + 1, j); }
        else if (dels) { range(i0 + 1, i); printf("d%d", j0); }
        else { printf("%da", i0); range(j0 + 1, j); }
        putchar('\n');
        for (int k = i0; k < i; k++) line_out('<', &A, k);
        if (dels && ins) puts("---");
        for (int k = j0; k < j; k++) line_out('>', &B, k);
    }
}

static void unified(int ctx)
{
    printf("--- %s\n+++ %s\n", A.name, B.name);
    /* walk hunks: indices into ops, with positions */
    int p = 0, i = 0, j = 0;
    while (p < nops) {
        if (ops[p] == 0) { p++; i++; j++; continue; }
        /* hunk start: back up ctx lines */
        int hs = p, hi = i, hj = j, back = 0;
        while (back < ctx && hs > 0 && ops[hs - 1] == 0) { hs--; hi--; hj--; back++; }
        /* extend while gaps between changes are <= 2*ctx */
        int q = p, last = p;
        while (q < nops) {
            if (ops[q] != 0) { last = q; q++; continue; }
            int run = 0, r = q;
            while (r < nops && ops[r] == 0) { r++; run++; }
            if (r >= nops || run > 2 * ctx) break;
            q = r;
        }
        /* end: last change plus ctx lines */
        int he = last + 1, ei, ej;
        { int ii = hi, jj = hj; for (int t = hs; t < he; t++) { if (ops[t] != 2) ii++; if (ops[t] != 1) jj++; } ei = ii; ej = jj; }
        int fwd = 0;
        while (fwd < ctx && he < nops && ops[he] == 0) { he++; ei++; ej++; fwd++; }
        int alen = ei - hi, blen = ej - hj;
        printf("@@ -%d", alen ? hi + 1 : hi); if (alen != 1) printf(",%d", alen);
        printf(" +%d", blen ? hj + 1 : hj); if (blen != 1) printf(",%d", blen);
        puts(" @@");
        int ii = hi, jj = hj;
        for (int t = hs; t < he; t++) {
            if (ops[t] == 0) { line_out(' ', &A, ii); ii++; jj++; }
            else if (ops[t] == 1) { line_out('-', &A, ii); ii++; }
            else { line_out('+', &B, jj); jj++; }
        }
        p = he; i = ei; j = ej;
    }
}

int main(int argc, char **argv)
{
    int o_u = 0, o_q = 0, ctx = 3, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-u")) o_u = 1;
        else if (!strcmp(argv[i], "-q")) o_q = 1;
        else if (!strncmp(argv[i], "-U", 2)) { o_u = 1; ctx = atoi(argv[i][2] ? argv[i] + 2 : (++i < argc ? argv[i] : "3")); }
        else { fprintf(stderr, "diff: invalid option %s\n", argv[i]); return 2; }
    }
    if (argc - i != 2) { fputs("usage: diff [-u | -U N] [-q] FILE1 FILE2\n", stderr); return 2; }
    if (readf(argv[i], &A) || readf(argv[i + 1], &B)) return 2;
    myers();
    int differ = A.noeol != B.noeol;
    for (int p = 0; p < nops; p++) if (ops[p]) differ = 1;
    if (!differ) return 0;
    if (o_q) { printf("Files %s and %s differ\n", A.name, B.name); return 1; }
    if (o_u) unified(ctx); else normal();
    return 1;
}
