/* The LOADER, host-side: what browser.c does between "bytes arrived" and
 * "pixels", when the page itself asks to go somewhere else.
 *
 * ---------------------------------------------------------------------------
 * THE BUG THIS EXISTS FOR
 *
 * https://www.baidu.com/ loaded in 760 ms, dialled one connection, discovered
 * ZERO sub-resources and painted a blank white viewport. Two wrong facts, and
 * they turned out to be one: baidu SNIFFS THE USER-AGENT. To
 * "Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0" it does not serve the 697 KB
 * home page. It serves 227 bytes -- committed verbatim as
 * tests/fixtures/browser/baidu-ua-stub.html -- whose whole content is
 *
 *     <script>location.replace(location.href.replace("https://","http://"));</script>
 *
 * That document really has no stylesheets, no <script src>, no images and no
 * text, so BOTH observed facts were the correct rendering of the wrong
 * document. The same URL over http:// returns the full page.
 *
 * js_webapi.c recorded the location.replace() correctly and printed "the
 * loader does not consume this yet". browser.c is the loader. It does now.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS LINKS THE REAL LOADER
 *
 * browser.c needs exactly two things a host process cannot give it: a window
 * and a network. tests/unit/loaderhost/logit.h shadows the window syscalls
 * with recorders (and re-uses painthost's five drawing recorders), and
 * tests/unit/loader_fakebfetch.c implements bfetch.h over an in-memory site.
 * Everything else in the link is the real thing -- the real tokenizer, tree
 * builder, DOM, LibCSS cascade, layout, painter, QuickJS runtime and DOM
 * bindings. browser_load() is the one seam browser.c exports for this.
 *
 * The three fixture checks below are the diagnosis, kept honest by fixtures
 * captured from the live site rather than written by hand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include "logit.h"                 /* the recorders, via -Itests/unit/loaderhost */
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "loader_fakebfetch.h"

/* ---- the recorders' storage (declared extern by the two shadow headers) ---- */
struct paintop paint_ops[PAINT_MAXOPS];
int paint_nops;
int host_win_w = 1180, host_win_h = 620, host_flushes;
struct logit_event host_evq[HOST_EVQ];
int host_evq_head, host_evq_tail;
jmp_buf host_exit_jmp;
int host_exit_code, host_exited;
unsigned long long host_clock;

/* ---- stubs for the primitives the kernel normally provides ---- */
size_t malloc_peak;                      /* browser.c prints it after a load */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *url, uint8_t **buf, int *len)
{ (void)url; (void)buf; (void)len; return -1; }
void img_free(struct image *o) { (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out)
{ (void)p; (void)n; (void)out; return -1; }
void img_init(void) { }
/* The Rust staticlib is in the link for http1.c's inflate_raw (gzip/deflate
 * Content-Encoding); its image decoders register themselves against these. No
 * fixture here is an image, so registration is a no-op rather than a link of
 * c/lib/image. */
void img_register(img_detect_fn detect, img_decode_fn decode)
{ (void)detect; (void)decode; }
void img_register_anim(img_detect_fn detect, img_decode_fn decode, img_anim_fn anim)
{ (void)detect; (void)decode; (void)anim; }

void browser_load(const char *u);        /* browser.c's seam */

static int fail;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fail = 1; } \
                           else printf("ok: %s\n", msg); } while (0)

/* ------------------------------------------------------------------ *
 * Part 1 -- the fixtures: both symptoms follow from the bytes         *
 * ------------------------------------------------------------------ */

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("FAIL: short read %s\n", path); exit(2); }
    b[n] = 0; fclose(f); *len = (int)n;
    return b;
}

struct census { int css_links, script_srcs, imgs; };
static void census(struct node *n, struct census *c)
{
    if (!n) return;
    if (n->type == N_ELEM) {
        if (!strcmp(n->tag, "link")) {
            const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
            if (rel && href && strstr(rel, "stylesheet")) c->css_links++;
        } else if (!strcmp(n->tag, "script")) {
            if (dom_attr(n, "src")) c->script_srcs++;
        } else if (!strcmp(n->tag, "img")) {
            if (dom_attr(n, "src")) c->imgs++;
        }
    }
    for (struct node *k = n->first_child; k; k = k->next) census(k, c);
}

static int tag_is(const char *t, const char *l)
{ int i = 0; for (; l[i]; i++) if (t[i] != l[i]) return 0; return t[i] == 0; }
static int collect_style(struct node *n, char *o, int p, int max)
{
    if (!n) return p;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && p < max - 1; i++) o[p++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next) p = collect_style(c, o, p, max);
    return p;
}

static char g_css[4 << 20], g_exp[5 << 20];

/* Lay a fixture out exactly the way browser.c's first pass does, and report
 * how much text survived to the display list and to the painter. */
static void render_fixture(const char *path, struct census *cen,
                           int *text_items, long *text_chars, int *painted_chars)
{
    int n = 0;
    char *html = slurp(path, &n);
    struct node *root = dom_parse(html, n);
    if (!root) { printf("FAIL: parse returned NULL for %s\n", path); exit(2); }

    memset(cen, 0, sizeof *cen);
    census(root, cen);

    int cl = collect_style(root, g_css, 0, (int)sizeof g_css);
    int el = css_expand_vars(g_css, cl, g_exp, (int)sizeof g_exp);
    css_apply(root, g_exp, el);
    css_extra_apply(root, g_exp, el);
    layout_page(root, 1180);

    *text_items = 0; *text_chars = 0;
    int ni = layout_count();
    const struct item *it = layout_items();
    for (int i = 0; i < ni; i++)
        if (it[i].type == IT_TEXT) { (*text_items)++; *text_chars += it[i].len; }

    paint_nops = 0;
    browser_paint(0, 0, 1180, 572, 0);
    *painted_chars = 0;
    for (int i = 0; i < paint_nops; i++)
        if (paint_ops[i].kind == OP_TEXT) *painted_chars += paint_ops[i].len;

    layout_free();
    dom_free(root);
    free(html);
}

static void part1_fixtures(void)
{
    struct census cen;
    int items = 0, painted = 0;
    long chars = 0;

    printf("\n-- part 1: the two documents --\n");

    /* (a) The 227 bytes our User-Agent actually receives. Both reported
     *     symptoms have to be visible here, or the diagnosis is wrong. */
    render_fixture("tests/fixtures/browser/baidu-ua-stub.html",
                   &cen, &items, &chars, &painted);
    printf("   stub: %d css links, %d script src, %d img, %d text items, "
           "%ld chars laid out, %d painted\n",
           cen.css_links, cen.script_srcs, cen.imgs, items, chars, painted);
    CHECK(cen.css_links == 0 && cen.script_srcs == 0 && cen.imgs == 0,
          "FACT 1 EXPLAINED: the document our UA receives has no sub-resources "
          "to discover");
    CHECK(items == 0 && painted == 0,
          "FACT 2 EXPLAINED: and no text at all -- the blank page was a correct "
          "rendering of it");

    /* (b) The 697 KB document a browser-shaped User-Agent receives. This is
     *     the control for (a): if the pipeline were the problem, this would be
     *     empty too. It is not. */
    render_fixture("tests/fixtures/browser/baidu.html",
                   &cen, &items, &chars, &painted);
    printf("   real: %d css links, %d script src, %d img, %d text items, "
           "%ld chars laid out, %d painted\n",
           cen.css_links, cen.script_srcs, cen.imgs, items, chars, painted);
    CHECK(cen.css_links >= 2, "CONTROL: the real document's stylesheets ARE discovered");
    CHECK(cen.script_srcs >= 11, "CONTROL: and its external scripts");
    CHECK(cen.imgs >= 20, "CONTROL: and its images");
    CHECK(items >= 40 && painted > 0,
          "CONTROL: and its text reaches the display list AND the painter -- so "
          "neither the parser nor the cascade nor layout was ever at fault");
}

/* ------------------------------------------------------------------ *
 * Part 2 -- the loader follows the page's own navigation              *
 * ------------------------------------------------------------------ */

/* baidu's stub, retargeted at the fixture site. The string surgery
 * (location.href.replace) is baidu's own and is kept: it only works if
 * location.href is a real absolute URL. */
static const char STUB[] =
    "<html>\n<head>\n\t<script>\n"
    "\t\tlocation.replace(location.href.replace(\"stub.html\",\"real.html\"));\n"
    "\t</script>\n</head>\n<body>\n"
    "\t<noscript><meta http-equiv=\"refresh\" content=\"0;url=/real.html\"></noscript>\n"
    "</body>\n</html>\n";

/* The destination carries a sub-resource of each kind, so "sub-resources are
 * discovered" is asserted on the page we were supposed to be looking at. */
static const char REAL[] =
    "<!doctype html><html><head><title>real</title>"
    "<link rel=\"stylesheet\" href=\"/site.css\">"
    "<script src=\"/site.js\"></script></head>"
    "<body><h1>DESTINATION</h1><p>the text that was missing</p>"
    "<img src=\"/pic.png\"></body></html>";
static const char SITECSS[] = "h1 { color: #112233; }";
static const char SITEJS[]  = "var loaded = 1;";

/* The negative control: the same shape, the same <script> tag, no navigation. */
static const char INERT[] =
    "<html>\n<head>\n\t<script>\n"
    "\t\tvar x = location.href.replace(\"inert.html\",\"real.html\");\n"
    "\t</script>\n</head>\n<body><h1>INERT</h1></body>\n</html>\n";

/* The loop guard's subject: two pages that replace each other, for ever.
 *
 * It is a PAIR and not a single self-replacing page on purpose. js_webapi.c
 * classifies a location write whose URL differs only in the fragment as a
 * same-document change and records no navigation at all, so a page replacing
 * itself with its own URL never loops -- it would have "passed" the guard by
 * never testing it. */
static const char PING[] =
    "<html><head><script>location.replace('http://fixture.test/pong.html');"
    "</script></head><body><h1>PING</h1></body></html>";
static const char PONG[] =
    "<html><head><script>location.replace('http://fixture.test/ping.html');"
    "</script></head><body><h1>PONG</h1></body></html>";

static void site_up(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/stub.html", STUB);
    fake_site_add("http://fixture.test/real.html", REAL);
    fake_site_add("http://fixture.test/inert.html", INERT);
    fake_site_add("http://fixture.test/ping.html", PING);
    fake_site_add("http://fixture.test/pong.html", PONG);
    fake_site_add("http://fixture.test/site.css", SITECSS);
    fake_site_add("http://fixture.test/site.js", SITEJS);
    fake_site_add("http://fixture.test/pic.png", "");
}

/* Did the painter draw this string? The claim "the text reached the pixels"
 * is only worth making against the painter's own op list. */
static int painted_text(const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i < paint_nops; i++) {
        struct paintop *o = &paint_ops[i];
        if (o->kind != OP_TEXT || !o->text) continue;
        for (int k = 0; k + nl <= o->len; k++)
            if (!memcmp(o->text + k, needle, (size_t)nl)) return 1;
    }
    return 0;
}

static void part2_navigation(void)
{
    printf("\n-- part 2: does the loader follow location.replace()? --\n");

    /* (a) THE NEGATIVE CONTROL, run first so it cannot be explained by state
     *     the subject left behind. */
    site_up();
    browser_load("http://fixture.test/inert.html");
    CHECK(fake_site_fetched("inert.html") == 1, "CONTROL: the inert page was fetched");
    CHECK(fake_site_fetched("real.html") == 0,
          "CONTROL: a page whose script does NOT call location.replace does NOT "
          "navigate");

    /* (b) THE SUBJECT. */
    site_up();
    browser_load("http://fixture.test/stub.html");

    CHECK(fake_site_fetched("stub.html") == 1, "the stub was fetched");
    CHECK(fake_site_fetched("real.html") >= 1,
          "location.replace() NAVIGATED: the loader fetched the destination");

    /* Fact 1, on the page we were supposed to be on. */
    CHECK(fake_site_fetched("site.css") >= 1,
          "FACT 1 FIXED: the destination's stylesheet was discovered and fetched");
    CHECK(fake_site_fetched("site.js") >= 1,
          "FACT 1 FIXED: and its external script");

    /* Fact 2: the painter's own op list, not a layout count. */
    CHECK(painted_text("DESTINATION"),
          "FACT 2 FIXED: the destination's text reached the PAINTER");

    /* (c) THE LOOP GUARD. Two pages that bounce to each other must be stopped.
     *     Both halves of the claim are asserted: it really did loop (so the
     *     guard is what ended it, not a failure to navigate at all), and it
     *     ended within the budget. */
    site_up();
    browser_load("http://fixture.test/ping.html");
    int hits = fake_site_fetched("ping.html") + fake_site_fetched("pong.html");
    printf("   ping/pong fetched %d times in total\n", hits);
    CHECK(hits > 2, "the bouncing pages DID navigate repeatedly");
    CHECK(hits <= 12, "and the redirect loop was BOUNDED rather than infinite");
}

int main(void)
{
    css_init();
    css_viewport(1180, 620);
    css_set_post_pass(css_extra_apply);

    part1_fixtures();

    if (setjmp(host_exit_jmp) == 0)
        part2_navigation();
    else
        { printf("FAIL: the loader called app_exit(%d)\n", host_exit_code); fail = 1; }

    printf(fail ? "\nloader_test: FAIL\n" : "\nloader_test: PASS\n");
    return fail;
}
