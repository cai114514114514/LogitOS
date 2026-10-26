/* Host AUI harness transport. See aui_host.h.
 *
 * Every legacy case below cites the `case SYS_*` line in c/kernel/gui/wm.c it
 * mirrors (line numbers as of 74b1d7ade) and calls the same ol_display_*
 * function that case reaches through fb_*. Text is deliberately NOT the real
 * rasteriser: it is deterministic box glyphs, so a golden moves when layout or
 * paint moves and never when a font changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "logit_pack.h"

struct aui_host *aui_host_current;

/* ---------------------------------------------------------------- scaling */

/* VERBATIM fb_pt (c/kernel/gui/fb/fb.c:184-188) with ui_scale as an argument.
 * Floor toward negative infinity. aui_host_scale_check.c asserts equality. */
int aui_host_pt(int points, int pct)
{
    if (pct < 100) pct = 100;
    if (pct > 400) pct = 400;
    if (points >= 0) return points * pct / 100;
    return -(((-points) * pct + 99) / 100);
}
/* fb_dev2pt (fb.c:200-205), same treatment. */
int aui_host_dev2pt(int px, int pct)
{
    if (pct < 100) pct = 100;
    if (pct > 400) pct = 400;
    int a = 100 * (px + 1), b = pct;
    int q = (a >= 0) ? (a + b - 1) / b : -((-a) / b);
    return q - 1;
}
#define S(v)  aui_host_pt((v), h->pct)
#define PT(v) aui_host_dev2pt((v), h->pct)

/* ------------------------------------------------------------- registries */

static aui_host_fn g_handler[AUI_HOST_NSYS];
void aui_host_on(long num, aui_host_fn fn)
{
    if (num >= 0 && num < AUI_HOST_NSYS) g_handler[num] = fn;
}
void aui_host_handlers_clear(void) { memset(g_handler, 0, sizeof g_handler); }

static struct aui_host_v2 *g_v2_head;
void aui_host_v2_register(struct aui_host_v2 *r)
{
    r->next = g_v2_head;
    g_v2_head = r;
}
unsigned aui_host_enable_v2(struct aui_host *h)
{
    unsigned bits = 0;
    /* registration order is constructor order, which is link order and so not
     * a thing to depend on: the INFO mask is an OR and handlers are keyed by
     * number, so neither is order-sensitive unless two files claim a number. */
    for (struct aui_host_v2 *r = g_v2_head; r; r = r->next) bits |= r->info_bits;
    h->info = bits;
    for (struct aui_host_v2 *r = g_v2_head; r; r = r->next)
        if (r->reg) r->reg(h);
    return bits;
}

/* ---------------------------------------------------------------- lifetime */

static void rebind(struct aui_host *h)
{
    struct ol_pixel_target t = { h->px, h->w, h->h, h->clip_on,
                                 h->clx0, h->cly0, h->clx1, h->cly1 };
    ol_display_bind(&h->disp, &t, 16, 8, 0);       /* BGRX */
}

static void surface_alloc(struct aui_host *h, int w_pt, int h_pt)
{
    free(h->px);
    h->win_w_pt = w_pt; h->win_h_pt = h_pt;
    h->w = aui_host_pt(w_pt, h->pct);
    h->h = aui_host_pt(h_pt, h->pct);
    h->px = calloc((size_t)h->w * (size_t)h->h, 4);
    h->clip_on = 0;
    rebind(h);
}

struct aui_host *aui_host_new(int win_w_pt, int win_h_pt, int pct)
{
    struct aui_host *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->pct = pct < 100 ? 100 : pct > 400 ? 400 : pct;
    h->disp.allocate = (void *(*)(unsigned long))malloc;
    h->disp.release = free;
    h->screen_w_pt = 1280; h->screen_h_pt = 800;
    h->focused = 1;
    h->wall = (struct logit_time){ 2026, 10, 26, 9, 41, 0, 1 };
    surface_alloc(h, win_w_pt, win_h_pt);
    aui_host_current = h;
    return h;
}

void aui_host_free(struct aui_host *h)
{
    if (!h) return;
    ol_display_destroy(&h->disp);
    free(h->px);
    if (aui_host_current == h) aui_host_current = 0;
    free(h);
}

void aui_host_clock_set(unsigned long long ms)
{ if (aui_host_current) aui_host_current->clock_ms = ms; }
void aui_host_clock_advance(unsigned long long ms)
{ if (aui_host_current) aui_host_current->clock_ms += ms; }

void aui_host_push_event(const struct logit_event *e)
{
    struct aui_host *h = aui_host_current;
    if (!h || h->evq_n >= AUI_HOST_EVQ) return;
    h->evq[(h->evq_head + h->evq_n++) % AUI_HOST_EVQ] = *e;
}

void aui_host_stats_reset(struct aui_host *h)
{
    memset(h->calls, 0, sizeof h->calls);
    memset(h->unhandled, 0, sizeof h->unhandled);
    h->px_written = 0;
    h->ndmg = 0;
}
int aui_host_unhandled_total(const struct aui_host *h)
{
    int n = 0;
    for (int i = 0; i < AUI_HOST_NSYS; i++) if (h->unhandled[i]) n++;
    return n;
}

/* ------------------------------------------------------------------- stats */

/* Device pixels a write of this box can touch: the box clipped to the surface
 * and to the target clip. Deterministic, which is what makes it a budget. */
static void note_px(struct aui_host *h, int x, int y, int w, int hh)
{
    long x0 = x, y0 = y, x1 = (long)x + w, y1 = (long)y + hh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > h->w) x1 = h->w;
    if (y1 > h->h) y1 = h->h;
    if (h->clip_on) {
        if (x0 < h->clx0) x0 = h->clx0;
        if (y0 < h->cly0) y0 = h->cly0;
        if (x1 > h->clx1) x1 = h->clx1;
        if (y1 > h->cly1) y1 = h->cly1;
    }
    if (x1 > x0 && y1 > y0) h->px_written += (unsigned long long)(x1 - x0) * (unsigned long long)(y1 - y0);
}

/* ------------------------------------------------------------- box text */

static unsigned utf8_next(const unsigned char **pp, const unsigned char *e)
{
    const unsigned char *p = *pp;
    unsigned c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF8 || (c >= 0x80 && c < 0xC0)) extra = 0;       /* stray byte: one cell */
    else if (extra) c &= (0x3F >> extra);
    while (extra-- > 0 && p < e && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}
static int is_wide(unsigned c)
{
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
           (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD);
}
/* round(0.55*px), or px for East-Asian-wide. */
static int advance_of(unsigned cp, int px) { return is_wide(cp) ? px : (55 * px + 50) / 100; }

int aui_host_text_advance(const char *s, int len, int px)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + (len > 0 ? len : 0);
    int sum = 0;
    while (p < e) sum += advance_of(utf8_next(&p, e), px);
    return sum;
}

/* One filled box per code point (nothing for ASCII space and controls). The
 * box is advance minus a gap wide, 0.7*px tall, and sits on the baseline at
 * y + round(0.8*px). `cell` > 0 forces a fixed pitch (SYS_GUI_TEXT_MONO). */
static void draw_boxes(struct aui_host *h, int x, int y, int px, int cell,
                       uint32_t color, const char *s, int len)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + (len > 0 ? len : 0);
    int base = y + (80 * px + 50) / 100, ht = (70 * px + 50) / 100, cur = 0;
    if (ht < 1) ht = 1;
    while (p < e) {
        unsigned cp = utf8_next(&p, e);
        int adv = cell > 0 ? (is_wide(cp) ? 2 * cell : cell) : advance_of(cp, px);
        if (cp > 0x20 && cp != 0x7F) {
            int gap = adv / 6, bw = adv - gap;
            if (bw < 1) bw = 1;
            ol_display_fill_rect(&h->disp, x + cur, base - ht, bw, ht, color & 0xFFFFFF);
            note_px(h, x + cur, base - ht, bw, ht);
        }
        cur += adv;
    }
}

/* -------------------------------------------------------------------- icon */

/* Placeholder icon: a 1-device-px outline plus a diagonal whose slope comes
 * from the id (eight angles). Deterministic, and it moves if x/y/px/colour do. */
static void draw_icon(struct aui_host *h, int id, int x, int y, int px, uint32_t color)
{
    color &= 0xFFFFFF;
    ol_display_fill_rect(&h->disp, x, y, px, 1, color);
    ol_display_fill_rect(&h->disp, x, y + px - 1, px, 1, color);
    ol_display_fill_rect(&h->disp, x, y, 1, px, color);
    ol_display_fill_rect(&h->disp, x + px - 1, y, 1, px, color);
    int k = (int)((unsigned)id % 8u) + 1;
    for (int i = 0; i < px; i++)
        ol_display_put(&h->disp, x + i, y + (px - 1) - (i * (px - 1) * k) / (8 * (px > 1 ? px - 1 : 1)), color);
    note_px(h, x, y, px, px);
}

/* ---------------------------------------------------------------- dispatch */

static void log_damage(struct aui_host *h, int x, int y, int w, int hh)
{
    if (h->ndmg < AUI_HOST_DMG_MAX) h->dmg[h->ndmg++] = (struct aui_host_rect){ x, y, w, hh };
}

long aui_host_sys_builtin(struct aui_host *h, long n, long a, long b, long c)
{
    switch (n) {
    case SYS_GUI_CREATE: {                                        /* wm.c:2682 */
        if (a) { strncpy(h->title, (const char *)a, sizeof h->title - 1); h->title[sizeof h->title - 1] = 0; }
        int w = LOGIT_GUI_CREATE_B_W(b), hh = LOGIT_GUI_CREATE_B_H(b);
        if (w != h->win_w_pt || hh != h->win_h_pt) surface_alloc(h, w, hh);
        return 0;
    }
    case SYS_GUI_CLEAR:                                           /* wm.c:2831 */
        ol_display_clear(&h->disp, (uint32_t)a);
        note_px(h, 0, 0, h->w, h->h);
        return 0;
    case SYS_GUI_RECT: {                                          /* wm.c:2836 */
        int x = S(LOGIT_GUI_RECT_A_X(a)), y = S(LOGIT_GUI_RECT_A_Y(a));
        int rw = S(LOGIT_GUI_RECT_A_X(a) + LOGIT_GUI_RECT_B_W(b)) - x;
        int rh = S(LOGIT_GUI_RECT_A_Y(a) + LOGIT_GUI_RECT_B_H(b)) - y;
        if (rw > h->w - x) rw = h->w - x;
        if (rh > h->h - y) rh = h->h - y;
#ifdef AUI_HOST_NEGCTL_SHIFT
        x += 1;                       /* THE CONTROL: every RECT 1 device px right */
#endif
        ol_display_fill_rect(&h->disp, x, y, rw, rh, (uint32_t)c);
        note_px(h, x, y, rw, rh);
        return 0;
    }
    case SYS_GUI_RRECT: {                                         /* wm.c:2854 */
        int x = S(LOGIT_GUI_RRECT_A_X(a)), y = S(LOGIT_GUI_RRECT_A_Y(a));
        int rw = S(LOGIT_GUI_RRECT_A_X(a) + LOGIT_GUI_RRECT_B_W(b)) - x;
        int rh = S(LOGIT_GUI_RRECT_A_Y(a) + LOGIT_GUI_RRECT_B_H(b)) - y;
        int radius = S(LOGIT_GUI_RRECT_C_RADIUS(c));
        if (rw > h->w - x) rw = h->w - x;
        if (rh > h->h - y) rh = h->h - y;
#ifdef AUI_HOST_NEGCTL_SHIFT
        x += 1;
#endif
        ol_display_round_rect(&h->disp, x, y, rw, rh, radius, (uint32_t)LOGIT_GUI_RRECT_C_COLOR(c));
        note_px(h, x, y, rw, rh);
        return 0;
    }
    case SYS_GUI_TEXT: {                                          /* wm.c:2867 */
        int x = S(LOGIT_GUI_TEXT_A_X(a)), y = S(LOGIT_GUI_TEXT_A_Y(a));
        const char *s = (const char *)c;
        if (!s) return -1;
        /* fb_text draws at the scaled UI size: fb_ui_px() = TEXT_UI_PX(16)*pct/100 */
        draw_boxes(h, x, y, 16 * h->pct / 100, 0, (uint32_t)b, s, (int)strlen(s));
        return 0;
    }
    case SYS_GUI_TEXT_MONO: {                                     /* wm.c:2877 */
        int x = S(LOGIT_GUI_TEXT_MONO_A_X(a)), y = S(LOGIT_GUI_TEXT_MONO_A_Y(a));
        int cell = S(LOGIT_GUI_TEXT_MONO_B_CELL(b));
        uint32_t color = (uint32_t)LOGIT_GUI_TEXT_MONO_B_COLOR(b);
        const char *s = (const char *)c;
        if (!s) return -1;
        draw_boxes(h, x, y, 16 * h->pct / 100, cell, color, s, (int)strlen(s));
        return 0;
    }
    case SYS_GUI_TEXT_RUN: {                                      /* wm.c:3137 */
        const struct logit_run *r = (const struct logit_run *)a;
        if (!r || r->px < 1 || r->px > 512) return -1;
        int len = r->len; if (len < 0) len = 0; if (len > 1023) len = 1023;
        draw_boxes(h, S(r->x), S(r->y), S(r->px), 0, r->color, r->s, len);
        return 0;
    }
    case SYS_GUI_BLIT: {                                          /* wm.c:3188 */
        const struct logit_blit *bl = (const struct logit_blit *)a;
        if (!bl || bl->sw <= 0 || bl->sh <= 0 || bl->sw > 4096 || bl->sh > 4096) return -1;
        if (bl->w <= 0 || bl->h <= 0) return -1;
        int bx = S(bl->x), by = S(bl->y);
        int dw = S(bl->x + bl->w) - bx, dh = S(bl->y + bl->h) - by;
        ol_display_blit_rgba(&h->disp, bx, by, dw, dh, bl->rgba, bl->sw, bl->sh);   /* fb_blit_rgba */
        note_px(h, bx, by, dw, dh);
        return 0;
    }
    case SYS_GUI_ICON: {                                          /* wm.c:2889 */
        int x = S(LOGIT_GUI_ICON_A_X(a)), y = S(LOGIT_GUI_ICON_A_Y(a));
        int id = LOGIT_GUI_ICON_B_ID(b), px = S(LOGIT_GUI_ICON_B_PX(b));
        if (px < 1 || px > 512) return -1;
        if (id == 65535) {                    /* LOGIT_ICON_APP: name in c */
            const char *name = (const char *)c;
            unsigned hash = 5381;
            for (; name && *name; name++) hash = hash * 33 + (unsigned char)*name;
            draw_icon(h, (int)hash, x, y, px, 0x808080);
            return 0;
        }
        draw_icon(h, id, x, y, px, (uint32_t)c);
        return 0;
    }
    case SYS_GUI_GLASS: {                                         /* wm.c:2907 */
        int x = S(LOGIT_GUI_GLASS_A_X(a)), y = S(LOGIT_GUI_GLASS_A_Y(a));
        int gw = S(LOGIT_GUI_GLASS_A_X(a) + LOGIT_GUI_GLASS_B_W(b)) - x;
        int gh = S(LOGIT_GUI_GLASS_A_Y(a) + LOGIT_GUI_GLASS_B_H(b)) - y;
        int radius = S(LOGIT_GUI_GLASS_C_RADIUS(c));
        ol_display_liquid_glass(&h->disp, x, y, gw, gh, radius,
                                (uint8_t)LOGIT_GUI_GLASS_C_TR(c), (uint8_t)LOGIT_GUI_GLASS_C_TG(c),
                                (uint8_t)LOGIT_GUI_GLASS_C_TB(c), (uint8_t)LOGIT_GUI_GLASS_C_TA(c));
        note_px(h, x, y, gw, gh);
        return 0;
    }
    case SYS_GUI_CLIP: {                                          /* wm.c:3212 */
        int x = S(LOGIT_GUI_CLIP_A_X(a)), y = S(LOGIT_GUI_CLIP_A_Y(a));
        int cw = LOGIT_GUI_CLIP_B_W(b), ch = LOGIT_GUI_CLIP_B_H(b);
        if (cw == 0 && ch == 0) h->clip_on = 0;
        else {
            h->clip_on = 1; h->clx0 = x; h->cly0 = y;
            h->clx1 = x + (S(LOGIT_GUI_CLIP_A_X(a) + cw) - x);
            h->cly1 = y + (S(LOGIT_GUI_CLIP_A_Y(a) + ch) - y);
        }
        rebind(h);
        return 0;
    }
    case SYS_GUI_FLUSH:                                           /* wm.c:2918 */
        log_damage(h, 0, 0, h->w, h->h);
        return 0;
    case SYS_GUI_FLUSH_RECT: {                                    /* wm.c:2937 */
        int rx = S(LOGIT_GUI_FLUSH_RECT_A_X(a)), ry = S(LOGIT_GUI_FLUSH_RECT_A_Y(a));
        int rw = S(LOGIT_GUI_FLUSH_RECT_A_X(a) + LOGIT_GUI_FLUSH_RECT_B_W(b)) - rx;
        int rh = S(LOGIT_GUI_FLUSH_RECT_A_Y(a) + LOGIT_GUI_FLUSH_RECT_B_H(b)) - ry;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rw > h->w - rx) rw = h->w - rx;
        if (rh > h->h - ry) rh = h->h - ry;
        if (rw <= 0 || rh <= 0) log_damage(h, 0, 0, h->w, h->h);   /* degenerate == whole canvas */
        else log_damage(h, rx, ry, rw, rh);
        return 0;
    }
    case SYS_TEXT_MEASURE: {                                      /* wm.c:3134 + text_measure_dispatch.inc */
        if (b == LOGIT_TEXT_METRICS_QUERY && c == 0) {
            const struct logit_text_metrics_query *q = (const struct logit_text_metrics_query *)a;
            if (q->size != sizeof *q || q->version != LOGIT_TEXT_METRICS_VERSION) return 0;
            if (q->flags || q->len < 0 || q->len > LOGIT_TEXT_METRICS_MAX_BYTES ||
                q->px < 1 || q->px > 512 || (q->face & ~3)) return -1;
            int px = S(q->px);
            struct logit_text_metrics m;
            memset(&m, 0, sizeof m);
            m.scale_percent = h->pct;
            m.advance = aui_host_text_advance(q->s, q->len, px);
            m.baseline = m.ascent = (80 * px + 50) / 100;
            m.descent = -(20 * px + 50) / 100;
            m.line_gap = 0;
            int ht = (70 * px + 50) / 100, nonblank = 0;
            for (int i = 0; i < q->len; i++) if ((unsigned char)q->s[i] > 0x20) nonblank = 1;
            if (nonblank) {
                m.has_ink = 1; m.ink_left = 0; m.ink_right = m.advance;
                m.ink_top = m.baseline - ht; m.ink_bottom = m.baseline;
            }
            memcpy(q->out, &m, sizeof m);
            return 1;
        }
        if (b == -2) return 0;      /* carets query: unsupported unless a v2 handler says otherwise */
        const char *s = (const char *)a; int len = (int)b;
        int px = (int)((c >> 2) & 0x3FFFFFFF);
        if (len < 0 || len > 1024) return 0;
        if (px < 1 || px > 512) return 0;
        return PT(aui_host_text_advance(s, len, S(px)));
    }
    case SYS_MONOTONIC_MS:
        return (long)h->clock_ms;
    case SYS_CLOCK_GETTIME: {
        struct logit_timespec *ts = (struct logit_timespec *)b;
        if (!ts) return -1;
        ts->tv_sec = (long)(h->clock_ms / 1000);
        ts->tv_nsec = (long)(h->clock_ms % 1000) * 1000000L;
        return 0;
    }
    case SYS_GET_TIME:
        if (a) *(struct logit_time *)a = h->wall;
        return 0;
    case SYS_POLL_EVENT:
    case SYS_WAIT_EVENT: {
        if (h->evq_n == 0) {
            if (n == SYS_WAIT_EVENT) h->clock_ms += (unsigned long long)(b > 0 ? b : 0);
            return 0;
        }
        struct logit_event e = h->evq[h->evq_head];
        h->evq_head = (h->evq_head + 1) % AUI_HOST_EVQ; h->evq_n--;
        if (a) *(struct logit_event *)a = e;
        return 1;
    }
    case SYS_SCREEN_INFO:                                         /* wm.c:3228 */
        switch ((int)a) {
        case SCREEN_W:     return h->screen_w_pt;
        case SCREEN_H:     return h->screen_h_pt;
        case SCREEN_SCALE: return h->pct;
        case SCREEN_DEV_W: return aui_host_pt(h->screen_w_pt, h->pct);
        case SCREEN_DEV_H: return aui_host_pt(h->screen_h_pt, h->pct);
        }
        return -1;
    case SYS_UI_DARK:                                             /* wm.c:3014 */
        if ((int)a >= 0) h->dark = (int)a != 0;
        return h->dark;
    case SYS_GUI_WIN_STATE:                                       /* wm.c:3257 */
        switch ((int)a) {
        case WINS_W:         return h->win_w_pt;
        case WINS_H:         return h->win_h_pt;
        case WINS_ZOOMED:    return 0;
        case WINS_MINIMIZED: return h->minimized;
        case WINS_FOCUSED:   return h->focused;
        case WINS_KEY_HELD:  return 0;
        case WINS_WANT_KEYUP: return (int)b != 0;
        }
        return -1;
    case SYS_SETTING_GET:                 /* no key set: callers read their default */
        return -1;
    case SYS_GUI2_INFO:
        return (long)h->info;
    default:
        break;
    }
    if (n > SYS_GUI2_INFO && n <= AUI_HOST_V2_LAST) return AUI_HOST_UNSUPPORTED;
    h->unhandled[n >= 0 && n < AUI_HOST_NSYS ? n : AUI_HOST_NSYS - 1]++;
    return -1;
}

long aui_host_sys(long n, long a, long b, long c)
{
    struct aui_host *h = aui_host_current;
    if (!h) return -1;
    h->calls[n >= 0 && n < AUI_HOST_NSYS ? n : AUI_HOST_NSYS - 1]++;
    if (n >= 0 && n < AUI_HOST_NSYS && g_handler[n]) return g_handler[n](h, a, b, c);
    return aui_host_sys_builtin(h, n, a, b, c);
}

/* -------------------------------------------------------------------- ppm */

int aui_host_write_ppm(const struct aui_host *h, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", h->w, h->h);
    for (int i = 0; i < h->w * h->h; i++) {
        uint32_t p = h->px[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) ? -1 : 0;
}
