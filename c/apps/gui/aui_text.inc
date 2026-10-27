/* aui_text.inc -- text drawing: label, heading, sized/sliced/aligned/ellipsised text,
 * wrapped paragraphs, alpha text.
 * Unity-included by aui.c (one translation unit): every static it names is
 * file-scope in aui.c, and the variant builds (-DAUI_NO_AA, -DAUI_COST, ...)
 * reach it automatically. Do not compile on its own. */

#include "../../lib/text/para.h"   /* tl_para_layout; libtl.a via $(AUI_LIBS) */

/* ------------------------------------------------------------------ text */

#define PX AUI_FS_BODY

static int tw(const char *s) { return text_measure_px(s, slen(s), PX, 0); }
static int twn(const char *s, int n) { return n <= 0 ? 0 : text_measure_px(s, n, PX, 0); }
static void txt(int x, int y, unsigned c, const char *s)
{ gui_text_run(X_(x), Y_(y), PX, 0, c, s, slen(s)); }

int  aui_text_w(const char *s, int px) { return text_measure_px(s, slen(s), px, 0); }
void aui_text_sz(int x, int y, const char *s, unsigned color, int px)
{ gui_text_run(X_(x), Y_(y), px, 0, color, s, slen(s)); }
/* A SLICE of a buffer, not a NUL-terminated string. Every text view that
 * scrolls has this shape -- one big buffer, a table of (offset, length) lines,
 * and only the visible ones drawn -- and until this existed the only way to
 * draw one was to call gui_text_run directly. That is a trap, because
 * gui_text_run takes WINDOW coordinates and every aui text call takes
 * CONTAINER ones: inside aui_scroll_begin the two differ by the container's
 * origin AND by the scroll offset, so the labels moved with the scroll and the
 * text did not. The symptom was a transcript whose role headings sat two rows
 * away from the text they belonged to, which reads as a wrapping bug and is
 * not one. Found by c/apps/gui/ch.c, the first scrolling text view in the tree
 * that is not the browser's own painter. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px)
{ if (len > 0) gui_text_run(X_(x), Y_(y), px, 0, color, s, len); }
void aui_heading(int x, int y, const char *s, unsigned color) { aui_text_sz(x, y, s, color, AUI_FS_TITLE); }
void aui_label(int x, int y, const char *s, unsigned color) { txt(x, y, color, s); }

/* ------------------------------------------------ v2 text: capabilities */

/* anim_gen is defined (with its initialiser) in aui_motion.inc, which comes
 * after this file; the tentative definition lets the per-frame key below name it. */
static unsigned anim_gen;

#define TX_MAX   1023     /* LOGIT_TEXT_METRICS_MAX_BYTES: the kernel's cap on one query */
#define TX_LINES 64
#define TX_NEED  (LOGIT_GUI2_F_CARETS | LOGIT_GUI2_F_TEXT_RUN2)

/* SYS_GUI2_INFO answered once per drawn frame (anim_gen advances in aui_begin),
 * lazily, so a frame that draws no text asks nothing. A kernel without the
 * syscall answers <= 0 and every path below stays on its legacy branch. */
static int tx_caps_ok;
static unsigned tx_caps_gen, tx_caps_v;
static unsigned tx_caps(void)
{
    if (!tx_caps_ok || tx_caps_gen != anim_gen) {
        long v = ol_gui2_info(0);
        tx_caps_v = v > 0 ? (unsigned)v & 0xFFFFu : 0u;
        tx_caps_gen = anim_gen; tx_caps_ok = 1;
    }
    return tx_caps_v;
}
static int tx_has_layout(void) { return (tx_caps() & TX_NEED) == TX_NEED; }
static int tx_has_run2(void) { return (tx_caps() & LOGIT_GUI2_F_TEXT_RUN2) != 0; }

static int tx_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }
/* Largest k <= n that starts a code point. s[n] must be readable (n < len, or the
 * NUL of a C string); a stray continuation run is walked back to its lead byte. */
static int tx_u8_cut(const char *s, int n)
{
    while (n > 0 && tx_cont(s[n])) n--;
    return n;
}

/* floor(a / b) for b > 0 */
static int tx_fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* ------------------------------------------------------ metrics cache */

/* Ascent and descent in DEVICE pixels from the metrics query, per
 * (px, face, scale_percent): a scale change re-keys, so it misses. */
struct tx_met { int px, face, pct, asc, dsc, ok; };
static struct tx_met tx_mc[8];
static unsigned tx_mc_n;

static int tx_metrics(int px, int face, int *asc, int *dsc)
{
    int pct = aui_scale();
    for (int i = 0; i < 8; i++) {
        struct tx_met *e = &tx_mc[i];
        if (e->ok && e->px == px && e->face == face && e->pct == pct) {
            *asc = e->asc; *dsc = e->dsc; return 1;
        }
    }
    struct logit_text_metrics m;
    if (text_run_metrics_px("", 0, px, face, &m) != 1 || m.ascent <= 0) return 0;
    struct tx_met *e = &tx_mc[tx_mc_n++ & 7u];
    e->px = px; e->face = face; e->pct = pct; e->ok = 1;
    e->asc = m.ascent; e->dsc = m.descent < 0 ? -m.descent : m.descent;
    *asc = e->asc; *dsc = e->dsc;
    return 1;
}

/* ------------------------------------------------------ run2 + layout */

/* One run at 24.8 WINDOW points. `alpha` 255 is opaque. Damage is noted at the
 * run's nominal box exactly as gui_text_run does. */
static void tx_run2(int x24, int y24, int px, int face, unsigned argb,
                    const char *s, int len, int alpha)
{
    if (len <= 0) return;
    struct logit_run2 r = { 0, x24, y24, px * 256, face & 3, argb, s, len, alpha, 0, 0 };
    ad_note_text(tx_fdiv(x24, 256), tx_fdiv(y24, 256), px, (face & 3) | (alpha << 8), argb, s, len);
    ol_window_text_run2(&r);
}

/* Legacy-path run honouring a face mask. */
static void tx_run1(int x, int y, int px, int face, unsigned color, const char *s, int len)
{
    if (len <= 0) return;
    if (!(face & 3)) { gui_text_run(x, y, px, 0, color, s, len); return; }
    ad_note_text(x, y, px, face & 3, color, s, len);
    gui_text_run_w(x, y, px, face & 1, color, s, len, (face >> 1) & 1);
}

/* The one carets query (SYS_TEXT_MEASURE b = -2). Positions are DEVICE 24.8. */
static int tx_carets_cb(void *ctx, const char *s, int len, int px, unsigned face,
                        unsigned short *off, int *x24_8, int max)
{
    (void)ctx;
    int cnt = 0;
    struct logit_text_carets_query q = { sizeof q, LOGIT_TEXT_CARETS_VERSION, s, len, px,
                                         (int)(face & 3), 0, off, x24_8, max, &cnt };
    if (_sys(SYS_TEXT_MEASURE, (long)&q, LOGIT_TEXT_CARETS_QUERY, 0) != 1) return -1;
    return cnt;
}

/* Scratch for one layout. Not reentrant (para.c's is not either); aui is one
 * thread. */
static unsigned short tx_off[TX_MAX + 1];
static int tx_x[TX_MAX + 1];
static struct tl_line tx_ln[TX_LINES];
static char tx_buf[TX_MAX + 8];

/* AUI_TRUNC_* -> TL_TRUNC_* */
static int tx_trunc(int t)
{
    return t == AUI_TRUNC_MIDDLE ? TL_TRUNC_MIDDLE : t == AUI_TRUNC_HEAD ? TL_TRUNC_HEAD
         : t == AUI_TRUNC_NONE ? TL_TRUNC_NONE : TL_TRUNC_TAIL;
}

/* Lay `s[0..len)` out in a box `wdev` DEVICE px wide into tx_ln[]. Lines, or -1. */
static int tx_layout(const char *s, int len, int px, int face, int wdev, int maxl,
                     int align, int trunc, int line_h)
{
    struct tl_para_opts o;
    o.width24_8 = (wdev < 1 ? 1 : wdev) * 256;
    o.px = px; o.face = face & 3; o.line_height24_8 = line_h * 256;
    o.max_lines = maxl; o.align = align; o.trunc = tx_trunc(trunc);
    struct tl_measure_cb cb = { tx_carets_cb, 0 };
    return tl_para_layout(s, len, &o, &cb, tx_ln, TX_LINES, tx_off, tx_x, TX_MAX + 1);
}

/* Draw one laid-out line (the text pieces and U+2026 composed into ONE run so
 * the shaper sees the real string) at 24.8 window points x24, y24. */
static void tx_draw_line(const char *s, const struct tl_line *L, int x24, int y24,
                         int px, int face, unsigned argb, int alpha)
{
    int n = 0;
    if (!L->ellipsis) {
        int e = L->len;
        while (e > 0 && (s[L->start + e - 1] == '\n' || s[L->start + e - 1] == '\r' ||
                         s[L->start + e - 1] == ' ')) e--;     /* blanks draw nothing */
        for (int i = 0; i < e; i++) tx_buf[n++] = s[L->start + i];
    } else {
        int pre = L->cut_off - L->start;
        int post = L->start + L->len - (L->cut_off + L->cut_len);
        for (int i = 0; i < pre; i++) tx_buf[n++] = s[L->start + i];
        tx_buf[n++] = (char)0xE2; tx_buf[n++] = (char)0x80; tx_buf[n++] = (char)0xA6;   /* U+2026 */
        for (int i = 0; i < post; i++) tx_buf[n++] = s[L->cut_off + L->cut_len + i];
    }
    int pct = aui_scale();
    x24 += (int)(((long long)L->x_off24_8 * 100 + pct - 1) / pct);   /* device 24.8 -> point 24.8, up */
    tx_run2(x24, y24, px, face, argb, tx_buf, n, alpha);
}

/* ---------------------------------------------------- baseline alignment */

/* Centre the BODY-size line box in the rect and put every size's baseline on
 * that one line, so a 13 px and a 17 px label in the same row share a baseline.
 * Metrics come from the query (ascent, descent in device px); a text too tall for
 * the rect is shifted to fit instead. Falls back to the old estimate when the
 * query is unsupported. */
void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align)
{
    int w = aui_text_w(s, px);
    int x = r.x;
    if (align == AUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
    else if (align == AUI_ALIGN_RIGHT) x = r.x + r.w - w;
    int asc, dsc, rasc, rdsc, n = slen(s);
    if (n > 0 && tx_metrics(px, 0, &asc, &dsc) && tx_metrics(PX, 0, &rasc, &rdsc)) {
        int pct = aui_scale();
        int top = aui_dev(Y_(r.y)), hh = aui_dev(Y_(r.y + r.h)) - top;
        int ty = (2 * top + hh + rasc - rdsc) / 2 - asc;
        if (hh >= asc + dsc) {
            if (ty < top) ty = top;
            if (ty + asc + dsc > top + hh) ty = top + hh - asc - dsc;
        }
        if (tx_has_run2()) {
            int y24 = (int)(((long long)ty * 25600 + pct - 1) / pct);   /* round up: S(y) lands on ty */
            tx_run2(X_(x) * 256, y24, px, 0, color, s, n, 255);
        } else {
            gui_text_run(X_(x), tx_fdiv(ty * 100 + pct / 2, pct), px, 0, color, s, n);
        }
        return;
    }
    aui_text_sz(x, r.y + (r.h - px) / 2 - 1, s, color, px);
}

/* ------------------------------------------------------------ alpha text */

void aui_text_a(int x, int y, const char *s, unsigned color, int px, int alpha)
{
    int n = slen(s);
    if (n <= 0 || alpha <= 0) return;
    if (alpha >= 255) { aui_text_sz(x, y, s, color, px); return; }
    if (tx_has_run2()) { tx_run2(X_(x) * 256, Y_(y) * 256, px, 0, color, s, n, alpha); return; }
    aui_text_sz(x, y, s, aui_mix(AUI_BG, color, alpha), px);      /* no TEXT_RUN2: approximate */
}

/* -------------------------------------------------------------- ellipsis */

/* The pre-v2 cut: binary search over the measured prefix, "..." appended. Cuts
 * at a code point, never inside one (a CJK or emoji string used to leave a stray
 * lead byte before the dots). -DAUI_TEXT_LEGACY_ELLIPSIS keeps the byte cut:
 * tests/aui-text-adopt.mk's negative control. */
static void tx_ell_legacy(int x, int y, int maxw, const char *s, int n, unsigned color, int px, int face)
{
    if (text_measure_px(s, n, px, face) <= maxw) { tx_run1(X_(x), Y_(y), px, face, color, s, n); return; }
    int ew = text_measure_px("...", 3, px, face);
    int lo = 0, hi = n;
    while (lo < hi) {                                  /* binary search, not a scan:
                                                        * each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (text_measure_px(s, mid, px, face) + ew <= maxw) lo = mid; else hi = mid - 1;
    }
#ifndef AUI_TEXT_LEGACY_ELLIPSIS
    while (lo > 0 && lo < n && tx_cont(s[lo])) lo--;
#endif
    tx_run1(X_(x), Y_(y), px, face, color, s, lo);
#ifdef AUI_SPLIT_NEGCTL            /* tests/aui-split.mk: must change the object hash */
    tx_run1(X_(x) + text_measure_px(s, lo, px, face), Y_(y), px, face, color, "..", 2);
#else
    tx_run1(X_(x) + text_measure_px(s, lo, px, face), Y_(y), px, face, color, "...", 3);
#endif
}

/* Fit `s` to `maxw` points. With CARETS + TEXT_RUN2 the cut is laid out by
 * tl_para_layout from ONE carets query (a second only for U+2026 when a cut
 * happens), at cluster boundaries, with a real U+2026; mode picks which part is
 * elided. Otherwise the legacy cut, which only knows the tail. */
void aui_text_ellipsis_ex(int x, int y, int maxw, const char *s, unsigned color, int px, int mode)
{
    int n = slen(s);
#ifndef AUI_TEXT_LEGACY_ELLIPSIS
    if (n > 0 && n <= TX_MAX && tx_has_layout()) {
        if (tx_layout(s, n, px, 0, aui_dev(maxw), 1, AUI_ALIGN_LEFT, mode, 0) == 1) {
            if (!tx_ln[0].ellipsis) aui_text_sz(x, y, s, color, px);
            else tx_draw_line(s, &tx_ln[0], X_(x) * 256, Y_(y) * 256, px, 0, color, 255);
            return;
        }
    }
#else
    (void)mode;
#endif
    tx_ell_legacy(x, y, maxw, s, n, color, px, 0);
}

void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px)
{ aui_text_ellipsis_ex(x, y, maxw, s, color, px, AUI_TRUNC_TAIL); }

/* ------------------------------------------------------------------ wrap */

/* Fallback (no CARETS / TEXT_RUN2): largest code point boundary e in (a, b] with
 * measure(s[a..e)) <= wpt, at least one code point. One measure syscall per probe. */
static int tx_fit(const char *s, int len, int a, int b, int px, int face, int wpt)
{
    int lo = a, hi = b;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2, m = mid;
        while (m > a && m < len && tx_cont(s[m])) m--;
        if (m <= lo) { m = lo + 1; while (m < len && tx_cont(s[m])) m++; }
        if (m > hi) break;
        if (text_measure_px(s + a, m - a, px, face) <= wpt) lo = m; else hi = m - 1;
    }
    if (lo == a) { lo = a + 1; while (lo < len && tx_cont(s[lo])) lo++; }
    return lo;
}

static int tx_wrap_fb(struct aui_rect r, const char *s, int len, int px, int face,
                      unsigned argb, int line_h, int limit, int align, int trunc, int draw)
{
    int cur = 0, nl = 0;
    while (cur < len && nl < limit) {
        int he = cur; while (he < len && s[he] != '\n') he++;       /* end of this hard line */
        int e = he, next = he < len ? he + 1 : len;
        if (he > cur && text_measure_px(s + cur, he - cur, px, face) > r.w) {
            e = tx_fit(s, len, cur, he, px, face, r.w);
            if (e < he && s[e] == ' ') next = e + 1;           /* the break falls on a space */
            else {
                int bp = -1;
                for (int i = e - 1; i > cur; i--) if (s[i] == ' ') { bp = i; break; }
                if (bp > cur) { e = bp; next = bp + 1; } else next = e;   /* else: mid-word */
            }
        }
        if (draw) {
            int y = r.y + nl * line_h;
            if (nl == limit - 1 && next < len && trunc != AUI_TRUNC_NONE)
                tx_ell_legacy(r.x, y, r.w, s + cur, he - cur, argb, px, face);
            else {
                while (e > cur && s[e - 1] == ' ') e--;
                int lw = e > cur ? text_measure_px(s + cur, e - cur, px, face) : 0;
                int x = align == AUI_ALIGN_CENTER ? r.x + (r.w - lw) / 2
                      : align == AUI_ALIGN_RIGHT ? r.x + r.w - lw : r.x;
                tx_run1(X_(x), Y_(y), px, face, argb, s + cur, e - cur);
            }
        }
        nl++; cur = next;
    }
    return nl;
}

static int tx_wrap(struct aui_rect r, const char *s, int len, int px, unsigned face,
                   unsigned argb, int line_h, int max_lines, int align, int trunc, int draw)
{
    if (!s) return 0;
    if (len < 0) len = slen(s);
    if (len > TX_MAX) len = tx_u8_cut(s, TX_MAX);
    if (len <= 0) return 0;
    if (line_h <= 0) line_h = px + px / 4;
    int limit = max_lines > 0 ? max_lines : (draw && r.h > 0 ? r.h / line_h : TX_LINES);
    if (limit < 1) limit = 1;
    if (limit > TX_LINES) limit = TX_LINES;
    if (tx_has_layout()) {
        int nl = tx_layout(s, len, px, (int)face, aui_dev(r.w), limit, align, trunc, line_h);
        if (nl >= 0) {
            for (int i = 0; draw && i < nl; i++)
                tx_draw_line(s, &tx_ln[i], X_(r.x) * 256, (Y_(r.y) + i * line_h) * 256,
                             px, (int)face, argb, 255);
            return nl;
        }
    }
    return tx_wrap_fb(r, s, len, px, (int)face & 3, argb, line_h, limit, align, trunc, draw);
}

/* Wrap `s` (len < 0: NUL-terminated) into rect r, line_h points apart, at most
 * max_lines (<= 0: as many as fit in r.h, never more than 64). The last allowed
 * line is cut per `trunc`. Text past 1023 bytes is not drawn. Returns lines drawn. */
int aui_text_wrap(struct aui_rect r, const char *s, int len, int px, unsigned face,
                  unsigned argb, int line_h, int max_lines, int align, int trunc)
{ return tx_wrap(r, s, len, px, face, argb, line_h, max_lines, align, trunc, 1); }

/* The height in points aui_text_wrap would use for a box w points wide
 * (lines * line_h); max_lines <= 0 means up to 64 lines. */
int aui_text_measure_wrap(int w, const char *s, int len, int px, unsigned face,
                          int line_h, int max_lines)
{
    struct aui_rect r = { 0, 0, w, 0 };
    if (line_h <= 0) line_h = px + px / 4;
    return tx_wrap(r, s, len, px, face, 0, line_h, max_lines, AUI_ALIGN_LEFT, AUI_TRUNC_NONE, 0) * line_h;
}
