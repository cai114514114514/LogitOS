#include "aqua.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "js_dom.h"

/* A web browser. The whole render pipeline now runs in this ring-3 app: the
 * kernel does DNS+TCP+TLS+HTTP (SYS_HTTP_GET) and hands us the raw body
 * (SYS_HTTP_BODY); we parse HTML->DOM (net/dom.c), apply CSS (net/css.c), lay
 * out a display list (net/layout.c) and paint it via the GUI render syscalls
 * (browser_paint.c). Inline <script> runs through QuickJS. */

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
static char status[96] = "ready -- edit URL, Enter to load";
static struct node *g_root;              /* current page DOM (owns display-list strings) */

static void set_status(const char *s)
{ int i = 0; while (s[i] && i < (int)sizeof status - 1) { status[i] = s[i]; i++; } status[i] = 0; }

static void redraw(int editing);

/* ---- page JavaScript via QuickJS (console.log only; no DOM bindings yet) ---- */
#include "quickjs.h"
int printf(const char *, ...);
unsigned long strlen(const char *);

static char jsout[1024]; static int jslen;
static void jsput(const char *s)
{ for (const char *p = s; *p && jslen < (int)sizeof jsout - 1; p++) jsout[jslen++] = *p; jsout[jslen] = 0; }

static JSValue js_log(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) jsput(" ");
        jsput(s); printf("%s ", s);
        JS_FreeCString(ctx, s);
    }
    jsput("\n"); printf("\n");
    return JS_UNDEFINED;
}

static int run_js(const char *src)        /* returns 1 if the script mutated the DOM */
{
    JSRuntime *rt = JS_NewRuntime(); if (!rt) return 0;
    /* Our ring-3 user stack is 256 KiB; QuickJS defaults its overflow guard to
     * 256 KiB too, so a deeply-nested script (e.g. bilibili's minified inline
     * JS) overflows the real stack before the guard fires -> page fault in
     * JS_ThrowError2. Bound it well under the real stack so QuickJS throws a
     * catchable "stack overflow" instead of crashing. */
    JS_SetMaxStackSize(rt, 128 * 1024);
    JSContext *ctx = JS_NewContext(rt); if (!ctx) { JS_FreeRuntime(rt); return 0; }
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue con = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, con, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, g, "console", con);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_log, "print", 1));
    JS_FreeValue(ctx, g);
    js_dom_init(ctx, g_root);             /* document + Element bound to the live DOM */
    JSValue v = JS_Eval(ctx, src, strlen(src), "<page>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[browser] JS exception: %s\n", m ? m : "?");
        jsput("[exception] "); if (m) jsput(m);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    int mutated = js_dom_dirty();
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return mutated;
}

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

static int collect_scripts(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "script") && !dom_attr(n, "src")) {
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
        if (o < max - 1) out[o++] = '\n';
        if (o < max - 1) out[o++] = ';';
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_scripts(c, out, o, max);
    return o;
}

static unsigned char css_tmp[65536];     /* scratch for one external stylesheet */

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
 * Budgeted: each fetch is a full HTTPS handshake to a CDN, so cap the count. */
static int g_css_budget;
static int fetch_css_links(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (g_css_budget > 0 && o < max - 64 && n->type == N_ELEM && tag_is(n->tag, "link")) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        if (href && has_ci(rel, "stylesheet")) {
            g_css_budget--;
            int got = res_fetch_raw(href, css_tmp, (int)sizeof css_tmp);
            for (int i = 0; i < got && o < max - 1; i++) out[o++] = (char)css_tmp[i];
            if (got > 0 && o < max - 1) out[o++] = '\n';
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = fetch_css_links(c, out, o, max);
    return o;
}

static char bodybuf[65536];
static char author_css[262144];          /* inline <style> + fetched external <link> CSS (raw, with var()) */
static char css_expanded[393216];        /* author_css after var() expansion -> LibCSS */
static int  css_exlen;

static void load(const char *u)
{
    set_status("loading...");
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
    layout_page(g_root, WINW);
    ph = layout_height();
    set_status("loaded -- fetching stylesheets...");
    redraw(0);                       /* first paint: HTML + inline CSS, before slow CDN fetches */

    g_css_budget = 4;                /* fetch external <link> stylesheets, then re-style */
    int css2 = fetch_css_links(g_root, author_css, css_len, (int)sizeof author_css);
    if (css2 > css_len) {
        css_len = css2;
        css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
        css_apply(g_root, css_expanded, css_exlen);
        layout_page(g_root, WINW);
        ph = layout_height();
        redraw(0);                   /* re-paint with the page's real stylesheets */
    }
    set_status("loaded");
    if (layout_load_images(3) > 0) { /* ... then fetch a few images and repaint */
        ph = layout_height();
        redraw(0);
    }
    /* run the page's inline <script> through QuickJS */
    jslen = 0; jsout[0] = 0;
    static char scr[16384];
    int sn = collect_scripts(g_root, scr, 0, sizeof scr);
    if (sn > 1) {
        int mutated = run_js(scr);
        if (mutated) {                   /* JS changed the DOM -> re-style + re-layout */
            css_apply(g_root, css_expanded, css_exlen);
            layout_page(g_root, WINW);
            ph = layout_height();
        }
        if (jslen > 0) {
            char st[96]; int p = 0; const char *pre = "JS: ";
            while (*pre) st[p++] = *pre++;
            for (int i = 0; jsout[i] && jsout[i] != '\n' && p < 92; i++) st[p++] = jsout[i];
            st[p] = 0; set_status(st);
        } else set_status(mutated ? "loaded (script updated the page)" : "loaded (ran script, no output)");
        redraw(0);
    }
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    /* address bar */
    gui_rect(0, 0, WINW, BARH, rgb(225, 228, 234));
    gui_rect(10, 5, WINW - 20, 20, rgb(255, 255, 255));
    gui_text(14, 7, rgb(40, 40, 48), url);
    if (editing) gui_rect(14 + ulen * 8, 7, 8, 16, rgb(90, 150, 240));
    /* the page */
    browser_paint(0, BARH, WINW, VIEW_H, scroll);
    /* status line */
    gui_rect(0, WINH - 18, WINW, 18, rgb(238, 240, 244));
    gui_text(10, WINH - 16, rgb(110, 110, 120), status);
    gui_flush();
}

void app_main(void)
{
    css_init();             /* build the UA default stylesheet */
    img_init();             /* register PNG + GIF decoders */
    gui_create("Browser", WINW, WINH);
    redraw(1);
    int editing = 1;

    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_KEY) {
                int k = e.a;
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                if      (k == KEY_DOWN) scroll += 40;
                else if (k == KEY_UP)   scroll -= 40;
                else if (k == KEY_PGDN) scroll += VIEW_H - 40;
                else if (k == KEY_PGUP) scroll -= VIEW_H - 40;
                else if (k == KEY_HOME) scroll = 0;
                else if (k == KEY_END)  scroll = maxs;
                else if (k == '\n') { editing = 0; load(url); editing = 1; }
                else if (k == '\b') { if (ulen > 0) url[--ulen] = 0; }
                else if (k >= ' ' && k < 0x7f && ulen < (int)sizeof url - 1) { url[ulen++] = (char)k; url[ulen] = 0; }
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                redraw(editing);
            } else if (e.type == EV_MOUSE) {
                int mx = e.a, my = e.b;              /* window-local */
                if (my >= BARH && my < BARH + VIEW_H) {
                    char nu[256];
                    if (browser_hittest(mx, my - BARH, scroll, nu, sizeof nu)) {
                        int i = 0; while (nu[i] && i < (int)sizeof url - 1) { url[i] = nu[i]; i++; }
                        url[i] = 0; ulen = i;
                        load(url);
                    }
                    redraw(editing);
                }
            }
        }
        sys_yield();
    }
}
