#include "logit.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_module.h"           /* <script type="module"> + the module loader */
#include "bfetch.h"              /* the pooled ring-3 resource fetcher */
#include "url.h"                 /* url_parse + url_resolve for link clicks */
#include <stdlib.h>              /* malloc/realloc/free -- resources are sized to fit */

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

static int has_ci(const char *h, const char *n);   /* defined below */
static int str_eq(const char *a, const char *b);

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

static int str_eq(const char *a, const char *b)
{ int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }

/* ======================= sub-resources: the fetch table =======================
 *
 * THE BUDGETS ARE GONE, and it is worth writing down what they were and why
 * they could not stay.
 *
 *   g_css_budget = 24, g_js_budget = 8, MAX_CSS_SEEN = 32.
 *
 * They existed because every fetch went through SYS_RES_FETCH, which is one
 * blocking kernel request with `Connection: close` -- so the count of resources
 * WAS the count of TLS handshakes, and the count of handshakes was the page
 * load time. Capping the count was the only lever there was.
 *
 * Two things retired them. Commit 65eb2c7 made each <script> its own program,
 * so more scripts no longer means more truncation; and bfetch pools
 * connections, so 28 stylesheets from one CDN cost ONE handshake rather than
 * 28. The caps are now pure loss: kimi.com links 28 stylesheets and would have
 * lost four of them to a limit that no longer buys anything.
 *
 * So the table grows instead. It holds a pointer into the DOM for the
 * reference, and the fetched bytes are malloc'd to the size that actually
 * arrived -- which also retires the other silent limit, the 1 MiB static
 * scratch buffer that kimi's 1.55 MB main bundle already exceeded. */
struct resent {
    struct node *node;
    const char  *ref;         /* the raw attribute value; NULL for an inline script */
    int   module;             /* <script type="module"> */
    int   id;                 /* bfetch request id while in flight, else -1 */
    unsigned char *data;      /* fetched (or inline) source, owned */
    int   len;
    char *url;                /* absolute URL after redirects; the module name */
};

static struct resent *g_res;
static int g_nres, g_cres;

static void res_reset(void)
{
    for (int i = 0; i < g_nres; i++) {
        if (g_res[i].id >= 0) bfetch_release(g_res[i].id);
        free(g_res[i].data);
        free(g_res[i].url);
    }
    g_nres = 0;
}

static struct resent *res_add(struct node *n, const char *ref, int module)
{
    if (g_nres == g_cres) {
        int nc = g_cres ? g_cres * 2 : 16;
        struct resent *nv = realloc(g_res, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        g_res = nv; g_cres = nc;
    }
    struct resent *e = &g_res[g_nres++];
    e->node = n; e->ref = ref; e->module = module;
    e->id = -1; e->data = 0; e->len = 0; e->url = 0;
    return e;
}

static char *dupstr(const char *s)
{
    int n = 0; while (s[n]) n++;
    char *p = malloc((size_t)n + 1);
    if (p) { for (int i = 0; i <= n; i++) p[i] = s[i]; }
    return p;
}

/* Keep the window answering while a load is in flight.
 *
 * Nothing below this point blocks in the kernel any more -- bfetch runs over
 * the non-blocking socket ABI, so the WM thread keeps composing the desktop and
 * running net_poll (which is what advances our own sockets). This hook is what
 * the BROWSER'S OWN window does with that: honour the close button, and show
 * progress instead of a frozen "loading...".
 *
 * Input other than close is dropped on purpose while loading. There is no page
 * to deliver it to yet, and queueing it would replay a burst of keystrokes into
 * whatever document finally arrives. */
static int  g_prog_done, g_prog_total;
static unsigned long long g_prog_last;
static const char *g_prog_what = "";

static void num_append(char *st, int *p, int v)
{
    if (v < 0) v = 0;
    int d = 1; while (v / d >= 10) d *= 10;
    while (d) { st[(*p)++] = (char)('0' + (v / d) % 10); d /= 10; }
}

static void load_tick(void)
{
    struct logit_event e;
    while (poll_event(&e))
        if (e.type == EV_CLOSE) { js_page_close(); bfetch_close_all(); app_exit(0); }

    unsigned long long now = monotonic_ms();
    if (now - g_prog_last < 400) return;
    g_prog_last = now;
    char st[96]; int p = 0;
    for (const char *s = g_prog_what; *s && p < 60; s++) st[p++] = *s;
    if (g_prog_total > 0) {
        st[p++] = ' ';
        num_append(st, &p, g_prog_done);
        st[p++] = '/';
        num_append(st, &p, g_prog_total);
    }
    st[p++] = ' '; st[p++] = '.'; st[p++] = '.'; st[p++] = '.';
    st[p] = 0;
    set_status(st);
    if (g_root) redraw(0);
}

/* Fetch every entry in the table CONCURRENTLY.
 *
 * The old code fetched one resource at a time inside a recursive DOM walk, and
 * each of those was a full DNS+TCP+TLS+HTTP round trip in ring 0. Here the
 * requests are all in flight together over pooled connections, so a page with
 * 28 stylesheets on one CDN is one handshake and 28 pipelined-in-parallel
 * requests rather than 28 handshakes end to end. */
#define RES_INFLIGHT 8            /* < bfetch's table, and <= the pool's caps */

static void res_fetch_all(const char *what)
{
    int next = 0, inflight = 0;
    g_prog_done = 0; g_prog_total = g_nres; g_prog_what = what; g_prog_last = 0;
    while (next < g_nres || inflight > 0) {
        while (next < g_nres && inflight < RES_INFLIGHT) {
            struct resent *e = &g_res[next++];
            if (!e->ref) { g_prog_done++; continue; }      /* inline: nothing to fetch */
            e->id = bfetch_start(e->ref);
            if (e->id < 0) { printf("[browser] cannot fetch %s\n", e->ref); g_prog_done++; continue; }
            inflight++;
        }
        if (inflight == 0) break;
        bfetch_pump();
        for (int i = 0; i < next; i++) {
            struct resent *e = &g_res[i];
            if (e->id < 0) continue;
            int st = bfetch_state(e->id);
            if (st == BF_PENDING) continue;
            if (st == BF_DONE && bfetch_status(e->id) / 100 == 2) {
                e->url = dupstr(bfetch_url(e->id));
                e->len = bfetch_take(e->id, &e->data);
                if (e->len < 0) { e->len = 0; e->data = 0; }
            } else {
                printf("[browser] fetch failed (status %d) %s: %s\n",
                       bfetch_status(e->id), e->ref, bfetch_error(e->id));
                bfetch_release(e->id);
            }
            e->id = -1;
            inflight--;
            g_prog_done++;
        }
        load_tick();
        sys_yield();
    }
}

/* ---- what the DOM offers ---- */

static void collect_css_links(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM && tag_is(n->tag, "link")) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        /* a11y override themes are inactive unless the user selected them;
         * skipping saves ~1 MiB of CSS on github.com. This is a CORRECTNESS
         * filter (the sheets do not apply), not a budget. */
        if (href && (has_ci(href, "high_contrast") || has_ci(href, "colorblind") ||
                     has_ci(href, "tritanopia"))) href = 0;
        if (href && has_ci(rel, "stylesheet") && !has_ci(href, "data:")) {
            int dup = 0;                       /* github links the same module CSS 3x */
            for (int i = 0; i < g_nres; i++)
                if (g_res[i].ref && str_eq(g_res[i].ref, href)) { dup = 1; break; }
            if (!dup) res_add(n, href, 0);
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect_css_links(c);
}

/* Every <script> in document order, classified.
 *
 * `type` is a whitelist per spec: "module" selects the module goal, a
 * JavaScript MIME type (or nothing) selects the classic goal, and ANYTHING ELSE
 * is a data block that must not be executed -- which is how <script
 * type="application/json"> and <script type="importmap"> stop being reported as
 * the page's own syntax errors. `nomodule` marks a fallback for engines without
 * module support; we have it, so those are skipped. */
static void collect_scripts(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM && tag_is(n->tag, "script")) {
        const char *type = dom_attr(n, "type");
        const char *src  = dom_attr(n, "src");
        int module = js_module_is_module_type(type);
        int classic = !module && js_module_is_classic_type(type);
        if (!module && !classic) {
            printf("[browser] skipping <script type=\"%s\"> (not executable)\n", type ? type : "");
        } else if (!module && dom_attr(n, "nomodule")) {
            /* the fallback for a browser without modules; we are not one */
        } else if (src) {
            if (!has_ci(src, "javascript:") && !has_ci(src, "data:"))
                res_add(n, src, module);
        } else {
            /* inline: reassemble the text nodes into one exactly-sized buffer.
             * The old path had a 256 KiB static cap and skipped anything over
             * it; sizing to the content removes the question. */
            int total = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text) total += c->textlen;
            if (total <= 0) { /* empty inline script */ }
            else {
                struct resent *e = res_add(n, 0, module);
                if (e) {
                    e->data = malloc((size_t)total + 1);
                    if (e->data) {
                        int o = 0;
                        for (struct node *c = n->first_child; c; c = c->next)
                            if (c->type == N_TEXT && c->text)
                                for (int i = 0; i < c->textlen; i++) e->data[o++] = (unsigned char)c->text[i];
                        e->data[o] = 0;
                        e->len = o;
                    }
                }
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect_scripts(c);
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

/* The document source. The DOM borrows text out of it, so it has to outlive the
 * tree -- which is also why it is a malloc'd buffer sized to the response and no
 * longer a 1 MiB static: a page bigger than that used to be silently cut. */
static unsigned char *g_page_src;
static char author_css[4194304];         /* inline <style> + fetched external <link> CSS (4 MiB; github.com ships ~3.25 MiB) */
static char css_expanded[4718592];       /* author_css after var() expansion -> LibCSS (4.5 MiB) */
static int  css_exlen;

/* Re-style + re-lay-out after script changed the DOM. Every path that can run
 * JS ends here, so a mutation from a click handler and one from a timer take
 * exactly the same route back to the screen. Returns 1 if the page actually
 * changed and needs repainting. */
static int restyle(void);

static int settle_dom(void)
{
    if (!js_dom_dirty()) return 0;
    int changed = restyle();
    js_dom_clear_dirty();
    return changed;
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

/* Run the page's scripts.
 *
 * ORDER IS THE SPEC, and it is two passes rather than one:
 *
 *   - classic scripts run first, in document order. (In a real browser they run
 *     as the parser reaches them; we have already finished parsing, so document
 *     order is the same answer.)
 *   - MODULES ARE DEFERRED. Every <script type="module"> is implicitly `defer`,
 *     so they all run after the document is parsed and after every classic
 *     script, still in document order among themselves.
 *
 * Each script is its own program -- see 65eb2c7 -- so a bundle that throws no
 * longer takes the page's inline scripts down with it. Returns how many ran. */
static int run_collected_scripts(const char *page_url)
{
    int ran = 0, inline_n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_nres; i++) {
            struct resent *e = &g_res[i];
            if (e->module != pass) continue;            /* pass 0 classic, pass 1 module */
            if (!e->data || e->len <= 0) continue;
            if (!e->module) {
                js_page_eval((const char *)e->data, e->len, e->url ? e->url : "<inline>");
                ran++;
                continue;
            }
            /* A module needs a UNIQUE ABSOLUTE URL: it is both the key in the
             * module map (so an import of the same file twice instantiates it
             * once) and the base every specifier inside it resolves against.
             * An inline module has no URL of its own, so it gets the document's
             * with a discriminator -- which resolves relative specifiers exactly
             * as the spec says, against the document. */
            char name[600];
            const char *nm = e->url;
            if (!nm) {
                int p = 0;
                for (const char *s = page_url; *s && p < 560; s++) name[p++] = *s;
                const char *tag = "#inline-module-";
                for (const char *s = tag; *s && p < 590; s++) name[p++] = *s;
                num_append(name, &p, ++inline_n);
                name[p] = 0;
                nm = name;
            }
            js_module_eval((const char *)e->data, e->len, nm);
            ran++;
        }
    }
    return ran;
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
    res_reset();
    free(g_page_src); g_page_src = 0;
    ph = 0; scroll = 0;

    /* A navigation ends the old page's connections: keeping them would hold
     * pool slots (and kernel socket slots) for an origin the new page may have
     * nothing to do with. */
    bfetch_init();
    bfetch_set_tick(load_tick);
    bfetch_close_all();
    bfetch_reset_stats();
    js_module_reset();
    bfetch_set_base(u);

    g_prog_what = "fetching page"; g_prog_total = 0; g_prog_done = 0; g_prog_last = 0;
    int doc = bfetch_start(u);
    if (doc < 0) { set_status("load failed: bad URL (need http:// or https://)"); return; }
    bfetch_wait(doc, load_tick);
    if (bfetch_state(doc) != BF_DONE) {
        printf("[browser] page fetch failed: %s\n", bfetch_error(doc));
        set_status("load failed: could not fetch the page");
        bfetch_release(doc);
        return;
    }
    int code = bfetch_status(doc);
    /* The URL AFTER redirects is the base for every relative reference on the
     * page. Resolving against the typed URL instead is how a redirected page
     * ends up asking the wrong origin for its own stylesheets. */
    char base[600];
    { const char *f = bfetch_url(doc); int i = 0;
      while (f[i] && i < (int)sizeof base - 1) { base[i] = f[i]; i++; } base[i] = 0; }
    bfetch_set_base(base);
    int blen = 0;
    blen = bfetch_take(doc, &g_page_src);
    if (code / 100 != 2) {
        char st[64]; int p = 0; const char *pre = "error: HTTP ";
        while (*pre) st[p++] = *pre++;
        num_append(st, &p, code); st[p] = 0;
        set_status(st);
        if (blen <= 0) return;                       /* still render an error body */
    }
    if (blen <= 0) { set_status("error: empty response"); return; }
    g_root = dom_parse((const char *)g_page_src, blen);
    if (!g_root) { set_status("error: parse failed"); return; }
    int css_len = collect_style(g_root, author_css, 0, (int)sizeof author_css);
    css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
    css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, WINW);
    ph = layout_height();
    set_status("loaded -- fetching stylesheets...");
    redraw(0);                       /* first paint: HTML + inline CSS, before slow CDN fetches */

    /* ---- external stylesheets, all at once, no budget ---- */
    res_reset();
    collect_css_links(g_root);
    int nsheets = g_nres;
    res_fetch_all("stylesheets");
    int css2 = css_len, got_sheets = 0;
    for (int i = 0; i < g_nres; i++) {
        struct resent *e = &g_res[i];
        if (!e->data || e->len <= 0) continue;
        got_sheets++;
        for (int k = 0; k < e->len && css2 < (int)sizeof author_css - 1; k++)
            author_css[css2++] = (char)e->data[k];
        if (css2 < (int)sizeof author_css - 1) author_css[css2++] = '\n';
    }
    res_reset();
    /* report what actually arrived: sheet count + KiB (debug aid for CDN fetch issues) */
    { char st[96]; int p = 0; const char *pre = "loaded, ";
      while (*pre) st[p++] = *pre++;
      num_append(st, &p, got_sheets);
      st[p++] = '/'; num_append(st, &p, nsheets);
      const char *mid = " sheets, ";
      while (*mid) st[p++] = *mid++;
      num_append(st, &p, css2 / 1024);
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
    /* Images ride the same pooled connections now, so eight of them from one
     * host is one handshake rather than eight. That is why this number can go
     * up without the load time going with it. */
    g_prog_what = "images"; g_prog_total = 0; g_prog_last = 0;
    if (layout_load_images(16) > 0) {
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
    js_page_set_location(base);
    js_page_open(g_root);

    /* Fetch every external script CONCURRENTLY, then run them in spec order.
     * Real sites ship huge minified bundles that assume a full browser env;
     * with no real DOM they just throw -- but they throw ALONE. The runtime's
     * 2 MiB stack guard bounds recursive scripts so a bad bundle raises a
     * catchable RangeError instead of faulting. */
    res_reset();
    collect_scripts(g_root);
    res_fetch_all("scripts");
    g_prog_what = "running scripts"; g_prog_total = 0; g_prog_last = 0;
    int had_script = run_collected_scripts(base) > 0;
    { int dials = 0, reuses = 0, reqs = 0, mods = 0, modfail = 0;
      bfetch_stats(&dials, &reuses, &reqs);
      js_module_stats(&mods, &modfail);
      printf("[browser] load done: %d requests, %d connections dialled, %d reused"
             ", %d modules loaded (%d failed)\n", reqs, dials, reuses, mods, modfail); }
    res_reset();

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

/* Re-run the cascade + layout after a script mutation. The expanded stylesheet
 * is whatever the last fetch produced, so this is safe to call at any point
 * after the first css_apply.
 *
 * This used to be "css_apply over the whole document, then layout_page over the
 * whole document", unconditionally, for every mutation. With pages live, that
 * is what a setInterval nudging one element cost every single tick. Now the
 * mutation says WHERE it happened (js_dom.c's invalidation record) and HOW
 * much can have moved, and the work follows:
 *
 *   - a marked scope re-styles that subtree (+ its following siblings, for the
 *     sibling combinators) instead of the document;
 *   - the cascade reports whether anything actually came out different, so a
 *     class toggle that matches no rule costs no layout and no repaint at all;
 *   - a structural change skips that question, because inserting or removing a
 *     node moves boxes whatever the computed styles say.
 *
 * The fallbacks are all in the safe direction: no scopes, too many scopes, or
 * a scope whose node was destroyed before we got here all mean "do what this
 * function used to do". */
static int restyle(void)
{
    if (!g_root) return 0;
    int level = js_dom_inval_level();
    if (level == INVAL_NONE) return 0;

    int nroots = js_dom_inval_roots();
    int changed = CSS_CHANGED_NONE;
    for (int i = 0; i < nroots; i++) {
        int sib = 0;
        struct node *n = js_dom_inval_root(i, &sib);
        if (!n) { nroots = 0; break; }        /* destroyed since it was marked */
        changed |= css_apply_scoped(n, sib, css_expanded, css_exlen);
    }
    if (nroots == 0) {                        /* whole document */
        css_apply(g_root, css_expanded, css_exlen);
        css_extra_apply(g_root, css_expanded, css_exlen);
        changed = CSS_CHANGED_LAYOUT;
    }
    /* Nodes came or went: the box tree changed even if every computed style
     * came back identical. */
    if (level >= INVAL_LAYOUT) changed |= CSS_CHANGED_LAYOUT;

    if (changed == CSS_CHANGED_NONE) return 0;
    /* CSS_CHANGED_PAINT still rebuilds the display list: layout_page is what
     * fills in every painted colour, so there is no cheaper path to take until
     * layout grows one. The tier is already carried this far, so adding it is
     * a change on the layout side alone. */
    layout_page(g_root, WINW);
    ph = layout_height();
    return 1;
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
    /* css_extra patches node->style after the cascade, so a scoped re-style has
     * to run it before it decides whether anything changed -- see css.h. */
    css_set_post_pass(css_extra_apply);
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
