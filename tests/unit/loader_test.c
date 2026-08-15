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
#include "quickjs.h"
#include "js_page.h"               /* part 2.5 reads the live page runtime */

/* ---- the recorders' storage (declared extern by the two shadow headers) ---- */
struct paintop paint_ops[PAINT_MAXOPS];
int paint_nops;
int host_win_w = 1180, host_win_h = 620, host_flushes;
struct logit_event host_evq[HOST_EVQ];
int host_evq_head, host_evq_tail;
jmp_buf host_exit_jmp;
int host_exit_code, host_exited;
unsigned long long host_clock;
char host_clip[HOST_CLIP_MAX];
int  host_clip_len;

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
/* img_register / img_register_anim are stubbed in loader_fakebfetch.c, not
 * here: that TU does not include img.h, so the stubs do not have to track the
 * codec registry's evolving function-pointer typedefs. */

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

    /* (b) THE SUBJECT.
     *
     * paint_nops is reset first, and that is not tidiness. painted_text()
     * dereferences paint_ops[].text, which points INTO the DOM that produced
     * it; part 1 freed its fixtures' DOMs, so scanning ops left over from part
     * 1 is a read of freed memory -- which is what ASan reports if this line is
     * removed. The claim being made is about THIS load's ops anyway. */
    site_up();
    paint_nops = 0;
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

/* ================================================================== *
 * Part 3 -- TABS: two live pages, and what a background one costs     *
 * ================================================================== *
 *
 * THE QUESTION THIS PART EXISTS TO ANSWER, because nobody had the number:
 * what does a second tab cost, and does having tabs undo the connection
 * pooling? Both are measurements here rather than claims, over the SAME real
 * 697 KB document part 1 uses.
 *
 * The design being tested is in c/apps/browser/tabs.h: the render engine is a
 * singleton (one display list, one LibCSS context, one JSRuntime), so exactly
 * one tab is live and the others keep the BYTES they were built from. The two
 * things that has to buy are asserted separately:
 *
 *   - switching back RENDERS, from the tab's own bytes;
 *   - and costs ZERO network requests, so N tabs do not multiply handshakes.
 */

#include "tabs.h"
#include "js_page.h"                /* js_page_eval: the mutation goes through
                                     * the real bindings, not a shortcut */

void browser_tab_switch(int i);
void browser_res_split(int *from_tab, int *from_net);

/* The store, in memory. The browser writes its session through
 * struct bstore_ops precisely so this can exist: a session that is only
 * testable by rebooting QEMU is a session nobody re-tests. */
#define FS_MAX 8
static struct { char path[64]; char *data; int len; } g_fs[FS_MAX];
static int g_fsn;

static int memfs_read(const char *p, void *b, int max)
{
    for (int i = 0; i < g_fsn; i++)
        if (!strcmp(g_fs[i].path, p)) {
            int n = g_fs[i].len < max ? g_fs[i].len : max;
            memcpy(b, g_fs[i].data, (size_t)n);
            return n;
        }
    return -1;
}
static int memfs_write(const char *p, const void *b, int len)
{
    for (int i = 0; i < g_fsn; i++)
        if (!strcmp(g_fs[i].path, p)) {
            free(g_fs[i].data);
            g_fs[i].data = malloc((size_t)len + 1);
            memcpy(g_fs[i].data, b, (size_t)len);
            g_fs[i].data[len] = 0; g_fs[i].len = len;
            return 0;
        }
    if (g_fsn >= FS_MAX) return -1;
    snprintf(g_fs[g_fsn].path, sizeof g_fs[0].path, "%s", p);
    g_fs[g_fsn].data = malloc((size_t)len + 1);
    memcpy(g_fs[g_fsn].data, b, (size_t)len);
    g_fs[g_fsn].data[len] = 0; g_fs[g_fsn].len = len;
    g_fsn++;
    return 0;
}
static int memfs_mkdir(const char *p) { (void)p; return 0; }
static const struct bstore_ops memfs = { memfs_read, memfs_write, memfs_mkdir };

/* Two documents distinguishable in the PAINTER'S op list, which is the only
 * channel that can say "this page, not that one, is on the screen". */
static const char PAGE_A[] =
    "<!doctype html><html><head><title>Alpha</title>"
    "<link rel=\"stylesheet\" href=\"/a.css\"></head>"
    "<body><h1>PAGE-ALPHA</h1><p>alpha body text</p></body></html>";
static const char PAGE_B[] =
    "<!doctype html><html><head><title>Beta</title>"
    "<link rel=\"stylesheet\" href=\"/b.css\"><script src=\"/b.js\"></script></head>"
    "<body><h1>PAGE-BETA</h1><p>beta body text</p></body></html>";
static const char A_CSS[] = "h1 { color: #101010; }";
static const char B_CSS[] = "h1 { color: #202020; }";
static const char B_JS[]  = "var beta = 1;";

/* A page whose only box is as wide as the canvas, so the display list reports
 * the layout width directly. The <div> has an id because the mutation below has
 * to name something. */
static const char WIDE[] =
    "<!doctype html><html><head><title>WIDE</title></head>"
    "<body style=\"margin:0\">"
    "<div id=\"w\" style=\"width:100%;height:20px;background:#123456\">w</div>"
    "</body></html>";

/* The right-most edge anything was laid out to. This is the LAYOUT's own
 * answer -- a width browser.c reported about itself would agree with the bug
 * being tested for. */
static int layout_right_edge(void)
{
    int n = layout_count(), right = 0;
    const struct item *it = layout_items();
    for (int i = 0; i < n; i++)
        if (it[i].x + it[i].w > right) right = it[i].x + it[i].w;
    return right;
}

/* Run a line of the page's own JavaScript, the way an event handler would.
 *
 * The length is strlen and NOT -1. js_page_eval takes an explicit byte count
 * (QuickJS needs one; the source is not required to be NUL-terminated), and a
 * -1 sails straight past every prototype as a size_t of 2^64-1. The engine then
 * reported `SyntaxError: unexpected end of string`, the mutation never
 * happened, the DOM was never dirty, restyle() never ran -- and every
 * assertion after it PASSED, because the display list still held the correct
 * answer from the resize. That is what a vacuous test looks like from the
 * outside, which is why the settle's return value is checked at the call
 * site. */
static int mutate(const char *js)
{ return js_page_eval(js, (int)strlen(js), "http://fixture.test/wide.html"); }

static char *g_real_html;              /* the 697 KB fixture, for the size numbers */

static void tabsite_up(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/a.html", PAGE_A);
    fake_site_add("http://fixture.test/b.html", PAGE_B);
    fake_site_add("http://fixture.test/a.css", A_CSS);
    fake_site_add("http://fixture.test/b.css", B_CSS);
    fake_site_add("http://fixture.test/b.js",  B_JS);
    if (g_real_html) {
        fake_site_add("http://fixture.test/real1.html", g_real_html);
        fake_site_add("http://fixture.test/real2.html", g_real_html);
        fake_site_add("http://fixture.test/real3.html", g_real_html);
        fake_site_add("http://fixture.test/real4.html", g_real_html);
        fake_site_add("http://fixture.test/real5.html", g_real_html);
        fake_site_add("http://fixture.test/real6.html", g_real_html);
        fake_site_add("http://fixture.test/real7.html", g_real_html);
        fake_site_add("http://fixture.test/real8.html", g_real_html);
    }
}

/* Load `url` into a NEW tab and make that tab current. Mirrors what Cmd+T
 * followed by Enter does in the app. */
static int open_tab(const char *u)
{
    int idx = tabs_new(u);
    if (idx < 0) return -1;
    if (tabs_active() != idx) browser_tab_switch(idx);
    else browser_load(u);
    return idx;
}

/* ---- part 2.5: the Vite legacy probe ------------------------------------
 * weixin.qq.com (and every site built with @vitejs/plugin-legacy) decides
 * whether the browser is "modern" by running, inline, as a module:
 *
 *     import.meta.url; import("_").catch(()=>1); (async function*(){})().next();
 *
 * and loading the real app only if none of that killed the script. The
 * dangerous half is import("_"): a BARE specifier, which our loader refuses
 * in mod_normalize (js_module.c) by design -- there is no import map. The
 * refusal is CORRECT; what this part pins is its DELIVERY: the throw must
 * arrive as a REJECTION of that import()'s promise, caught by the page's own
 * .catch(), with the module's remaining statements having run -- not as a
 * top-level module failure that kills the probe (which is indistinguishable,
 * to the site, from "old browser" and produces the BLANK the scoreboard
 * recorded). js_dynimport_test cannot make this claim: it links its own
 * loader, not js_module.c. This is the real normalizer, through the real
 * page pipeline, fixture-fed. */
static const char VITEPROBE[] =
    "<html><head><script type=\"module\">"
    "import.meta.url;"
    "import(\"_\").catch(function(){ globalThis.__legacy_caught = 1; });"
    "(async function*(){})().next();"
    "globalThis.__probe_ran = 1;"
    "</script></head><body>VITE</body></html>";

static void part2_5_vite_probe(void)
{
    printf("\n-- part 2.5: the Vite legacy probe (bare import() must REJECT, not kill) --\n");
    fake_site_reset();
    fake_site_add("http://fixture.test/vite.html", VITEPROBE);
    browser_load("http://fixture.test/vite.html");
    js_page_pump();                       /* the rejection lands as a job */

    JSContext *ctx = js_page_ctx();
    CHECK(ctx != NULL, "the page runtime survived the probe");
    if (!ctx) return;
    JSValue v = JS_Eval(ctx,
        "String(globalThis.__probe_ran) + ':' + String(globalThis.__legacy_caught)",
        (size_t)strlen("String(globalThis.__probe_ran) + ':' + String(globalThis.__legacy_caught)"),
        "<probe-read>", JS_EVAL_TYPE_GLOBAL);
    const char *s = JS_ToCString(ctx, v);
    CHECK(s && !strcmp(s, "1:1"),
          "the probe ran to completion AND its .catch() caught the bare import");
    if (s && strcmp(s, "1:1") == 0) { /* nothing */ }
    else printf("   (read back: %s)\n", s ? s : "<null>");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

static void part3_tabs(void)
{
    printf("\n-- part 3: tabs --\n");
    tabs_set_store(&memfs);
    tabs_init();
    tabsite_up();

    /* (a) TWO PAGES OPEN AT ONCE, and each one renders when it is looked at. */
    int a = open_tab("http://fixture.test/a.html");
    paint_nops = 0; browser_paint(0, 0, 1180, 520, 0);
    CHECK(painted_text("PAGE-ALPHA"), "tab 1 rendered");

    int b = open_tab("http://fixture.test/b.html");
    paint_nops = 0; browser_paint(0, 0, 1180, 520, 0);
    CHECK(painted_text("PAGE-BETA"), "tab 2 rendered");
    CHECK(!painted_text("PAGE-ALPHA"),
          "and tab 1 is no longer on the screen -- one document is live, "
          "which is the whole model");
    CHECK(tabs_count() == 2, "both tabs are open");
    { struct tab *ta = tab_at(a), *tb = tab_at(b);
      CHECK(ta && !strcmp(ta->title, "Alpha") && tb && !strcmp(tb->title, "Beta"),
            "each tab took its label from its own <title>"); }

    /* (b) SWITCH BACK. The two claims, separately. */
    fake_site_clear_log();
    browser_tab_switch(a);
    paint_nops = 0; browser_paint(0, 0, 1180, 520, 0);
    CHECK(painted_text("PAGE-ALPHA"),
          "switching back RE-RENDERED tab 1 -- a background tab is still a page");
    int reqs = fake_site_requests();
    printf("   switching back cost %d network requests, %d dials\n",
           reqs, fake_site_dials());
    CHECK(reqs == 0,
          "and cost ZERO network requests: the switch REPLAYED the tab's own "
          "bytes. This is the claim tabs would otherwise break -- N tabs must "
          "not multiply the handshakes the connection pool removed");
    { int ft = 0, fn = 0; browser_res_split(&ft, &fn);
      CHECK(fn == 0, "no sub-resource came from the network on the replay"); }

    /* And forward again, to prove (b) is not "tab 1 was never really gone". */
    fake_site_clear_log();
    browser_tab_switch(b);
    paint_nops = 0; browser_paint(0, 0, 1180, 520, 0);
    CHECK(painted_text("PAGE-BETA"), "and forward to tab 2 again");
    CHECK(fake_site_requests() == 0, "also with no network");

    /* (b2) A NAVIGATION IN A TAB REPLACES WHAT THE TAB HOLDS.
     *
     * The retained set is keyed by absolute URL, so a tab that navigated from A
     * to B while keeping A's bytes would serve A's stylesheet to B whenever the
     * two share a URL -- and would keep growing, one page's worth per
     * navigation. Both are invisible until they are wrong. */
    browser_load("http://fixture.test/b.html");        /* tab `a` navigates */
    { struct tab *ta = tab_cur();
      CHECK(ta && ta->src && ta->srclen > 0, "after navigating, the tab holds a document");
      int stale = 0;
      for (int i = 0; i < ta->nres; i++)
          if (strstr(ta->res[i].url, "a.css")) stale = 1;
      CHECK(!stale,
            "and NOT the page it navigated away from -- a navigation drops what "
            "the tab was retaining, so the retained set is one page and not a "
            "cache that grows for ever"); }
    fake_site_clear_log();
    browser_load("http://fixture.test/a.html");        /* back to A, fresh */
    CHECK(fake_site_fetched("a.css") == 1,
          "and the stylesheet is fetched again rather than served stale");

    /* (c) PER-TAB HISTORY. The back stack belongs to the tab, not the window:
     *     going Back in one tab must not walk the other tab's trail. */
    { struct tab *ta = tab_at(a), *tb = tab_at(b);
      tab_hist_push(ta, "http://fixture.test/a.html");
      tab_hist_push(ta, "http://fixture.test/a2.html");
      char got[TAB_URL];
      CHECK(tab_hist_go(ta, -1, got, sizeof got) && strstr(got, "a.html"),
            "Back in tab 1 lands on tab 1's previous page");
      CHECK(!tab_hist_can(tb, -1),
            "and tab 2's back stack is untouched by any of it"); }

    /* (d) THE MEMORY NUMBER. N real pages, the same 697 KB document, measured
     *     rather than estimated -- this is the figure that decides whether tabs
     *     are usable and it is the one nobody had. */
    if (g_real_html) {
        printf("\n   -- what a background tab costs (real 697 KB document) --\n");
        tabs_init();
        tabsite_up();
        size_t at1 = 0;
        int wanted[9] = { 0, 1, 2, 4, 8, 0, 0, 0, 0 };
        (void)wanted;
        char u[64];
        for (int n = 1; n <= 8; n++) {
            snprintf(u, sizeof u, "http://fixture.test/real%d.html", n);
            open_tab(u);
            if (n == 1 || n == 2 || n == 4 || n == 8) {
                size_t bytes = tabs_retained_bytes();
                printf("   %d tab%s open: %6lu KB retained (%lu KB per tab)\n",
                       n, n == 1 ? " " : "s", (unsigned long)(bytes / 1024),
                       (unsigned long)(bytes / 1024 / (unsigned)n));
                if (n == 1) at1 = bytes;
            }
        }
        size_t at8 = tabs_retained_bytes();
        CHECK(tabs_count() == 8, "eight tabs are open at once");
        /* The shape of the claim, not a magic number: the cost is LINEAR in the
         * bytes, so eight tabs cost about eight times one -- and nothing like
         * eight QuickJS runtimes, which at the measured 12.76 MB apiece would be
         * 102 MB against a 96 MiB arena and would simply not fit. */
        CHECK(at8 > at1 * 6 && at8 < at1 * 10,
              "and eight tabs cost about eight times one -- linear in the bytes "
              "kept, which is what dehydrating to bytes was for");
        printf("   for scale: 8 live QuickJS runtimes at the measured 12.76 MB "
               "each = %lu KB, against a 96 MiB arena\n",
               (unsigned long)(8UL * 12760UL));
        CHECK(at8 < 8UL * 12760UL * 1024UL,
              "eight dehydrated tabs cost less than EIGHT LIVE RUNTIMES would");
    }

    /* (d2) WHAT THE STRIP COSTS TO DRAW.
     *
     * SYS_GUI_FLUSH carries no rectangle, so ANY repaint costs the whole window
     * canvas -- measured at 24-27 ms at 1920x1200, and 24.5 ms/composite at
     * 1280x800 in the QMP run of this change. That is the budget the tab strip
     * has to respect, and it respects it in two ways which are separate claims:
     *
     *   - it is CHEAP TO DRAW: a bounded handful of ops per tab, against the
     *     thousands of text runs a page repaint already costs. Counted here.
     *   - it causes NO EXTRA REPAINTS: there is no hover state, so pointer
     *     motion over the strip cannot mark the frame dirty. That is true by
     *     construction (browser.c only builds a mouse event for y >= VIEW_Y)
     *     and is the reason there is no hover highlight to look at.
     */
    {
        void browser_redraw_now(void);
        paint_nops = 0; browser_redraw_now();
        int with8 = paint_nops;
        /* the same page with ONE tab, for the difference */
        tabs_init(); tabsite_up();
        open_tab("http://fixture.test/real1.html");
        paint_nops = 0; browser_redraw_now();
        int with1 = paint_nops;
        printf("   chrome repaint: %d draw ops with 1 tab, %d with 8 "
               "(%d ops per extra tab)\n", with1, with8, (with8 - with1) / 7);
        CHECK(with8 - with1 <= 7 * 4,
              "the tab strip costs at most 4 draw ops per extra tab -- a "
              "rounding error against a page repaint, which is what a repaint "
              "with no damage rectangle can afford");
    }

    /* (d3) THE WINDOW SIZE -- and the layout path that used to ignore it.
     *
     * THE BUG, stated so the test cannot be weakened into passing: three of the
     * four layout_page() call sites in browser.c passed the LIVE window width
     * and one passed WINW, the born-at constant. The one that did not was
     * restyle() -- the CSS invalidation path -- which does not run on load. It
     * runs when a script mutates the DOM. So a resized window laid out
     * correctly right up until the page changed anything, and then snapped back
     * to 1180 px and stayed there, because every subsequent mutation did it
     * again. Any real application mutates constantly.
     *
     * A TEST THAT ONLY RESIZES PASSES AGAINST THE BROKEN CODE. The mutation is
     * the whole test. It goes through js_page_eval and browser_settle -- the
     * real bindings and the real settle path the event loop uses after every
     * handler -- rather than calling restyle() directly, because "the function
     * I called used the right width" is not the claim; "the browser lays out at
     * the window's width after a page changes itself" is.
     *
     * Measured off the DISPLAY LIST, not off a variable browser.c reports about
     * itself: a self-reported width would agree with the bug. */
    {
        void browser_resize(int w, int h);
        int  browser_settle(void);

        tabs_init();
        tabsite_up();
        fake_site_add("http://fixture.test/wide.html", WIDE);
        open_tab("http://fixture.test/wide.html");

        int born = layout_right_edge();
        browser_resize(900, 500);
        int resized = layout_right_edge();
        printf("   layout right edge: %d at the born-at 1180, %d after a resize to 900\n",
               born, resized);
        CHECK(born > resized && resized <= 900,
              "a resize re-lays the page out at the NEW width");

        /* Now let the page change itself, which is what a real one does.
         *
         * The settle's return value is CHECKED, and that check is load-bearing:
         * if the mutation did not reach the invalidation path, restyle() never
         * lays out, the display list is still the one the resize produced, and
         * every assertion below passes while measuring nothing. A vacuous pass
         * here is exactly how this bug survived a resize handler in the first
         * place. */
        int ev = mutate("document.getElementById('w').textContent = 'mutated';");
        int settled = browser_settle();
        printf("   mutation: eval rc=%d, settle re-laid-out=%d\n", ev, settled);
        CHECK(settled == 1,
              "the mutation REACHED the invalidation path and forced a "
              "re-layout (without this the checks below measure nothing)");
        int mutated = layout_right_edge();
        printf("   after a DOM mutation: %d (must stay %d, must not snap back to %d)\n",
               mutated, resized, born);
        CHECK(mutated != born,
              "A DOM MUTATION DOES NOT SNAP THE LAYOUT BACK to the window's "
              "born-at width -- the invalidation path lays out at the LIVE "
              "width like every other path");
        CHECK(mutated == resized,
              "and it lays out at exactly the width the resize established");

        /* And once more, because the reported symptom was that it happened
         * repeatedly: a second mutation must not undo the first answer. */
        mutate("document.getElementById('w').textContent = 'again';");
        browser_settle();
        CHECK(layout_right_edge() == resized,
              "and stays there across further mutations");

        browser_resize(1180, 620);          /* leave the window as we found it */
    }

    /* (e) THE SESSION, across a restart of the app.
     *     tabs_init() is what app_main does on startup; session_restore() is
     *     what it does next. Running them after a save is exactly the restart. */
    tabs_init();
    tabsite_up();
    open_tab("http://fixture.test/a.html");
    open_tab("http://fixture.test/b.html");
    int saved = session_save();
    CHECK(saved >= 0, "the session was written");

    tabs_init();                       /* <- the restart: every tab is gone */
    CHECK(tabs_count() == 0, "after the restart there are no tabs");
    int restored = session_restore();
    printf("   session restored %d tabs\n", restored);
    CHECK(restored == 2, "SESSION RESTORE: both tabs came back");
    { struct tab *t0 = tab_at(0), *t1 = tab_at(1);
      CHECK(t0 && strstr(t0->url, "a.html") && t1 && strstr(t1->url, "b.html"),
            "with their URLs");
      CHECK(t0 && !strcmp(t0->title, "Alpha") && t1 && !strcmp(t1->title, "Beta"),
            "and their titles, so the strip is readable before anything loads");
      CHECK(t0 && !t0->src && t1 && !t1->src,
            "and NO bytes -- restoring eight tabs must not be eight page loads"); }
    CHECK(tabs_active() == 1, "and the tab that was in front is in front again");

    /* A restored tab loads when it is first selected, and then it is a page. */
    fake_site_clear_log();
    browser_tab_switch(0);
    paint_nops = 0; browser_paint(0, 0, 1180, 520, 0);
    CHECK(painted_text("PAGE-ALPHA"), "and a restored tab loads when selected");
    CHECK(fake_site_fetched("a.html") == 1,
          "-- by fetching it, exactly once, because it had no bytes to replay");

    /* (f) HISTORY, BOOKMARKS, DOWNLOADS: the persistent lists. */
    history_clear();
    history_add("http://fixture.test/a.html", "Alpha", 100);
    history_add("http://fixture.test/b.html", "Beta", 200);
    history_add("http://fixture.test/a.html", "Alpha", 300);   /* a re-visit */
    CHECK(history_count() == 2, "history de-duplicates a re-visit");
    CHECK(history_at(0) && strstr(history_at(0)->url, "a.html"),
          "and moves it to the front");
    { int hits[8];
      int n = history_search("beta", hits, 8);
      CHECK(n == 1 && strstr(history_at(hits[0])->url, "b.html"),
            "history search matches the TITLE, case-insensitively");
      n = history_search("a.html", hits, 8);
      CHECK(n == 1, "and the URL");
      n = history_search("", hits, 8);
      CHECK(n == 2, "an empty query is everything, not nothing"); }
    history_save();
    history_clear();
    CHECK(history_count() == 0, "history cleared");
    history_load();
    CHECK(history_count() == 2 && history_at(0) && !strcmp(history_at(0)->title, "Alpha"),
          "HISTORY SURVIVES A RESTART, titles and all");

    CHECK(bookmark_add("http://fixture.test/b.html", "Beta") >= 0, "a bookmark was added");
    CHECK(bookmark_add("http://fixture.test/b.html", "Beta") == 0,
          "adding it twice does not add it twice");
    CHECK(bookmark_find("http://fixture.test/b.html") == 0, "and it can be found by URL");
    bookmarks_save();
    bookmark_remove(0);
    CHECK(bookmark_count() == 0, "and removed");
    bookmarks_load();
    CHECK(bookmark_count() == 1, "BOOKMARKS SURVIVE A RESTART");

    { char nm[64];
      download_name("https://h.test/a/b/logit.iso?v=2#x", nm, sizeof nm);
      CHECK(!strcmp(nm, "logit.iso"), "a download's name is the last path segment");
      download_name("https://h.test/", nm, sizeof nm);
      CHECK(nm[0] && !strchr(nm, '/'), "and is never empty and never a path");
      CHECK(download_is_downloadable("https://h.test/x/logit.iso") == 1 &&
            download_is_downloadable("https://h.test/index.html") == 0,
            "a page is rendered and an archive is saved");
      int d = download_record("https://h.test/a/logit.iso", (const unsigned char *)"DATA", 4);
      const struct download *rec = download_at(d);
      CHECK(rec && rec->ok && !strcmp(rec->path, "/downloads/logit.iso"),
            "DOWNLOAD: the bytes landed on the disk where Finder can see them");
      char back[16];
      CHECK(memfs_read("/downloads/logit.iso", back, sizeof back) == 4 &&
            !memcmp(back, "DATA", 4),
            "and they are the bytes that arrived"); }
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

    if (setjmp(host_exit_jmp) == 0)
        part2_5_vite_probe();
    else
        { printf("FAIL: the vite probe called app_exit(%d)\n", host_exit_code); fail = 1; }

    { int n = 0; g_real_html = slurp("tests/fixtures/browser/baidu.html", &n); }
    if (setjmp(host_exit_jmp) == 0)
        part3_tabs();
    else
        { printf("FAIL: the tab test called app_exit(%d)\n", host_exit_code); fail = 1; }

    printf(fail ? "\nloader_test: FAIL\n" : "\nloader_test: PASS\n");
    return fail;
}
