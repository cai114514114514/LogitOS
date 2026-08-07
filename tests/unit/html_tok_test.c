/* tests/unit/html_tok_test.c -- run the shared html5lib TOKENIZER suite
 * against c/apps/browser/html_tokenizer.c and report a pass count.
 *
 * Companion to tests/unit/html5lib_test.c, which does the same for tree
 * construction.  Same reasoning: the point is to have a NUMBER for how
 * spec-conformant the tokenizer is rather than a feeling, and the tokenizer is
 * the layer where that number can honestly be near 100% -- it is mechanical,
 * it has no interaction with the DOM, and the suite covers it exhaustively
 * (6810 cases, of which the entity table alone accounts for 4210).
 *
 * The cases are converted from JSON to a C table by tools/gen_html5lib_tok.py;
 * see that file for the serialisation format both sides produce.  Two rules of
 * the suite show up here:
 *
 *   - adjacent character tokens are coalesced.  A tokenizer may split a text
 *     run across as many tokens as it likes (ours does: the zero-copy fast
 *     path emits a run, then a character reference emits its expansion), so
 *     the comparison is over the concatenation.
 *
 *   - the "errors" arrays are parse-error CODES AND POSITIONS.  We do not
 *     report parse errors at all -- nothing above the tokenizer consumes them,
 *     and carrying line/column through the state machine would cost every
 *     state a counter.  Cases whose expected TOKENS therefore cannot match are
 *     listed in expected_fail[] below, with the reason.
 *
 * Usage: html_tok_test [-v [n]]
 * Exit code is 0 whatever the rate: this is a MEASUREMENT, not a gate --
 * same policy as make test-html5lib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_tokenizer.h"
#include "html5lib_tok_cases.inc"

/* --------------------------------------------------------------------------
 * Known-unpassable cases.
 *
 * Keep this list SHORT and each entry justified: it is the difference between
 * "we measured 99%" and "we measured 99% of what we felt like measuring".
 * ------------------------------------------------------------------------ */
static const struct { const char *file, *desc; } expected_fail[] = {
    /* Empty, and that is the honest answer rather than an omission: every case
     * in the suite whose expectation is a TOKEN STREAM passes.  The suite's
     * "errors" arrays (codes plus line/column) are the part we do not
     * implement, and no case's token stream depends on them, so nothing had to
     * be excused.  The mechanism stays because a future spec sync will
     * eventually add a case we choose not to follow, and the alternative --
     * quietly lowering the target -- is how a conformance number stops meaning
     * anything.
     *
     * xmlViolationTests (4 cases in xmlViolation.test) are not in the corpus at
     * all: they ask for a different output mode, in which the tokenizer mangles
     * characters so the result is well-formed XML.  The converter drops that
     * file; see tools/gen_html5lib_tok.py. */
    { 0, 0 }
};

static int is_expected_fail(const char *file, const char *desc)
{
    for (unsigned i = 0; i < sizeof expected_fail / sizeof expected_fail[0]; i++)
        if (expected_fail[i].file && expected_fail[i].desc &&
            !strcmp(expected_fail[i].file, file) && !strcmp(expected_fail[i].desc, desc))
            return 1;
    return 0;
}

/* ------------------------------------------------------------- string --- */
struct sb { char *p; size_t len, cap; };

static void sb_put(struct sb *s, const char *d, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 128;
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

static const char HEX[] = "0123456789ABCDEF";

/* Escape so the record separators cannot occur inside a field -- see the
 * generator: an attribute value of "a|b" must not read as two attributes. */
static void sb_esc(struct sb *s, const char *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (c == '|' || c == ',' || c == '=' || c == '%' || c < 0x20 || c >= 0x7F) {
            char t[3] = { '%', HEX[c >> 4], HEX[c & 15] };
            sb_put(s, t, 3);
        } else {
            sb_put(s, (const char *)&c, 1);
        }
    }
}

#define NONE "%00"

/* ------------------------------------------------------------- runner --- */

/* Our attributes come out in source order; the suite treats them as a set, so
 * emit them sorted by name (insertion sort -- tags have a handful of
 * attributes, and a qsort comparator would need a global for the escaping). */
static void put_attrs(struct sb *s, const struct html_attr *a, int n)
{
    int *ord = malloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) {
        int v = ord[i], j = i - 1;
        while (j >= 0) {
            const struct html_attr *x = &a[ord[j]], *y = &a[v];
            uint32_t k = x->nl < y->nl ? x->nl : y->nl;
            int c = k ? memcmp(x->n, y->n, k) : 0;
            if (c == 0) c = (int)x->nl - (int)y->nl;
            if (c <= 0) break;
            ord[j + 1] = ord[j];
            j--;
        }
        ord[j + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        if (i) sb_str(s, ",");
        sb_esc(s, a[ord[i]].n, a[ord[i]].nl);
        sb_str(s, "=");
        sb_esc(s, a[ord[i]].v, a[ord[i]].vl);
    }
    free(ord);
}

/* stream == 0: hand the tokenizer the whole input at once, the way the browser
 * does today.
 * stream == 1: hand it ONE BYTE AT A TIME, growing the buffer and only setting
 * eof on the last byte.  Every case then drives the rule-3 rewind path -- a
 * token that spans a "network boundary" gets abandoned and re-tokenized -- and
 * the two runs must produce identical token streams.  That is the whole claim
 * of the streaming design, checked 7032 times instead of asserted in a comment.
 */
static char *tokenize(const struct tokcase *c, int stream)
{
    struct html_tokenizer tk;
    struct sb out = { 0, 0, 0 }, chars = { 0, 0, 0 };
    char num[32];
    size_t fed = stream ? 0 : (size_t)c->inputlen;

    html_tok_init(&tk, c->input, fed, !stream || c->inputlen == 0);
    html_tok_set_state(&tk, c->state);
    if (c->last_start_tag)
        html_tok_set_last_start_tag(&tk, c->last_start_tag,
                                    (uint32_t)strlen(c->last_start_tag));

    for (;;) {
        struct html_token t;
        int r = html_tok_next(&tk, &t);
        if (r == 0) {                       /* needs more input */
            if (fed >= (size_t)c->inputlen) break;   /* cannot happen at eof */
            fed++;
            html_tok_feed(&tk, c->input, fed, fed >= (size_t)c->inputlen);
            continue;
        }
        if (r < 0) break;
        if (t.type == TOK_EOF) break;

        /* Rule 2's contract: a TOK_CHARS payload may point straight into the
         * input, so it has to be consumed before the next call.  Buffering it
         * here is also how the suite's coalescing rule is honoured. */
        if (t.type == TOK_CHARS) { sb_put(&chars, t.data, t.datalen); continue; }
        if (chars.len) { sb_str(&out, "T|"); sb_esc(&out, chars.p, (uint32_t)chars.len);
                         sb_str(&out, "\n"); chars.len = 0; }

        switch (t.type) {
        case TOK_DOCTYPE:
            sb_str(&out, "D|");
            if (t.has_name) sb_esc(&out, t.name, t.namelen); else sb_str(&out, NONE);
            sb_str(&out, "|");
            if (t.has_pubid) sb_esc(&out, t.pubid, t.pubidlen); else sb_str(&out, NONE);
            sb_str(&out, "|");
            if (t.has_sysid) sb_esc(&out, t.sysid, t.sysidlen); else sb_str(&out, NONE);
            snprintf(num, sizeof num, "|%d\n", t.force_quirks ? 1 : 0);
            sb_str(&out, num);
            break;
        case TOK_START:
            sb_str(&out, "S|");
            sb_esc(&out, t.name, t.namelen);
            sb_str(&out, "|");
            put_attrs(&out, t.attrs, t.nattr);
            snprintf(num, sizeof num, "|%d\n", t.self_closing ? 1 : 0);
            sb_str(&out, num);
            break;
        case TOK_END:
            sb_str(&out, "E|");
            sb_esc(&out, t.name, t.namelen);
            sb_str(&out, "\n");
            break;
        case TOK_COMMENT:
            sb_str(&out, "C|");
            sb_esc(&out, t.data, t.datalen);
            sb_str(&out, "\n");
            break;
        default:
            break;
        }
    }
    if (chars.len) { sb_str(&out, "T|"); sb_esc(&out, chars.p, (uint32_t)chars.len);
                     sb_str(&out, "\n"); }

    sb_free(&chars);
    html_tok_free(&tk);
    if (!out.p) sb_str(&out, "");
    return out.p;
}

/* ---------------------------------------------------------------- main -- */
int main(int argc, char **argv)
{
    int verbose = 0, vmax = 10, shown = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) { verbose = 1; if (i + 1 < argc) vmax = atoi(argv[i + 1]); }

    long total = 0, pass = 0, known = 0, known_pass = 0, stream_diff = 0;

    /* per-file tallies, in first-seen order */
    const char *fnames[64]; long fp[64], ft[64]; int nf = 0;

    for (int i = 0; i < NTOKCASES; i++) {
        const struct tokcase *c = &tokcases[i];

        int fi;
        for (fi = 0; fi < nf; fi++) if (!strcmp(fnames[fi], c->file)) break;
        if (fi == nf && nf < 64) { fnames[nf] = c->file; fp[nf] = ft[nf] = 0; nf++; }

        char *got = tokenize(c, 0);
        char *str = tokenize(c, 1);
        if (strcmp(got, str)) {
            stream_diff++;
            if (verbose && shown < vmax) {
                shown++;
                printf("\n--- STREAM MISMATCH %s: %s ---\n--- whole ---\n%s"
                       "--- byte-at-a-time ---\n%s", c->file, c->desc, got, str);
            }
        }
        free(str);
        int ok = !strcmp(got, c->expect);
        int exp_fail = is_expected_fail(c->file, c->desc);

        if (exp_fail) {
            known++;
            if (ok) known_pass++;
        } else {
            total++; ft[fi]++;
            if (ok) { pass++; fp[fi]++; }
            else if (verbose && shown < vmax) {
                shown++;
                printf("\n--- FAIL %s: %s ---\ninput(%d): ",
                       c->file, c->desc, c->inputlen);
                for (int k = 0; k < c->inputlen; k++) {
                    unsigned char ch = (unsigned char)c->input[k];
                    if (ch >= 0x20 && ch < 0x7F) putchar(ch);
                    else printf("\\x%02x", ch);
                }
                printf("\nstate=%d last=%s\n--- want ---\n%s--- got ---\n%s",
                       c->state, c->last_start_tag ? c->last_start_tag : "(none)",
                       c->expect, got);
            }
        }
        free(got);
    }

    for (int i = 0; i < nf; i++)
        if (ft[i]) printf("  %-32s %5ld/%-5ld\n", fnames[i], fp[i], ft[i]);

    printf("\nhtml5lib tokenizer: %ld/%ld passed (%.2f%%), %ld expected-fail"
           " (%ld of those pass anyway)\n",
           pass, total, total ? 100.0 * (double)pass / (double)total : 0.0,
           known, known_pass);
    printf("streaming cross-check: %ld/%d cases identical byte-at-a-time\n",
           (long)NTOKCASES - stream_diff, NTOKCASES);
    return 0;
}
