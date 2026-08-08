#ifndef LOGIT_BROWSER_TABS_H
#define LOGIT_BROWSER_TABS_H

#include <stddef.h>

/* ============================================================================
 * tabs -- per-tab state for a browser whose render engine is a SINGLETON.
 *
 * THE CONSTRAINT THAT DECIDES THE WHOLE DESIGN, said first because everything
 * below follows from it and nothing below makes sense without it:
 *
 *     layout.c, css_engine.c, js_page.c and js_dom.c each hold their state in
 *     file-scope globals. There is one display list, one LibCSS selection
 *     context, one JSRuntime and one wrapper table IN THE PROCESS.
 *
 * So "two live documents" is not a memory question that a big enough heap
 * answers. It is a structural one: a second live document would need a second
 * instance of four modules that cannot currently have one. (Those four files
 * belong to other lines this round -- css_engine.c and layout.c to the CSS
 * line, js_page.c to the JS line -- so instancing them is not a change this
 * line gets to make, and hacking around it from the outside would mean two
 * documents scribbling on one set of globals, which is worse than no tabs.)
 *
 * Hence the model: EXACTLY ONE TAB IS LIVE. The live tab owns the engine --
 * DOM, cascade, display list, JS runtime, timers, listeners. Every other tab
 * is DEHYDRATED: it keeps the BYTES it was built from and nothing that was
 * built from them.
 *
 *     live tab       DOM + cascade + display list + JSRuntime  (megabytes)
 *     background tab document bytes + stylesheet bytes         (kilobytes)
 *
 * THE MEMORY NUMBER THAT DEFENDS IT. A QuickJS runtime for a real page's
 * bundle was measured at 12.76 MB of live heap for 3.2 MB of source, and a
 * single real bundle at 5.6 MB. Eight tabs each keeping a runtime alive is
 * ~100 MB against a 96 MiB browser arena -- it does not fit, so "keep every
 * runtime" was never actually on the table. Eight tabs keeping their bytes is
 * the sum of eight documents plus eight stylesheet sets: tens to a few hundred
 * kilobytes each. tabs_retained_bytes() reports the real figure rather than
 * this estimate, and the tab test prints it for N = 1, 2, 4, 8.
 *
 * WHAT DEHYDRATION IS NOT. It is not "close the tab and re-open it later":
 * re-hydrating touches NO NETWORK. The document and every stylesheet are
 * replayed from the tab's own bytes, so switching tabs cannot multiply TLS
 * handshakes -- which is the property the connection-pool work bought and the
 * one a naive tab implementation would have thrown away first. What IS lost on
 * a switch is JS heap state: a background tab's timers stop and its variables
 * go. That is the honest cost, it is what tab discarding costs in a real
 * browser too, and it is stated here rather than discovered.
 * ========================================================================== */

#define TAB_MAX      12          /* tab strip stops being legible well before this */
#define TAB_URL      600         /* browser.c's url[] size; the same everywhere */
#define TAB_TITLE    96
#define TAB_HIST_MAX 64          /* per-tab back/forward entries */

/* A resource this tab was built from, kept so a re-hydrate needs no network.
 * `url` is the ABSOLUTE url after redirects, which is what bfetch keys on. */
struct tabres {
    char          *url;
    unsigned char *data;
    int            len;
};

struct tab {
    int  used;
    char url[TAB_URL];           /* what the address bar shows */
    char base[TAB_URL];          /* the url AFTER redirects: the resolution base */
    char title[TAB_TITLE];
    int  scroll;                 /* pixel scroll offset, preserved across switches */
    int  ph;                     /* laid-out page height (clamps scroll on return) */
    int  loaded;                 /* has ever completed a load */

    /* Per-tab session history. A ring of malloc'd strings rather than the old
     * global char[32][600] -- that array was 19 KB, and twelve of them would
     * have been 230 KB of .bss for a feature whose whole point is to be cheap
     * per tab. hcur indexes it; htop is the newest reachable entry (forward
     * beyond it was truncated by a navigation). */
    char *hist[TAB_HIST_MAX];
    int   hcur, htop;

    /* The dehydrated document: the bytes, not the tree. */
    unsigned char *src;
    int            srclen;
    /* The concatenated author CSS (inline <style> + every external sheet that
     * arrived), exactly as browser.c assembled it. Kept so re-hydration does
     * not re-fetch a single stylesheet. */
    char          *css;
    int            csslen;
    /* Sub-resources (images) so a re-hydrate paints the same pixels without
     * dialling anything. Owned. */
    struct tabres *res;
    int            nres, cres;
};

/* ---------------------------------------------------------------- lifetime */
void  tabs_init(void);
int   tabs_count(void);          /* how many slots are in use */
int   tabs_active(void);         /* index of the live tab, -1 if none */
struct tab *tab_at(int i);       /* NULL if out of range or unused */
struct tab *tab_cur(void);

/* Open a tab. `url` may be NULL for a blank one. Returns its index, or -1 when
 * the strip is full. Does NOT make it active. */
int   tabs_new(const char *url);
/* Close tab `i`, freeing everything it retained. Returns the index that should
 * become active (never -1: closing the last tab opens a fresh blank one, which
 * is what every browser does and what stops the window becoming unusable). */
int   tabs_close(int i);
/* Make `i` the active tab. Returns 1 if the active tab changed. */
int   tabs_select(int i);
int   tabs_next(int dir);        /* cycle; returns the new active index */

/* --------------------------------------------------------------- retention */
/* Hand the tab the bytes it must keep to be re-hydratable. Each takes
 * ownership of the pointer (or copies, for the const forms). Passing NULL
 * clears. */
void  tab_keep_src(struct tab *t, unsigned char *src, int len);
void  tab_keep_css(struct tab *t, const char *css, int len);
void  tab_keep_res(struct tab *t, const char *url, const unsigned char *d, int len);
/* The bytes this tab kept for `url` (absolute), or NULL. */
const struct tabres *tab_res_find(const struct tab *t, const char *url);
void  tab_drop_content(struct tab *t);      /* free src/css/res, keep identity */

/* Total bytes this module is holding on behalf of every tab. THE number for
 * "what does another tab cost", and exact rather than sampled: it is the sum of
 * the allocations tabs.c itself made. */
size_t tabs_retained_bytes(void);
size_t tab_retained_bytes(const struct tab *t);

/* ----------------------------------------------------------------- history */
void  tab_hist_push(struct tab *t, const char *u);
void  tab_hist_replace(struct tab *t, const char *u);
/* -1 back / +1 forward. On success copies the entry into `out` and returns 1. */
int   tab_hist_go(struct tab *t, int delta, char *out, int max);
int   tab_hist_can(const struct tab *t, int delta);

/* =========================== persistence ==================================
 *
 * The store is an INTERFACE, not a syscall. Two reasons, and the second is the
 * one that matters: the host loader test drives the real browser.c with no
 * kernel under it, and a settings/persistence line is building a config store
 * this round -- when it lands, pointing the browser at it is one call to
 * tabs_set_store() rather than an edit to every save path.
 *
 * Semantics are deliberately the ones write_file/read_file already have:
 * whole-file, replace-on-write. `read` returns the byte count or < 0; `write`
 * returns 0 on success. */
struct bstore_ops {
    int (*read)(const char *path, void *buf, int max);
    int (*write)(const char *path, const void *buf, int len);
    int (*mkdir)(const char *path);
};
void tabs_set_store(const struct bstore_ops *ops);

/* Where the browser's own state lives. A directory rather than four files at
 * the root, so Finder shows one thing and not four. */
#define BROWSER_DIR        "/browser"
#define SESSION_PATH       "/browser/session"
#define HISTORY_PATH       "/browser/history"
#define BOOKMARKS_PATH     "/browser/bookmarks"
#define DOWNLOAD_DIR       "/downloads"

/* Write the open tabs (url, title, scroll, active index). Returns 0 on success.
 * Called after every navigation and every tab open/close -- a session that is
 * only written at exit is a session that a crash loses, and a crash is exactly
 * when you wanted it. */
int  session_save(void);
/* Re-open the tabs the last session had. Returns how many were restored (0 if
 * there was no session). The tabs come back DEHYDRATED AND EMPTY: a restored
 * tab has its URL, title and scroll, and loads when it is first selected. That
 * is the only shape that can restore eight tabs without eight page loads. */
int  session_restore(void);

/* ------------------------------------------------------- history (browsable)
 * Distinct from the per-tab back/forward stack: this is the list you can open,
 * scroll and search. Newest first, de-duplicated on URL. */
struct hist_entry { char url[TAB_URL]; char title[TAB_TITLE]; unsigned when; };

#define HISTORY_MAX 256
void  history_load(void);
void  history_add(const char *url, const char *title, unsigned when);
int   history_count(void);
const struct hist_entry *history_at(int i);
/* Case-insensitive substring match over url AND title. Fills `out` with up to
 * `max` indices, newest first; returns how many. */
int   history_search(const char *q, int *out, int max);
int   history_save(void);
void  history_clear(void);

/* ---------------------------------------------------------------- bookmarks */
#define BOOKMARK_MAX 128
void  bookmarks_load(void);
int   bookmark_add(const char *url, const char *title);   /* index, or -1 */
int   bookmark_find(const char *url);                     /* index, or -1 */
int   bookmark_remove(int i);
int   bookmark_count(void);
const struct hist_entry *bookmark_at(int i);
int   bookmarks_save(void);

/* ---------------------------------------------------------------- downloads
 * A download is a fetch whose bytes go to the disk instead of the parser. The
 * fetching is the browser's (bfetch); this module owns the naming, the record
 * and the write, so the same code is testable on the host. */
struct download { char url[TAB_URL]; char path[128]; int len; int ok; };
#define DOWNLOAD_MAX 32
/* Derive a filename from a URL ("/a/b/c.png?x=1" -> "c.png"), never empty,
 * never containing '/'. Returns the length written. */
int  download_name(const char *url, char *out, int max);
/* Record + write `len` bytes to DOWNLOAD_DIR/<name>. Returns the record index
 * or -1. The record is kept even when the write fails, with ok = 0: a download
 * that silently did not land is the failure mode worth seeing. */
int  download_record(const char *url, const unsigned char *data, int len);
int  download_count(void);
const struct download *download_at(int i);

/* True when the URL names something to save rather than render. Extension
 * based, because our HTTP client hands the loader a body and a status and the
 * loader is what decides; a Content-Type hook can replace this without any
 * caller changing. */
int  download_is_downloadable(const char *url);

#endif /* LOGIT_BROWSER_TABS_H */
