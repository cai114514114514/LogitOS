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
 * Foreign elements carry their namespace: "<svg circle>", "<math mi>".
 *
 * Two optional sections change how a case is run:
 *   #document-fragment <context>   parse with the fragment algorithm
 *   #script-on / #script-off       the expected tree for a parser with
 *                                  scripting enabled / disabled
 *
 * We are scripting-ENABLED (browser.c runs a page's <script>s), so #script-on
 * cases are run and #script-off cases are skipped. That is a choice, not an
 * omission: no parser can satisfy both, and picking the one that matches the
 * product is the only honest option.
 *
 * ------------------------------------------------------------------------
 * THE BASELINE
 * ------------------------------------------------------------------------
 * A pass RATE tells you where you are; it does not tell you whether the change
 * you just made broke something. tests/unit/html5lib_expected_fail.txt lists
 * every case that fails today, by "<file>:<case index>". The runner diffs
 * against it and reports NEW FAILURES and NEWLY PASSING separately, so a change
 * that fixes twelve cases and breaks one is visible as exactly that instead of
 * as "+11".
 *
 * Usage: html5lib_test <dir-with-.dat-files> [-v [n]] [-b <baseline>]
 *                      [--write-baseline] [--strict]
 *   -v [n]            print the first n failing cases with expected/got trees
 *   -b <path>         baseline file (default tests/unit/html5lib_expected_fail.txt)
 *   --write-baseline  rewrite the baseline from this run's failures
 *   --strict          exit non-zero if there are new failures
 *
 * Without --strict the exit code is 0 whatever the rate: this is a
 * MEASUREMENT. Wiring it as a gate only makes sense once there is a rate worth
 * defending; until then a red build every run teaches people to ignore the
 * build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "dom.h"
#include "html_tree.h"
#include "dom_serialize.h"

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

/* ------------------------------------------------------- .dat parsing ---- */
struct kase {
    char *data;        /* input HTML */
    long  datalen;
    char *doc;         /* expected serialisation */
    char *frag;        /* #document-fragment context, or NULL */
    int   script_off;  /* #script-off -- expects scripting disabled */
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
        char *soff = strstr(errs, "\n#script-off\n");
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

        if (frag && (!nxt || frag < nxt)) {
            char *fs = frag + 20;                  /* past "\n#document-fragment\n" */
            char *fe = strchr(fs, '\n');
            if (fe) {
                k[n].frag = malloc((size_t)(fe - fs) + 1);
                memcpy(k[n].frag, fs, (size_t)(fe - fs));
                k[n].frag[fe - fs] = 0;
            }
        }
        k[n].script_off = soff && (!nxt || soff < nxt);
        n++;
        p = nxt ? nxt + 1 : end;
    }
    *out = k;
    return n;
}

/* --------------------------------------------------------- baseline ------ */
struct baseline { char **id; int n, cap; };

static void bl_add(struct baseline *b, const char *id)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->id = realloc(b->id, (size_t)b->cap * sizeof *b->id);
    }
    b->id[b->n++] = strdup(id);
}

static int bl_has(const struct baseline *b, const char *id)
{
    for (int i = 0; i < b->n; i++) if (!strcmp(b->id[i], id)) return 1;
    return 0;
}

static void bl_load(struct baseline *b, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*s) bl_add(b, s);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- main -- */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dir> [-v [n]] [-b <baseline>]"
                                    " [--write-baseline] [--strict]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    const char *blpath = "tests/unit/html5lib_expected_fail.txt";
    int verbose = 0, vmax = 5, writebl = 0, strict = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; if (i + 1 < argc && argv[i+1][0] != '-') vmax = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) blpath = argv[++i];
        else if (!strcmp(argv[i], "--write-baseline")) writebl = 1;
        else if (!strcmp(argv[i], "--strict")) strict = 1;
    }

    struct baseline expected = { 0, 0, 0 };
    bl_load(&expected, blpath);

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

    struct baseline failures = { 0, 0, 0 };

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
            /* We are scripting-enabled; a #script-off case asks for a tree only
             * a scripting-disabled parser can produce. */
            if (k[i].script_off) { skipped++; continue; }
            ftotal++; total++;

            struct dom_doc *doc = 0;
            struct node *root;
            if (k[i].frag) {
                /* Context is "tag" or "<ns> <tag>" ("svg path", "math ms"). */
                const char *ctx = k[i].frag;
                int ns = NS_HTML;
                const char *sp = strchr(ctx, ' ');
                if (sp) {
                    if (!strncmp(ctx, "svg ", 4)) ns = NS_SVG;
                    else if (!strncmp(ctx, "math ", 5)) ns = NS_MATHML;
                    ctx = sp + 1;
                }
                root = html_parse_fragment(&doc, k[i].data, (int)k[i].datalen,
                                           ctx, (int)strlen(ctx), ns);
            } else {
                root = html_parse(&doc, k[i].data, (int)k[i].datalen);
            }

            char *got = root ? dom_serialize_test(root) : 0;
            if (!got) got = strdup("");
            int ok = !strcmp(got, k[i].doc);
            char id[288];
            snprintf(id, sizeof id, "%s:%d", names[fi], i);
            if (ok) { pass++; fpass++; }
            else {
                bl_add(&failures, id);
                if (verbose && shown < vmax && !bl_has(&expected, id)) {
                    shown++;
                    printf("\n--- FAIL %s ---\ninput: %s\n%s--- want ---\n%s--- got ---\n%s",
                           id, k[i].data, k[i].frag ? "" : "",
                           k[i].doc, got);
                }
            }
            free(got);
            if (doc) dom_free(dom_doc_root(doc));
        }

        if (ftotal) printf("  %-40s %4ld/%-4ld\n", names[fi], fpass, ftotal);
        for (int i = 0; i < nk; i++) { free(k[i].data); free(k[i].doc); free(k[i].frag); }
        free(k);
        free(buf);
    }

    printf("\nhtml5lib tree-construction: %ld/%ld passed (%.1f%%), %ld skipped"
           " (#script-off: we parse with scripting enabled)\n",
           pass, total, total ? 100.0 * (double)pass / (double)total : 0.0, skipped);

    /* Diff against the baseline. */
    int newfail = 0, newpass = 0;
    for (int i = 0; i < failures.n; i++)
        if (!bl_has(&expected, failures.id[i])) {
            if (newfail < 20) printf("  NEW FAILURE: %s\n", failures.id[i]);
            newfail++;
        }
    for (int i = 0; i < expected.n; i++)
        if (!bl_has(&failures, expected.id[i])) newpass++;

    if (expected.n)
        printf("baseline %s: %d expected failures; %d new, %d newly passing\n",
               blpath, expected.n, newfail, newpass);
    else
        printf("baseline %s: not found (run with --write-baseline to create it)\n", blpath);

    if (writebl) {
        FILE *f = fopen(blpath, "wb");
        if (f) {
            fprintf(f, "# html5lib tree-construction cases that fail today, as\n"
                       "# \"<file>:<case index>\". Regenerate with --write-baseline.\n"
                       "# Shrinking this file is the point; growing it needs a reason\n"
                       "# written next to the new entries.\n");
            for (int i = 0; i < failures.n; i++) fprintf(f, "%s\n", failures.id[i]);
            fclose(f);
            printf("wrote %d entries to %s\n", failures.n, blpath);
        } else {
            fprintf(stderr, "cannot write %s\n", blpath);
        }
    }

    for (int i = 0; i < nn; i++) free(names[i]);
    for (int i = 0; i < failures.n; i++) free(failures.id[i]);
    for (int i = 0; i < expected.n; i++) free(expected.id[i]);
    free(failures.id); free(expected.id);

    return (strict && newfail) ? 1 : 0;
}
