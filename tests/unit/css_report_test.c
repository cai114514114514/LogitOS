/* css_report_test -- the control for the stylesheet accounting record.
 *
 * WHAT THIS GATE IS FOR. c/apps/browser/css_report.c is an INSTRUMENT, and an
 * instrument whose counter never moves is not an instrument -- it is a
 * decoration that reads like evidence. The tree has paid for that shape
 * repeatedly (test-bidi-negctl printing "negative control ok" because a corpus
 * file was absent; test-url reporting 32/32 while saying both corpora were
 * missing). So this gate does three things and the second and third matter
 * more than the first:
 *
 *   1. a document with deliberately unparseable CSS: every counter must move,
 *      by kind, by the predicted amount;
 *   2. a CLEAN document: every drop counter must stay at ZERO. A drop counter
 *      that is never zero is a counter measuring the harness;
 *   3. the same dirty document with the hooks DETACHED: every counter must
 *      stay at zero. This is what proves the numbers come from the parser and
 *      not from anything in this file -- rule 5 of CLAUDE.md, and the
 *      structural version of the WPT scar (linking a translation unit is not
 *      running it).
 *
 * It also runs the pipeline the browser runs -- dom_parse, then css_apply --
 * rather than poking LibCSS directly, so the record is measured through the
 * same door the product uses.
 *
 * With an argument it becomes a probe rather than a gate:
 *     css_report_test <page.html> [sheet.css ...]
 * concatenates every <style> in the document plus every named .css file, runs
 * one css_apply over the lot, and prints the report. That is how a real page's
 * "the CSS rendering cannot be used" is turned into a cause.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "css.h"
#include "css_report.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int fails;
#define CHECK(c, m) do { \
    if (!(c)) { printf("FAIL: %s\n", m); fails = 1; } \
    else printf("ok: %s\n", m); } while (0)

/* Run one document + one stylesheet through the browser's own entry points and
 * leave the record holding the result. Returns the parsed root. */
static struct node *run(const char *html, const char *css)
{
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root) { printf("FAIL: dom_parse\n"); fails = 1; return NULL; }
    css_apply(root, css, (int)strlen(css));
    return root;
}

static const char *DOC =
    "<html><head></head><body><div id=a><p>x</p></div></body></html>";

/* Four constructs, one per way a rule can die, chosen to stay unparseable no
 * matter what modern CSS the parser grows next week: a made-up pseudo-class, a
 * made-up at-rule, a made-up property, and a valid property with a value that
 * is not one. A gate written against :has() or @layer would go green the day
 * somebody implements them and would then be measuring nothing. */
static const char *DIRTY =
    "div:logit-no-such-pseudo { color: red }\n"
    "@logit-no-such-at-rule { div { color: blue } }\n"
    "div { -logit-no-such-property: 1 }\n"
    "div { color: notacolour }\n"
    "p { margin: 0 }\n";

static const char *CLEAN =
    "div { color: #ff0000 }\n"
    "p { margin: 0 }\n"
    "#a { display: block }\n";

/* ---------------------------------------------------------------------- */

static struct node *find_id(struct node *n, const char *id)
{
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "id");
        if (v && strcmp(v, id) == 0) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_id(c, id);
        if (r) return r;
    }
    return NULL;
}

/* Count <link rel=stylesheet> THE WAY browser.c's collect_css_links does --
 * by walking the DOM, not by scanning the bytes. The two can disagree, and
 * when they do it is the whole answer: a <link> the tree builder relocated or
 * dropped is a stylesheet that is never requested, which is candidate 1 of the
 * five and looks exactly like candidate 4 from outside. */
static void count_dom_links(struct node *n, int *total, int *data_uri)
{
    if (!n) return;
    if (n->type == N_ELEM && strcmp(n->tag, "link") == 0) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        if (rel && href) {
            const char *p = rel;
            int hit = 0;
            for (; *p && !hit; p++) {
                const char *a = p, *b = "stylesheet";
                while (*a && *b) {
                    int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                    if (ca != *b) break;
                    a++; b++;
                }
                if (!*b) hit = 1;
            }
            if (hit) {
                (*total)++;
                if (strncmp(href, "data:", 5) == 0) (*data_uri)++;
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        count_dom_links(c, total, data_uri);
}

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) { printf("cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { /* short read is still data */ }
    b[n] = 0; *len = (int)n;
    fclose(f);
    return b;
}

/* Pull every <style> block out of raw HTML bytes. Deliberately a text scan and
 * not the DOM: this probe wants the bytes the page shipped even when the tree
 * builder moved or dropped the element. */
static int collect_style_text(const char *html, int hlen, char *out, int cap)
{
    int i, o = 0;
    for (i = 0; i + 6 < hlen; i++) {
        if (html[i] != '<') continue;
        if (strncasecmp(html + i, "<style", 6) != 0) continue;
        {
            int j = i + 6;
            while (j < hlen && html[j] != '>') j++;
            j++;
            while (j < hlen && o < cap - 1) {
                if (html[j] == '<' && j + 7 < hlen &&
                    strncasecmp(html + j, "</style", 7) == 0) break;
                out[o++] = html[j++];
            }
            if (o < cap - 1) out[o++] = '\n';
            i = j;
        }
    }
    out[o] = 0;
    return o;
}

static int probe(int argc, char **argv)
{
    static char css[8 * 1024 * 1024];
    int clen = 0, hlen = 0, i;
    char *html = slurp(argv[1], &hlen);
    struct node *root;

    if (!html) return 2;

    clen = collect_style_text(html, hlen, css, (int)sizeof css);
    printf("[probe] %s: %d bytes of HTML, %d bytes of inline <style>\n",
           argv[1], hlen, clen);
    css_report_style(clen);

    for (i = 2; i < argc; i++) {
        int slen = 0;
        char *s;
        if (argv[i][0] == '#') continue;     /* an element to dump, not a sheet */
        s = slurp(argv[i], &slen);
        if (!s) continue;
        printf("[probe]   + %s: %d bytes\n", argv[i], slen);
        css_report_link(argv[i], 0, 0);
        css_report_fetched(argv[i], 200, slen, CSSSH_OK, 0);
        if (clen + slen < (int)sizeof css - 1) {
            memcpy(css + clen, s, (size_t)slen);
            clen += slen;
            css[clen++] = '\n';
        }
        free(s);
    }
    css[clen] = 0;

    root = dom_parse(html, hlen);
    if (!root) { printf("[probe] dom_parse failed\n"); return 2; }
    {
        int dom_links = 0, dom_data = 0;
        count_dom_links(root, &dom_links, &dom_data);
        printf("[probe] <link rel=stylesheet> reachable in the DOM: %d "
               "(%d of them data:)\n", dom_links, dom_data);
    }

    /* THE SAME TWO STEPS THE BROWSER RUNS, IN THE SAME ORDER. browser.c does
     * css_expand_vars() and only then css_apply(), and skipping the first here
     * would make every var() reference an unparseable value and every
     * --custom-property a dropped declaration -- a report of a pipeline that
     * is not the product's. Measured on bing's 88 sheets, where nine of them
     * are nothing but `html{--token:value}`, that difference is most of the
     * drop count. */
    {
        static char expanded[10 * 1024 * 1024];
        int elen = css_expand_vars(css, clen, expanded, (int)sizeof expanded);
        int left = 0, k;
        /* A var() the expander could not resolve is left verbatim, and LibCSS
         * has no var() -- so the declaration dies in parseValue, BEFORE
         * parseProperty, which means language.c's drop hook never learns its
         * name. Those show up only as `parser resync: N declaration`. Counting
         * the survivors here is what connects the two numbers. */
        for (k = 0; k + 4 <= elen; k++)
            if (expanded[k] == 'v' && expanded[k+1] == 'a' &&
                expanded[k+2] == 'r' && expanded[k+3] == '(') left++;
        printf("[probe] var() expansion: %d -> %d bytes; %d var( still unresolved\n",
               clen, elen, left);
        css_report_concat(clen, 0, 0);
        css_report_expand(clen, elen, (int)sizeof expanded);
        css_apply(root, expanded, elen);
        css_report_parsed(1, elen);
    }
    css_report_print();

    /* Any argument beginning with '#' names an element to dump the COMPUTED
     * style of. This is what separates "the declaration was dropped" from "the
     * declaration applied and the layout engine did something else with it" --
     * the two halves of the ambiguity this whole record exists to break, and
     * the drop count alone cannot tell them apart. */
    for (i = 2; i < argc; i++) {
        struct node *e;
        if (argv[i][0] != '#') continue;
        e = find_id(root, argv[i] + 1);
        if (!e) { printf("[probe] #%s: not in the document\n", argv[i] + 1); continue; }
        if (!e->style) { printf("[probe] #%s: NO COMPUTED STYLE (the cascade "
                                "never reached it)\n", argv[i] + 1); continue; }
        {
            struct cstyle *s = (struct cstyle *)e->style;
            printf("[probe] #%s <%s>: display=%d list_item=%d list_style=%d "
                   "font_px=%d\n", argv[i] + 1, e->tag, s->display,
                   s->list_item, (int)s->list_style, s->font_px);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const struct css_report *r;

    css_init();          /* builds the UA sheet; not the page's CSS */

    if (argc > 1) {
        css_report_verbose = 1;
        css_report_reset();
        return probe(argc, argv);
    }

    /* --- 1. the counter moves, by kind, by the predicted amount ---------- */
    css_report_reset();
    run(DOC, DIRTY);
    r = css_report_get();
    printf("-- dirty: drops=%d sel=%d at=%d unk=%d bad=%d trail=%d kept=%d\n",
           r->drops, r->drop_selector, r->drop_atrule, r->drop_unknown_prop,
           r->drop_bad_value, r->drop_trailing, r->decl_accepted);
    css_report_print();
    CHECK(r->drop_selector >= 1, "unparseable selector is counted");
    CHECK(r->drop_atrule >= 1, "unknown at-rule is counted");
    CHECK(r->drop_unknown_prop >= 1, "unknown property is counted");
    CHECK(r->drop_bad_value >= 1, "bad value is counted");
    CHECK(r->drops >= 4, "total drops covers all four kinds");
    CHECK(r->decl_accepted >= 1, "the surviving declaration is counted as KEPT");
    CHECK(r->nreason >= 4, "each kind got its own reason bucket");

    /* The label, not just the count. A bucket key that does not name the
     * construct is a bucket nobody can act on. */
    {
        int saw_sel = 0, saw_at = 0, i;
        for (i = 0; i < r->nreason; i++) {
            if (strcmp(r->reason[i].kind, "selector") == 0 &&
                strstr(r->reason[i].key, "logit-no-such-pseudo")) saw_sel = 1;
            if (strcmp(r->reason[i].kind, "at-rule") == 0 &&
                strstr(r->reason[i].key, "@logit-no-such-at-rule")) saw_at = 1;
        }
        CHECK(saw_sel, "selector bucket names the pseudo-class that killed it");
        CHECK(saw_at, "at-rule bucket names the at-keyword that killed it");
    }

    /* --- 2. THE CONTROL: a clean document must read zero ----------------- */
    css_report_reset();
    run(DOC, CLEAN);
    r = css_report_get();
    printf("-- clean: drops=%d (sel=%d at=%d unk=%d bad=%d trail=%d) kept=%d\n",
           r->drops, r->drop_selector, r->drop_atrule, r->drop_unknown_prop,
           r->drop_bad_value, r->drop_trailing, r->decl_accepted);
    CHECK(r->drops == 0, "clean stylesheet drops NOTHING");
    CHECK(r->decl_accepted >= 3, "clean stylesheet's declarations are all kept");
    CHECK(r->recovered_selector == 0 && r->recovered_atrule == 0 &&
          r->recovered_decl == 0, "clean stylesheet needs no parser resync");

    /* --- 3. THE OTHER CONTROL: detached hooks must read zero on the DIRTY
     * document. If this fails, something in this harness is producing the
     * numbers and the gate above proves nothing about the parser. --------- */
    css_report_reset();
    css_report_detach();
    run(DOC, DIRTY);
    r = css_report_get();
    printf("-- detached: drops=%d kept=%d\n", r->drops, r->decl_accepted);
    CHECK(r->drops == 0 && r->decl_accepted == 0,
          "with the hooks detached the record stays at zero (the parser is the source)");

    /* --- 3b. the parse cache must not look like an absent stylesheet ------
     * css_engine.c caches the author sheet against its exact bytes, so the
     * SECOND css_apply of identical CSS never runs the parser and therefore
     * fires no drop hook. Without css_report_parse_cached() that load would
     * report "parsed: 0, dropped: 0" -- word for word what "the CSS never
     * arrived" looks like. This is the check that keeps the two apart. */
    css_report_reset();
    run(DOC, CLEAN);
    r = css_report_get();
    CHECK(r->parsed >= 1, "a fresh stylesheet is counted as parsed");
    {
        int first = r->parsed;
        css_report_reset();
        run(DOC, CLEAN);               /* byte-identical: the cache answers */
        r = css_report_get();
        CHECK(first >= 1 && r->parsed == 0 && r->parsed_cached >= 1,
              "a cache hit reports itself instead of reading as 'never parsed'");
    }

    /* --- 4. reset really resets ------------------------------------------ */
    css_report_reset();
    r = css_report_get();
    CHECK(r->drops == 0 && r->nreason == 0 && r->linked_style == 0,
          "reset clears the record");

    /* --- 5. discovery and truncation accounting -------------------------- */
    css_report_reset();
    css_report_style(100);
    css_report_link("https://x/a.css", 0, 0);
    css_report_link("https://x/b.css", 1, "duplicate");
    css_report_fetched("https://x/a.css", 200, 4096, CSSSH_OK, 0);
    css_report_concat(100, 300000, 200000);
    r = css_report_get();
    CHECK(r->linked_style == 1 && r->linked_link == 2 && r->link_skipped == 1,
          "discovery counts <style>, <link> and the skipped ones separately");
    CHECK(r->fetch_ok == 1, "a fetched sheet is counted");
    CHECK(r->truncated == 1 && r->truncated_bytes == 100000,
          "a stylesheet cut short by the buffer is named, with the byte count");

    printf(fails ? "css_report_test: FAIL\n" : "css_report_test: all checks passed\n");
    return fails;
}
