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
/* THE NEGATIVE CONTROL, and it costs one #define because the control IS the
 * behaviour that shipped yesterday. -DBROWSER_NO_FOCUS compiles the routing
 * out: no element takes focus from a click, Tab does not move it, and a
 * keystroke goes to <body> exactly as it did before this change. The device
 * test (tests/qmp/qmp_forms.py --expect-no-focus) must FAIL against that build,
 * and if it does not then it is not measuring the focus model. */
#ifdef BROWSER_NO_FOCUS
#define FOCUS_ROUTING 0
#else
#define FOCUS_ROUTING 1
#endif

#include "forms.h"               /* form control state + submission */
#include "focus.h"               /* the focused element and Tab navigation */

/* js_forms.c's teardown: it holds the page's JSContext for the editing-event
 * dispatcher, and that context dies with the page. Weak because that
 * translation unit is absent from BROWSER_PIPE and from the host loader test,
 * where there is nothing to clean up. */
void js_forms_cleanup(void) __attribute__((__weak__));

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
/* The open <select>'s list. Drawn last, over everything, because the display
 * list has no z-order above itself -- see the popup section further down. */
static void draw_select_popup(void);
/* Dismiss it. Called from the two teardown paths as well, which run long before
 * the popup section itself. */
static void popup_close(void);

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

/* case-insensitive PREFIX test, for URL schemes. has_ci() is a substring
 * search, and using it to ask "is this a javascript:/data: URL" silently
 * dropped any http(s) URL that merely CONTAINED the word -- e.g. a script src
 * with `?fallback=data:...` in its query string vanished with no line in the
 * log. A scheme is a prefix; test it as one. */
static int starts_ci(const char *h, const char *pre)
{
    if (!h) return 0;
    for (; *pre; h++, pre++) {
        int ca = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
        int cb = (*pre >= 'A' && *pre <= 'Z') ? *pre + 32 : *pre;
        if (ca != cb) return 0;
    }
    return 1;
}

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

/* The smallest window the browser will accept. Used by win_set_min() (which
 * tells the WM) and by pick_born_size() (which clamps to it), so it is defined
 * before both rather than repeated in either. */
#define WIN_MIN_W 480
#define WIN_MIN_H 320

static void win_set_min(void)
{
#if !defined(LOADERHOST_LOGIT_H) && defined(SYS_GUI_WIN_MIN)
    /* Below this the tab strip cannot show a tab AND its close button, and the
     * address bar cannot show a URL. A floor is the honest answer to "lay out
     * at any width" -- the layout is fluid down to here and refuses below it. */
    _sys(SYS_GUI_WIN_MIN, ((long)WIN_MIN_W << 16) | WIN_MIN_H, 0, 0);
#endif
}

/* ---- how big the window is BORN --------------------------------------------
 *
 * WINW/WINH used to be the answer, full stop: 1180x620, whatever the display.
 * On the 1280x800 test screen that fills most of it and looks deliberate; on
 * 2560x1600 it is a small box in a corner, which is what "the browser window is
 * too small" was about.
 *
 * TWO RULES, in this order, and the order is the design decision:
 *
 *   1. WHAT THE USER CHOSE WINS. If they have resized the window before, that
 *      size is what they meant, and no proportion of the screen is a better
 *      guess than an explicit one. This is the whole reason a machine has a
 *      settings store -- and the store is the machine's, not a second one built
 *      here: two small integers are precisely what SET_VALLEN's 80 bytes and
 *      SET_MAXKV's 64 keys are FOR, which is the same measurement that said the
 *      tab session could not live there.
 *   2. OTHERWISE, PROPORTIONAL. A first run should use the display it is on.
 *      88% of the width and 80% of the height leaves the menu bar and the dock
 *      visible without this app having to model either -- it does not place its
 *      own window, the WM does, so it only picks a size and leaves room.
 *
 * Clamped both ways: never below the floor win_set_min() enforces (a smaller
 * window is one the WM will refuse anyway), and never so large that the frame
 * cannot fit on the desktop. On 1280x800 this yields 1126x640, which is within
 * a few percent of the old constants -- so nothing about the existing screens
 * or screenshots moves, and 2560x1600 gets 2252x1280 instead of a corner box.
 */
static void pick_born_size(void)
{
#ifndef LOADERHOST_LOGIT_H
    int sw = screen_w(), sh = screen_h();
    if (sw < WIN_MIN_W || sh < WIN_MIN_H) return;    /* no usable answer: keep WINW/WINH */
    int w = setting_int("browser.win.w", 0);
    int h = setting_int("browser.win.h", 0);
    if (w < WIN_MIN_W || h < WIN_MIN_H) {            /* nothing remembered */
        w = sw * 88 / 100;
        h = sh * 80 / 100;
    }
    if (w > sw - 40)  w = sw - 40;                   /* leave the frame somewhere to be */
    if (h > sh - 120) h = sh - 120;                  /* menu bar + title bar + dock */
    if (w < WIN_MIN_W) w = WIN_MIN_W;
    if (h < WIN_MIN_H) h = WIN_MIN_H;
    win_w = w; win_h = h;
#endif
}

/* Remember a size the user chose. RAM only (commit = 0): a resize drag produces
 * a stream of sizes and each commit is a whole-file LogitFS write, so the
 * setting is updated on every one and written ONCE, at close. The cost of that
 * choice is bounded and worth naming: a machine that loses power mid-session
 * forgets the window size. It does not forget the tabs -- those are saved
 * eagerly, because losing them is the loss that matters. */
static void remember_size(void)
{
#ifndef LOADERHOST_LOGIT_H
    char b[16];
    int v[2] = { win_w, win_h };
    const char *k[2] = { "browser.win.w", "browser.win.h" };
    for (int i = 0; i < 2; i++) {
        int p = 0, n = v[i] < 0 ? 0 : v[i], d = 1;
        while (n / d >= 10) d *= 10;
        while (d) { b[p++] = (char)('0' + (n / d) % 10); d /= 10; }
        b[p] = 0;
        setting_set(k[i], b, 0);
    }
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
    const char *err;          /* why the fetch failed; bfetch's static strings */
    int   status;             /* HTTP status of a failed fetch, 0 = no response */
};

static struct resent *g_res;
static int g_nres, g_cres;

/* ---------------------------------------------------------------------------
 * LATE IMAGES. Declared here rather than in layout.h on purpose: layout.h is
 * owned by another line this week, and these three are a private contract
 * between this file and layout.c -- the eight other host harnesses that link
 * layout.c neither call them nor need to see them. If the contract outlives
 * the week it belongs in the header.
 *
 *   layout_img_cached()      has this src already been answered (decoded, or
 *                            proven undecodable)? Asked before queueing a
 *                            network fetch, so a URL is requested once a page.
 *   layout_images_pending()  how much new work layout still owes -- the cue to
 *                            run another pass on the next frame.
 *   layout_images_reset()    drop the decoded-image cache. MUST be called on
 *                            navigation and on a tab switch: the cache key is
 *                            the raw src attribute, which means something else
 *                            under a different base URL.
 */
int  layout_img_cached(const char *url);
int  layout_images_pending(void);
void layout_images_reset(void);

/* THE TWO BUDGETS, and where the numbers come from.
 *
 * IMG_LOAD_MAX is the first pass, which happens while the page is still
 * loading and the user is looking at a progress line. It was 16, in two places
 * (this call and the prefetch loop's `g_nres < 16`), and 16 is smaller than
 * six of the nineteen sites in tests/qmp/sites_corpus.tsv: counted from the
 * host inventory in tests/scoreboard/2026-08-16-full/*.json, apple.com serves
 * 98 <img> tags, stripe 35, wikipedia 26, github 24, baidu 20, qq 17. Every
 * one of those lost pictures to the cap alone -- wikipedia's own census line
 * from that run reads `[img] 11/22 decoded`. 128 covers the corpus maximum
 * with headroom and is bounded underneath by the cache's 64 MiB.
 *
 * IMG_FRAME_BUDGET is the per-frame allowance for images discovered AFTER
 * load. It is RES_INFLIGHT, i.e. one round of pooled requests, because that is
 * the largest amount of network a frame can start and finish without the
 * window going unresponsive -- res_fetch_all() blocks until its batch lands
 * and drops everything but the close button while it does. A page that reveals
 * forty images gets eight a frame for five frames rather than one stall. */
#define IMG_LOAD_MAX     128
#define IMG_FRAME_BUDGET 8

/* 1 when the image pass has work it has not finished -- see settle_frame(). */
static int g_img_owed;

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
    e->err = 0; e->status = 0;
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
                e->status = bfetch_status(e->id);
                e->url = dupstr(bfetch_url(e->id));
                e->len = bfetch_take(e->id, &e->data);
                if (e->len < 0) { e->len = 0; e->data = 0; }
                g_res_from_net++;
                if (keep && e->data && e->len > 0)
                    tab_keep_res(tab_cur(), e->url, e->data, e->len);
            } else {
                /* Keep the reason on the entry: the consumer of a script entry
                 * (run_collected_scripts) must be able to say LOUDLY that a
                 * script the document asked for will not run, and by then the
                 * bfetch slot is long gone. bfetch_error() returns static
                 * strings, so holding the pointer past release is fine; the
                 * final URL is dup'd because it is not. */
                e->err    = bfetch_error(e->id);
                e->status = bfetch_status(e->id);
                e->url    = dupstr(bfetch_url(e->id));
                printf("[browser] fetch failed (status %d) %s: %s\n",
                       e->status, e->ref, e->err);
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

/* ============================ THE IMAGE PASS ==============================
 *
 * ONE function for every image load on the page, first or late, because there
 * used to be one and it ran ONCE -- straight after the first layout -- and
 * anything the page discovered afterwards was never fetched at all:
 *
 *   - an <img> a script inserted (every video card on bilibili.com; they were
 *     empty grey boxes for exactly this reason);
 *   - a lazy image whose real URL sits in data-src until script moves it into
 *     src (layout.c reads data-src as a fallback, but only at the moment the
 *     item is built, so a swap after load was invisible);
 *   - an image revealed by a re-layout;
 *   - and, worst and least obvious, images that HAD loaded: layout_page()
 *     opens with layout_free(), which frees every decoded bitmap, so the first
 *     script mutation stripped the pictures off a page that had them. That one
 *     is fixed in layout.c by the URL-keyed cache -- a re-layout now gets its
 *     pictures back for free -- and this function is what finds the new ones.
 *
 * SHAPE: queue every wanted URL into the resource table FIRST, fetch the batch
 * concurrently over the pooled connections, push the bodies into bfetch's
 * cache, and only then let layout decode. That is the same ordering the first
 * load has used since the pool landed and for the same measured reason (see
 * the long note at the original call site): a decode is seconds on an emulated
 * CPU and a CDN keep-alive is often five, so fetching one image at a time
 * inside the decode loop handed back sockets the server had already closed.
 *
 * `budget` bounds the NEW work: URLs the cache has not already answered.
 * Anything already decoded costs a pointer copy and is not charged. Returns >0
 * if the display list changed and the caller should repaint. */
static int load_late_images(int budget)
{
    if (budget <= 0) return 0;
    int n = layout_count();
    const struct item *items = layout_items();
    if (n <= 0 || !items) return 0;

    res_reset();
    int queued = 0;
    for (int i = 0; i < n && queued < budget; i++) {
        if (items[i].type != IT_IMAGE || items[i].img || !items[i].imgsrc) continue;
        if (layout_img_cached(items[i].imgsrc)) continue;   /* answered already */
        int dup = 0;
        for (int k = 0; k < g_nres; k++)
            if (g_res[k].ref && str_eq(g_res[k].ref, items[i].imgsrc)) { dup = 1; break; }
        if (!dup) { res_add(items[i].node, items[i].imgsrc, 0); queued++; }
    }
    if (queued == 0) { res_reset(); return 0; }

    /* res_fetch_all()'s progress ticker writes the status bar, which on a LATE
     * pass is overwriting whatever the loaded page put there (a console line,
     * an error). Put it back afterwards -- the fetch is a few hundred
     * milliseconds and the message it replaced is the page's, not ours. */
    char keep[sizeof status];
    for (unsigned k = 0; k < sizeof status; k++) keep[k] = status[k];

    g_prog_what = "images"; g_prog_total = 0; g_prog_last = 0;
    res_fetch_all("images", 1);
    for (int i = 0; i < g_nres; i++)
        if (g_res[i].data && g_res[i].len > 0 && g_res[i].url)
            bfetch_cache_put(g_res[i].url, g_res[i].data, g_res[i].len);
    res_reset();
    int got = layout_load_images(budget);
    set_status(keep);
    return got;
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
        if (href && has_ci(rel, "stylesheet") && !starts_ci(href, "data:")) {
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
            /* Scheme filter as a PREFIX, not a substring (see starts_ci), and
             * every drop says so: a <script src> the document asks for and we
             * choose not to fetch must leave a line, because the scoreboard's
             * asked/got gap is exactly the count of silent drops. */
            if (starts_ci(src, "javascript:") || starts_ci(src, "data:"))
                printf("[browser] skipping <script src=\"%.60s\"> (unsupported scheme)\n", src);
            else if (!res_add(n, src, module))
                printf("[browser] script LOST: %.200s: resource table alloc failed\n", src);
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

/* settle_dom() plus the image pass the re-layout it may have done makes
 * necessary. THE FRAME LOOP CALLS THIS, not settle_dom().
 *
 * Two conditions, and neither alone is enough:
 *
 *   - the DOM settled into a new layout, so there may be <img> boxes that did
 *     not exist a frame ago (script inserted one, or moved data-src into src);
 *   - layout still owes work from the last pass, because IMG_FRAME_BUDGET
 *     stopped it. Nothing marks the DOM dirty for that, so without the second
 *     test a page that reveals forty images at once would load eight and then
 *     sit there. layout_images_pending() is what makes the drain terminate:
 *     every URL it counts becomes a cache entry, positive or negative, on the
 *     pass that reaches it.
 *
 * settle_dom() itself is left alone because browser_settle() is its exported
 * form and the host loader harness calls that with no network underneath it. */
static int settle_frame(void)
{
    int changed = settle_dom();
    if (changed) g_img_owed = 1;
    /* THE GATE IS A FLAG, NOT THE SCAN. layout_images_pending() walks the whole
     * display list -- up to MAXITEM entries -- and this function runs on every
     * turn of the event loop, including the idle ones. Asking it unconditionally
     * would put a 16k-entry walk into the hot path to answer "no" almost every
     * time. The flag is set by the two things that can create work (a settled
     * mutation, and a pass that stopped at its budget) and the scan runs only
     * then. */
    if (g_img_owed) {
        if (load_late_images(IMG_FRAME_BUDGET) > 0) { ph = layout_height(); changed = 1; }
        g_img_owed = layout_images_pending() > 0;
    }
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

/* Does a fetched "script" body actually look like an HTML document?
 *
 * The failure this closes, measured on www.2345.com: a <script src> fetch
 * follows a redirect to an HTML page (an error page, a login stub, the site's
 * own homepage), arrives with status 200, and gets handed to JS_Eval -- which
 * reports `SyntaxError: unexpected token '<'` at line 1 UNDER THE PAGE'S OWN
 * URL, i.e. the page is blamed for a CDN's error page. Refuse it before eval,
 * out loud, naming the URL the bytes actually came from.
 *
 * The one '<' opener that IS JavaScript is `<!--`: HTML-like comments are
 * grammar (Annex B.1.1) and 1990s-era scripts really start with them, so that
 * prefix is exempt. `<!DOCTYPE`, `<html`, `<?xml` are not. */
static int body_is_html_not_js(const unsigned char *p, int len)
{
    int i = 0;
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;  /* UTF-8 BOM */
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n' || p[i] == '\f'))
        i++;
    if (i >= len || p[i] != '<') return 0;
    if (len - i >= 4 && p[i+1] == '!' && p[i+2] == '-' && p[i+3] == '-') return 0;
    return 1;
}

static int run_collected_scripts(const char *page_url)
{
    int ran = 0, inline_n = 0, classic_n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_nres; i++) {
            struct resent *e = &g_res[i];
            if (e->module != pass) continue;            /* pass 0 classic, pass 1 module */
            if (!e->data || e->len <= 0) {
                /* An EXTERNAL script with no bytes is a script the page asked
                 * for that will never run -- jQuery on jd.com was this, and the
                 * only trace was seven downstream ReferenceErrors blamed on
                 * other files. Say it once, plainly, with the reason the fetch
                 * recorded. (An inline entry with no bytes is just an empty
                 * <script></script>; nothing was lost.) */
                if (e->ref)
                    printf("[browser] script LOST: %s: %s (status %d)\n",
                           e->url ? e->url : e->ref,
                           e->err ? e->err : "no body", e->status);
                continue;
            }
            if (e->ref && body_is_html_not_js(e->data, e->len)) {
                printf("[browser] script REFUSED (HTML, not JS): %s (status %d, %d bytes)\n",
                       e->url ? e->url : e->ref, e->status, e->len);
                continue;
            }
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
                dom_script_mark_done(e->node);   /* never re-run via DOM insertion */
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
            dom_script_mark_done(e->node);
            ran++;
        }
    }
    return ran;
}

/* ---- dynamically-inserted scripts --------------------------------------
 * A <script> that enters the document by DOM insertion (appendChild etc.)
 * after parse must be prepared and run per HTML5. js_dom.c's insert_run
 * detects that and calls the sink below -- which does NOT run anything on
 * that stack (the inserting script is still executing; recursing the
 * evaluator through a DOM callback is the trap the spec doc names). It
 * ENQUEUES the node; run_pending_inserted_scripts() drains the queue from
 * the per-frame loop, exactly where run_collected_scripts already runs and
 * where res_fetch_all already pumps the network.
 *
 * A src script is fetched (synchronously here, on the frame loop, not the
 * insertion stack -- the same bfetch_sync the module loader uses) and run;
 * an inline script runs from its own text. Insertion order is preserved by
 * the queue being FIFO. dom_script_mark_done stamps each before running so a
 * re-insertion never re-runs it, and so offer_scripts never re-queues one
 * already queued (the stamp is set at run, but the QUEUED set is checked by
 * pointer below to stop a double-enqueue between insertion and drain). */
#define PENDING_MAX 64
static struct node *g_pending[PENDING_MAX];
static int g_pending_n;

static void on_script_inserted(struct node *n)
{
    if (!n || g_pending_n >= PENDING_MAX) {
        if (n) printf("[browser] inserted-script queue full -- dropping one\n");
        return;
    }
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i] == n) return;  /* already queued */
    g_pending[g_pending_n++] = n;
}

/* Run every queued inserted script, in order. Re-entrant by construction: a
 * script run here that inserts another appends to g_pending (via the sink)
 * and this loop, which re-reads g_pending_n each turn, picks it up -- without
 * ever recursing, because the sink only enqueues. Returns how many ran. */
static int run_pending_inserted_scripts(const char *page_url)
{
    int ran = 0, guard = 0;
    while (g_pending_n > 0) {
        if (++guard > 4 * PENDING_MAX) {          /* a script re-inserting forever */
            printf("[browser] inserted-script drain guard tripped -- stopping\n");
            g_pending_n = 0;
            break;
        }
        struct node *n = g_pending[0];
        for (int i = 1; i < g_pending_n; i++) g_pending[i - 1] = g_pending[i];
        g_pending_n--;
        if (dom_script_is_done(n)) continue;
        dom_script_mark_done(n);                  /* stamp BEFORE running: run-once even if it throws */

        int is_module = 0;
        const char *type = dom_attr(n, "type");
        if (type && (has_ci(type, "module"))) is_module = 1;
        const char *src = dom_attr(n, "src");

        unsigned char *data = 0; int len = 0; char urlbuf[600];
        const char *name = page_url;
        if (src && src[0]) {
            /* Fetch on the frame loop, not the insertion stack. bfetch_sync
             * pumps the network the same way the module loader's mod_loader
             * does. */
            int rc = bfetch_resolve(page_url, src, urlbuf, sizeof urlbuf);
            if (rc != 0) { printf("[browser] inserted script: bad src %s\n", src); continue; }
            int fd = bfetch_start(urlbuf);
            if (fd < 0) { printf("[browser] inserted script: cannot fetch %s\n", urlbuf); continue; }
            while (bfetch_state(fd) == BF_PENDING) bfetch_pump();
            if (bfetch_state(fd) == BF_DONE && bfetch_status(fd) / 100 == 2) {
                len = bfetch_take(fd, &data);
                if (len < 0) { len = 0; data = 0; }
                name = urlbuf;
            } else {
                printf("[browser] inserted script LOST: %s: %s (status %d)\n",
                       urlbuf, bfetch_error(fd), bfetch_status(fd));
                bfetch_release(fd);
                continue;
            }
        } else {
            /* Inline: reassemble the child text nodes, exactly as
             * collect_scripts does at parse time. */
            int total = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text) total += c->textlen;
            if (total <= 0) continue;
            data = malloc((size_t)total + 1);
            if (!data) continue;
            int o = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text)
                    for (int i = 0; i < c->textlen; i++) data[o++] = (unsigned char)c->text[i];
            data[o] = 0; len = o;
        }
        if (data && len > 0 && !(src && body_is_html_not_js(data, len))) {
            if (is_module) js_module_eval((const char *)data, len, name);
            else           js_page_eval((const char *)data, len, name);
            ran++;
        }
        free(data);
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

    /* about:text -- print the words the LAST paint put on the screen, and stay
     * where we are. Not a navigation and not a page: it answers a question
     * about the page already loaded, so navigating away to answer it would
     * destroy the thing being asked about.
     *
     * THE ADDRESS BAR IS THE CHANNEL BECAUSE IT IS THE ONE THAT IS PROVEN.
     * This started as a Ctrl+Alt+D chord, which is the obvious shape and which
     * produced no output at all through the QMP harness -- twice, once unpaced
     * and once paced one scancode at a time. Nothing in the kernel explains it
     * (kbd_mods reports EV_MOD_ALT, wm_shortcut only claims SUPER chords), so
     * the failure is somewhere in a path this line cannot observe, and an
     * instrument whose trigger cannot be observed is not an instrument. Typing
     * a URL is what every driver in tests/qmp already does forty times a run.
     * The chord stays wired as well; if it ever starts arriving it costs
     * nothing, and it is the convenient one for a person at the keyboard. */
    /* The address every load() was actually handed, once. Two lines cost
     * nothing and both were missing when they were wanted: `[browser] load
     * done` says a load finished and never said OF WHAT, so a dropped or
     * doubled keystroke in a QMP harness -- the failure this file's own
     * comment at the fetch-failed branch calls "the whole question" -- was
     * only visible when the fetch failed. It is equally the whole question
     * when the fetch SUCCEEDS and the wrong page arrives. */
    printf("[browser] load: %s\n", cur);
    if (cur[0] == 'a' && cur[1] == 'b' && cur[2] == 'o' && cur[3] == 'u' &&
        cur[4] == 't' && cur[5] == ':' && cur[6] == 't' && cur[7] == 'e' &&
        cur[8] == 'x' && cur[9] == 't' && cur[10] == 0) {
        browser_paint_text_dump();
        set_status("painted text dumped to the serial console");
        return;
    }
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
    /* js_forms.c's editing-event dispatcher holds the JS context js_page_close
     * just freed. Left installed, the next keystroke would call into it. Weak
     * so a build without js_forms.o (BROWSER_PIPE, the host loader test) still
     * links -- there is nothing to clean up there. */
    if (js_forms_cleanup) js_forms_cleanup();
    /* Focus and every control's state point INTO the document that is about to
     * be freed. dom.c recycles node slots, so a pointer kept across this line
     * would not merely dangle -- it would silently name a DIFFERENT element in
     * the next document, which is the worse failure. Dropped BEFORE dom_free,
     * on this path and on the tab-switch one, because both free the tree. */
    popup_close();
    focus_reset();
    fc_reset();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    /* The decoded-image cache goes with the document. Its key is the raw
     * src attribute, so under a different base URL the same key names a
     * different picture -- keeping it across a navigation would paint the
     * previous page's images onto this one. layout_free() deliberately does
     * NOT do this: it runs at the top of every layout_page(), and dropping
     * the cache there is precisely the defect being fixed. */
    layout_images_reset();
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
    /* The ONE navigation in this file; everything else bfetch_start()s is a
     * subresource. That distinction is a cookie rule, not a label -- bfetch.h. */
    int doc = bfetch_start_nav(u);
    if (doc < 0) { set_status("load failed: bad URL (need http:// or https://)"); return; }
    bfetch_wait(doc, load_tick);
    if (bfetch_state(doc) != BF_DONE) {
        /* Name the URL as the fetcher saw it: when this fires under a test
         * harness, "which exact string did the address bar hand over" is the
         * whole question (a dropped or doubled keystroke lives right here). */
        const char *why = bfetch_error(doc);
        printf("[browser] page fetch failed: %s (%s)\n", why, bfetch_url(doc));
        /* The REASON, in the one place a person is looking. "could not fetch
         * the page" was the same sentence for a rejected certificate, a name
         * that does not resolve and a network that is down -- three different
         * things to do about it, and the diagnosis that separates them already
         * existed the whole way up from x509 through sock_poll's error byte
         * (see the SOCK_P_ERROR branch in browser_rt.c). It was thrown away
         * here. No snprintf in this TU on purpose -- see build_get's note in
         * browser_rt.c; status[] is 96 and the longest sock_why() sentence is
         * 57, so nothing here can truncate. */
        { char st[sizeof status];
          const char *pre = "load failed: ";
          int p = 0;
          while (pre[p] && p < (int)sizeof st - 1) { st[p] = pre[p]; p++; }
          for (int i = 0; why[i] && p < (int)sizeof st - 1; i++) st[p++] = why[i];
          st[p] = 0;
          set_status(st); }
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
    /* Where forms.c resolves a Selection API position FROM. It normally starts
     * at the caret, and the one call that has no caret to start at is the one
     * that matters: a page placing the caret itself before the user has
     * clicked anything. */
    fc_ce_set_root(g_root);
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
    /* Images ride the same pooled connections, so eight of them from one host
     * is one handshake rather than eight. That is why IMG_LOAD_MAX can go up
     * without the load time going with it. The queue-everything-then-decode
     * ordering and the measurement behind it now live in load_late_images(),
     * which is where they belong: they apply to every pass, not this one.
     *
     * The first pass is load_late_images() too -- ONE image path, so a fix to
     * either the queueing or the budget cannot land on only one of them. The
     * two differ in exactly one number, see IMG_LOAD_MAX. */
    if (load_late_images(IMG_LOAD_MAX) > 0) {
        ph = layout_height();
        redraw(0);
    }
    /* A page with more than IMG_LOAD_MAX images has some left; the frame loop
     * drains them. Set unconditionally rather than from the pass's own count,
     * because the scripts about to run are the other producer. */
    g_img_owed = 1;
    /* Open the page's JS runtime. It stays open until the next navigation --
     * that is the whole point: listeners, timers and pending promises all live
     * past the end of the script that created them. It is opened even when the
     * page has no <script>, so that a later dispatchEvent/on-attribute path has
     * a context to run in. */
    js_page_output_clear();
    js_out_shown = 0;
    js_page_set_location(base);
    js_page_open(g_root);
    g_pending_n = 0;                       /* fresh page, empty inserted-script queue */
    js_dom_set_script_sink(on_script_inserted);
    /* The form/focus JS surface (element.value, .checked, form.submit(),
     * document.activeElement) is installed by js_page_open() itself, alongside
     * every other module's -- NOT from here.
     *
     * It was called from here first, to avoid touching js_page.c at all, and
     * then ALSO from js_page.c so that the WPT runner (which links js_page.c
     * and not browser.c) would see the bindings. Installing it twice into one
     * context aborts the process during page load -- reproduced on the device,
     * bisected to exactly that, and not diagnosed further because one install
     * is the correct shape anyway and matches every other module here. Do not
     * "helpfully" add a second call back. */

    /* Fetch every external script CONCURRENTLY, then run them in spec order.
     * Real sites ship huge minified bundles that assume a full browser env;
     * with no real DOM they just throw -- but they throw ALONE. The runtime's
     * 2 MiB stack guard bounds recursive scripts so a bad bundle raises a
     * catchable RangeError instead of faulting. */
    res_reset();
    collect_scripts(g_root);
    /* The guest's own inventory, printed BEFORE fetching: the scoreboard's
     * asked/got gap compares the host's count of the document's <script src>
     * against requests we issued, and without this line a shortfall cannot be
     * split into "the parser never produced the element" vs "the fetch never
     * happened". One number from each side of that boundary. */
    { int xc = 0, xm = 0, in = 0;
      for (int i = 0; i < g_nres; i++) {
          if (!g_res[i].ref) in++;
          else if (g_res[i].module) xm++;
          else xc++;
      }
      printf("[browser] scripts collected: %d external classic, %d external module, %d inline\n",
             xc, xm, in); }
    res_fetch_all("scripts", 1);
    g_prog_what = "running scripts"; g_prog_total = 0; g_prog_last = 0;
    int had_script = run_collected_scripts(base) > 0;
    /* A parse-time script may have inserted more <script>s (an AMD/loader
     * shim is the common case). Drain them here, on this stack, before the
     * page is declared loaded -- run_pending_inserted_scripts is itself
     * re-entrant, so a chain of loaders resolves fully. */
    if (run_pending_inserted_scripts(base) > 0) had_script = 1;
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

    /* settle_FRAME: a `load` handler that inserts images is the single most
     * common way a real page's pictures arrive after the first image pass, and
     * this is the first point at which they exist. */
    if (settle_frame() || had_script) {
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
    /* js_forms.c's editing-event dispatcher holds the JS context js_page_close
     * just freed. Left installed, the next keystroke would call into it. Weak
     * so a build without js_forms.o (BROWSER_PIPE, the host loader test) still
     * links -- there is nothing to clean up there. */
    if (js_forms_cleanup) js_forms_cleanup();
    /* Focus and every control's state point INTO the document that is about to
     * be freed. dom.c recycles node slots, so a pointer kept across this line
     * would not merely dangle -- it would silently name a DIFFERENT element in
     * the next document, which is the worse failure. Dropped BEFORE dom_free,
     * on this path and on the tab-switch one, because both free the tree. */
    popup_close();
    focus_reset();
    fc_reset();
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    /* The decoded-image cache goes with the document. Its key is the raw
     * src attribute, so under a different base URL the same key names a
     * different picture -- keeping it across a navigation would paint the
     * previous page's images onto this one. layout_free() deliberately does
     * NOT do this: it runs at the top of every layout_page(), and dropping
     * the cache there is precisely the defect being fixed. */
    layout_images_reset();
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
/* What the event loop does after a script has run: take the DOM's invalidation
 * record through the cascade and layout. The seam exists because THAT is the
 * path that laid out at the wrong width, and a test that only resizes cannot
 * reach it. */
int browser_settle(void);
int browser_settle(void) { return settle_dom(); }
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
     * a change on the layout side alone.
     *
     * `win_w`, NOT `WINW`. This was the last call site still laying out at the
     * born-at constant, and it is the worst one to miss: it is the INVALIDATION
     * path, so it does not run on load -- it runs when a script mutates the DOM.
     * A resized window therefore laid out correctly until the page changed
     * anything, and then snapped back to 1180 px and stayed there, because every
     * subsequent mutation did it again. Any real application mutates
     * constantly, so on a resized window this fired immediately and repeatedly
     * and looked like "the sizing adaptation is wrong" rather than like a
     * layout width.
     *
     * A resize handler that re-lays-out is not enough on its own: EVERY path
     * that lays out has to agree about the width, and the one that does not is
     * invisible to any test that resizes without mutating. See the test in
     * tests/unit/loader_test.c part 3 (f), which mutates on purpose. */
    layout_page(g_root, win_w);
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

/* The window changed size.
 *
 * A FUNCTION and not four lines inside the event switch, for one reason: the
 * bug this file just carried was a second layout path that disagreed about the
 * width, and the way to stop that recurring is for there to be one place that
 * answers "the window is now this big" -- reachable by the test, so the test
 * drives the shipped code rather than a copy of it.
 *
 * EV_RESIZE is NOT ADVISORY: the canvas behind the window has already been
 * reallocated when it arrives, and the compositor is showing a STRETCHED copy
 * of the old one until we paint. An app that ignores it does not keep its old
 * layout -- it shows a magnified one for ever.
 *
 * The cascade re-runs before layout because @media, vw and vh are all functions
 * of the viewport: laying out again without re-styling would move the boxes and
 * leave every width:50vw box at its old size. */
void browser_resize(int w, int h);
void browser_resize(int w, int h)
{
    if (w > 100 && h > 100) { win_w = w; win_h = h; remember_size(); }
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
}

/* The contenteditable caret + selection, drawn over the page. Defined with the
 * rest of the editing wiring further down (it needs the display list and the
 * geometry helpers); declared here because redraw() is the only caller. */
static void draw_ce_overlay(void);

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
    draw_ce_overlay();
    draw_select_popup();
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

/* ======================= focus, form controls, submission ==================
 *
 * The piece that was missing. Until this block existed, browser.c line 1860
 * routed every keystroke to <body> with the comment "No focus model yet", and
 * the consequence -- stated the way it should have been stated then -- was that
 * NO WEB PAGE ON THIS MACHINE COULD ACCEPT A SINGLE CHARACTER.
 *
 * Three seams meet here and nowhere else:
 *
 *   focus.c owns WHICH element has the keyboard, and knows nothing about the
 *   window, the scroll or QuickJS.
 *
 *   forms.c owns WHAT a control holds and what a keystroke does to it, and
 *   knows nothing about events beyond a function pointer.
 *
 *   this file owns the WINDOW: it installs that function pointer (so the DOM
 *   events focus.c and forms.c raise are built by whoever owns event
 *   construction), it decides what the default action is when a page does not
 *   preventDefault it, and it is the only place that may navigate.
 */

/* The dispatcher focus.c and forms.c call through. `bubbles`/`cancelable` come
 * from the caller because the spec assigns them per event type and the two
 * files that raise them are the ones that know which type they are raising. */
static int forms_dispatch(struct node *target, const char *type,
                          int bubbles, int cancelable)
{
    struct js_event_init ji = { 0 };
    ji.bubbles = bubbles;
    ji.cancelable = cancelable;
    /* `key` non-NULL is what makes js_dom_dispatch build a KeyboardEvent, and a
     * focus or input event is not one -- so it is deliberately left unset and
     * these arrive on the generic pointer-shaped prototype. The event line owns
     * FocusEvent/InputEvent proper; this is the seam, not the shape. */
    return js_dom_dispatch(target, type, &ji);
}

/* Is `n` inside the current document? Everything below refuses to act on a node
 * that a script has already detached. */
static struct node *doc_root_of(struct node *n)
{
    while (n && n->parent) n = n->parent;
    return n;
}

/* The element a click at `n` should give focus to. A click on the TEXT of a
 * <label> focuses (and toggles) the control the label is for, which is how a
 * very large share of real checkboxes are actually operated -- and a click that
 * lands on a non-focusable element must walk UP, because a <span> inside a
 * <button> is what the hit test returns. */
static struct node *focus_target_for_click(struct node *n, struct node **label_out)
{
    if (label_out) *label_out = 0;
    for (struct node *p = n; p && p->type == N_ELEM; p = p->parent) {
        if (fc_kind(p) != FC_NONE && focus_is_focusable(p)) return p;
        if (p->tag[0] == 'l' && p->tag[1] == 'a' && p->tag[2] == 'b' &&
            p->tag[3] == 'e' && p->tag[4] == 'l' && p->tag[5] == 0) {
            struct node *t = fc_label_target(p);
            if (t && focus_is_focusable(t)) { if (label_out) *label_out = p; return t; }
        }
        if (focus_is_focusable(p)) return p;
    }
    return 0;
}

/* Focus a control and remember its value, so `change` has something to compare
 * against when focus leaves. */
static void focus_control(struct node *n)
{
    struct node *old = focus_current();
    if (old == n) return;
    if (old && fc_kind(old) != FC_NONE) fc_commit(old);
    focus_set(n);
    if (n && fc_kind(n) != FC_NONE) fc_mark_focus(n);
}

/* ---- submission -------------------------------------------------------- */

/* A form submission is a NAVIGATION, and this browser has exactly one of those
 * (load() through bfetch). So a GET form is turned into a URL and handed to the
 * same follow_link() a clicked <a> goes through -- no second network path, and
 * therefore no second set of redirect, cookie and history rules to get wrong.
 *
 * POST IS NOT WIRED, and saying so is the honest answer rather than sending the
 * fields as a query string and calling it done. bfetch (browser_rt.c, another
 * line's file) builds a GET and only a GET; giving it a method and a body is a
 * change to that file, not to this one. What IS here: the payload is built and
 * unit-tested, the `submit` event fires and is cancelable, and a POST form says
 * so in the status bar instead of silently navigating to the wrong URL. */
static char g_submit_buf[8192];

/* ---- the <select> popup ------------------------------------------------ */
/* Drawn by browser.c and not by the painter, because it has to float above
 * everything in the display list and the display list has no z-order above
 * itself. The open list's geometry is recomputed from the control's box each
 * frame, so a scroll or a re-layout can never leave it stranded. */
static struct node *g_popup;             /* the open <select>, or NULL */
static uint32_t     g_popup_serial;
static int          g_popup_hi;          /* highlighted row */

#define POPUP_ROW 22
#define POPUP_MAXROWS 12

static void popup_close(void)
{ if (g_popup) fc_select_set_open(g_popup, 0); g_popup = 0; g_popup_serial = 0; }

static struct node *popup_live(void)
{
    if (!g_popup) return 0;
    if (g_popup->serial != g_popup_serial) { g_popup = 0; return 0; }
    return g_popup;
}

/* The control's border box in DOCUMENT coordinates, from the display list.
 * Returns 0 if the control has no box (display:none, or not laid out yet). */
static int control_box(struct node *n, int *bx, int *by, int *bw, int *bh)
{
    const struct item *it = layout_items();
    int cnt = layout_count();
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_CONTROL || it[i].node != n) continue;
        *bx = it[i].x; *by = it[i].y; *bw = it[i].w; *bh = it[i].h;
        return 1;
    }
    return 0;
}

/* ---- the contenteditable caret, on screen -------------------------------
 *
 * WHY THE GEOMETRY IS HERE AND NOT IN forms.c. A contenteditable's text is not
 * a control's string -- it is ordinary page text, laid out by layout.c into the
 * display list like every other word on the page. So the caret's pixel position
 * is a question about the DISPLAY LIST, and forms.c deliberately holds no
 * layout dependency (that is what keeps eight host test binaries linking). The
 * caret is drawn as an OVERLAY after browser_paint, the same way the <select>
 * popup is and for the same reason: it has to sit above content the display
 * list has no z-order above.
 *
 * The link between the two is one subtraction. layout.c emits one IT_TEXT per
 * word with `text` pointing INTO the text node's own buffer, so
 * `item.text - node->text` is that word's byte offset within the node, and the
 * caret's offset picks out the word it falls in. */
/* browser_rt.c's cached measurer -- the same one layout.c measured the runs
 * with. Declared rather than included for the reason layout.c and forms.c both
 * give: measuring through logit.h's raw text_measure_px would issue a syscall
 * per word and, worse, could disagree with the widths layout already used. */
int text_measure(const char *s, int len, int px, int mono);

static int ce_run_for(struct node *t, int off, const struct item **out, int *rel)
{
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *best = 0;
    int brel = 0;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].node != t || !it[i].text) continue;
        long base = it[i].text - t->text;
        if (base < 0 || base > t->textlen) continue;           /* not this buffer */
        if (off < base || off > base + it[i].len) continue;
        best = &it[i];
        brel = off - (int)base;
        break;                    /* the first run that covers it: a caret at a
                                   * word's end belongs to that word, not to the
                                   * start of the next one */
    }
    if (!best) return 0;
    if (out) *out = best;
    if (rel) *rel = brel;
    return 1;
}

/* The caret rectangle in DOCUMENT coordinates. 0 when there is no caret, or the
 * page it belonged to has been replaced. */
static int ce_caret_box(int *cx, int *cy, int *ch)
{
    struct node *n = 0;
    int off = 0;
    if (!fc_ce_selection(0, 0, &n, &off)) return 0;   /* the FOCUS end blinks */
    if (!fc_ce_host(n)) return 0;

    if (n->type == N_TEXT) {
        const struct item *r = 0;
        int rel = 0;
        if (ce_run_for(n, off, &r, &rel)) {
            *cx = r->x + text_measure(r->text, rel, r->font_px, r->mono);
            *cy = r->y;
            *ch = r->h > 0 ? r->h : r->font_px;
            return 1;
        }
        /* An empty run, or a node laid out nowhere (collapsed whitespace):
         * fall through to the containing element's box. */
        n = n->parent;
    }
    /* An ELEMENT position -- the empty composer. Its box is a plain IT_RECT, so
     * the caret goes at the content's start. Without this the composer nobody
     * has typed into yet shows no caret at all, which is indistinguishable from
     * a click that did not focus anything. */
    for (struct node *e = n; e; e = e->parent) {
        const struct item *it = layout_items();
        int cnt = layout_count();
        for (int i = 0; i < cnt; i++) {
            if (it[i].node != e || it[i].type == IT_TEXT) continue;
            int fh = it[i].font_px > 0 ? it[i].font_px : 16;
            *cx = it[i].x + 2;
            *cy = it[i].y + 2;
            *ch = it[i].h > 4 && it[i].h < fh * 3 ? it[i].h - 4 : fh;
            return 1;
        }
    }
    return 0;
}

/* Caret + selection, over the painted page. The selection is a TINT rather than
 * a filled rect: this runs after the text is on screen, so anything opaque
 * would hide the very characters it is meant to show as selected. */
static void draw_ce_overlay(void)
{
    if (!FOCUS_ROUTING) return;
    const struct item *it = layout_items();
    int cnt = layout_count();
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || !it[i].node || it[i].node->type != N_TEXT) continue;
        int a = 0, b = 0;
        if (!fc_ce_run_range(it[i].node, &a, &b)) continue;
        long base = it[i].text - it[i].node->text;
        int r0 = a - (int)base, r1 = b - (int)base;
        if (r0 < 0) r0 = 0;
        if (r1 > it[i].len) r1 = it[i].len;
        if (r1 <= r0) continue;
        int x0 = it[i].x + text_measure(it[i].text, r0, it[i].font_px, it[i].mono);
        int x1 = it[i].x + text_measure(it[i].text, r1, it[i].font_px, it[i].mono);
        int sy = VIEW_Y + it[i].y - scroll;
        if (sy + it[i].h < VIEW_Y || sy > VIEW_Y + VIEW_H) continue;
        gui_glass(x0, sy, x1 - x0, it[i].h, 0, 90, 150, 240, 110);
    }
    int cx, cy, chh;
    if (!ce_caret_box(&cx, &cy, &chh)) return;
    int sy = VIEW_Y + cy - scroll;
    if (sy + chh < VIEW_Y || sy > VIEW_Y + VIEW_H) return;
    gui_rect(cx, sy, 2, chh, rgb(30, 30, 40));
}

/* Place the caret from a click inside an editing host. `vx`,`vy` are viewport
 * coordinates (the caller has already subtracted VIEW_Y).
 *
 * Aims at the TEXT RUN under the pointer, not at the element the hit test
 * returned: browser_hittest_node() climbs to an element because a DOM event
 * target cannot be a text node, and a caret has to go the other way. Finding no
 * run is the empty composer, and fc_ce_caret_in handles it -- that path is not
 * a fallback, it is the case that matters most. */
static void ce_caret_from_click(struct node *host, int vx, int vy)
{
    int dy = vy + scroll;
    const struct item *it = layout_items();
    int cnt = layout_count();
    const struct item *hit = 0;
    long bestd = -1;
    for (int i = 0; i < cnt; i++) {
        if (it[i].type != IT_TEXT || it[i].hidden) continue;
        if (!it[i].node || it[i].node->type != N_TEXT) continue;
        if (fc_ce_host(it[i].node) != host) continue;
        if (dy < it[i].y || dy >= it[i].y + it[i].h) continue;
        /* On the pointer's LINE. Nearest run horizontally, so a click past the
         * end of a short line still lands on that line's last word instead of
         * missing everything. */
        long d = 0;
        if (vx < it[i].x) d = it[i].x - vx;
        else if (vx > it[i].x + it[i].w) d = vx - (it[i].x + it[i].w);
        if (bestd < 0 || d < bestd) { bestd = d; hit = &it[i]; }
    }
    if (!hit) { fc_ce_caret_in(host, 1); return; }

    /* The byte in the run nearest the pointer. Linear over the run's characters
     * for the reason fc_offset_at_px gives: the measurement is monotone in
     * characters and not in bytes, so a binary search over bytes is a bug
     * waiting for a multi-byte character. */
    int relx = vx - hit->x;
    if (relx < 0) relx = 0;
    int best = 0;
    long bd = -1;
    for (int i = 0; i <= hit->len; ) {
        int w = text_measure(hit->text, i, hit->font_px, hit->mono);
        long d = w > relx ? w - relx : relx - w;
        if (bd < 0 || d < bd) { bd = d; best = i; }
        if (i >= hit->len) break;
        i++;
        while (i < hit->len && ((unsigned char)hit->text[i] & 0xC0) == 0x80) i++;
    }
    long base = hit->text - hit->node->text;
    fc_ce_set_caret(hit->node, (int)base + best);
}

/* Re-style and re-lay-out after an EDIT changed the DOM.
 *
 * Not settle_dom(): that one asks js_dom.c what a SCRIPT invalidated, and an
 * edit made by the keyboard is not a script mutation -- js_dom.c never saw it
 * and would report INVAL_NONE. The scope is the editing host, which is the
 * smallest thing that is certainly enough: Enter creates elements that have no
 * computed style at all yet, and layout.c reads `node->style`. */
static int ce_settle(struct node *host)
{
    if (!g_root) return 0;
    if (host) css_apply_scoped(host, 0, css_expanded, css_exlen);
    else      css_apply(g_root, css_expanded, css_exlen);
    layout_page(g_root, win_w);
    ph = layout_height();
    return 1;
}

/* 1 if the browser navigated (so the caller stops draining events).
 *
 * `fire_event` is 0 only for form.submit(), which the spec defines as NOT
 * firing the submit event -- the difference between it and requestSubmit() is
 * exactly that, and a page that calls submit() from inside its own submit
 * handler would otherwise recurse. */
static int form_submit_ex(struct node *form, struct node *submitter, int fire_event)
{
    if (!form) return 0;
    /* `submit` fires AT THE FORM, bubbles and is cancelable -- and a page that
     * cancels it and does its own fetch() is the single most common shape of
     * form handling on the modern web, so honouring the cancel matters more
     * than the navigation does. */
    if (fire_event && !forms_dispatch(form, "submit", 1, 1)) {
        set_status("submit cancelled by the page");
        return 0;
    }
    int n = fc_encode(form, submitter, g_submit_buf, (int)sizeof g_submit_buf);
    if (n < 0) { set_status("form is too large to submit"); return 0; }

    char target[700];
    int o = 0;
    const char *act = fc_action(form);
    if (act[0]) {
        while (act[o] && o < 640) { target[o] = act[o]; o++; }
    } else {
        /* No action: this page. The existing query and fragment are dropped,
         * which is what the spec's "URL record with the query replaced" means
         * and what a search box on a results page depends on. */
        for (int i = 0; url[i] && url[i] != '?' && url[i] != '#' && o < 640; i++)
            target[o++] = url[i];
    }
    target[o] = 0;

    if (fc_method_post(form)) {
        /* Deliberately loud rather than silently wrong. See the comment above:
         * the payload is built and correct, the network path is not this
         * line's file. */
        set_status("POST form: payload built, but POST is not wired yet");
        printf("[browser] FORM-POST %s body=%s\n", target, g_submit_buf);
        return 0;
    }

    int q = 0;
    while (target[q] && target[q] != '?' && target[q] != '#') q++;
    target[q] = 0;
    if (n > 0 && q < 660) {
        target[q++] = '?';
        for (int i = 0; i < n && q < (int)sizeof target - 1; i++) target[q++] = g_submit_buf[i];
    }
    target[q] = 0;
    printf("[browser] FORM-GET %s\n", target);
    follow_link(target);
    return 1;
}

static int form_submit(struct node *form, struct node *submitter)
{ return form_submit_ex(form, submitter, 1); }

/* The default form for an implicit submission (Enter in a text field), and the
 * form a submit button belongs to. */
static int implicit_submit(struct node *ctl)
{
    struct node *form = fc_form_of(ctl);
    if (!form) return 0;
    return form_submit(form, 0);
}

static void draw_select_popup(void)
{
    struct node *sel = popup_live();
    if (!sel) return;
    int bx, by, bw, bh;
    if (!control_box(sel, &bx, &by, &bw, &bh)) { popup_close(); return; }
    int n = fc_option_count(sel);
    int rows = n > POPUP_MAXROWS ? POPUP_MAXROWS : n;
    if (rows <= 0) { popup_close(); return; }
    int px = bx, py = VIEW_Y + by - scroll + bh;
    int pw = bw < 140 ? 140 : bw;
    int ph2 = rows * POPUP_ROW + 8;
    /* Flip above the control when there is no room below -- a dropdown that
     * runs off the bottom of the window is a dropdown you cannot use. */
    if (py + ph2 > win_h - 18) {
        int above = VIEW_Y + by - scroll - ph2;
        if (above > VIEW_Y) py = above;
    }
    gui_clip(0, VIEW_Y, win_w, VIEW_H);
    gui_rrect(px, py, pw, ph2, 6, rgb(0xB0, 0xB4, 0xBA));
    gui_rrect(px + 1, py + 1, pw - 2, ph2 - 2, 5, rgb(0xFF, 0xFF, 0xFF));
    int cur = fc_selected_index(sel);
    for (int i = 0; i < rows; i++) {
        char lbl[128];
        struct node *o = fc_option_at(sel, i);
        int l = o ? fc_option_label(o, lbl, (int)sizeof lbl) : 0;
        int ry = py + 4 + i * POPUP_ROW;
        if (i == g_popup_hi || (g_popup_hi < 0 && i == cur))
            gui_rrect(px + 3, ry, pw - 6, POPUP_ROW, 4, rgb(0x25, 0x63, 0xEB));
        unsigned col = (i == g_popup_hi) ? rgb(255, 255, 255) : rgb(0x1D, 0x1D, 0x1F);
        gui_text_run(px + 10, ry + 3, 14, 0, col, lbl, l);
    }
    if (n > rows) {
        char more[32];
        int p = 0;
        const char *pre = "...";
        while (*pre) more[p++] = *pre++;
        more[p] = 0;
        gui_text_run(px + 10, py + 4 + rows * POPUP_ROW - 12, 12, 0, rgb(150, 150, 158), more, p);
    }
    gui_clip(0, 0, 0, 0);
}

/* Activate a control the way a click or Space/Enter does. */
static int control_activate(struct node *n, int *navigated)
{
    int k = fc_kind(n);
    if (FC_IS_TOGGLE(k)) {
        if (fc_disabled(n)) return 1;
        if (k == FC_RADIO) fc_set_checked(n, 1);
        else               fc_set_checked(n, !fc_checked(n));
        forms_dispatch(n, "input", 1, 0);
        forms_dispatch(n, "change", 1, 0);
        return 1;
    }
    if (FC_IS_BUTTON(k)) {
        if (fc_disabled(n)) return 1;
        /* The button's own `click` has already been dispatched by the caller
         * for a mouse click; for a keyboard activation it has not, so it is
         * raised here and its cancellation suppresses the submit. */
        if (k == FC_RESET) { fc_reset_form(fc_form_of(n)); return 1; }
        if (k == FC_SUBMIT || k == FC_IMAGEBTN) {
            struct node *form = fc_form_of(n);
            if (form && form_submit(form, n)) { if (navigated) *navigated = 1; }
        }
        return 1;
    }
    if (k == FC_SELECT) {
        if (fc_disabled(n)) return 1;
        if (popup_live() == n) popup_close();
        else { popup_close(); g_popup = n; g_popup_serial = n->serial;
               g_popup_hi = fc_selected_index(n); fc_select_set_open(n, 1); }
        return 1;
    }
    return 0;
}

/* Move the caret one visual line in a <textarea>. Kept here rather than in
 * forms.c because "a line" is a wrapping question and forms.c does not lay
 * anything out -- this is the HARD-BREAK version, which is right for a textarea
 * whose content has explicit newlines and approximate for one relying on soft
 * wrapping. Named as an approximation rather than hidden as one. */
static int textarea_line_move(struct node *n, int down, int extend)
{
    int vl = 0;
    const char *v = fc_value(n, &vl);
    int s0, s1;
    fc_selection(n, &s0, &s1);
    int pos = down ? s1 : s0;
    int ls = pos; while (ls > 0 && v[ls - 1] != '\n') ls--;
    int col = pos - ls;
    int np;
    if (down) {
        int le = pos; while (le < vl && v[le] != '\n') le++;
        if (le >= vl) return 0;
        int ns = le + 1, ne = ns;
        while (ne < vl && v[ne] != '\n') ne++;
        np = ns + col; if (np > ne) np = ne;
    } else {
        if (ls == 0) return 0;
        int pe = ls - 1, ps = pe;
        while (ps > 0 && v[ps - 1] != '\n') ps--;
        np = ps + col; if (np > pe) np = pe;
    }
    if (extend) { int a = s0 == s1 ? pos : (down ? s0 : s1);
                  fc_set_selection(n, np < a ? np : a, np < a ? a : np); }
    else fc_set_selection(n, np, np);
    return 1;
}

/* A keystroke that reached a focused control. Returns 1 if the control
 * consumed it -- in which case the browser's own default action (scrolling,
 * history) must NOT also happen, which is the whole point of a focus model. */
static int control_key(struct node *n, int k, const struct logit_event *ev,
                       int *navigated)
{
    int kind = fc_kind(n);
    if (kind == FC_NONE) return 0;
    int shift = (ev->mods & EV_MOD_SHIFT) != 0;
    int ctrl  = (ev->mods & EV_MOD_CTRL) != 0 || (ev->mods & EV_MOD_SUPER) != 0;

    if (kind == FC_SELECT) {
        int cnt = fc_option_count(n), cur = fc_selected_index(n);
        if (k == KEY_DOWN || k == KEY_UP) {
            int nx = cur + (k == KEY_DOWN ? 1 : -1);
            if (nx < 0) nx = 0;
            if (nx >= cnt) nx = cnt - 1;
            if (nx != cur && nx >= 0) {
                fc_set_selected_index(n, nx);
                g_popup_hi = nx;
                forms_dispatch(n, "input", 1, 0);
                forms_dispatch(n, "change", 1, 0);
            }
            return 1;
        }
        if (k == '\n' || k == ' ') { control_activate(n, navigated); return 1; }
        if (k == 0x1b) { popup_close(); return 1; }
        return 0;
    }

    if (FC_IS_TOGGLE(kind) || FC_IS_BUTTON(kind)) {
        if (k == ' ' || (k == '\n' && FC_IS_BUTTON(kind))) {
            /* A keyboard activation still raises `click`, and a page that
             * preventDefaults it must not get the submit. */
            struct js_event_init ji = { 0 };
            ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
            if (!js_dom_dispatch(n, "click", &ji)) return 1;
            control_activate(n, navigated);
            return 1;
        }
        if (k == '\n' && FC_IS_TOGGLE(kind)) { if (implicit_submit(n)) *navigated = 1; return 1; }
        return 0;
    }

    if (!FC_IS_TEXTUAL(kind)) return 0;

    /* ---- a text field ---- */
    if (k == '\n') {
        if (kind == FC_TEXTAREA) return fc_edit_insert(n, "\n", 1) ? 1 : 1;
        /* Enter in a single-line field COMMITS (fires `change`) and then
         * implicitly submits the form -- which is exactly what a search box
         * is, and the reason this whole line of work exists. */
        fc_commit(n);
        if (implicit_submit(n)) *navigated = 1;
        return 1;
    }
    if (k == '\b') return fc_edit_backspace(n) ? 1 : 1;
    if (k == 0x1b) { focus_control(0); return 1; }
    if (k == KEY_LEFT)  { fc_edit_move(n, -1, ctrl, shift); return 1; }
    if (k == KEY_RIGHT) { fc_edit_move(n, +1, ctrl, shift); return 1; }
    if (k == KEY_HOME)  { fc_edit_home(n, shift); return 1; }
    if (k == KEY_END)   { fc_edit_end(n, shift); return 1; }
    if (k == KEY_UP || k == KEY_DOWN) {
        if (kind != FC_TEXTAREA) return 0;      /* let the page scroll */
        textarea_line_move(n, k == KEY_DOWN, shift);
        return 1;
    }
    /* Ctrl+letter arrives folded to a control code (keyboard.c). The chrome's
     * own shortcut table has already had its turn on these, so only the ones it
     * does not claim reach here. */
    if (k == 0x01) { fc_edit_select_all(n); return 1; }                 /* Ctrl+A */
    if (k == 0x03 || k == 0x18) {                                       /* copy / cut */
        int s0, s1, vl = 0;
        fc_selection(n, &s0, &s1);
        const char *v = fc_value(n, &vl);
        if (s1 > s0) clip_set(CLIP_F_TEXT, v + s0, s1 - s0);
        if (k == 0x18) fc_edit_insert(n, "", 0);
        return 1;
    }
    if (k == 0x16) {                                                    /* Ctrl+V */
        static char pb[4096];
        int got = clip_get(CLIP_F_TEXT, pb, (int)sizeof pb);
        if (got > 0) fc_edit_insert(n, pb, got);
        return 1;
    }
    if (k >= ' ' && k < 0x7f) { char c = (char)k; fc_edit_insert(n, &c, 1); return 1; }
    return 0;
}

/* A keystroke that reached a focused CONTENTEDITABLE. Same contract as
 * control_key: 1 means the editing host consumed it, so the browser's own
 * default (scrolling, history) must not also happen.
 *
 * `*dirty` is set when the DOM changed, because unlike a text field -- whose
 * value is a string forms.c owns -- an edit here moves boxes and the page has
 * to be re-styled and re-laid-out before it can be painted.
 *
 * ENTER IS NOT SPECIAL-CASED HERE and that is the point: a chat composer
 * cancels it in its own keydown handler to send the message, the page's keydown
 * has already had its turn by the time this runs, and the caller only calls
 * this when the page did NOT cancel. So "Enter sends" and "Enter makes a new
 * paragraph" are the same code path with the page choosing. */
static int ce_key(struct node *host, int k, const struct logit_event *ev, int *dirty)
{
    int shift = (ev->mods & EV_MOD_SHIFT) != 0;
    int ctrl  = (ev->mods & EV_MOD_CTRL) != 0 || (ev->mods & EV_MOD_SUPER) != 0;
    if (!host || fc_disabled(host)) return 0;
    /* The caret may never have been placed: focus arrived by Tab, or a script
     * called focus(). Put it at the end of the content, which is where a real
     * browser puts it. */
    if (!fc_ce_selection(0, 0, 0, 0) || fc_ce_caret_host() != host)
        fc_ce_caret_in(host, 1);

    switch (k) {
    case '\n':   if (fc_ce_enter(shift)) *dirty = 1; return 1;
    case '\b':   if (fc_ce_backspace())  *dirty = 1; return 1;
    /* NO KEY REACHES THIS ON THIS MACHINE. The PS/2 driver
     * (c/drivers/char/keyboard.c, a kernel file and another line's) maps the
     * E0 arrows, Home, End and the page keys and does not map E0 0x53, so
     * forward-Delete never arrives. The operation is real and host-tested; the
     * key that would invoke it is one line in a file this line may not edit. */
    case 0x7f:   if (fc_ce_delete())     *dirty = 1; return 1;
    case 0x1b:   focus_control(0); fc_ce_clear(); return 1;
    case KEY_LEFT:  fc_ce_move(-1, ctrl, shift); return 1;
    case KEY_RIGHT: fc_ce_move(+1, ctrl, shift); return 1;
    case KEY_HOME:  fc_ce_home(shift); return 1;
    case KEY_END:   fc_ce_end(shift); return 1;
    case 0x01:      fc_ce_select_all(host); return 1;                  /* Ctrl+A */
    default: break;
    }
    if (k == 0x03 || k == 0x18) {                                      /* copy / cut */
        static char cb[4096];
        int got = fc_ce_selection_text(cb, (int)sizeof cb);
        if (got > 0) clip_set(CLIP_F_TEXT, cb, got);
        if (k == 0x18 && got > 0 && fc_ce_backspace()) *dirty = 1;
        return 1;
    }
    if (k == 0x16) {                                                   /* Ctrl+V */
        static char pb[4096];
        int got = clip_get(CLIP_F_TEXT, pb, (int)sizeof pb);
        if (got > 0 && fc_ce_insert(pb, got)) *dirty = 1;
        return 1;
    }
    /* Up/Down are left to the page: this caret is paragraph-scoped and has no
     * notion of a visual line, so moving by one would be a guess. Saying so is
     * better than guessing wrong -- the page scrolls, which is what an
     * unhandled arrow does. */
    if (k >= ' ' && k < 0x7f) {
        char c = (char)k;
        if (fc_ce_insert(&c, 1)) *dirty = 1;
        return 1;
    }
    return 0;
}

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
    /* focus.c and forms.c raise DOM events through a function pointer rather
     * than including js_dom.h -- they are compiled into BROWSER_PIPE, which has
     * no QuickJS include path. This is where that pointer is installed, and it
     * is the ONLY coupling between the focus/forms model and the script engine:
     * uninstalled (the host tests), focus still moves and typing still works,
     * the page simply does not hear about it. */
    fc_set_dispatch(forms_dispatch);
    /* Submitting is a navigation and this file owns navigation, so forms.c and
     * the JS bindings reach it through a pointer rather than the other way
     * round. */
    fc_set_submit(form_submit_ex);
    /* The size is chosen BEFORE the window exists, and the cascade is told
     * about it again afterwards: css_viewport was called above with the
     * placeholder, and @media/vw/vh would otherwise evaluate against a window
     * that never existed. */
    pick_born_size();
    gui_create("Browser", win_w, win_h);
    win_set_min();
    win_query_size();       /* the WM may have clamped what we asked for */
    css_viewport(win_w, win_h);

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
#ifndef LOADERHOST_LOGIT_H
    /* DIAGNOSTIC PROBE (2026-08-16). What it found, so the next reader does not
     * re-derive it: every GUI app launched by the Dock runs with caps=0x0 --
     * wm_launch's proc_create() never grants a capability (named as `not_done`
     * in the comment above proc.c's `p->caps = 0`), and since the M28 gate
     * landed (24130fcef, c/kernel/exec/syscall.c syscall_dispatch's cap check)
     * every CAP_NET and CAP_FS syscall from a GUI process is refused with -1.
     * That is why this line prints `rodata=-1 stack=-1 bss=-1 caps=0x0` on
     * today's builds, why session restore reads 0 tabs, and why the site
     * scoreboard's self-test fails machine-wide. DELETE this probe when
     * wm_launch grants capabilities and the scoreboard self-test passes again;
     * until then it is the one serial line that names the blocker. Sockets are
     * closed immediately; gateway:9 answers nothing, which is fine --
     * sock_open returning a handle is the whole measurement. */
    { static char bsshost[16] = "10.0.2.2";
      char stackhost[16]; for (int i = 0; i < 9; i++) stackhost[i] = "10.0.2.2"[i];
      int a = sock_open("10.0.2.2", 9, 0);
      int b = sock_open(stackhost, 9, 0);
      int c = sock_open(bsshost, 9, 0);
      long caps = _sys(SYS_CAP_QUERY, 0, 0, 0);
      printf("[browser] sock probe: rodata=%d stack=%d bss=%d caps=0x%x\n",
             a, b, c, (unsigned)caps);
      if (a >= 0) sock_close(a);
      if (b >= 0) sock_close(b);
      if (c >= 0) sock_close(c); }
#endif
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
                /* The window size the user settled on. Set in RAM by every
                 * resize; this is the one write to disk. */
#ifndef LOADERHOST_LOGIT_H
                remember_size(); setting_commit();
#endif
                js_page_close(); bfetch_close_all(); app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                browser_resize(e.a, e.b);
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
                    /* FIRST IN THE CHAIN, and that is not a style choice:
                     * Ctrl+D three branches below bookmarks the page, so a
                     * chord that merely ADDS Alt to it would never be reached.
                     * Ctrl+Alt+D dumps the words that reached the screen to the
                     * serial console -- an instrument rather than a feature,
                     * which is also why it takes a modifier no page and no
                     * hand sends by accident. browser_paint.h says what it
                     * answers that `changed px` cannot. */
                    if (c == 'd' && (e.mods & EV_MOD_ALT)) {
                        browser_paint_text_dump();
                        set_status("painted text dumped to the serial console");
                        handled = 1;
                    } else if (c == 't') {                       /* new tab */
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
                    } else if (c == 'l') {                       /* focus the bar */
                        /* FOCUS AND *SELECT*, which is what Ctrl+L does in
                         * every browser: the next keystroke REPLACES the
                         * address, it does not append to it. There is no
                         * selection model in this bar, and clearing is what
                         * "type over the selection" looks like from the
                         * outside for the only thing anyone does after Ctrl+L.
                         *
                         * It used to only set `editing`, and the bug that hid
                         * behind that is worth naming because it hid well:
                         * tests/qmp/qmp_site.py drives every navigation with
                         * Ctrl+L then the URL, and its FIRST navigation is out
                         * of an empty tab -- so appending and replacing are
                         * the same thing and it worked for a year. The second
                         * navigation in a boot silently produced
                         * `https://site/what-was-typed`. */
                        editing = 1; ulen = 0; url[0] = 0;
                        handled = 1;
                    }
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
                struct node *fnode = 0;
                if (!editing) {
                    char one[2];
                    struct js_event_init ji = { 0 };
                    ji.bubbles = 1; ji.cancelable = 1;
                    ji.key = key_name(k, one);
                    ji.code = ji.key;
                    ji.key_code = (k >= ' ' && k < 0x7f) ? k : k & 0xFF;
                    mods_of(&e, &ji);
                    /* THE KEYSTROKE GOES TO THE FOCUSED ELEMENT.
                     *
                     * This line used to read "No focus model yet, so keys go to
                     * <body>", and that was the entire reason no web page on
                     * this machine could be typed into. The fallback is kept
                     * and is not a compromise: with nothing focused, <body> IS
                     * the DOM's answer for activeElement, so a document-level
                     * listener sees the key bubble past exactly as before. */
                    fnode = FOCUS_ROUTING ? focus_current() : 0;
                    struct node *body = g_root ? dom_doc_body(g_root->doc) : 0;
                    struct node *tgt = fnode ? fnode : (body ? body : js_dom_root());
                    allow = js_dom_dispatch(tgt, "keydown", &ji);
                    /* A keydown handler is entitled to move focus, or to remove
                     * the focused element outright. Re-read rather than trust
                     * the pointer taken three lines ago. */
                    fnode = FOCUS_ROUTING ? focus_current() : 0;
                    if (allow && k >= ' ' && k < 0x7f) {
                        /* keypress: legacy, but a very large amount of real
                         * form code still cancels typing through it. */
                        struct js_event_init jp = ji;
                        allow = js_dom_dispatch(tgt, "keypress", &jp);
                        fnode = FOCUS_ROUTING ? focus_current() : 0;
                    }
                }

                /* Tab moves focus. Before the control's own handling, because a
                 * text field must not eat the key that leaves it, and after the
                 * page's keydown, because a focus trap cancels Tab. */
                if (FOCUS_ROUTING && allow && !editing && k == '\t' && g_root) {
                    popup_close();
                    struct node *cur = focus_current();
                    if (cur && fc_kind(cur) != FC_NONE) fc_commit(cur);
                    if (focus_advance(g_root, (e.mods & EV_MOD_SHIFT) != 0)) {
                        struct node *nf = focus_current();
                        if (nf && fc_kind(nf) != FC_NONE) fc_mark_focus(nf);
                        /* Scroll it into view: a Tab that focuses something off
                         * screen is indistinguishable from a Tab that did
                         * nothing. */
                        int bx, by, bw, bh;
                        if (nf && control_box(nf, &bx, &by, &bw, &bh)) {
                            if (by < scroll + 8) scroll = by - 8;
                            else if (by + bh > scroll + VIEW_H - 8) scroll = by + bh - VIEW_H + 8;
                            if (scroll < 0) scroll = 0;
                            if (scroll > maxs) scroll = maxs;
                            sync_scroll();
                        }
                    }
                    need = 1;
                    allow = 0;
                }

                /* The open <select> owns the keyboard. */
                if (FOCUS_ROUTING && allow && !editing && popup_live()) {
                    struct node *sel = popup_live();
                    int cnt = fc_option_count(sel);
                    if (k == KEY_DOWN) { g_popup_hi++; if (g_popup_hi >= cnt) g_popup_hi = cnt - 1; allow = 0; }
                    else if (k == KEY_UP) { g_popup_hi--; if (g_popup_hi < 0) g_popup_hi = 0; allow = 0; }
                    else if (k == '\n' || k == ' ') {
                        if (g_popup_hi >= 0 && g_popup_hi != fc_selected_index(sel)) {
                            fc_set_selected_index(sel, g_popup_hi);
                            forms_dispatch(sel, "input", 1, 0);
                            forms_dispatch(sel, "change", 1, 0);
                        }
                        popup_close();
                        allow = 0;
                    } else if (k == 0x1b) { popup_close(); allow = 0; }
                    if (!allow) need = 1;
                }

                /* The focused control gets first refusal on everything else. */
                if (FOCUS_ROUTING && allow && !editing && fnode && fc_kind(fnode) != FC_NONE) {
                    if (control_key(fnode, k, &e, &navigated)) {
                        allow = 0;
                        need = 1;
                    }
                }

                /* ...and so does a focused contenteditable, which is not a form
                 * control and therefore never reached the branch above. THIS
                 * WAS THE WHOLE GAP: focus.c has always known a contenteditable
                 * can hold focus, so the keystroke arrived at a focused
                 * composer and fc_kind() answered FC_NONE and it was dropped on
                 * the floor -- silently, with nothing on the serial. */
                if (FOCUS_ROUTING && allow && !editing && fnode) {
                    struct node *ceh = fc_ce_host(fnode);
                    uint32_t ceser = ceh ? ceh->serial : 0;
                    int ce_dirty = 0;
                    if (ceh && ce_key(ceh, k, &e, &ce_dirty)) {
                        allow = 0;
                        need = 1;
                        /* An edit moved boxes: re-style the host (Enter creates
                         * elements with no computed style at all) and re-lay
                         * out before anything is painted.
                         *
                         * THE HOST MAY BE GONE. The `input` event ran the
                         * page's own handler, and a React composer's response
                         * to one is routinely to tear the subtree down and
                         * rebuild it -- so the pointer we came in with can name
                         * a recycled slot by now. The serial says which, and
                         * the fallback is the whole document, which is always
                         * correct and merely slower. */
                        if (ce_dirty) {
                            int live = ceh->serial == ceser &&
                                       doc_root_of(ceh) == g_root;
                            ce_settle(live ? ceh : 0);
                        }
                        /* A script may have reacted to the `input` event. */
                        if (settle_frame()) need = 1;
                    }
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
                    /* An open <select> is modal over the viewport, exactly like
                     * the library panel above: a click in the list picks a row,
                     * a click outside it dismisses, and neither reaches the
                     * page underneath. */
                    struct node *pop = popup_live();
                    if (pop) {
                        int bx, by, bw, bh;
                        if (control_box(pop, &bx, &by, &bw, &bh)) {
                            int n2 = fc_option_count(pop);
                            int rows = n2 > POPUP_MAXROWS ? POPUP_MAXROWS : n2;
                            int px = bx, py = VIEW_Y + by - scroll + bh;
                            int pw = bw < 120 ? 120 : bw;
                            int ph2 = rows * POPUP_ROW + 8;
                            if (mx >= px && mx < px + pw && my >= py && my < py + ph2) {
                                int row = (my - py - 4) / POPUP_ROW;
                                if (row >= 0 && row < rows) {
                                    if (row != fc_selected_index(pop)) {
                                        fc_set_selected_index(pop, row);
                                        forms_dispatch(pop, "input", 1, 0);
                                        forms_dispatch(pop, "change", 1, 0);
                                    }
                                }
                            }
                        }
                        popup_close();
                        press_node = 0;
                        need = 1;
                        continue;
                    }
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
                    int okdown = js_dom_dispatch(n, e.type == EV_MOUSE_R ? "contextmenu" : "mousedown", &ji);
                    /* FOCUS FOLLOWS THE MOUSE DOWN, not the click -- that is
                     * what makes click-and-drag inside a field select text in
                     * every real browser, and what makes preventDefault() on
                     * mousedown the documented way to stop a control taking
                     * focus (every custom dropdown on the web relies on it). */
                    if (FOCUS_ROUTING && okdown && e.type == EV_MOUSE && e.button == EV_BTN_LEFT) {
                        struct node *lbl = 0;
                        struct node *tgt = focus_target_for_click(n, &lbl);
                        focus_control(tgt);
                        if (tgt && FC_IS_TEXTUAL(fc_kind(tgt))) {
                            /* Put the caret where the pointer is. */
                            int bx, by, bw, bh;
                            if (control_box(tgt, &bx, &by, &bw, &bh)) {
                                const struct item *its = layout_items();
                                int cnt = layout_count(), font = 14, mono = 0;
                                for (int i = 0; i < cnt; i++)
                                    if (its[i].type == IT_CONTROL && its[i].node == tgt) {
                                        font = its[i].ctl_font ? its[i].ctl_font : its[i].font_px;
                                        mono = its[i].ctl_mono; break;
                                    }
                                int relx = mx - (bx + FC_BORDER + FC_PAD_X);
                                int off = fc_offset_at_px(tgt, relx, font, mono);
                                fc_set_selection(tgt, off, off);
                            }
                        }
                        /* A click inside an editing host puts the CARET there.
                         * Separate from the control case above and it has to
                         * be: focus_target_for_click walks UP to the nearest
                         * focusable element, which for a composer is the host
                         * -- but the caret belongs at the character under the
                         * pointer, which is a fact about the TEXT NODE the hit
                         * landed in, several levels down. */
                        if (fc_ce_host(tgt)) ce_caret_from_click(tgt, mx, my - VIEW_Y);
                    }
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
                        if (settle_frame()) need = 1;
                        /* THE CONTROL'S DEFAULT ACTION. A checkbox toggles, a
                         * submit button submits, a <select> opens -- and every
                         * one of them is suppressed by preventDefault(), which
                         * is what `go` carries. A click on a <label> activates
                         * the control it labels, which is how most checkboxes
                         * on the web are actually ticked. */
                        if (FOCUS_ROUTING && go) {
                            struct node *lbl = 0;
                            struct node *tgt = focus_target_for_click(n, &lbl);
                            if (tgt && fc_kind(tgt) != FC_NONE) {
                                if (control_activate(tgt, &navigated)) need = 1;
                            }
                        }
                        if (go && !navigated && href[0]) { follow_link(href); navigated = 1; }
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
            if (!navigated && settle_frame()) need = 1;   /* a handler rewrote the DOM */
        }

        /* Due timers + animation frames. `js_page_pending()` is a pointer test,
         * so an idle page does not even read the clock -- the loop is exactly as
         * hot as it was before timers existed. */
        if (!navigated && js_page_pending()) {
            if (js_page_run_due() > 0) {
                /* A timer/rAF callback can inject a <script> too -- drain the
                 * queue on the frame loop, never on the callback's own stack. */
                if (g_pending_n > 0) run_pending_inserted_scripts(url);
                if (settle_frame()) need = 1;
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
