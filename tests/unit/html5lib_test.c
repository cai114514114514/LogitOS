/* tests/unit/html5lib_test.c -- run the shared HTML5 tree-construction suite
 * against our parser and report a pass count.
 *
 * The suite is the one every HTML parser is measured against. Its cases are
 * plain data (third_party/html5lib-tests/), so this runner is ours; the point
 * is to have a NUMBER for how spec-conformant the parser is, instead of a
 * feeling. A hand-written parser that says "production-grade" has to be able
 * to answer "compared to what?", and this is the answer.
 *
 * Case format (one blank line between cases):
 *
 *     #data
 *     <the input HTML>
 *     #errors
 *     (line,col): error-code            <- we do not check these
 *     #document
 *     | <html>
 *     |   <head>
 *     |   <body>
 *     |     "text"
 *
 * The expected tree is 2 spaces of indent per depth after a "| " prefix.
 * Elements are <tag>, attributes appear on their own lines sorted by name,
 * text is quoted, comments are <!-- ... -->, doctypes are <!DOCTYPE ...>.
 *
 * Usage: html5lib_test <dir-with-.dat-files> [-v]
 *   -v prints the first N failing cases with expected/got trees.
 *
 * Exit code is 0 whatever the pass rate: this is a MEASUREMENT, not a gate.
 * Wiring it as a gate only makes sense once there is a rate worth defending;
 * until then a red build every run teaches people to ignore the build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "dom.h"

/* dom.c allocates through the browser's arena shims; on the host they are
 * plain malloc/free, the same stubs every other browser test uses. */
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }

/* ------------------------------------------------------------- helpers -- */
static char *xread(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return 0; }
    b[n] = 0;
    fclose(f);
    *len = n;
    return b;
}

/* A growable string, so serialisation has no length cap of its own -- the
 * point of this suite is to find OUR caps, not to add new ones. */
struct sb { char *p; size_t len, cap; };

static void sb_put(struct sb *s, const char *d, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 256;
        while (c < s->len + n + 1) c *= 2;
        s->p = realloc(s->p, c);
        s->cap = c;
    }
    memcpy(s->p + s->len, d, n);
    s->len += n;
    s->p[s->len] = 0;
}
static void sb_str(struct sb *s, const char *d) { sb_put(s, d, strlen(d)); }
static void sb_free(struct sb *s) { free(s->p); s->p = 0; s->len = s->cap = 0; }

/* --------------------------------------------------- our tree -> text ---- */
/* Serialise in the suite's own format so the comparison is a plain strcmp.
 * Attributes are emitted sorted by name, which is what the format requires
 * (it is a set, not a sequence). */
static void ser_indent(struct sb *s, int depth)
{
    sb_str(s, "| ");
    for (int i = 0; i < depth; i++) sb_str(s, "  ");
}

static void ser_node(struct sb *s, const struct node *n, int depth)
{
    if (n->type == N_TEXT) {
        ser_indent(s, depth);
        sb_str(s, "\"");
        sb_put(s, n->text ? n->text : "", n->text ? (size_t)n->textlen : 0);
        sb_str(s, "\"\n");
        return;
    }

    ser_indent(s, depth);
    sb_str(s, "<");
    sb_str(s, n->tag);
    sb_str(s, ">\n");

    /* attributes, sorted by name */
    if (n->nattr > 0) {
        int *order = malloc(sizeof(int) * (size_t)n->nattr);
        for (int i = 0; i < n->nattr; i++) order[i] = i;
        for (int i = 1; i < n->nattr; i++) {          /* insertion sort */
            int v = order[i], j = i - 1;
            while (j >= 0 && strcmp(n->attrs[order[j]].name, n->attrs[v].name) > 0) {
                order[j + 1] = order[j]; j--;
            }
            order[j + 1] = v;
        }
        for (int i = 0; i < n->nattr; i++) {
            ser_indent(s, depth + 1);
            sb_str(s, n->attrs[order[i]].name);
            sb_str(s, "=\"");
            sb_str(s, n->attrs[order[i]].val);
            sb_str(s, "\"\n");
        }
        free(order);
    }

    for (const struct node *c = n->first_child; c; c = c->next)
        ser_node(s, c, depth + 1);
}

static char *serialize(const struct node *root)
{
    struct sb s = { 0, 0, 0 };
    for (const struct node *c = root->first_child; c; c = c->next)
        ser_node(&s, c, 0);
    if (!s.p) sb_str(&s, "");
    return s.p;
}

/* ------------------------------------------------------- .dat parsing ---- */
struct kase {
    char *data;        /* input HTML */
    long  datalen;
    char *doc;         /* expected serialisation */
    int   fragment;    /* #document-fragment case -- we do not support these */
    int   scripted;    /* #script-on -- needs a script-executing parser */
};

/* Split one .dat buffer into cases. The separator is a blank line followed by
 * "#data", which is the only reliable delimiter: a blank line alone can occur
 * inside the input and inside the expected tree. */
static int split_cases(char *buf, long len, struct kase **out)
{
    int cap = 64, n = 0;
    struct kase *k = calloc((size_t)cap, sizeof *k);
    char *p = buf, *end = buf + len;

    while (p < end) {
        char *d = strstr(p, "#data\n");
        if (!d) break;
        if (d != buf && d[-1] != '\n') { p = d + 6; continue; }
        char *body = d + 6;

        char *errs = strstr(body, "\n#errors\n");
        if (!errs) break;
        char *doc = strstr(errs, "\n#document\n");
        char *frag = strstr(errs, "\n#document-fragment\n");
        char *son  = strstr(errs, "\n#script-on\n");
        if (!doc) break;

        /* the next case starts at the next "\n#data\n" after this #document */
        char *nxt = strstr(doc, "\n#data\n");
        char *docend = nxt ? nxt : end;

        if (n == cap) { cap *= 2; k = realloc(k, (size_t)cap * sizeof *k);
                        memset(k + n, 0, (size_t)(cap - n) * sizeof *k); }

        k[n].datalen = errs - body;
        k[n].data = malloc((size_t)k[n].datalen + 1);
        memcpy(k[n].data, body, (size_t)k[n].datalen);
        k[n].data[k[n].datalen] = 0;

        char *ds = doc + 11;                       /* past "\n#document\n" */
        long dl = docend - ds;
        while (dl > 0 && ds[dl - 1] == '\n') dl--;  /* trailing blank line */
        k[n].doc = malloc((size_t)dl + 2);
        memcpy(k[n].doc, ds, (size_t)dl);
        k[n].doc[dl] = '\n';
        k[n].doc[dl + 1] = 0;

        k[n].fragment = frag && (!nxt || frag < nxt);
        k[n].scripted = son && (!nxt || son < nxt);
        n++;
        p = nxt ? nxt + 1 : end;
    }
    *out = k;
    return n;
}

/* ---------------------------------------------------------------- main -- */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dir> [-v [n]]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    int verbose = 0, vmax = 5;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; if (i + 1 < argc) vmax = atoi(argv[i + 1]); }
    }

    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open %s\n", dir); return 2; }

    long total = 0, pass = 0, skipped = 0, shown = 0;
    struct dirent *de;
    /* collect + sort filenames so the report is stable run to run */
    char *names[256]; int nn = 0;
    while ((de = readdir(d)) && nn < 256) {
        size_t l = strlen(de->d_name);
        if (l > 4 && !strcmp(de->d_name + l - 4, ".dat")) names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    for (int i = 1; i < nn; i++) {
        char *v = names[i]; int j = i - 1;
        while (j >= 0 && strcmp(names[j], v) > 0) { names[j + 1] = names[j]; j--; }
        names[j + 1] = v;
    }

    for (int fi = 0; fi < nn; fi++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[fi]);
        long len = 0;
        char *buf = xread(path, &len);
        if (!buf) continue;

        struct kase *k = 0;
        int nk = split_cases(buf, len, &k);
        long fpass = 0, ftotal = 0;

        for (int i = 0; i < nk; i++) {
            if (k[i].fragment || k[i].scripted) { skipped++; continue; }
            ftotal++; total++;

            struct node *root = dom_parse(k[i].data, (int)k[i].datalen);
            char *got = root ? serialize(root) : strdup("");
            int ok = !strcmp(got, k[i].doc);
            if (ok) { pass++; fpass++; }
            else if (verbose && shown < vmax) {
                shown++;
                printf("\n--- FAIL %s case %d ---\ninput: %s\n--- want ---\n%s--- got ---\n%s",
                       names[fi], i, k[i].data, k[i].doc, got);
            }
            free(got);
            if (root) dom_free(root);
        }

        if (ftotal) printf("  %-40s %4ld/%-4ld\n", names[fi], fpass, ftotal);
        for (int i = 0; i < nk; i++) { free(k[i].data); free(k[i].doc); }
        free(k);
        free(buf);
        free(names[fi]);
    }

    printf("\nhtml5lib tree-construction: %ld/%ld passed (%.1f%%), %ld skipped"
           " (fragment/scripted cases we do not support)\n",
           pass, total, total ? 100.0 * (double)pass / (double)total : 0.0, skipped);
    return 0;
}
