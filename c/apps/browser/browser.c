#include "logit.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_module.h"           /* <script type="module"> + the module loader */
/* js_webapi.o is absent from browser-nofetch.aex (the negative control for
 * test-webapi-page), so every entry point has to be weak here exactly as it is
 * in js_page.c -- otherwise that link breaks. */
#define JS_WEBAPI_OPTIONAL
#include "js_webapi.h"           /* script-initiated navigation (location.*) */
#include "bfetch.h"              /* the pooled ring-3 resource fetcher */
#include "tabs.h"                /* per-tab state, session, history, bookmarks */
#include "url.h"                 /* url_parse + url_resolve for link clicks */
#include <stdlib.h>              /* malloc/realloc/free -- resources are sized to fit */
#include <string.h>

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

#define WINW 1180                        /* the size the window is BORN at */
#define WINH 620
#define BARH 30
#define TABH 30                          /* the tab strip, above the address bar */

/* The window is resizable (EV_RESIZE, see include/abi/logit_abi.h) and the tab
 * strip has to lay out at any width, so nothing below may derive geometry from
 * WINW/WINH. These two are the truth, and they are updated from EV_RESIZE and
 * from SYS_GUI_WIN_STATE at startup. */
static int win_w = WINW, win_h = WINH;

#define VIEW_Y   (TABH + BARH)
#define VIEW_H   (win_h - VIEW_Y - 18)   /* viewport (below the bars, above status) */

static char url[600] = "http://example.com/";
static int  ulen = 19;
static int  scroll;                      /* pixel scroll offset */
static int  ph;                          /* laid-out page height */
static char status[96] = "ready -- Enter loads; Cmd+T new tab, Ctrl+Tab switches";
static struct node *g_root;              /* current page DOM (owns display-list strings) */

/* ===================== history: now PER TAB, not global =====================
 *
 * These three used to own a `char hist[32][600]` of their own. They now
 * delegate to the active tab, and that is the whole of the change at the call
 * sites -- follow_link, the address bar, the redirect chain and the arrow keys
 * all still say hist_push/hist_replace/hist_go and all of them are now about
 * the tab the user is looking at.
 *
 * Doing it this way rather than threading a `struct tab *` through every caller
 * is deliberate: a back/forward stack that belongs to the wrong tab is a bug
 * you find by clicking Back, and the smaller the diff at the call sites the
 * fewer places that bug can hide. */
static void hist_push(const char *u)    { tab_hist_push(tab_cur(), u); }
static void hist_replace(const char *u) { tab_hist_replace(tab_cur(), u); }

static int hist_go(int delta)              /* -1 back, +1 forward; 1 if moved */
{
    if (!tab_hist_go(tab_cur(), delta, url, (int)sizeof url)) return 0;
    ulen = 0; while (url[ulen]) ulen++;
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

/* =========================== the persistent store ==========================
 *
 * tabs.c writes the session, the history list and the bookmarks through a
 * three-function interface rather than calling the syscalls itself, for the two
 * reasons in tabs.h: the host loader test drives this same code with no kernel
 * under it, and a settings/persistence line is building a config store this
 * round -- when it lands, this struct is the one place that changes.
 *
 * The host build (tests/unit/loaderhost/logit.h) has no file syscalls, so it
 * installs its own backend and this one is not compiled at all. */
#ifndef LOADERHOST_LOGIT_H
static int os_store_read(const char *p, void *b, int m)  { return read_file(p, b, m); }
static int os_store_write(const char *p, const void *b, int l) { return write_file(p, b, l); }
static int os_store_mkdir(const char *p) { return make_dir(p); }
static const struct bstore_ops os_store = { os_store_read, os_store_write, os_store_mkdir };
#endif

/* The window's real size. EV_RESIZE tells us when it changes, but an app that
 * has not received one yet (and one that missed one) has to ask -- re-deriving
 * it from gui_create's argument is exactly the assumption resize invalidates.
 *
 * Both are no-ops in the host test, which has no window manager: there the
 * window is whatever host_win_w/h say and never changes. */
static void win_query_size(void)
{
#if !defined(LOADERHOST_LOGIT_H) && defined(SYS_GUI_WIN_STATE)
    int w = (int)_sys(SYS_GUI_WIN_STATE, WINS_W, 0, 0);
    int h = (int)_sys(SYS_GUI_WIN_STATE, WINS_H, 0, 0);
    if (w > 200 && h > 200) { win_w = w; win_h = h; }
#endif
}

static void win_set_min(void)
{
#if !defined(LOADERHOST_LOGIT_H) && defined(SYS_GUI_WIN_MIN)
    /* Below this the tab strip cannot show a tab AND its close button, and the
     * address bar cannot show a URL. A floor is the honest answer to "lay out
     * at any width" -- the layout is fluid down to here and refuses below it. */
    _sys(SYS_GUI_WIN_MIN, ((long)480 << 16) | 320, 0, 0);
#endif
}

/* ---- the page's <title>, which is what a tab is labelled with ----
 *
 * Falls back to the host, then to the URL: a tab strip where three tabs all say
 * the same thing is a tab strip you cannot use, and plenty of real pages have
 * no <title> at all. */
static void title_of(struct node *n, char *out, int max, int *done)
{
    if (!n || *done) return;
    if (n->type == N_ELEM && tag_is(n->tag, "title")) {
        int o = 0;
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) {
                    char ch = c->text[i];
                    if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
                    /* collapse runs of space: a <title> is often indented markup */
                    if (ch == ' ' && (o == 0 || out[o-1] == ' ')) continue;
                    out[o++] = ch;
                }
        while (o > 0 && out[o-1] == ' ') o--;
        out[o] = 0;
        if (o > 0) { *done = 1; return; }
    }
    for (struct node *c = n->first_child; c; c = c->next) title_of(c, out, max, done);
}

static void url_label(const char *u, char *out, int max)
{
    /* host + first path segment is what a tab has room for */
    const char *p = u;
    if (!p) { out[0] = 0; return; }
    const char *h = p;
    for (const char *s = p; *s; s++)
        if (s[0] == ':' && s[1] == '/' && s[2] == '/') { h = s + 3; break; }
    int o = 0;
    if (h[0] == 'w' && h[1] == 'w' && h[2] == 'w' && h[3] == '.') h += 4;
    for (const char *s = h; *s && *s != '/' && o < max - 1; s++) out[o++] = *s;
    out[o] = 0;
    if (o == 0) { int i = 0; while (u[i] && i < max - 1) { out[i] = u[i]; i++; } out[i] = 0; }
}

/* Update the active tab's label from whatever the document offered. */
static void tab_retitle(void)
{
    struct tab *t = tab_cur();
    if (!t) return;
    char title[TAB_TITLE]; title[0] = 0;
    int done = 0;
    if (g_root) title_of(g_root, title, (int)sizeof title, &done);
    if (!done || !title[0]) url_label(url, title, (int)sizeof title);
    if (!title[0]) { const char *b = "Untitled"; int i = 0; while (b[i]) { title[i] = b[i]; i++; } title[i] = 0; }
    int i = 0; while (title[i] && i < TAB_TITLE - 1) { t->title[i] = title[i]; i++; }
    t->title[i] = 0;
}

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

/* getBoundingClientRect reports VIEWPORT coordinates, and js_dom.c cannot know
 * the scroll offset -- the embedder owns it. Push it whenever it moves.
 *
 * The two coincide at scroll 0, which is where every page starts, so a missing
 * call here is invisible to every test and wrong the instant a user scrolls.
 * Hence one function called from every site that touches `scroll`, rather than
 * an assignment sprinkled next to each of them. */
static int g_scroll_pushed;
static void sync_scroll(void)
{
    if (scroll == g_scroll_pushed) return;
    g_scroll_pushed = scroll;
    js_dom_set_scroll(0, scroll);
}

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

    /* REPAINT BETWEEN RESOURCES, NEVER DURING ONE.
     *
     * This used to repaint on a 400 ms timer, and that timer was a bug with a
     * measurable failure: repainting a wikipedia page is thousands of text-run
     * syscalls, and while it runs nobody drains the socket. TCP's receive ring
     * is 64 KiB, so a 216 KB stylesheet arriving during a repaint overran the
     * window and the transfer died with `connection closed mid-message` -- on
     * the first attempt AND on the retry, which is why the page then rendered
     * with none of its stylesheets.
     *
     * A progress counter only moves when a resource FINISHES, so keying the
     * repaint to it puts the expensive work in the gaps between transfers,
     * which is exactly where it belongs. The close button is still serviced on
     * every single tick, because that is cheap and it is what makes the window
     * feel alive. */
    unsigned long long now = monotonic_ms();
    static int last_done = -1;
    if (g_prog_done == last_done && now - g_prog_last < 3000) return;
    last_done = g_prog_done;
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

/* Bytes served out of the active tab's retained set instead of the network,
 * counted so the tab test can assert that a re-hydrate did not dial. */
static int g_res_from_tab, g_res_from_net;

/* Fill `e` from the tab's own bytes if it has them. This is what makes bringing
 * a background tab back cost NO connections: the tab kept every script and
 * image it was built from (tabs.h), keyed by absolute URL, and bfetch_resolve
 * gives us that key from the raw attribute value. */
static int res_try_tab(struct resent *e)
{
    struct tab *t = tab_cur();
    if (!t || !e->ref) return 0;
    char abs[600];
    if (bfetch_resolve(0, e->ref, abs, (int)sizeof abs) != 0) return 0;
    const struct tabres *r = tab_res_find(t, abs);
    if (!r || !r->data || r->len <= 0) return 0;
    e->data = malloc((size_t)r->len + 1);
    if (!e->data) return 0;
    for (int i = 0; i < r->len; i++) e->data[i] = r->data[i];
    e->data[r->len] = 0;
    e->len = r->len;
    e->url = dupstr(abs);
    g_res_from_tab++;
    return 1;
}

/* `keep` = record what arrives in the active tab, so a later re-hydrate can
 * replay it. Stylesheets pass 0: their bytes are retained ONCE, concatenated,
 * as the tab's `css` -- keeping them a second time individually would double
 * the largest thing a page ships (github's is 3.25 MB). */
static void res_fetch_all(const char *what, int keep)
{
    int next = 0, inflight = 0;
    g_prog_done = 0; g_prog_total = g_nres; g_prog_what = what; g_prog_last = 0;
    while (next < g_nres || inflight > 0) {
        while (next < g_nres && inflight < RES_INFLIGHT) {
            struct resent *e = &g_res[next++];
            if (!e->ref) { g_prog_done++; continue; }      /* inline: nothing to fetch */
            if (res_try_tab(e)) { g_prog_done++; continue; }
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
                g_res_from_net++;
                if (keep && e->data && e->len > 0)
                    tab_keep_res(tab_cur(), e->url, e->data, e->len);
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
    int ran = 0, inline_n = 0, classic_n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_nres; i++) {
            struct resent *e = &g_res[i];
            if (e->module != pass) continue;            /* pass 0 classic, pass 1 module */
            if (!e->data || e->len <= 0) continue;
            if (!e->module) {
                /* A CLASSIC script's URL is not decoration either: it is the
                 * base a dynamic import() inside it resolves against. An
                 * inline classic script used to be handed the literal
                 * "<inline>", which is not a URL, so `import('./x.js')` from
                 * an inline <script> resolved against nothing and failed --
                 * while the identical call in an external script, or in the
                 * inline MODULE path ten lines below, worked. Same rule as the
                 * module path: the document's URL with a discriminator. */
                char cname[600];
                const char *cnm = e->url;
                if (!cnm) {
                    int p = 0;
                    for (const char *s = page_url; *s && p < 560; s++) cname[p++] = *s;
                    const char *tag = "#inline-script-";
                    for (const char *s = tag; *s && p < 590; s++) cname[p++] = *s;
                    num_append(cname, &p, ++classic_n);
                    cname[p] = 0;
                    cnm = cname;
                }
                js_page_eval((const char *)e->data, e->len, cnm);
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

/* ===================== script-initiated navigation ==========================
 *
 * A page is allowed to move itself: `location.href = ...`, location.assign(),
 * location.replace(), location.reload(). js_webapi.c parses and records those
 * and NOTHING USED TO READ THE RECORD -- it printed "the loader does not
 * consume this yet" and the browser sat on the document.
 *
 * That is not a corner case. It is how https://www.baidu.com/ renders blank:
 * baidu sniffs the User-Agent, and to ours it serves 227 bytes whose entire
 * body is
 *
 *     <script>location.replace(location.href.replace("https://","http://"));</script>
 *
 * with no stylesheet, no <script src>, no image and no text. So "the loader
 * found no sub-resources" and "the page was blank" were one fact, not two: we
 * rendered a redirect stub, correctly, for ever. (The `<noscript><meta
 * http-equiv=refresh>` beside it is NOT a second chance -- with scripting
 * enabled, noscript content is raw text by spec and our tokenizer treats it as
 * such, which is right.)
 *
 * WHERE IT IS CONSUMED, and why it is not consumed where it is produced: this
 * runs from the loader and from the top of the event loop, never from inside a
 * JS callback. loc_set() is reached from arbitrarily deep inside a script; if
 * it navigated in place it would dom_free() and js_page_close() the very
 * document whose handler is on the stack. So the record is taken at a point
 * where nothing of the old page is live any more.
 *
 * THE BUDGET IS A LOOP GUARD, not a policy. A page that replaces itself with
 * itself never stops, and a pair of pages that bounce to each other never stop
 * either. Real browsers cap the chain; so does this, and it says so in the
 * status bar rather than freezing. The budget is per user-initiated load, so
 * following ten redirects and then clicking a link gives the next chain a
 * fresh ten.
 *
 * Overridable so that the host test can build a loader with a budget of ZERO,
 * which is byte-for-byte the behaviour this fix replaced: the record is taken
 * and thrown away, the stub stays on screen. `make test-loader-negctl` builds
 * exactly that and requires the test to FAIL -- without it, test-loader passing
 * would not be evidence that it measures anything. */
#ifndef NAV_MAX_HOPS
#define NAV_MAX_HOPS 10
#endif

/* Take a pending script navigation into `out`. 0 if there is none, or if this
 * build has no js_webapi.o at all (browser-nofetch). */
static int take_script_nav(char *out, int max)
{
    if (!js_webapi_take_navigation) return 0;
    return js_webapi_take_navigation(out, max) ? 1 : 0;
}

static void load_once(const char *u);

/* A user-initiated load, plus every navigation the page itself then asks for.
 *
 * The chain is a LOOP rather than recursion on purpose: a redirect chain of n
 * hops must cost one stack frame, not n, and each hop tears down the previous
 * document completely before the next one starts. */
static void load(const char *u)
{
    char cur[600], next[600];
    /* `u` is usually &url[0] -- follow_link and the address bar both write it
     * before calling. Copy first: the chain rewrites `url` on every hop. */
    { int i = 0; while (u[i] && i < (int)sizeof cur - 1) { cur[i] = u[i]; i++; } cur[i] = 0; }
    /* THE INVARIANT: after load(u), the browser is AT u. Every in-app caller
     * already wrote `url` before calling, so this is a no-op for them -- but it
     * has to be stated, because the moment it is only true by convention it
     * stops being true. It was not true for browser_load() from the host test,
     * and tab_dehydrate() -- which records `url` as the tab's address -- then
     * stamped a stale URL onto the tab it was putting away. */
    { int i = 0; while (cur[i] && i < (int)sizeof url - 1) { url[i] = cur[i]; i++; }
      url[i] = 0; ulen = i; }

    /* Discard anything the PREVIOUS page left pending. js_webapi_install does
     * not clear the record, so a navigation requested by a page the user then
     * abandoned would otherwise fire against the new one. */
    take_script_nav(next, sizeof next);

    for (int hops = 0; ; hops++) {
        load_once(cur);
        if (!take_script_nav(next, sizeof next)) return;
        if (hops >= NAV_MAX_HOPS) {
            set_status("stopped: too many redirects");
            redraw(0);
            return;
        }
        /* The address bar follows, and so does history -- but by REPLACING the
         * current entry. See hist_replace: a redirect that pushes is a Back
         * button that cannot escape. */
        { int i = 0; while (next[i] && i < (int)sizeof url - 1) { url[i] = next[i]; i++; }
          url[i] = 0; ulen = i; }
        hist_replace(url);
        { int i = 0; while (url[i] && i < (int)sizeof cur - 1) { cur[i] = url[i]; i++; } cur[i] = 0; }
    }
}

/* Exposed for the host loader test (tests/unit/loader_test.c), which links this
 * file against a fake bfetch and a recording GUI so that the redirect chain
 * above is testable without QEMU. Nothing in the app calls it. */
void browser_load(const char *u);
void browser_load(const char *u) { load(u); }

/* 1 while a tab is being replayed from its own retained bytes rather than
 * loaded from the network. Everything it changes is marked `hydrating` below. */
static int g_hydrating;

static void load_once(const char *u)
{
    set_status(g_hydrating ? "restoring tab..." : "loading...");
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
    ph = 0;
    if (!g_hydrating) scroll = 0;     /* hydrating: the tab's scroll is restored */
    g_scroll_pushed = 0;          /* js_dom_init resets its side to 0 as well */

    /* A navigation ends the old page's connections: keeping them would hold
     * pool slots (and kernel socket slots) for an origin the new page may have
     * nothing to do with.
     *
     * A TAB SWITCH IS NOT A NAVIGATION and must not do either of those. Closing
     * the pool on a switch would make N tabs cost N times the handshakes, which
     * is precisely the multiplication tabs are supposed not to cause -- and the
     * pool is process-global (bfetch_init is idempotent), so leaving it alone is
     * all that "the pool is shared across tabs" requires. */
    bfetch_init();
    bfetch_set_tick(load_tick);
    if (!g_hydrating) {
        bfetch_cache_clear();
        bfetch_close_all();
        /* A NAVIGATION REPLACES WHAT THE TAB IS, so what it retained goes with
         * it. Two things depend on this and both are silent if it is missing:
         * res_try_tab serves by absolute URL, so the old page's script bytes
         * would be handed to the new page whenever the two share a URL -- a
         * cache with no expiry that nobody asked for -- and tab_keep_res only
         * ever appends, so a tab navigated ten times would hold ten pages'
         * sub-resources. The old document is already freed by the teardown
         * above, so nothing is pointing into these bytes. */
        tab_drop_content(tab_cur());
    }
    bfetch_reset_stats();
    g_res_from_tab = g_res_from_net = 0;
    js_module_reset();
    bfetch_set_base(u);

    g_prog_what = "fetching page"; g_prog_total = 0; g_prog_done = 0; g_prog_last = 0;
    char base[600];
    int code = 200, blen = 0;
    struct tab *ht = g_hydrating ? tab_cur() : 0;
    if (ht && ht->src && ht->srclen > 0) {
        /* HYDRATING: the document is already here. Copy rather than borrow --
         * the tab has to still own its master copy for the next switch, and
         * the DOM borrows text straight out of whatever we hand dom_parse. */
        blen = ht->srclen;
        g_page_src = malloc((size_t)blen + 1);
        if (!g_page_src) { set_status("restore failed: out of memory"); return; }
        for (int i = 0; i < blen; i++) g_page_src[i] = ht->src[i];
        g_page_src[blen] = 0;
        { const char *f = ht->base[0] ? ht->base : u; int i = 0;
          while (f[i] && i < (int)sizeof base - 1) { base[i] = f[i]; i++; } base[i] = 0; }
        bfetch_set_base(base);
        /* Put every byte the tab kept back where res_fetch() will look for it,
         * so layout's <img> loop finds its images without a connection. */
        for (int i = 0; i < ht->nres; i++)
            bfetch_cache_put(ht->res[i].url, ht->res[i].data, ht->res[i].len);
    } else {
    /* Not hydrating after all -- either this is a real navigation, or the tab
     * had no retained bytes (a session-restored tab, which is empty by design).
     * Clearing `ht` is what makes every "am I replaying?" test below correct;
     * leaving it set would apply a stale stylesheet to fresh markup. */
    ht = 0;
    int doc = bfetch_start(u);
    if (doc < 0) { set_status("load failed: bad URL (need http:// or https://)"); return; }
    bfetch_wait(doc, load_tick);
    if (bfetch_state(doc) != BF_DONE) {
        printf("[browser] page fetch failed: %s\n", bfetch_error(doc));
        set_status("load failed: could not fetch the page");
        bfetch_release(doc);
        return;
    }
    code = bfetch_status(doc);
    /* The URL AFTER redirects is the base for every relative reference on the
     * page. Resolving against the typed URL instead is how a redirected page
     * ends up asking the wrong origin for its own stylesheets. */
    { const char *f = bfetch_url(doc); int i = 0;
      while (f[i] && i < (int)sizeof base - 1) { base[i] = f[i]; i++; } base[i] = 0; }
    bfetch_set_base(base);
    blen = bfetch_take(doc, &g_page_src);
    /* A DOWNLOAD is a body that goes to the disk instead of the parser. It is
     * decided here and not earlier because "is this a page" is a property of
     * the response, and this is the first point at which the response exists. */
    if (code / 100 == 2 && blen > 0 && download_is_downloadable(base)) {
        int d = download_record(base, g_page_src, blen);
        const struct download *rec = download_at(d);
        char st[96]; int p = 0;
        const char *pre = rec && rec->ok ? "downloaded to " : "download FAILED: ";
        while (*pre) st[p++] = *pre++;
        for (const char *s = rec ? rec->path : "?"; *s && p < 92; s++) st[p++] = *s;
        st[p] = 0;
        set_status(st);
        printf("[browser] %s (%d bytes)\n", st, blen);
        free(g_page_src); g_page_src = 0;
        /* Stay on the page that linked it, exactly as a real browser does. */
        { struct tab *t = tab_cur(); if (t && t->url[0]) {
            int i = 0; while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
            url[i] = 0; ulen = i; } }
        return;
    }
    }
    { struct tab *t = tab_cur(); if (t) {
        int i = 0; while (base[i] && i < TAB_URL - 1) { t->base[i] = base[i]; i++; } t->base[i] = 0; } }
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
    /* HYDRATING: the tab kept the FULL author stylesheet (inline + every
     * external sheet, concatenated exactly as assembled below), so the whole
     * stylesheet phase -- discovery, fetch, concatenation -- is replaced by a
     * copy and costs no connection at all. */
    if (ht && ht->css && ht->csslen > 0) {
        css_len = ht->csslen < (int)sizeof author_css - 1 ? ht->csslen : (int)sizeof author_css - 1;
        for (int i = 0; i < css_len; i++) author_css[i] = ht->css[i];
        author_css[css_len] = 0;
    }
    css_exlen = css_expand_vars(author_css, css_len, css_expanded, (int)sizeof css_expanded);
    css_apply(g_root, css_expanded, css_exlen);
    css_extra_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, win_w);
    ph = layout_height();
    set_status(ht ? "restoring tab..." : "loaded -- fetching stylesheets...");
    redraw(0);                       /* first paint: HTML + inline CSS, before slow CDN fetches */

    /* ---- external stylesheets, all at once, no budget ---- */
    int css2 = css_len, got_sheets = 0, nsheets = 0;
    if (!ht) {
    res_reset();
    collect_css_links(g_root);
    nsheets = g_nres;
    res_fetch_all("stylesheets", 0);
    for (int i = 0; i < g_nres; i++) {
        struct resent *e = &g_res[i];
        if (!e->data || e->len <= 0) continue;
        got_sheets++;
        for (int k = 0; k < e->len && css2 < (int)sizeof author_css - 1; k++)
            author_css[css2++] = (char)e->data[k];
        if (css2 < (int)sizeof author_css - 1) author_css[css2++] = '\n';
    }
    res_reset();
    /* Retain ONE copy of the finished stylesheet, not one per sheet: this is
     * the byte count a background tab actually costs for its CSS. */
    author_css[css2 < (int)sizeof author_css ? css2 : (int)sizeof author_css - 1] = 0;
    tab_keep_css(tab_cur(), author_css, css2);
    }
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
        layout_page(g_root, win_w);
        ph = layout_height();
        { extern size_t malloc_peak; printf("[browser] heap peak %uK\n", (unsigned)(malloc_peak / 1024)); }
        redraw(0);                   /* re-paint with the page's real stylesheets */
    }
    /* Images ride the same pooled connections now, so eight of them from one
     * host is one handshake rather than eight. That is why this number can go
     * up without the load time going with it. */
    /* QUEUE THEM ALL FIRST, then decode.
     *
     * layout_load_images() fetches one image, decodes it, and only then asks
     * for the next -- and a decode is seconds on an emulated CPU while a CDN's
     * keep-alive timeout is often five. The pool therefore handed back sockets
     * the server had already closed: 8 hits, 5 of them dead, 7 handshakes for
     * 8 images from one host. Queueing every image up front means they all
     * transfer inside one window with no decode in between, and res_fetch()
     * then serves each one out of the prefetch cache.
     *
     * They go through the resource TABLE rather than bfetch_prefetch now, for
     * one reason: the table is where a resource's bytes can be recorded on the
     * tab. Same requests, same pooled connections, same "queue everything
     * before decoding anything" -- res_fetch_all serves whatever the tab
     * already holds and fetches only the rest, and bfetch_cache_put then hands
     * every body to layout's own res_fetch() so nothing is fetched twice. */
    g_prog_what = "images"; g_prog_total = 0; g_prog_last = 0;
    res_reset();
    { int n = layout_count();
      const struct item *it = layout_items();
      for (int i = 0; i < n && g_nres < 16; i++) {
          if (it[i].type != IT_IMAGE || it[i].img || !it[i].imgsrc) continue;
          int dup = 0;
          for (int k = 0; k < g_nres; k++)
              if (g_res[k].ref && str_eq(g_res[k].ref, it[i].imgsrc)) { dup = 1; break; }
          if (!dup) res_add(it[i].node, it[i].imgsrc, 0);
      } }
    if (g_nres > 0) {
        res_fetch_all("images", 1);
        for (int i = 0; i < g_nres; i++)
            if (g_res[i].data && g_res[i].len > 0 && g_res[i].url)
                bfetch_cache_put(g_res[i].url, g_res[i].data, g_res[i].len);
    }
    res_reset();
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
    res_fetch_all("scripts", 1);
    g_prog_what = "running scripts"; g_prog_total = 0; g_prog_last = 0;
    int had_script = run_collected_scripts(base) > 0;
    { int dials = 0, reuses = 0, reqs = 0, mods = 0, modfail = 0;
      int hits = 0, evicted = 0, closed = 0;
      bfetch_stats(&dials, &reuses, &reqs);
      bfetch_pool_stats(&hits, &evicted, &closed);
      js_module_stats(&mods, &modfail);
      printf("[browser] load done: %d requests, %d connections dialled, %d reused"
             ", %d modules loaded (%d failed)\n", reqs, dials, reuses, mods, modfail);
      printf("[browser] pool: %d hits, %d evicted, %d closed\n",
             hits, evicted, closed);
      /* The claim tabs have to keep: a switch replays, it does not reload.
       * `from tab` counting the whole resource set and `dialled 0` are the two
       * halves of it, and they are printed on every load so a regression shows
       * up in the serial log of any test that loads a page twice. */
      printf("[browser] resources: %d from tab, %d from network (tab %d of %d)\n",
             g_res_from_tab, g_res_from_net, tabs_active(), tabs_count()); }
    res_reset();

    /* The tab keeps the DOCUMENT bytes. Handing over ownership rather than
     * copying: g_page_src is re-made from the tab's copy on the next hydrate,
     * so there is exactly one master copy per tab at any moment. */
    if (!ht) {
        tab_keep_src(tab_cur(), g_page_src, blen);
        g_page_src = 0;
    }

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

    /* The tab is now what it will look like in the strip and in the history
     * list. Both are done HERE and not in load(): a redirect chain calls
     * load_once once per hop, and only the hop that actually rendered should
     * name the tab or leave a history entry. */
    { struct tab *t = tab_cur();
      if (t) {
          t->loaded = 1;
          t->ph = ph;
          int i = 0; while (base[i] && i < TAB_URL - 1) { t->url[i] = base[i]; i++; }
          t->url[i] = 0;
          tab_retitle();
          if (!ht) {                       /* a replay is not a new visit */
              history_add(t->url, t->title, (unsigned)(monotonic_ms() / 1000));
              history_save();
          }
          if (ht) {                        /* restore where the user had scrolled to */
              int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
              scroll = t->scroll > maxs ? maxs : t->scroll;
              sync_scroll();
          }
          session_save();
      } }
}

/* ======================= tabs: dehydrate / hydrate =========================
 *
 * The two halves of a tab switch. See tabs.h for why they exist at all: the
 * engine is a singleton, so a switch is not "show the other document", it is
 * "put this one away and take that one out".
 *
 * WHAT DEHYDRATION KEEPS is decided in load_once, not here -- the tab already
 * holds its document bytes, its finished stylesheet and every sub-resource by
 * the time it goes into the background. This function's whole job is to record
 * the two things that only exist while the tab is live (where the user had
 * scrolled to, and how tall the page turned out) and then to let go of
 * everything derived. */
static void tab_dehydrate(void)
{
    struct tab *t = tab_cur();
    if (t) {
        t->scroll = scroll;
        t->ph = ph;
        int i = 0; while (url[i] && i < TAB_URL - 1) { t->url[i] = url[i]; i++; }
        t->url[i] = 0;
    }
    /* Same teardown order as a navigation, and for the same reason: the runtime
     * holds {node, serial} handles into the DOM, so it dies first. */
    js_page_close();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    res_reset();
    free(g_page_src); g_page_src = 0;
    ph = 0; scroll = 0;
    g_scroll_pushed = 0;
}

/* Bring the active tab back to the screen. Returns 1 if it rendered from its
 * own bytes (no network), 0 if it has none and needs a real load. */
static int tab_hydrate(void)
{
    struct tab *t = tab_cur();
    if (!t) return 0;
    int i = 0; while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
    url[i] = 0; ulen = i;
    if (!t->src || t->srclen <= 0) return 0;
    scroll = t->scroll;
    g_hydrating = 1;
    load_once(url);
    g_hydrating = 0;
    return 1;
}

/* Switch to tab `i`. The one function every caller uses -- the strip, the
 * keyboard and session restore -- so there is exactly one order of operations
 * for "put one document away and bring another out". */
static void tab_switch_to(int i)
{
    if (i == tabs_active() || !tab_at(i)) return;
    tab_dehydrate();
    tabs_select(i);
    if (!tab_hydrate()) {
        struct tab *t = tab_cur();
        if (t && t->url[0]) load(url);     /* restored-but-never-loaded: fetch it */
        else { set_status("new tab -- type a URL and press Enter"); redraw(1); }
    }
    session_save();
}

/* Seams for the host tab test, alongside browser_load. Nothing in the app calls
 * them -- the app reaches the same code through the strip and the keyboard --
 * but a switch is the operation the whole design rests on, and testing it
 * through a synthesised mouse click would be testing the hit test. */
void browser_tab_switch(int i);
void browser_tab_switch(int i) { tab_switch_to(i); }
/* One full repaint, for the test that counts what the tab strip costs. */
void browser_redraw_now(void);
void browser_redraw_now(void) { redraw(0); }
int  browser_view_h(void);
int  browser_view_h(void) { return VIEW_H; }
/* How many resources the last load took from the tab's own bytes, and how many
 * from the network. The proof that a switch replays. */
void browser_res_split(int *from_tab, int *from_net);
void browser_res_split(int *from_tab, int *from_net)
{ if (from_tab) *from_tab = g_res_from_tab; if (from_net) *from_net = g_res_from_net; }

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

/* ============================== the tab strip ==============================
 *
 * Hand-drawn over gui_*, and that is a decision with a reason rather than an
 * omission. c/apps/gui/aui.c -- the toolkit, which does have tabs, hover states
 * and keyboard focus -- is NOT in browser.aex's link (BROWSER_PIPE in the
 * Makefile), and putting it there costs two things this line should not spend:
 * a hunk in the Makefile, which has been clobbered three times this week, and a
 * toolkit dependency in the host loader test, whose window is five drawing
 * recorders. The browser's own chrome (the glass bar, the URL field) is already
 * drawn this way, so the strip matches what is beside it. When the browser does
 * link aui, aui_tabs/aui_text_ellipsis/aui_icon_button replace this block and
 * the geometry functions below stay as they are.
 *
 * HOVER IS DELIBERATELY ABSENT. SYS_GUI_FLUSH carries no rectangle, so any
 * repaint costs the whole window canvas -- 24-27 ms at 1920x1200. A tab strip
 * that highlights under the pointer demands one of those per pointer sample,
 * which is the difference between a window that feels alive and one that feels
 * slow. The close button therefore lives on the ACTIVE tab only, where its
 * presence is stable and costs nothing to keep drawn. */
#define TAB_MINW  84
#define TAB_MAXW  200
#define TAB_GAP   4
#define TAB_PLUSW 26
#define TAB_LEFT  6

static int g_tab_first;                  /* first tab shown, when they overflow */

static int tab_slot_w(int nvis)
{
    if (nvis < 1) nvis = 1;
    int avail = win_w - TAB_LEFT * 2 - TAB_PLUSW - TAB_GAP;
    int w = (avail - (nvis - 1) * TAB_GAP) / nvis;
    if (w > TAB_MAXW) w = TAB_MAXW;
    if (w < TAB_MINW) w = TAB_MINW;
    return w;
}

/* How many tabs fit at the minimum width. At least one, always: a window
 * narrow enough to fit no tab still has to show the one you are looking at. */
static int tab_visible_max(void)
{
    int avail = win_w - TAB_LEFT * 2 - TAB_PLUSW - TAB_GAP;
    int n = (avail + TAB_GAP) / (TAB_MINW + TAB_GAP);
    return n < 1 ? 1 : n;
}

/* The used-slot indices, in strip order. Returns the count. */
static int tab_order(int *out)
{
    int n = 0;
    for (int i = 0; i < TAB_MAX; i++) if (tab_at(i)) out[n++] = i;
    return n;
}

/* Keep the active tab inside the visible window after any change to the set. */
static void tab_scroll_into_view(void)
{
    int ord[TAB_MAX], n = tab_order(ord), vis = tab_visible_max();
    int pos = 0;
    for (int i = 0; i < n; i++) if (ord[i] == tabs_active()) { pos = i; break; }
    if (g_tab_first > n - vis) g_tab_first = n - vis;
    if (g_tab_first < 0) g_tab_first = 0;
    if (pos < g_tab_first) g_tab_first = pos;
    if (pos >= g_tab_first + vis) g_tab_first = pos - vis + 1;
}

/* Truncate `s` to `maxpx` at the 8-px-per-character estimate the address bar
 * already uses, appending an ellipsis. Deliberately not text_measure_px: this
 * file links host-side against a window that has no font, and a tab label that
 * is a few pixels wide of ideal is not worth the divergence. */
static void tab_label(const char *s, int maxpx, char *out, int max)
{
    int budget = maxpx / 8;
    if (budget > max - 1) budget = max - 1;
    if (budget < 1) budget = 1;
    int n = 0; while (s[n]) n++;
    if (n <= budget) { int i = 0; for (; s[i]; i++) out[i] = s[i]; out[i] = 0; return; }
    int keep = budget - 1; if (keep < 1) keep = 1;
    for (int i = 0; i < keep; i++) out[i] = s[i];
    out[keep] = '~';                       /* one byte, and the font has it */
    out[keep + 1] = 0;
}

static void draw_tab_strip(void)
{
    int ord[TAB_MAX], n = tab_order(ord);
    int vis = tab_visible_max();
    if (vis > n) vis = n;
    int w = tab_slot_w(vis);
    tab_scroll_into_view();

    gui_glass(0, 0, win_w, TABH, 1, 255, 255, 255, 60);
    int x = TAB_LEFT;
    for (int k = g_tab_first; k < n && k < g_tab_first + vis; k++) {
        int idx = ord[k];
        struct tab *t = tab_at(idx);
        int on = (idx == tabs_active());
        gui_glass(x, 4, w, TABH - 6, 7, 255, 255, 255, on ? 210 : 80);
        /* Two pixels of accent under the active tab. The glass alone does not
         * carry enough contrast over a light page to say WHICH tab you are on,
         * and "which tab am I on" is the one question a tab strip exists to
         * answer. It costs one rect and it is not animated, so it is free at
         * the only rate that matters -- repaints, of which it causes none. */
        if (on) gui_rect(x + 6, TABH - 4, w - 12, 2, rgb(90, 150, 240));
        char lab[64];
        tab_label(t->title[0] ? t->title : "New Tab", w - (on ? 32 : 16), lab, (int)sizeof lab);
        gui_text(x + 8, 8, on ? rgb(25, 25, 35) : rgb(105, 105, 118), lab);
        if (on && w >= TAB_MINW) {
            /* the close box, on the active tab only -- see the note above */
            gui_glass(x + w - 20, 9, 14, 14, 7, 255, 255, 255, 140);
            gui_text(x + w - 16, 8, rgb(90, 90, 100), "x");
        }
        x += w + TAB_GAP;
    }
    /* new tab */
    gui_glass(x, 4, TAB_PLUSW, TABH - 6, 7, 255, 255, 255, 90);
    gui_text(x + 9, 8, rgb(80, 80, 92), "+");
}

/* The hit test, sharing the geometry above so the two cannot drift. Returns the
 * tab index, or -1; -2 means the "+" button. `*close` is set when the point
 * landed on the active tab's close box. */
static int tab_strip_hit(int mx, int my, int *close)
{
    if (close) *close = 0;
    if (my < 0 || my >= TABH) return -1;
    int ord[TAB_MAX], n = tab_order(ord);
    int vis = tab_visible_max();
    if (vis > n) vis = n;
    int w = tab_slot_w(vis), x = TAB_LEFT;
    for (int k = g_tab_first; k < n && k < g_tab_first + vis; k++) {
        if (mx >= x && mx < x + w) {
            if (close && ord[k] == tabs_active() && mx >= x + w - 22) *close = 1;
            return ord[k];
        }
        x += w + TAB_GAP;
    }
    if (mx >= x && mx < x + TAB_PLUSW) return -2;
    return -1;
}

/* ============================ the library panel ============================
 * History, bookmarks and downloads are LISTS, and a list needs somewhere to be.
 * One overlay serves all three because they are the same shape (a title, a URL,
 * a row you can activate) and three panels would be three sets of scrolling and
 * selection bugs. */
enum { PANEL_NONE = 0, PANEL_HISTORY, PANEL_BOOKMARKS, PANEL_DOWNLOADS };
static int  g_panel, g_panel_sel, g_panel_top;
static char g_find[64];
static int  g_findlen;

#define PANEL_ROW 22

static int panel_rows(void) { int r = (VIEW_H - 46) / PANEL_ROW; return r < 1 ? 1 : r; }

/* The rows the panel is currently showing, as indices into the underlying list.
 * History filters through the search box; the other two do not (a bookmark list
 * you can search is a nice-to-have, a history you cannot search is not a
 * history). Returns the count. */
static int panel_list(int *out, int max)
{
    if (g_panel == PANEL_HISTORY) return history_search(g_find, out, max);
    int n = g_panel == PANEL_BOOKMARKS ? bookmark_count()
          : g_panel == PANEL_DOWNLOADS ? download_count() : 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = i;
    return n;
}

static void panel_row_text(int which, int idx, char *url_out, char *title_out)
{
    url_out[0] = title_out[0] = 0;
    if (which == PANEL_DOWNLOADS) {
        const struct download *d = download_at(idx);
        if (!d) return;
        int i = 0; for (; d->path[i] && i < TAB_TITLE - 1; i++) title_out[i] = d->path[i];
        title_out[i] = 0;
        i = 0; for (; d->url[i] && i < TAB_URL - 1; i++) url_out[i] = d->url[i];
        url_out[i] = 0;
        return;
    }
    const struct hist_entry *e = which == PANEL_BOOKMARKS ? bookmark_at(idx) : history_at(idx);
    if (!e) return;
    int i = 0; for (; e->title[i] && i < TAB_TITLE - 1; i++) title_out[i] = e->title[i];
    title_out[i] = 0;
    i = 0; for (; e->url[i] && i < TAB_URL - 1; i++) url_out[i] = e->url[i];
    url_out[i] = 0;
}

static void draw_panel(void)
{
    int px = 40, pw = win_w - 80;
    if (pw < 240) { px = 4; pw = win_w - 8; }
    int py = VIEW_Y + 8, phh = VIEW_H - 16;
    gui_glass(px, py, pw, phh, 12, 255, 255, 255, 235);
    const char *name = g_panel == PANEL_HISTORY ? "History"
                     : g_panel == PANEL_BOOKMARKS ? "Bookmarks" : "Downloads";
    gui_text(px + 14, py + 8, rgb(30, 30, 40), name);
    if (g_panel == PANEL_HISTORY) {
        gui_glass(px + 110, py + 6, pw - 130, 20, 6, 255, 255, 255, 200);
        gui_text(px + 116, py + 8, rgb(60, 60, 72), g_findlen ? g_find : "type to search");
        gui_rect(px + 116 + g_findlen * 8, py + 8, 8, 16, rgb(90, 150, 240));
    }
    int rows[HISTORY_MAX];
    int n = panel_list(rows, HISTORY_MAX);
    int vis = panel_rows();
    if (g_panel_sel >= n) g_panel_sel = n - 1;
    if (g_panel_sel < 0) g_panel_sel = 0;
    if (g_panel_sel < g_panel_top) g_panel_top = g_panel_sel;
    if (g_panel_sel >= g_panel_top + vis) g_panel_top = g_panel_sel - vis + 1;
    if (g_panel_top < 0) g_panel_top = 0;
    int y = py + 36;
    for (int i = g_panel_top; i < n && i < g_panel_top + vis; i++) {
        char u[TAB_URL], ti[TAB_TITLE];
        panel_row_text(g_panel, rows[i], u, ti);
        if (i == g_panel_sel) gui_glass(px + 8, y - 2, pw - 16, PANEL_ROW, 5, 120, 170, 255, 120);
        char lab[80];
        tab_label(ti[0] ? ti : u, (pw / 2) - 24, lab, (int)sizeof lab);
        gui_text(px + 14, y, rgb(25, 25, 35), lab);
        tab_label(u, (pw / 2) - 24, lab, (int)sizeof lab);
        gui_text(px + pw / 2, y, rgb(120, 120, 132), lab);
        y += PANEL_ROW;
    }
    if (n == 0)
        gui_text(px + 14, py + 40, rgb(140, 140, 150),
                 g_panel == PANEL_HISTORY ? "nothing matches" : "nothing here yet");
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    draw_tab_strip();
    /* Liquid Glass address bar + a glass URL field */
    gui_glass(0, TABH, win_w, BARH, 1, 255, 255, 255, 70);
    gui_glass(10, TABH + 5, win_w - 20, 20, 8, 255, 255, 255, 95);
    gui_text(14, TABH + 7, rgb(40, 40, 48), url);
    if (editing) gui_rect(14 + ulen * 8, TABH + 7, 8, 16, rgb(90, 150, 240));
    /* a star for "this page is bookmarked", right-aligned in the field */
    if (bookmark_find(url) >= 0) gui_text(win_w - 26, TABH + 7, rgb(240, 180, 60), "*");
    /* the page */
    browser_paint(0, VIEW_Y, win_w, VIEW_H, scroll);
    if (g_panel) draw_panel();
    /* glass status line (frosts the bottom of the page) */
    gui_glass(0, win_h - 18, win_w, 18, 1, 255, 255, 255, 70);
    gui_text(10, win_h - 16, rgb(110, 110, 120), status);
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

/* ============================ keyboard shortcuts ===========================
 *
 * ONE TABLE, because the window-management line owns the system shortcut table
 * and this one has to be handed over rather than rewritten. What the ABI
 * already settles (include/abi/logit_abi.h, EV_MOD_SUPER): the WM intercepts a
 * CLOSED list -- Cmd+W/Q/M/Tab/` -- before the focused app sees it, and
 * forwards every other Cmd combination to the app with EV_MOD_SUPER set.
 *
 * So the browser gets Cmd+T, Cmd+1..9, Cmd+D, Cmd+Y, Cmd+B and cannot have
 * Cmd+W or Cmd+Tab. Closing a TAB is therefore Ctrl+W and cycling tabs is
 * Ctrl+Tab -- Ctrl and not Cmd precisely because Cmd+W already means "close the
 * WINDOW" and a browser where the same chord sometimes closes a tab and
 * sometimes the window is worse than one where they are different keys.
 *
 * The Ctrl fallbacks also mean every one of these works TODAY: the keyboard
 * driver reports EV_MOD_SUPER, but wm.c's interception list is this round's
 * work on another line, and an app that only listened for Cmd would do nothing
 * at all until that lands. Ctrl+letter arrives as a CONTROL CODE (the driver
 * folds it: Ctrl+T is 0x14), which is why the table carries both forms. */
#define CTRL_OF(c)  ((c) - 'a' + 1)

/* Returns 1 if the key was a chrome shortcut and has been handled. */
static int is_cmd(const struct logit_event *e) { return (e->mods & EV_MOD_SUPER) != 0; }

void app_main(void)
{
    css_init();             /* build the UA default stylesheet */
    win_query_size();
    css_viewport(win_w, win_h);  /* @media/vw/vh evaluate against the real window */
    /* css_extra patches node->style after the cascade, so a scoped re-style has
     * to run it before it decides whether anything changed -- see css.h. */
    css_set_post_pass(css_extra_apply);
    img_init();             /* register PNG + GIF decoders */
    js_page_set_clock(clock_ms);
    gui_create("Browser", WINW, WINH);
    win_set_min();
    win_query_size();

    /* ---- the session, before the first paint ----
     *
     * Order matters: the store has to exist before anything reads it, and the
     * tab list has to exist before the strip is drawn. A restored tab comes
     * back with its URL and title and NO bytes, so restoring eight tabs costs
     * eight strings, not eight page loads -- only the one you are looking at
     * loads, and the others load when you first select them. */
    int want_restore = 1;
#ifndef LOADERHOST_LOGIT_H
    tabs_set_store(&os_store);
    /* WHETHER to restore is a PREFERENCE, and preferences belong in the
     * machine's settings store (SYS_SETTING_*, c/kernel/core/settings.c), which
     * says in its own header that another line should use it rather than build
     * a second one. So this line does.
     *
     * WHAT to restore does not go there, and the reason is a measurement, not a
     * preference: SET_VALLEN is 80 bytes and SET_MAXKV is 64 keys for the whole
     * machine. A URL is up to 600 bytes and a session is up to twelve of them,
     * with a 256-entry history beside it -- one tab would not fit in one value,
     * and the history alone would exhaust the machine's entire key budget. The
     * bulk therefore lives in /browser/*, which is what /etc/settings.conf is
     * too: a file. The store holds the switch; the files hold the data. */
    want_restore = setting_int("browser.restore_session", 1);
#endif
    tabs_init();
    history_load();
    bookmarks_load();
    int restored = want_restore ? session_restore() : 0;
    /* On the serial console, because a tab strip's labels are too small to read
     * out of a screendump and "did the session come back" needs an answer that
     * a CI harness can grep for. */
    printf("[browser] session restored %d tabs (restore=%d)\n", restored, want_restore);
    if (restored <= 0) tabs_new(url);
    { struct tab *t = tab_cur();
      if (t && t->url[0]) { int i = 0;
          while (t->url[i] && i < (int)sizeof url - 1) { url[i] = t->url[i]; i++; }
          url[i] = 0; ulen = i; } }
    if (restored > 0) {
        char st[96]; int p = 0; const char *pre = "restored ";
        while (*pre) st[p++] = *pre++;
        num_append(st, &p, restored);
        const char *post = " tabs -- Enter loads this one";
        while (*post) st[p++] = *post++;
        st[p] = 0;
        set_status(st);
    }

    redraw(1);
    int editing = 1;
    struct node *press_node = 0;      /* the element the last mousedown landed on */
    uint32_t press_serial = 0;

    for (;;) {
        struct logit_event e;
        int need = 0;                 /* coalesce: drain the whole event burst, repaint once */
        int navigated = 0;
        while (!navigated && poll_event(&e)) {
            sync_scroll();
            if (e.type == EV_CLOSE) {
                /* Record where the user was BEFORE tearing anything down: the
                 * whole value of a session is that it survives the thing that
                 * ended it. */
                { struct tab *t = tab_cur(); if (t) t->scroll = scroll; }
                session_save(); history_save(); bookmarks_save();
                js_page_close(); bfetch_close_all(); app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                /* NOT ADVISORY: the canvas behind the window has already been
                 * reallocated and the compositor is showing a stretched copy of
                 * the old one until we paint. Re-layout at the new width, since
                 * a web page's line breaking is a function of it. */
                if (e.a > 100 && e.b > 100) { win_w = e.a; win_h = e.b; }
                css_viewport(win_w, win_h);
                if (g_root) {
                    css_apply(g_root, css_expanded, css_exlen);
                    css_extra_apply(g_root, css_expanded, css_exlen);
                    layout_page(g_root, win_w);
                    ph = layout_height();
                }
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                if (scroll > maxs) scroll = maxs;
                sync_scroll();
                need = 1;
                continue;
            }
            if (e.type == EV_KEY) {
                int k = e.a;
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;

                /* ---- chrome shortcuts, before anything else can eat them ----
                 * See the table above app_main for why both a Cmd and a Ctrl
                 * form exist. Handled here rather than after the page's keydown
                 * because Cmd+T must open a tab whatever the page thinks. */
                int handled = 0;
                if (is_cmd(&e) || (e.mods & EV_MOD_CTRL)) {
                    int c = k;
                    if (c >= 1 && c <= 26) c = c + 'a' - 1;      /* the folded form */
                    if (c >= 'A' && c <= 'Z') c += 32;
                    if (c == 't') {                              /* new tab */
                        int n = tabs_new("");
                        if (n >= 0) { tab_dehydrate(); tabs_select(n);
                            url[0] = 0; ulen = 0; editing = 1;
                            set_status("new tab -- type a URL and press Enter");
                            session_save(); }
                        else set_status("too many tabs");
                        handled = 1;
                    } else if (c == 'w') {                       /* close tab */
                        int cur = tabs_active();
                        tab_dehydrate();
                        int nx = tabs_close(cur);
                        tabs_select(nx);
                        if (!tab_hydrate()) {
                            struct tab *t = tab_cur();
                            if (t && t->url[0]) { load(url); navigated = 1; }
                            else { url[0] = 0; ulen = 0; editing = 1;
                                   set_status("new tab -- type a URL and press Enter"); }
                        }
                        session_save();
                        handled = 1;
                    } else if (k == '\t') {                      /* cycle tabs */
                        /* The position is computed and THEN switched to, rather
                         * than using tabs_next(): that call moves the active
                         * index on its own, which would leave the old document
                         * live in the engine with a different tab selected --
                         * every switch has to go through tab_switch_to. */
                        int ord[TAB_MAX], n = tab_order(ord);
                        if (n > 1) {
                            int pos = 0;
                            for (int i = 0; i < n; i++) if (ord[i] == tabs_active()) { pos = i; break; }
                            pos += (e.mods & EV_MOD_SHIFT) ? -1 : 1;
                            if (pos < 0) pos = n - 1;
                            if (pos >= n) pos = 0;
                            tab_switch_to(ord[pos]);
                            editing = 0; navigated = 1;
                        }
                        handled = 1;
                    } else if (c >= '1' && c <= '9') {           /* nth tab */
                        int ord[TAB_MAX], n = tab_order(ord), want = c - '1';
                        if (c == '9') want = n - 1;              /* Cmd+9 = last, as everywhere */
                        if (want >= 0 && want < n && ord[want] != tabs_active()) {
                            tab_switch_to(ord[want]);
                            editing = 0; navigated = 1;
                        }
                        handled = 1;
                    } else if (c == 'd') {                       /* bookmark this page */
                        struct tab *t = tab_cur();
                        int at = bookmark_find(url);
                        if (at >= 0) { bookmark_remove(at); set_status("bookmark removed"); }
                        else { bookmark_add(url, t ? t->title : url); set_status("bookmarked"); }
                        bookmarks_save();
                        handled = 1;
                    } else if (c == 'y' || c == 'h') {           /* history */
                        g_panel = g_panel == PANEL_HISTORY ? PANEL_NONE : PANEL_HISTORY;
                        g_panel_sel = g_panel_top = 0; g_findlen = 0; g_find[0] = 0;
                        handled = 1;
                    } else if (c == 'b') {                       /* bookmarks */
                        g_panel = g_panel == PANEL_BOOKMARKS ? PANEL_NONE : PANEL_BOOKMARKS;
                        g_panel_sel = g_panel_top = 0;
                        handled = 1;
                    } else if (c == 'j') {                       /* downloads */
                        g_panel = g_panel == PANEL_DOWNLOADS ? PANEL_NONE : PANEL_DOWNLOADS;
                        g_panel_sel = g_panel_top = 0;
                        handled = 1;
                    } else if (c == 'l') { editing = 1; handled = 1; }   /* focus the bar */
                }
                if (handled) { need = 1; continue; }

                /* ---- the library panel owns the keyboard while it is open ---- */
                if (g_panel) {
                    int rows[HISTORY_MAX];
                    int n = panel_list(rows, HISTORY_MAX);
                    if (k == 0x1b) { g_panel = PANEL_NONE; }
                    else if (k == KEY_DOWN) g_panel_sel++;
                    else if (k == KEY_UP)   g_panel_sel--;
                    else if (k == KEY_PGDN) g_panel_sel += panel_rows();
                    else if (k == KEY_PGUP) g_panel_sel -= panel_rows();
                    else if (k == '\n') {
                        if (g_panel_sel >= 0 && g_panel_sel < n) {
                            char u[TAB_URL], ti[TAB_TITLE];
                            panel_row_text(g_panel, rows[g_panel_sel], u, ti);
                            if (u[0]) {
                                g_panel = PANEL_NONE; editing = 0;
                                int i = 0; while (u[i] && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
                                url[i] = 0; ulen = i;
                                hist_push(url); load(url); navigated = 1;
                            }
                        }
                    } else if (k == '\b') {
                        if (g_panel == PANEL_HISTORY && g_findlen > 0) g_find[--g_findlen] = 0;
                    } else if (g_panel == PANEL_HISTORY && k >= ' ' && k < 0x7f &&
                               g_findlen < (int)sizeof g_find - 1) {
                        g_find[g_findlen++] = (char)k; g_find[g_findlen] = 0;
                        g_panel_sel = g_panel_top = 0;
                    }
                    if (g_panel_sel < 0) g_panel_sel = 0;
                    need = 1;
                    continue;
                }
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
                sync_scroll();
                need = 1;
            } else if (e.type == EV_MOUSE || e.type == EV_MOUSE_R) {
                int mx = e.a, my = e.b;              /* window-local */
                if (my < TABH) {                     /* the tab strip */
                    int close = 0;
                    int hit = tab_strip_hit(mx, my, &close);
                    press_node = 0;
                    if (hit == -2) {                             /* the + button */
                        int n = tabs_new("");
                        if (n >= 0) { tab_dehydrate(); tabs_select(n);
                            url[0] = 0; ulen = 0; editing = 1;
                            set_status("new tab -- type a URL and press Enter");
                            session_save(); }
                    } else if (hit >= 0 && close) {
                        int cur = tabs_active();
                        tab_dehydrate();
                        tabs_select(tabs_close(cur));
                        if (!tab_hydrate()) {
                            struct tab *t = tab_cur();
                            if (t && t->url[0]) { load(url); navigated = 1; }
                            else { url[0] = 0; ulen = 0; editing = 1;
                                   set_status("new tab -- type a URL and press Enter"); }
                        }
                        session_save();
                    } else if (hit >= 0 && hit != tabs_active()) {
                        tab_switch_to(hit);
                        editing = 0; navigated = 1;
                    }
                    need = 1;
                }
                else if (my < VIEW_Y) { editing = 1; press_node = 0; }   /* click the bar to edit */
                else if (g_panel && my < VIEW_Y + VIEW_H) {
                    /* The panel is modal over the viewport: a click in it picks
                     * a row, and a click outside it dismisses. Routing it to the
                     * page underneath would hit-test a document the user cannot
                     * even see. */
                    int px = 40, pw = win_w - 80;
                    if (pw < 240) { px = 4; pw = win_w - 8; }
                    int py = VIEW_Y + 8;
                    if (mx < px || mx >= px + pw) g_panel = PANEL_NONE;
                    else {
                        int row = (my - (py + 36 - 2)) / PANEL_ROW;
                        int rows[HISTORY_MAX];
                        int n = panel_list(rows, HISTORY_MAX);
                        int sel = g_panel_top + row;
                        if (row >= 0 && sel < n) {
                            g_panel_sel = sel;
                            char u[TAB_URL], ti[TAB_TITLE];
                            panel_row_text(g_panel, rows[sel], u, ti);
                            if (u[0] && g_panel != PANEL_DOWNLOADS) {
                                g_panel = PANEL_NONE; editing = 0;
                                int i = 0; while (u[i] && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
                                url[i] = 0; ulen = i;
                                hist_push(url); load(url); navigated = 1;
                            }
                        }
                    }
                    need = 1;
                }
                else if (my >= VIEW_Y && my < VIEW_Y + VIEW_H) {
                    editing = 0;
                    struct node *n = 0;
                    browser_hittest_node(mx, my - VIEW_Y, scroll, &n, 0, 0);
                    press_node = n;
                    press_serial = n ? n->serial : 0;
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - VIEW_Y;
                    ji.button = dom_button(e.button);
                    ji.buttons = 1 << ji.button;
                    mods_of(&e, &ji);
                    js_dom_dispatch(n, e.type == EV_MOUSE_R ? "contextmenu" : "mousedown", &ji);
                    need = 1;
                }
            } else if (e.type == EV_MOUSE_UP) {
                int mx = e.a, my = e.b;
                if (my >= VIEW_Y && my < VIEW_Y + VIEW_H) {
                    struct node *n = 0;
                    char href[512]; href[0] = 0;
                    browser_hittest_node(mx, my - VIEW_Y, scroll, &n, href, sizeof href);
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
                    ji.client_x = mx; ji.client_y = my - VIEW_Y;
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
                if (js_dom_listener_count() > 0 && e.b >= VIEW_Y && e.b < VIEW_Y + VIEW_H) {
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1;
                    ji.client_x = e.a; ji.client_y = e.b - VIEW_Y;
                    mods_of(&e, &ji);
                    struct node *n = 0;
                    browser_hittest_node(e.a, e.b - VIEW_Y, scroll, &n, 0, 0);
                    js_dom_dispatch(n, "mousemove", &ji);
                }
            } else if (e.type == EV_WHEEL) {
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                int allow = 1;
                if (e.b >= VIEW_Y && e.b < VIEW_Y + VIEW_H) {
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.client_x = e.a; ji.client_y = e.b - VIEW_Y;
                    ji.detail = e.wheel;
                    ji.delta_y = (double)e.wheel * 40.0;
                    mods_of(&e, &ji);
                    struct node *n = 0;
                    browser_hittest_node(e.a, e.b - VIEW_Y, scroll, &n, 0, 0);
                    allow = js_dom_dispatch(n, "wheel", &ji);
                }
                if (allow) scroll += e.wheel * 40;
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                sync_scroll();
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

        /* A navigation the LIVE page asked for -- a click handler setting
         * location.href, a timer calling location.replace, a router. Taken
         * here, at the top of the loop, because this is the first point after
         * the callback returned at which tearing the document down is safe.
         *
         * This one PUSHES history: a page that moves seconds or minutes after
         * it loaded is acting on the user, and Back must come back here. The
         * redirect chain inside load() replaces instead -- see hist_replace. */
        if (!navigated) {
            char want[600];
            if (take_script_nav(want, sizeof want)) {
                editing = 0;
                int i = 0;
                while (want[i] && i < (int)sizeof url - 1) { url[i] = want[i]; i++; }
                url[i] = 0; ulen = i;
                hist_push(url);
                load(url);
                navigated = 1; need = 1;
            }
        }

        if (need) redraw(editing);    /* one repaint after the burst, not per keystroke */
        sys_yield();
    }
}
