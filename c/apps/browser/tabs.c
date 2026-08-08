/* tabs.c -- the per-tab state model, the session, history, bookmarks and
 * downloads. See tabs.h for WHY exactly one tab is live; this file is the
 * mechanism that follows from it.
 *
 * Nothing here draws, fetches or parses. It holds bytes and it writes files,
 * through a store interface, so the whole of it links into the host loader test
 * with no kernel underneath -- which is how the session-restore claim gets
 * asserted without booting QEMU. */

#include "tabs.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- small utils */

static void sncpy(char *d, const char *s, int max)
{
    int i = 0;
    if (max <= 0) return;
    if (s) for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static char *dupn(const char *s, int n)
{
    char *p = malloc((size_t)n + 1);
    if (!p) return 0;
    if (n > 0) memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

static char *dups(const char *s) { return dupn(s, s ? (int)strlen(s) : 0); }

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* case-insensitive substring */
static int ci_has(const char *h, const char *n)
{
    if (!h || !n || !n[0]) return 1;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- the tab list */

static struct tab g_tab[TAB_MAX];
static int g_active = -1;
static size_t g_retained;              /* running total, maintained at every
                                        * malloc/free below so the answer is a
                                        * read and not a walk */

static void retain(size_t n) { g_retained += n; }
static void release(size_t n) { g_retained = g_retained > n ? g_retained - n : 0; }

void tabs_init(void)
{
    for (int i = 0; i < TAB_MAX; i++) {
        if (g_tab[i].used) { tab_drop_content(&g_tab[i]);
            for (int k = 0; k < TAB_HIST_MAX; k++) { free(g_tab[i].hist[k]); g_tab[i].hist[k] = 0; } }
        memset(&g_tab[i], 0, sizeof g_tab[i]);
        g_tab[i].hcur = -1; g_tab[i].htop = -1;
    }
    g_active = -1;
    g_retained = 0;
}

int tabs_count(void)
{ int n = 0; for (int i = 0; i < TAB_MAX; i++) if (g_tab[i].used) n++; return n; }

int tabs_active(void) { return g_active; }

struct tab *tab_at(int i)
{ return (i >= 0 && i < TAB_MAX && g_tab[i].used) ? &g_tab[i] : 0; }

struct tab *tab_cur(void) { return tab_at(g_active); }

int tabs_new(const char *url)
{
    for (int i = 0; i < TAB_MAX; i++) {
        if (g_tab[i].used) continue;
        memset(&g_tab[i], 0, sizeof g_tab[i]);
        g_tab[i].used = 1;
        g_tab[i].hcur = -1; g_tab[i].htop = -1;
        sncpy(g_tab[i].url, url ? url : "", TAB_URL);
        sncpy(g_tab[i].title, "New Tab", TAB_TITLE);
        if (g_active < 0) g_active = i;
        return i;
    }
    return -1;
}

int tabs_close(int i)
{
    struct tab *t = tab_at(i);
    if (t) {
        tab_drop_content(t);
        for (int k = 0; k < TAB_HIST_MAX; k++) {
            if (t->hist[k]) { release(strlen(t->hist[k]) + 1); free(t->hist[k]); t->hist[k] = 0; }
        }
        memset(t, 0, sizeof *t);
        t->hcur = -1; t->htop = -1;
    }
    if (tabs_count() == 0) {                 /* never leave the window empty */
        int n = tabs_new("");
        g_active = n;
        return n;
    }
    if (g_active == i) {                     /* pick the neighbour to the left */
        int pick = -1;
        for (int k = i - 1; k >= 0; k--) if (g_tab[k].used) { pick = k; break; }
        if (pick < 0) for (int k = i + 1; k < TAB_MAX; k++) if (g_tab[k].used) { pick = k; break; }
        g_active = pick;
    }
    return g_active;
}

int tabs_select(int i)
{
    if (!tab_at(i) || i == g_active) return 0;
    g_active = i;
    return 1;
}

int tabs_next(int dir)
{
    if (tabs_count() == 0) return -1;
    int i = g_active;
    for (int k = 0; k < TAB_MAX; k++) {
        i += dir > 0 ? 1 : -1;
        if (i >= TAB_MAX) i = 0;
        if (i < 0) i = TAB_MAX - 1;
        if (g_tab[i].used) { g_active = i; return i; }
    }
    return g_active;
}

/* ---------------------------------------------------------- what tabs keep */

/* ---------------------------------------------------------------------------
 * THE NEGATIVE CONTROL LIVES HERE.
 *
 * -DTABS_NO_RETAIN builds a browser that has tabs and switches between them and
 * does everything the real one does EXCEPT keep what a tab was built from and
 * write the session down. That is byte for byte what a naive tab
 * implementation is, and it is what the tab test has to be able to tell apart
 * from this one: with it, switching back to a tab re-fetches the whole page
 * (so N tabs multiply the handshakes the connection-pool work removed) and a
 * restart restores nothing.
 *
 * `make test-tabs-negctl` builds exactly that and REQUIRES the test to fail, on
 * those checks and not on others. An assertion nobody has watched fail is not a
 * known-failing assertion.
 * ------------------------------------------------------------------------- */
void tab_keep_src(struct tab *t, unsigned char *src, int len)
{
#ifdef TABS_NO_RETAIN
    (void)t; (void)len; free(src); return;
#else
    if (!t) { free(src); return; }
    if (t->src) { release((size_t)t->srclen); free(t->src); }
    t->src = src; t->srclen = src ? len : 0;
    if (src) retain((size_t)len);
#endif
}

void tab_keep_css(struct tab *t, const char *css, int len)
{
#ifdef TABS_NO_RETAIN
    (void)t; (void)css; (void)len; return;
#else
    if (!t) return;
    if (t->css) { release((size_t)t->csslen); free(t->css); t->css = 0; t->csslen = 0; }
    if (!css || len <= 0) return;
    t->css = dupn(css, len);
    if (t->css) { t->csslen = len; retain((size_t)len); }
#endif
}

void tab_keep_res(struct tab *t, const char *url, const unsigned char *d, int len)
{
#ifdef TABS_NO_RETAIN
    (void)t; (void)url; (void)d; (void)len; return;
#else
    if (!t || !url || !d || len <= 0) return;
    for (int i = 0; i < t->nres; i++)
        if (t->res[i].url && strcmp(t->res[i].url, url) == 0) return;   /* already held */
    if (t->nres == t->cres) {
        int nc = t->cres ? t->cres * 2 : 8;
        struct tabres *nv = realloc(t->res, (size_t)nc * sizeof *nv);
        if (!nv) return;
        t->res = nv; t->cres = nc;
    }
    struct tabres *e = &t->res[t->nres];
    e->url = dups(url);
    e->data = (unsigned char *)dupn((const char *)d, len);
    if (!e->url || !e->data) { free(e->url); free(e->data); return; }
    e->len = len;
    retain(strlen(url) + 1 + (size_t)len);
    t->nres++;
#endif
}

const struct tabres *tab_res_find(const struct tab *t, const char *url)
{
    if (!t || !url) return 0;
    for (int i = 0; i < t->nres; i++)
        if (t->res[i].url && strcmp(t->res[i].url, url) == 0) return &t->res[i];
    return 0;
}

void tab_drop_content(struct tab *t)
{
    if (!t) return;
    if (t->src) { release((size_t)t->srclen); free(t->src); t->src = 0; t->srclen = 0; }
    if (t->css) { release((size_t)t->csslen); free(t->css); t->css = 0; t->csslen = 0; }
    for (int i = 0; i < t->nres; i++) {
        release((t->res[i].url ? strlen(t->res[i].url) + 1 : 0) + (size_t)t->res[i].len);
        free(t->res[i].url); free(t->res[i].data);
    }
    free(t->res); t->res = 0; t->nres = 0; t->cres = 0;
}

size_t tab_retained_bytes(const struct tab *t)
{
    if (!t || !t->used) return 0;
    size_t n = sizeof *t + (size_t)t->srclen + (size_t)t->csslen;
    for (int i = 0; i < t->nres; i++)
        n += (t->res[i].url ? strlen(t->res[i].url) + 1 : 0) + (size_t)t->res[i].len;
    for (int i = 0; i < TAB_HIST_MAX; i++)
        if (t->hist[i]) n += strlen(t->hist[i]) + 1;
    return n;
}

size_t tabs_retained_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < TAB_MAX; i++) if (g_tab[i].used) n += tab_retained_bytes(&g_tab[i]);
    return n;
}

/* --------------------------------------------------- per-tab back / forward */

void tab_hist_push(struct tab *t, const char *u)
{
    if (!t || !u || !u[0]) return;
    if (t->hcur >= 0 && t->hist[t->hcur] && strcmp(t->hist[t->hcur], u) == 0) return;
    if (t->hcur < TAB_HIST_MAX - 1) t->hcur++;
    else {                                   /* full: drop the oldest */
        release(t->hist[0] ? strlen(t->hist[0]) + 1 : 0);
        free(t->hist[0]);
        for (int i = 0; i < TAB_HIST_MAX - 1; i++) t->hist[i] = t->hist[i + 1];
        t->hist[TAB_HIST_MAX - 1] = 0;
    }
    /* Navigating truncates the forward branch: those entries are unreachable
     * now, and leaving them allocated is a leak that grows with every Back
     * followed by a click. */
    for (int i = t->hcur; i <= t->htop && i < TAB_HIST_MAX; i++) {
        if (t->hist[i]) { release(strlen(t->hist[i]) + 1); free(t->hist[i]); t->hist[i] = 0; }
    }
    t->hist[t->hcur] = dups(u);
    if (t->hist[t->hcur]) retain(strlen(u) + 1);
    t->htop = t->hcur;
}

void tab_hist_replace(struct tab *t, const char *u)
{
    if (!t || !u) return;
    if (t->hcur < 0) { tab_hist_push(t, u); return; }
    if (t->hist[t->hcur]) { release(strlen(t->hist[t->hcur]) + 1); free(t->hist[t->hcur]); }
    t->hist[t->hcur] = dups(u);
    if (t->hist[t->hcur]) retain(strlen(u) + 1);
    t->htop = t->hcur;
}

int tab_hist_can(const struct tab *t, int delta)
{
    if (!t) return 0;
    int x = t->hcur + delta;
    return x >= 0 && x <= t->htop && t->hist[x] != 0;
}

int tab_hist_go(struct tab *t, int delta, char *out, int max)
{
    if (!tab_hist_can(t, delta)) return 0;
    t->hcur += delta;
    sncpy(out, t->hist[t->hcur], max);
    return 1;
}

/* ============================== persistence ============================== */

static const struct bstore_ops *g_store;

void tabs_set_store(const struct bstore_ops *ops) { g_store = ops; }

static int st_read(const char *p, void *b, int m)
{ return g_store && g_store->read ? g_store->read(p, b, m) : -1; }
static int st_write(const char *p, const void *b, int l)
{ return g_store && g_store->write ? g_store->write(p, b, l) : -1; }
static void st_mkdir(const char *p)
{ if (g_store && g_store->mkdir) g_store->mkdir(p); }

/* The record format is one line per item, fields separated by TAB.
 *
 * TAB and not comma, and not JSON: a URL may contain a comma, a quote or a
 * brace, and a title is arbitrary page text. TAB is the one byte that cannot
 * appear in either (we strip it on the way in), so parsing is a split and there
 * is no escaping layer to get wrong. A newline in a title is stripped for the
 * same reason. */
static void put_field(char **p, char *end, const char *s, int sep)
{
    for (; s && *s && *p < end - 2; s++) {
        char c = *s;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        *(*p)++ = c;
    }
    if (*p < end - 1) *(*p)++ = (char)sep;
}

/* Split `line` (NUL-terminated, TAB separated) into up to `max` field pointers,
 * writing NULs in place. Returns the count. */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    return n;
}

static unsigned parse_u(const char *s)
{ unsigned v = 0; for (; s && *s >= '0' && *s <= '9'; s++) v = v * 10 + (unsigned)(*s - '0'); return v; }

static void append_u(char **p, char *end, unsigned v)
{
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n-- > 0 && *p < end - 1) *(*p)++ = tmp[n];
}

/* ------------------------------------------------------------- the session */

#define SESSION_BUF (TAB_MAX * (TAB_URL + TAB_TITLE + 32) + 64)

int session_save(void)
{
#ifdef TABS_NO_RETAIN
    return -1;                      /* the negative control: nothing is written */
#else
    static char buf[SESSION_BUF];
    char *p = buf, *end = buf + sizeof buf;

    /* Line 1 is a version + the active index. A store that cannot say which
     * version wrote it is a store that can only ever be read by the code that
     * wrote it. */
    const char *hdr = "logit-browser-session\t1\t";
    for (const char *s = hdr; *s && p < end - 1; s++) *p++ = *s;
    append_u(&p, end, (unsigned)(g_active < 0 ? 0 : g_active));
    if (p < end - 1) *p++ = '\n';

    for (int i = 0; i < TAB_MAX; i++) {
        struct tab *t = &g_tab[i];
        if (!t->used) continue;
        append_u(&p, end, (unsigned)i);
        if (p < end - 1) *p++ = '\t';
        put_field(&p, end, t->url, '\t');
        put_field(&p, end, t->title, '\t');
        append_u(&p, end, (unsigned)(t->scroll < 0 ? 0 : t->scroll));
        if (p < end - 1) *p++ = '\n';
    }
    st_mkdir(BROWSER_DIR);
    return st_write(SESSION_PATH, buf, (int)(p - buf));
#endif
}

int session_restore(void)
{
    static char buf[SESSION_BUF];
    int n = st_read(SESSION_PATH, buf, (int)sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = 0;

    int restored = 0, want_active = 0;
    char *line = buf;
    int first = 1;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (first) {
            char *f[4];
            int nf = split_tabs(line, f, 4);
            /* Refuse a file that is not ours rather than half-parsing it. */
            if (nf < 3 || strcmp(f[0], "logit-browser-session") != 0) return 0;
            want_active = (int)parse_u(f[2]);
            first = 0;
        } else if (line[0]) {
            char *f[5];
            int nf = split_tabs(line, f, 5);
            if (nf >= 3 && f[1][0]) {
                int idx = tabs_new(f[1]);
                if (idx >= 0) {
                    struct tab *t = tab_at(idx);
                    if (nf >= 3 && f[2][0]) sncpy(t->title, f[2], TAB_TITLE);
                    if (nf >= 4) t->scroll = (int)parse_u(f[3]);
                    /* A restored tab is dehydrated AND empty: it has no bytes,
                     * so selecting it loads. Restoring eight tabs must not be
                     * eight page loads. */
                    restored++;
                }
            }
        }
        line = nl ? nl + 1 : 0;
    }
    if (restored > 0) {
        if (!tab_at(want_active)) want_active = -1;
        if (want_active >= 0) g_active = want_active;
        else for (int i = 0; i < TAB_MAX; i++) if (g_tab[i].used) { g_active = i; break; }
    }
    return restored;
}

/* ---------------------------------------------------- history and bookmarks
 *
 * One record shape, two lists, one pair of load/save helpers. They differ only
 * in ordering (history is newest-first and capped; bookmarks are append-order
 * and explicit), so sharing the codec is not premature -- it is the reason a
 * bookmark and a history entry cannot drift into two incompatible files. */

static struct hist_entry g_hist[HISTORY_MAX];
static int g_nhist;
static struct hist_entry g_bm[BOOKMARK_MAX];
static int g_nbm;

#define REC_BUF(n) ((n) * (TAB_URL + TAB_TITLE + 16) + 64)

static int recs_save(const char *path, const char *magic,
                     const struct hist_entry *v, int n)
{
    static char buf[REC_BUF(HISTORY_MAX)];
    char *p = buf, *end = buf + sizeof buf;
    for (const char *s = magic; *s && p < end - 1; s++) *p++ = *s;
    if (p < end - 1) *p++ = '\n';
    for (int i = 0; i < n; i++) {
        put_field(&p, end, v[i].url, '\t');
        put_field(&p, end, v[i].title, '\t');
        append_u(&p, end, v[i].when);
        if (p < end - 1) *p++ = '\n';
    }
    st_mkdir(BROWSER_DIR);
    return st_write(path, buf, (int)(p - buf));
}

static int recs_load(const char *path, const char *magic,
                     struct hist_entry *v, int max)
{
    static char buf[REC_BUF(HISTORY_MAX)];
    int n = st_read(path, buf, (int)sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *line = buf; int first = 1, got = 0;
    while (line && *line && got < max) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (first) { if (strcmp(line, magic) != 0) return 0; first = 0; }
        else if (line[0]) {
            char *f[4];
            int nf = split_tabs(line, f, 4);
            if (nf >= 1 && f[0][0]) {
                sncpy(v[got].url, f[0], TAB_URL);
                sncpy(v[got].title, nf >= 2 ? f[1] : "", TAB_TITLE);
                v[got].when = nf >= 3 ? parse_u(f[2]) : 0;
                got++;
            }
        }
        line = nl ? nl + 1 : 0;
    }
    return got;
}

void history_load(void)
{ g_nhist = recs_load(HISTORY_PATH, "logit-browser-history\t1", g_hist, HISTORY_MAX); }

int history_save(void)
{ return recs_save(HISTORY_PATH, "logit-browser-history\t1", g_hist, g_nhist); }

void history_clear(void) { g_nhist = 0; }

void history_add(const char *url, const char *title, unsigned when)
{
    if (!url || !url[0]) return;
    /* De-duplicate by URL and move the hit to the front: a list where the page
     * you visit ten times a day appears ten times is a list you cannot use. */
    int at = -1;
    for (int i = 0; i < g_nhist; i++) if (strcmp(g_hist[i].url, url) == 0) { at = i; break; }
    struct hist_entry e;
    memset(&e, 0, sizeof e);
    sncpy(e.url, url, TAB_URL);
    /* Keep the better title: a re-visit that has not parsed <title> yet must
     * not blank the one we already had. */
    if (title && title[0]) sncpy(e.title, title, TAB_TITLE);
    else if (at >= 0) sncpy(e.title, g_hist[at].title, TAB_TITLE);
    e.when = when;
    if (at >= 0) {
        for (int i = at; i > 0; i--) g_hist[i] = g_hist[i - 1];
        g_hist[0] = e;
        return;
    }
    if (g_nhist < HISTORY_MAX) g_nhist++;
    for (int i = g_nhist - 1; i > 0; i--) g_hist[i] = g_hist[i - 1];
    g_hist[0] = e;
}

int history_count(void) { return g_nhist; }
const struct hist_entry *history_at(int i)
{ return (i >= 0 && i < g_nhist) ? &g_hist[i] : 0; }

int history_search(const char *q, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_nhist && n < max; i++)
        if (ci_has(g_hist[i].url, q) || ci_has(g_hist[i].title, q)) out[n++] = i;
    return n;
}

void bookmarks_load(void)
{ g_nbm = recs_load(BOOKMARKS_PATH, "logit-browser-bookmarks\t1", g_bm, BOOKMARK_MAX); }

int bookmarks_save(void)
{ return recs_save(BOOKMARKS_PATH, "logit-browser-bookmarks\t1", g_bm, g_nbm); }

int bookmark_find(const char *url)
{
    if (!url) return -1;
    for (int i = 0; i < g_nbm; i++) if (strcmp(g_bm[i].url, url) == 0) return i;
    return -1;
}

int bookmark_add(const char *url, const char *title)
{
    if (!url || !url[0]) return -1;
    int at = bookmark_find(url);
    if (at >= 0) {                            /* already bookmarked: refresh title */
        if (title && title[0]) sncpy(g_bm[at].title, title, TAB_TITLE);
        return at;
    }
    if (g_nbm >= BOOKMARK_MAX) return -1;
    memset(&g_bm[g_nbm], 0, sizeof g_bm[0]);
    sncpy(g_bm[g_nbm].url, url, TAB_URL);
    sncpy(g_bm[g_nbm].title, title && title[0] ? title : url, TAB_TITLE);
    return g_nbm++;
}

int bookmark_remove(int i)
{
    if (i < 0 || i >= g_nbm) return -1;
    for (int k = i; k < g_nbm - 1; k++) g_bm[k] = g_bm[k + 1];
    g_nbm--;
    return 0;
}

int bookmark_count(void) { return g_nbm; }
const struct hist_entry *bookmark_at(int i)
{ return (i >= 0 && i < g_nbm) ? &g_bm[i] : 0; }

/* ================================ downloads ============================== */

static struct download g_dl[DOWNLOAD_MAX];
static int g_ndl;

int download_name(const char *url, char *out, int max)
{
    if (max <= 0) return 0;
    out[0] = 0;
    if (!url) return 0;
    /* The last path segment, minus any query or fragment. */
    const char *q = url;
    const char *last = url;
    int in_path = 0;
    for (const char *p = url; *p; p++) {
        if (*p == '?' || *p == '#') break;
        if (*p == ':' && p[1] == '/' && p[2] == '/') { p += 2; last = p + 1; in_path = 0; continue; }
        if (*p == '/') { last = p + 1; in_path = 1; }
        q = p;
    }
    (void)q; (void)in_path;
    int n = 0;
    for (const char *p = last; *p && n < max - 1; p++) {
        if (*p == '?' || *p == '#') break;
        char c = *p;
        /* A name is written to a real filesystem: keep it to bytes a path can
         * hold. '/' would create a directory that was never asked for. */
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) c = '_';
        out[n++] = c;
    }
    out[n] = 0;
    if (n == 0) { sncpy(out, "download", max); n = (int)strlen(out); }
    return n;
}

int download_record(const char *url, const unsigned char *data, int len)
{
    if (g_ndl >= DOWNLOAD_MAX) return -1;
    struct download *d = &g_dl[g_ndl];
    memset(d, 0, sizeof *d);
    sncpy(d->url, url ? url : "", TAB_URL);
    char name[96];
    download_name(url, name, (int)sizeof name);
    int p = 0;
    for (const char *s = DOWNLOAD_DIR; *s && p < (int)sizeof d->path - 2; s++) d->path[p++] = *s;
    d->path[p++] = '/';
    for (const char *s = name; *s && p < (int)sizeof d->path - 1; s++) d->path[p++] = *s;
    d->path[p] = 0;
    d->len = len;
    st_mkdir(DOWNLOAD_DIR);
    d->ok = (data && len > 0 && st_write(d->path, data, len) >= 0) ? 1 : 0;
    return g_ndl++;
}

int download_count(void) { return g_ndl; }
const struct download *download_at(int i)
{ return (i >= 0 && i < g_ndl) ? &g_dl[i] : 0; }

int download_is_downloadable(const char *url)
{
    if (!url) return 0;
    /* The extension, taken from the last path segment before any query. */
    char name[96];
    download_name(url, name, (int)sizeof name);
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot || !dot[1]) return 0;
    static const char *const ext[] = {
        "zip","tar","gz","bz2","xz","7z","rar",
        "pdf","iso","exe","dmg","deb","rpm","aex",
        "mp3","mp4","wav","flac","ogg","mkv","avi","aac",
        "ttf","otf","woff","woff2", 0
    };
    for (int i = 0; ext[i]; i++) {
        const char *a = dot + 1, *b = ext[i];
        while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}
