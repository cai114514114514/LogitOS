/* tests/unit/html5lib_fuzz.c -- memory-safety stress for the HTML5 tree
 * builder.  Build with -fsanitize=address,undefined; the pass condition is
 * "the sanitizers said nothing", so it prints a parse count and exits 0.
 *
 * Why this exists separately from html5lib_test.c: that runner checks whether
 * the TREE is right, and it only ever parses well-formed corpus inputs as whole
 * documents.  The way a hand-written tree builder actually fails is different --
 * a use-after-free or a stale index in the adoption agency algorithm's
 * reparenting loop, reached only when the stack of open elements is in a shape
 * the corpus does not contain.  Three things produce those shapes cheaply:
 *
 *   - TRUNCATIONS. Cutting an input mid-tag leaves elements open at EOF, so
 *     every "pop until X" and every reset-insertion-mode walk runs against a
 *     stack that no end tag ever balanced.
 *   - MUTATIONS. Random bytes turn "</b>" into "</b\x03" and formatting
 *     elements into unknown ones, which is how the AAA's "formattingElement is
 *     not in the stack" and "no furthestBlock" branches get exercised.
 *   - FRAGMENT CONTEXTS. Parsing the same input as the contents of <td>, of
 *     <select>, of <svg path> puts the parser in insertion modes the document
 *     parse never starts in.
 *
 * The inputs come from the html5lib corpus rather than from nothing, because
 * random bytes almost never produce a nested formatting element; corpus text
 * with a few bytes changed does.
 *
 * Usage: html5lib_fuzz [<dir-with-.dat-files>]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "dom.h"
#include "html_tree.h"
#include "dom_serialize.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* xorshift64: the mutations must be identical run to run, or a crash cannot be
 * reproduced from the report. */
static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)rs; }

static long runs;

static void go(const char *s, int n)
{
    static const char *const HCTX[] = { "td", "tr", "table", "select", "body",
                                        "div", "script", "title", "template", 0 };
    static const char *const SCTX[] = { "path", "desc", "foreignObject", 0 };

    struct dom_doc *d = 0;
    struct node *r = html_parse(&d, s, n);
    free(dom_serialize_test(r));
    free(dom_serialize_html(r, 0));
    if (d) dom_free(dom_doc_root(d));
    runs++;

    for (int i = 0; HCTX[i]; i++) {
        struct dom_doc *f = 0;
        struct node *fr = html_parse_fragment(&f, s, n, HCTX[i], (int)strlen(HCTX[i]), NS_HTML);
        free(dom_serialize_test(fr));
        if (f) dom_free(dom_doc_root(f));
        runs++;
    }
    for (int i = 0; SCTX[i]; i++) {
        struct dom_doc *f = 0;
        struct node *fr = html_parse_fragment(&f, s, n, SCTX[i], (int)strlen(SCTX[i]), NS_SVG);
        free(dom_serialize_test(fr));
        if (f) dom_free(dom_doc_root(f));
        runs++;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "third_party/html5lib-tests/tree-construction";
    DIR *dp = opendir(dir);
    if (!dp) { fprintf(stderr, "cannot open %s\n", dir); return 2; }

    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 4, ".dat")) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *b = malloc((size_t)sz + 1);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); continue; }
        b[sz] = 0;
        fclose(f);

        for (char *p = b; (p = strstr(p, "#data\n")); ) {
            char *body = p + 6;
            char *e = strstr(body, "\n#errors\n");
            if (!e) break;
            int n = (int)(e - body);

            go(body, n);
            for (int k = 1; k <= 4; k++) go(body, n * k / 5);

            for (int m = 0; m < 3 && n > 0; m++) {
                char *c = malloc((size_t)n);
                if (!c) break;
                memcpy(c, body, (size_t)n);
                for (int j = 0; j < 3; j++) c[rnd() % (unsigned)n] = (char)(rnd() & 0x7F);
                go(c, n);
                free(c);
            }
            p = e + 1;
        }
        free(b);
    }
    closedir(dp);

    printf("html5lib_fuzz: %ld parses, no sanitizer reports\n", runs);
    return 0;
}
