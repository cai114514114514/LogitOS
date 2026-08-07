#include "logit.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "js_dom.h"
#include "js_page.h"
#include "url.h"                 /* url_parse + url_resolve for link clicks */

/* A web browser. The whole render pipeline runs in this ring-3 app: the
 * kernel does DNS+TCP+TLS+HTTP (SYS_HTTP_GET) and hands us the raw body
 * (SYS_HTTP_BODY); we parse HTML->DOM (dom.c), apply CSS (css_engine.c), lay
 * out a display list (layout.c) and paint it via the GUI render syscalls
 * (browser_paint.c).
 *
 * The page's JavaScript is LIVE: js_page.c holds a runtime that is opened with
 * the document and closed on navigation, so a handler registered by an inline
 * <script> is still there when the user clicks a minute later. This loop is
 * that runtime's event loop -- it delivers input as DOM events, services due
 * timers, drains the microtask queue, and only then performs the DEFAULT ACTION
 * (following a link, scrolling) if no listener called preventDefault(). */

/* http_get error codes (mirror include/http.h) */
#define HTTP_ERR_URL  -2
#define HTTP_ERR_DNS  -3
#define HTTP_ERR_CONN -4
#define HTTP_ERR_TLS  -5

#define WINW 1180
#define WINH 620
#define BARH 30
#define VIEW_H (WINH - BARH - 18)        /* viewport height (below bar, above status) */

static char url[600] = "http://example.com/";
static int  ulen = 19;
static int  scroll;                      /* pixel scroll offset */
static int  ph;                          /* laid-out page height */
static char status[96] = "ready -- Enter loads; Left/Right=history";
static struct node *g_root;              /* current page DOM (owns display-list strings) */

/* back/forward history: URL ring, hcur = current entry, htop = newest */
static char hist[32][600];
static int  hcur = -1, htop = -1;

static void hist_push(const char *u)
{
    if (hcur >= 0) {                       /* don't push a duplicate of current */
        int same = 1;
        for (int i = 0; hist[hcur][i] || u[i]; i++)
            if (hist[hcur][i] != u[i]) { same = 0; break; }
        if (same) return;
    }
    if (hcur < 31) hcur++;
    else {                                 /* full: drop the oldest */
        for (int i = 0; i < 31; i++)
            for (int j = 0; j < 600; j++) { hist[i][j] = hist[i+1][j]; if (!hist[i][j]) break; }
    }
    int i = 0; while (u[i] && i < 599) { hist[hcur][i] = u[i]; i++; } hist[hcur][i] = 0;
    htop = hcur;                           /* navigating truncates the forward branch */
}

static int hist_go(int delta)              /* -1 back, +1 forward; 1 if moved */
{
    int t = hcur + delta;
    if (t < 0 || t > htop) return 0;
    hcur = t;
    int i = 0; while (hist[hcur][i] && i < (int)sizeof url - 1) { url[i] = hist[hcur][i]; i++; }
    url[i] = 0; ulen = i;
    return 1;
}

static void set_status(const char *s)
{ int i = 0; while (s[i] && i < (int)sizeof status - 1) { status[i] = s[i]; i++; } status[i] = 0; }

static void redraw(int editing);

int printf(const char *, ...);
unsigned long strlen(const char *);

/* js_page.c is written against an injected clock so the host tests can step
 * time by hand; in the OS it is the kernel's 100 Hz monotonic counter. */
static unsigned long long clock_ms(void) { return monotonic_ms(); }

/* ---- DOM helpers: collect <style>/<script> text (moved from the kernel) ---- */
static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }

static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_style(c, out, o, max);
    return o;
}

static int has_ci(const char *h, const char *n);   /* defined below, next to fetch_css_links */
static int str_eq(const char *a, const char *b);

/* <script src="..."> fetch: same channel as fetch_css_links (res_fetch_raw; the
 * kernel resolves relative srcs against the page URL). Budgeted -- each fetch
 * is a full HTTPS handshake -- and duplicate srcs are skipped. A failed fetch
 * just skips that one script; the rest of the page's JS still runs. */
static int g_js_budget;
#define MAX_JS_SEEN 16
static const char *g_js_seen[MAX_JS_SEEN];
static int g_js_nseen;
static unsigned char js_tmp[65536];        /* scratch for one external script (64 KiB) */

static int collect_scripts(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "script")) {
        const char *src = dom_attr(n, "src");
        if (src) {                         /* external: fetch in document order */
            if (g_js_budget > 0 && o < max - 64 &&
                !has_ci(src, "javascript:") && !has_ci(src, "data:")) {
                int dup = 0;
                for (int i = 0; i < g_js_nseen; i++)
                    if (str_eq(g_js_seen[i], src)) { dup = 1; break; }
                if (!dup) {
                    if (g_js_nseen < MAX_JS_SEEN) g_js_seen[g_js_nseen++] = src;
                    g_js_budget--;
                    int got = res_fetch_raw(src, js_tmp, (int)sizeof js_tmp);
                    for (int i = 0; i < got && o < max - 1; i++) out[o++] = (char)js_tmp[i];
                    if (got > 0 && o < max - 1) out[o++] = '\n';
                }
            }
        } else {                           /* inline */
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text)
                    for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
            if (o < max - 1) out[o++] = '\n';
            if (o < max - 1) out[o++] = ';';
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_scripts(c, out, o, max);
    return o;
}

static unsigned char css_tmp[1048576];   /* scratch for one external stylesheet (1 MiB; github's site.css is 858 KiB) */

/* case-insensitive substring test (rel may be "stylesheet", "preload stylesheet", ...) */
static int has_ci(const char *h, const char *n)
{
    if (!h) return 0;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b) { int ca=(*a>='A'&&*a<='Z')?*a+32:*a, cb=(*b>='A'&&*b<='Z')?*b+32:*b; if(ca!=cb)break; a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Fetch each <link rel="stylesheet" href> over HTTP(S) and append its CSS to out
 * (relative hrefs resolve against the page; this is why github/wikipedia were bare).
 * Budgeted: each fetch is a full HTTPS handshake to a CDN, so cap the count.
 * Duplicate hrefs are skipped (github links the same module CSS 3x). */
static int g_css_budget;
#define MAX_CSS_SEEN 32
static const char *g_css_seen[MAX_CSS_SEEN];
static int g_css_nseen;
static int str_eq(const char *a, const char *b)
{ int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }
static int fetch_css_links(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (g_css_budget > 0 && o < max - 64 && n->type == N_ELEM && tag_is(n->tag, "link")) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        /* a11y override themes are inactive unless the user selected them;
         * skipping saves ~1 MiB of CSS and ~10 TLS handshakes on github.com */
        if (href && (has_ci(href, "high_contrast") || has_ci(href, "colorblind") ||
                     has_ci(href, "tritanopia"))) href = 0;
        if (href && has_ci(rel, "stylesheet")) {
            int dup = 0;
            for (int i = 0; i < g_css_nseen; i++)
                if (str_eq(g_css_seen[i], href)) { dup = 1; break; }
            if (!dup) {
                if (g_css_nseen < MAX_CSS_SEEN) g_css_seen[g_css_nseen++] = href;
                g_css_budget--;
                int got = res_fetch_raw(href, css_tmp, (int)sizeof css_tmp);
                for (int i = 0; i < got && o < max - 1; i++) out[o++] = (char)css_tmp[i];
                if (got > 0 && o < max - 1) out[o++] = '\n';
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = fetch_css_links(c, out, o, max);
    return o;
}

static void load(const char *u);

/* Follow a clicked link: resolve relative hrefs against the current page URL
 * (url_resolve handles absolute/protocol-relative refs itself), skip schemes we
 * can't act on. Without this every "/wiki/Foo"-style href died in url_parse. */
static void follow_link(const char *href)
{
    if (!href[0] || href[0] == '#') return;              /* pure fragment: no anchor support */
    if (has_ci(href, "javascript:") || has_ci(href, "mailto:") || has_ci(href, "data:")) return;
    char abs[600];
    struct url base;
    const char *target = href;
    if (url_parse(url, &base) == 0 && url_resolve(&base, href, abs, sizeof abs) == 0)
        target = abs;
    int i = 0; while (target[i] && i < (int)sizeof url - 1) { url[i] = target[i]; i++; }
    url[i] = 0; ulen = i;
    hist_push(url);
    load(url);
}

static char bodybuf[1048576];            /* page HTML (1 MiB) */
static char author_css[4194304];         /* inline <style> + fetched external <link> CSS (4 MiB; github.com ships ~3.25 MiB) */
static char css_expanded[4718592];       /* author_css after var() expansion -> LibCSS (4.5 MiB) */
static int  css_exlen;

/* Re-style + re-lay-out after script changed the DOM. Every path that can run
 * JS ends here, so a mutation from a click handler and one from a timer take
 * exactly the same route back to the screen. Returns 1 if anything changed. */
static void restyle(void);

static int settle_dom(void)
{
    if (!js_dom_dirty()) return 0;
    js_dom_clear_dirty();
    restyle();
    return 1;
}

/* Console bytes already reflected in the status bar. The status line only ever
 * shows the FIRST line of console output, so re-rendering it when nothing new
 * was logged is a repaint that changes no pixels -- and a setInterval firing
 * every tick would demand one every tick. */
static int js_out_shown;

/* Copy the page's first console line into the status bar -- the only channel a
 * headless screenshot test has for "the script ran". */
static void status_from_js(const char *fallback)
{
    js_out_shown = js_page_output_len();
    const char *out = js_page_output();
    if (!out || !out[0]) { set_status(fallback); return; }
    char st[96]; int p = 0;
    const char *pre = "JS: ";
    while (*pre) st[p++] = *pre++;
    for (int i = 0; out[i] && out[i] != '\n' && p < 92; i++) st[p++] = out[i];
    st[p] = 0;
    set_status(st);
}

static void load(const char *u)
{
    set_status("loading...");
    /* ORDER: the runtime dies before the DOM does. Every JS wrapper holds a
     * {node, serial} handle and every node holds a weak pointer back to its
     * wrapper, so freeing the document first would leave the runtime's
     * finalizers walking nodes that no longer exist -- and freeing the runtime
     * first is what clears the wrapper slots. */
    js_page_close();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    ph = 0; scroll = 0;
    int rc = http_get(u);
    if (rc < 0) {
        set_status(rc == HTTP_ERR_URL  ? "load failed: bad URL (need http:// or https://)" :
                   rc == HTTP_ERR_DNS  ? "load failed: DNS lookup failed" :
                   rc == HTTP_ERR_CONN ? "load failed: could not connect (timed out)" :
                   rc == HTTP_ERR_TLS  ? "load failed: TLS/certificate error" :
                                         "load failed");
        return;
    }
    if (http_status() != 2) { set_status("error: could not load"); return; }
    int blen = http_body(bodybuf, sizeof bodybuf);
    if (blen <= 0) { set_status("error: empty response"); return; }
    g_root = dom_parse(bodybuf, blen);
    if (!g_root) { set_status("error: parse failed"); return; }
    int css_len = collect_style(g_root, author_css, 0, (int)sizeof author_css);
    css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
    css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, WINW);
    ph = layout_height();
    set_status("loaded -- fetching stylesheets...");
    redraw(0);                       /* first paint: HTML + inline CSS, before slow CDN fetches */

    g_css_budget = 24;               /* fetch external <link> stylesheets, then re-style */
    g_css_nseen = 0;
    int css2 = fetch_css_links(g_root, author_css, css_len, (int)sizeof author_css);
    /* report what actually arrived: sheet count + KiB (debug aid for CDN fetch issues) */
    { char st[64]; int p = 0; const char *pre = "loaded, ";
      while (*pre) st[p++] = *pre++;
      int v = g_css_nseen, d = 100, started = 0;
      while (d) { int dig = v / d; if (dig || started || d == 1) { st[p++] = (char)('0' + dig); started = 1; } v %= d; d /= 10; }
      const char *mid = " sheets, ";
      while (*mid) st[p++] = *mid++;
      v = css2 / 1024; d = 10000; started = 0;
      while (d) { int dig = v / d; if (dig || started || d == 1) { st[p++] = (char)('0' + dig); started = 1; } v %= d; d /= 10; }
      st[p++] = 'K'; st[p] = 0; set_status(st); }
    if (css2 > css_len) {
        css_len = css2;
        css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
        css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
        layout_page(g_root, WINW);
        ph = layout_height();
        { extern size_t malloc_peak; printf("[browser] heap peak %uK\n", (unsigned)(malloc_peak / 1024)); }
        redraw(0);                   /* re-paint with the page's real stylesheets */
    }
    if (layout_load_images(8) > 0) { /* ... then fetch a few images and repaint */
        ph = layout_height();
        redraw(0);
    }
    /* Open the page's JS runtime. It stays open until the next navigation --
     * that is the whole point: listeners, timers and pending promises all live
     * past the end of the script that created them. It is opened even when the
     * page has no <script>, so that a later dispatchEvent/on-attribute path has
     * a context to run in. */
    js_page_output_clear();
    js_out_shown = 0;
    js_page_set_location(u);
    js_page_open(g_root);

    /* Run the page's <script>s (inline + up to 8 external), capped at 64 KiB
     * total. Real sites ship huge minified bundles that assume a full browser
     * env; with no real DOM they just throw. The cap keeps those out while
     * allowing real page scripts, and the runtime's 2 MiB stack guard bounds
     * recursive scripts so a bad bundle throws a catchable RangeError instead
     * of faulting. */
    static char scr[65536];
    g_js_budget = 8; g_js_nseen = 0;
    int sn = collect_scripts(g_root, scr, 0, sizeof scr);
    scr[sn] = 0;                   /* collect_scripts doesn't NUL-terminate; stale tail bytes from the previous page would be eval'd */
    int had_script = (sn > 1 && sn < (int)sizeof scr);
    if (had_script) js_page_eval(scr, sn, "<page>");

    /* The document is parsed and the scripts have run: fire the lifecycle events
     * pages hang their initialisation on. Without these, every page that defers
     * its work to `window.addEventListener('load', ...)` -- which is most of
     * them -- would sit there fully parsed and completely inert. */
    struct js_event_init li = { 0 };
    li.bubbles = 1;
    js_dom_dispatch(js_dom_root(), "DOMContentLoaded", &li);
    li.bubbles = 0;
    js_dom_dispatch(js_dom_root(), "load", &li);

    if (settle_dom() || had_script) {
        status_from_js(had_script ? "loaded (ran script, no output)" : "loaded");
        redraw(0);
    }
}

/* Re-run the cascade + layout over the current DOM. Shared by the load path and
 * by every script mutation; the expanded stylesheet is whatever the last fetch
 * produced, so this is safe to call at any point after the first css_apply. */
static void restyle(void)
{
    if (!g_root) return;
    css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, WINW);
    ph = layout_height();
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    /* Liquid Glass address bar + a glass URL field */
    gui_glass(0, 0, WINW, BARH, 1, 255, 255, 255, 70);
    gui_glass(10, 5, WINW - 20, 20, 8, 255, 255, 255, 95);
    gui_text(14, 7, rgb(40, 40, 48), url);
    if (editing) gui_rect(14 + ulen * 8, 7, 8, 16, rgb(90, 150, 240));
    /* the page */
    browser_paint(0, BARH, WINW, VIEW_H, scroll);
    /* glass status line (frosts the bottom of the page) */
    gui_glass(0, WINH - 18, WINW, 18, 1, 255, 255, 255, 70);
    gui_text(10, WINH - 16, rgb(110, 110, 120), status);
    gui_flush();
}

/* ---- input -> DOM events ----
 *
 * Everything below turns a `struct logit_event` into a trusted DOM event and
 * lets the page have first refusal on it. The value the dispatch returns is the
 * "proceed with the default action" answer -- that is the whole contract, and
 * it is what makes preventDefault() observable instead of decorative. */

static int mods_of(const struct logit_event *e, struct js_event_init *ji)
{
    ji->shift = (e->mods & EV_MOD_SHIFT) != 0;
    ji->ctrl  = (e->mods & EV_MOD_CTRL) != 0;
    ji->alt   = (e->mods & EV_MOD_ALT) != 0;
    return e->mods;
}

/* DOM button numbering: 0 left, 1 middle, 2 right. The ABI numbers them 1/2/3
 * with right == 2, so it is a remap, not a subtraction. */
static int dom_button(int btn)
{ return btn == EV_BTN_RIGHT ? 2 : btn == EV_BTN_MIDDLE ? 1 : 0; }

/* The KeyboardEvent `key`/`code` for our key codes. Only the named keys need a
 * table; a printable character is its own `key`, which is exactly what the DOM
 * says. */
static const char *key_name(int k, char *one)
{
    switch (k) {
    case KEY_UP:    return "ArrowUp";
    case KEY_DOWN:  return "ArrowDown";
    case KEY_LEFT:  return "ArrowLeft";
    case KEY_RIGHT: return "ArrowRight";
    case KEY_PGUP:  return "PageUp";
    case KEY_PGDN:  return "PageDown";
    case KEY_HOME:  return "Home";
    case KEY_END:   return "End";
    case '\n':      return "Enter";
    case '\b':      return "Backspace";
    case '\t':      return "Tab";
    case 0x1b:      return "Escape";
    }
    if (k >= ' ' && k < 0x7f) { one[0] = (char)k; one[1] = 0; return one; }
    return "Unidentified";
}

void app_main(void)
{
    css_init();             /* build the UA default stylesheet */
    css_viewport(WINW, WINH);   /* @media/vw/vh evaluate against the real window */
    img_init();             /* register PNG + GIF decoders */
    js_page_set_clock(clock_ms);
    gui_create("Browser", WINW, WINH);
    redraw(1);
    int editing = 1;
    struct node *press_node = 0;      /* the element the last mousedown landed on */
    uint32_t press_serial = 0;

    for (;;) {
        struct logit_event e;
        int need = 0;                 /* coalesce: drain the whole event burst, repaint once */
        int navigated = 0;
        while (!navigated && poll_event(&e)) {
            if (e.type == EV_CLOSE) { js_page_close(); app_exit(0); }
            if (e.type == EV_KEY) {
                int k = e.a;
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                /* Chrome keys belong to the chrome. While the address bar has
                 * focus the page never sees the keystroke -- otherwise a page
                 * could swallow the Enter that loads the next URL. */
                int allow = 1;
                if (!editing) {
                    char one[2];
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.key = key_name(k, one);
                    ji.code = ji.key;
                    ji.key_code = (k >= ' ' && k < 0x7f) ? k : k & 0xFF;
                    mods_of(&e, &ji);
                    /* No focus model yet, so keys go to <body> -- which is where
                     * document-level listeners see them bubble past anyway. */
                    struct node *body = g_root ? dom_doc_body(g_root->doc) : 0;
                    allow = js_dom_dispatch(body ? body : js_dom_root(), "keydown", &ji);
                }
                if (allow) {
                    if      (k == KEY_DOWN) scroll += 40;
                    else if (k == KEY_UP)   scroll -= 40;
                    else if (k == KEY_PGDN) scroll += VIEW_H - 40;
                    else if (k == KEY_PGUP) scroll -= VIEW_H - 40;
                    else if (k == KEY_HOME) scroll = 0;
                    else if (k == KEY_END)  scroll = maxs;
                    else if (k == KEY_LEFT)  { if (hist_go(-1)) { editing = 0; load(url); navigated = 1; } }
                    else if (k == KEY_RIGHT) { if (hist_go(+1)) { editing = 0; load(url); navigated = 1; } }
                    else if (editing && k == '\n') { editing = 0; hist_push(url); load(url); navigated = 1; }
                    else if (k == '\b') {
                        if (editing) { if (ulen > 0) url[--ulen] = 0; }
                        else if (hist_go(-1)) { load(url); navigated = 1; }   /* Backspace = back */
                    }
                    else if (editing && k >= ' ' && k < 0x7f && ulen < (int)sizeof url - 1) { url[ulen++] = (char)k; url[ulen] = 0; }
                }
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                need = 1;
            } else if (e.type == EV_MOUSE || e.type == EV_MOUSE_R) {
                int mx = e.a, my = e.b;              /* window-local */
                if (my < BARH) { editing = 1; press_node = 0; }   /* click the bar to edit */
                else if (my >= BARH && my < BARH + VIEW_H) {
                    editing = 0;
                    struct node *n = 0;
                    browser_hittest_node(mx, my - BARH, scroll, &n, 0, 0);
                    press_node = n;
                    press_serial = n ? n->serial : 0;
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - BARH;
                    ji.button = dom_button(e.button);
                    ji.buttons = 1 << ji.button;
                    mods_of(&e, &ji);
                    js_dom_dispatch(n, e.type == EV_MOUSE_R ? "contextmenu" : "mousedown", &ji);
                    need = 1;
                }
            } else if (e.type == EV_MOUSE_UP) {
                int mx = e.a, my = e.b;
                if (my >= BARH && my < BARH + VIEW_H) {
                    struct node *n = 0;
                    char href[512]; href[0] = 0;
                    browser_hittest_node(mx, my - BARH, scroll, &n, href, sizeof href);
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - BARH;
                    ji.button = dom_button(e.button);
                    mods_of(&e, &ji);
                    js_dom_dispatch(n, "mouseup", &ji);
                    /* A click needs a press and a release on the same element --
                     * dragging off a link and letting go must not navigate. The
                     * press target is re-validated by serial, because a mousedown
                     * handler is perfectly entitled to have deleted it. */
                    int same = press_node && n == press_node && press_node->serial == press_serial;
                    if (same && e.button == EV_BTN_LEFT) {
                        /* THIS is the default action. Navigation used to happen
                         * unconditionally on mousedown; now it is what happens
                         * when the click event survives the page's handlers. */
                        int go = js_dom_dispatch(n, "click", &ji);
                        if (settle_dom()) need = 1;
                        if (go && href[0]) { follow_link(href); navigated = 1; }
                    }
                    press_node = 0;
                    need = 1;
                }
            } else if (e.type == EV_MOUSE_MOVE) {
                /* Motion is the one event that arrives continuously, so it is
                 * the one worth not paying for: with no listeners registered
                 * anywhere, building an Event per sample is pure waste. Inline
                 * on-attributes are compiled lazily and so are invisible to this
                 * count -- onmousemove= in markup is the accepted casualty. */
                if (js_dom_listener_count() > 0 && e.b >= BARH && e.b < BARH + VIEW_H) {
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1;
                    ji.client_x = e.a; ji.client_y = e.b - BARH;
                    mods_of(&e, &ji);
                    struct node *n = 0;
                    browser_hittest_node(e.a, e.b - BARH, scroll, &n, 0, 0);
                    js_dom_dispatch(n, "mousemove", &ji);
                }
            } else if (e.type == EV_WHEEL) {
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                int allow = 1;
                if (e.b >= BARH && e.b < BARH + VIEW_H) {
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.client_x = e.a; ji.client_y = e.b - BARH;
                    ji.detail = e.wheel;
                    ji.delta_y = (double)e.wheel * 40.0;
                    mods_of(&e, &ji);
                    struct node *n = 0;
                    browser_hittest_node(e.a, e.b - BARH, scroll, &n, 0, 0);
                    allow = js_dom_dispatch(n, "wheel", &ji);
                }
                if (allow) scroll += e.wheel * 40;
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                need = 1;
            }
            if (!navigated && settle_dom()) need = 1;   /* a handler rewrote the DOM */
        }

        /* Due timers + animation frames. `js_page_pending()` is a pointer test,
         * so an idle page does not even read the clock -- the loop is exactly as
         * hot as it was before timers existed. */
        if (!navigated && js_page_pending()) {
            if (js_page_run_due() > 0) {
                if (settle_dom()) need = 1;
                if (js_page_output_len() != js_out_shown) { status_from_js("loaded"); need = 1; }
            }
        }

        if (need) redraw(editing);    /* one repaint after the burst, not per keystroke */
        sys_yield();
    }
}
