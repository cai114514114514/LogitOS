/* ui_aui_lists_test.c -- segmented control, tabs, lists/tables and document
 * tabs of the real ring-3 aui.c (make test-ui-aui-lists; design 4.5, 4.8, 4.11).
 *
 * The test builds aui.c with -DLOGIT_SYS_HOOK=ul_sys: every gui_* call lands in
 * ul_sys() below, which records rrects, rects, icons, text runs, path boxes,
 * shadow and glass counts and the serial log (the aims). Each case runs in a
 * forked child (aui keeps its statics at file scope). Checks, by the name that
 * is printed after FAIL:
 *
 *   segmented-geometry   track capsule fill.control, h 28, segment = max label
 *                        + 24 (icons 36), thumb h 24 inset 2, E1 (2 SHADOW2)
 *   segmented-glass2     the thumb is NOT glass: 0 GLASS2, however many frames
 *   segmented-glide      a click moves the thumb on snappy: it travels, never
 *                        jumps, settles within 160 ms and the app is then quiet;
 *                        reduced motion snaps in one frame
 *   segmented-colors     thumb #FFFFFF light / #5A5A64 dark, labels 13 pt:
 *                        500 primary on the thumb, 400 secondary elsewhere
 *   tabs-geometry        aui_tabs is a segmented control h 28 centred in w
 *   segmented-icons      icon segments are 36 wide, one 16 pt icon each
 *   list-pill            the focused selection is a solid accent pill inset 6,
 *                        row y + 1, h row - 2, r 6, onAccent text; unfocused
 *                        (EV_WINDOW_FOCUS 0) fill.inactiveSel with primary text
 *   list-merge           AUI_TBL_RANGE: one block over the whole range
 *   list-flat            no outer box: no stroke, no panel behind the body
 *   table-header         a 28 header, 12/500 secondary labels, a 1 dev strong
 *                        underline, numeric columns right-aligned
 *   sort-chevron         a header click sorts ascending (CHEVRON_U 10), again
 *                        descending (CHEVRON_D); a new column starts ascending
 *   zebra-rule           fill.zebra rows only with >= 4 columns
 *   row-h-26             row_h 26: pill h 24 at row y + 1
 *   resort-slide         re-ordered rows travel to their new y (never snap),
 *                        settle, and beyond 30 rows snap; a removed row's gap
 *                        collapses
 *   doctabs-active       the active tab is bg.content with r 8 top corners,
 *                        fused (square bottom) to the 30 pt strip's end
 *   doctabs-icons        CLOSE and PLUS are icon calls; no "x" text anywhere;
 *                        UNSAVED draws a dot, LOADING a spinner instead of the icon
 *   doctabs-actions      close / activate / new report the right action + index
 *   doctabs-springs      a new tab's width springs from 0; a removed one collapses
 *   aims                 tabs, segmented, list and table rows, and doctab tabs
 *                        publish aims at the NEW centres, none while moving
 *   rt                   Reduce Transparency: still 0 GLASS2 and 0 BACKDROP
 *   scales               125 % and 150 %: the same point geometry
 *
 * THE CONTROL (-DUI_LISTS_NEGCTL) also draws a glass thumb: it must exit 1
 * with 'FAIL segmented-glass2' and no other failing check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"

#define MAXREC 512
struct rrec { int x, y, w, h, r; unsigned col; };
struct irec { int id, x, y, px; unsigned col; };
struct trec { int x24, y24, px, weight; unsigned col; char s[48]; };
struct prec { int x0, y0, x1, y1; unsigned argb; int stroke; };

static struct world {
    unsigned info;
    int pct, dark, reduce_t, reduce_m;
    unsigned long long ms;
    int nrr, ni, nt, np, nrect, nglass, nbd, nshadow, nblit;
    struct rrec rr[MAXREC]; struct irec ic[MAXREC]; struct trec tx[MAXREC]; struct prec pa[MAXREC];
    struct { int x, y, w, h; unsigned col; } rect[MAXREC];
    char out[65536]; int nout;
    char all[262144]; int nall;     /* every serial byte since the case began */
} W = { .pct = 100, .ms = 5000 };

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); fflush(stdout); bad = 1; } while (0)

static void reset(void)
{
    W.nrr = W.ni = W.nt = W.np = W.nrect = W.nglass = W.nbd = W.nshadow = W.nblit = 0;
    W.nout = 0; W.out[0] = 0;
}

static int measure(const char *s, int len, int px)
{
    int w = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) w += px * 55 / 100;
        else if (c >= 0xE0) w += px;
        else if (c >= 0xC0) w += px * 60 / 100;
    }
    return w;
}

static int set_str(char *buf, int max, const char *v)
{
    int n = (int)strlen(v);
    if (n >= max) return -1;
    memcpy(buf, v, (size_t)n + 1);
    return n;
}

long ul_sys(long n, long a, long b, long c)
{
    switch (n) {
    case SYS_GUI2_INFO:   return a == 0 ? (long)W.info : 0;
    case SYS_SCREEN_INFO: return a == SCREEN_SCALE ? W.pct : a == SCREEN_W ? 1280 : a == SCREEN_H ? 800 : -1;
    case SYS_UI_DARK:     return W.dark;
    case SYS_MONOTONIC_MS: return (long)W.ms;
    case SYS_TEXT_MEASURE: return measure((const char *)a, (int)b, (int)(c >> 2));
    case SYS_SETTING_GET: {
        const char *k = (const char *)a;
        if (!strcmp(k, "ui.reduce_transparency")) return set_str((char *)b, (int)c, W.reduce_t ? "1" : "0");
        if (!strcmp(k, "ui.reduce_motion")) return set_str((char *)b, (int)c, W.reduce_m ? "1" : "0");
        return -1;
    }
    case SYS_WRITE:
        if (a == 1 && c > 0 && W.nout + c < (long)sizeof W.out) {
            memcpy(W.out + W.nout, (const void *)b, (size_t)c);
            W.nout += (int)c; W.out[W.nout] = 0;
        }
        if (a == 1 && c > 0 && W.nall + c < (long)sizeof W.all) {
            memcpy(W.all + W.nall, (const void *)b, (size_t)c);
            W.nall += (int)c; W.all[W.nall] = 0;
        }
        return c;
    case SYS_GUI_RRECT:
        if (W.nrr < MAXREC) {
            struct rrec *r = &W.rr[W.nrr];
            r->x = (short)((a >> 16) & 0xFFFF); r->y = (short)(a & 0xFFFF);
            r->w = (short)((b >> 16) & 0xFFFF); r->h = (short)(b & 0xFFFF);
            r->r = (int)((c >> 24) & 0xFF); r->col = (unsigned)(c & 0xFFFFFF);
        }
        W.nrr++;
        return 0;
    case SYS_GUI_RECT:
        if (W.nrect < MAXREC) {
            W.rect[W.nrect].x = (short)((a >> 16) & 0xFFFF); W.rect[W.nrect].y = (short)(a & 0xFFFF);
            W.rect[W.nrect].w = (short)((b >> 16) & 0xFFFF); W.rect[W.nrect].h = (short)(b & 0xFFFF);
            W.rect[W.nrect].col = (unsigned)(c & 0xFFFFFF);
        }
        W.nrect++;
        return 0;
    case SYS_GUI_ICON:
        if (W.ni < MAXREC) {
            struct irec *r = &W.ic[W.ni];
            r->x = (short)((a >> 16) & 0xFFFF); r->y = (short)(a & 0xFFFF);
            r->id = (int)((b >> 16) & 0xFFFF); r->px = (int)(b & 0xFFFF); r->col = (unsigned)c;
        }
        W.ni++;
        return 0;
    case SYS_GUI_TEXT_RUN2: {
        const struct logit_run2 *r = (const struct logit_run2 *)a;
        if (W.nt < MAXREC) {
            struct trec *t = &W.tx[W.nt];
            int k = r->len < 47 ? r->len : 47;
            memcpy(t->s, r->s, (size_t)k); t->s[k] = 0;
            t->x24 = r->x; t->y24 = r->y; t->px = r->px / 256; t->weight = r->weight; t->col = r->color & 0xFFFFFF;
        }
        W.nt++;
        return 0;
    }
    case SYS_GUI_TEXT_RUN: {
        const struct logit_run *r = (const struct logit_run *)a;
        if (W.nt < MAXREC) {
            struct trec *t = &W.tx[W.nt];
            int k = r->len < 47 ? r->len : 47;
            memcpy(t->s, r->s, (size_t)k); t->s[k] = 0;
            t->x24 = r->x * 256; t->y24 = r->y * 256; t->px = r->px; t->weight = r->bold ? 600 : 400; t->col = r->color & 0xFFFFFF;
        }
        W.nt++;
        return 0;
    }
    case SYS_GUI_PATH: {
        const struct logit_path2 *p = (const struct logit_path2 *)a;
        const struct logit_paint2 *pt = (const struct logit_paint2 *)b;
        if (W.np < MAXREC) {
            struct prec *r = &W.pa[W.np];
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
            for (unsigned i = 0; i + 1 < p->nxy; i += 2) {
                if (p->xy[i] < x0) x0 = p->xy[i];
                if (p->xy[i] > x1) x1 = p->xy[i];
                if (p->xy[i + 1] < y0) y0 = p->xy[i + 1];
                if (p->xy[i + 1] > y1) y1 = p->xy[i + 1];
            }
            r->x0 = x0 / 256; r->y0 = y0 / 256; r->x1 = x1 / 256; r->y1 = y1 / 256;
            r->argb = pt->argb; r->stroke = c != 0;
        }
        W.np++;
        return (W.info & LOGIT_GUI2_F_PATH) ? 0 : -3;
    }
    case SYS_GUI_GLASS2: if (getenv("UL_DEBUG")) { const struct logit_glass2 *g = (const void *)a; fprintf(stderr, "glass2 %d %d %d %d\n", g->x / 256, g->y / 256, g->w / 256, g->h / 256); } W.nglass++; return (W.info & LOGIT_GUI2_F_GLASS2) ? 0 : -3;
    case SYS_GUI_GLASS: W.nglass++; return 0;
    case SYS_GUI_BACKDROP: W.nbd++; return 0;
    case SYS_GUI_SHADOW2: W.nshadow++; return (W.info & LOGIT_GUI2_F_SHADOW2) ? 0 : -3;
    case SYS_GUI_BLIT: W.nblit++; return 0;
    default: return 0;
    }
}

#define V2 (LOGIT_GUI2_F_PATH | LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2 | LOGIT_GUI2_F_TEXT_RUN2)

static void setup(int pct, int dark)
{
    W.nall = 0; W.all[0] = 0;
    W.pct = pct; W.dark = dark; W.info = V2;
    aui_set_size(900, 700);
    ui_aim_set_app("t");
}

/* ---- frame driving ---- */
static void (*cur)(void);
static void frame(void) { aui_begin(AUI_BG); cur(); aui_end(); }
static void step(int ms, int n) { for (int i = 0; i < n; i++) { W.ms += (unsigned)ms; reset(); frame(); } }
static void ev(int type, int a, int b, int mods)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b; e.mods = mods;
    W.ms += 16; reset();
    aui_feed(&e); frame(); aui_feed_done();
}
static void click(int x, int y) { ev(EV_MOUSE_MOVE, x, y, 0); ev(EV_MOUSE, x, y, 0); ev(EV_MOUSE_UP, x, y, 0); }

/* ---- lookups in the last frame's records ---- */
static const struct rrec *rr_find(int w, int h, unsigned col)
{
    const struct rrec *f = 0;
    for (int i = 0; i < W.nrr && i < MAXREC; i++)
        if (W.rr[i].w == w && W.rr[i].h == h && (col == 0xFFFFFFFFu || W.rr[i].col == col)) f = &W.rr[i];
    return f;
}
static const struct irec *ic_find(int id, int px)
{
    const struct irec *f = 0;
    for (int i = 0; i < W.ni && i < MAXREC; i++) if (W.ic[i].id == id && W.ic[i].px == px) f = &W.ic[i];
    return f;
}
static int ic_count(int id) { int n = 0; for (int i = 0; i < W.ni && i < MAXREC; i++) n += W.ic[i].id == id; return n; }
static const struct trec *tx_find(const char *s)
{
    for (int i = 0; i < W.nt && i < MAXREC; i++) if (!strcmp(W.tx[i].s, s)) return &W.tx[i];
    return 0;
}
static int aim_get(const char *name, int *x, int *y)
{
    char key[64];
    snprintf(key, sizeof key, "] aim %s ", name);
    const char *p = strstr(W.all, key);
    if (!p) return 0;
    return sscanf(p + strlen(key), "%d %d", x, y) == 2;
}
/* the LAST aim line of `name` in the log */
static int aim_last(const char *name, int *x, int *y)
{
    char key[64]; int got = 0;
    snprintf(key, sizeof key, "] aim %s ", name);
    for (const char *p = W.all; (p = strstr(p, key)); p += strlen(key))
        got = sscanf(p + strlen(key), "%d %d", x, y) == 2;
    return got;
}

/* ===================================================================== */
static const char *const SEG[4] = { "概览", "性能", "内存", "磁盘" };
static int g_sel;
static void draw_seg(void) { aui_aim_prefix("seg"); aui_segmented(20, 30, 0, 28, SEG, 4, &g_sel); }

static void case_segmented(int arg)
{
    int dark = arg & 1, pct = (arg & 2) ? 125 : (arg & 4) ? 150 : 100;
    setup(pct, dark);
    const struct logit_tokens *T = aui_tok();
    g_sel = 0; cur = draw_seg;
    reset(); frame(); step(16, 3);
    /* label 26 + 24 = 50 per segment, 4 segments + 2 + 2 */
    const struct rrec *tr = rr_find(204, 28, T->fill_control);
    if (!tr || tr->x != 20 || tr->y != 30 || tr->r != 14)
        FAIL("segmented-geometry track: %s (x %d y %d r %d), want 204x28 capsule at 20,30 r 14 in fill.control",
             tr ? "found" : "none", tr ? tr->x : 0, tr ? tr->y : 0, tr ? tr->r : 0);
    unsigned thumb = dark ? 0x5A5A64u : 0xFFFFFFu;
    const struct rrec *th = rr_find(50, 24, thumb);
    if (!th) FAIL("segmented-colors thumb: no 50x24 rrect in %06x", thumb);
    else if (th->x != 22 || th->y != 32 || th->r != 12)
        FAIL("segmented-geometry thumb: x %d y %d r %d, want 22,32 r 12", th->x, th->y, th->r);
    if (W.nshadow != 2) FAIL("segmented-geometry E1: %d SHADOW2, want 2 (key + ambient)", W.nshadow);
    if (W.nglass != 0) FAIL("segmented-glass2: %d GLASS2 calls, the thumb must not be glass", W.nglass);
    const struct trec *l0 = tx_find("概览"), *l1 = tx_find("性能");
    if (!l0 || l0->px != 13 || l0->weight != 500 || l0->col != T->label_primary)
        FAIL("segmented-colors label on the thumb: want 13/500 label.primary (w %d col %06x)", l0 ? l0->weight : -1, l0 ? l0->col : 0);
    if (!l1 || l1->px != 13 || l1->weight != 400 || l1->col != T->label_secondary)
        FAIL("segmented-colors label elsewhere: want 13/400 label.secondary (w %d col %06x)", l1 ? l1->weight : -1, l1 ? l1->col : 0);
    /* divider 1x12 sep.strong between unselected neighbours 2|3 (segment 2 and 3), not beside the thumb */
    int div = 0;
    for (int i = 0; i < W.np; i++)
        if (W.pa[i].argb == T->sep_strong && W.pa[i].x1 - W.pa[i].x0 <= 2 && W.pa[i].y1 - W.pa[i].y0 >= 11 && W.pa[i].y1 - W.pa[i].y0 <= 13) div++;
    if (div != 2) FAIL("segmented-geometry dividers: %d, want 2 (between 1|2 and 2|3, none beside the thumb)", div);

    /* aims: at rest every segment centre */
    int x, y;
    if (!aim_get("seg-2", &x, &y) || x != 20 + 2 + 2 * 50 + 25 || y != 44) FAIL("aims seg-2 at rest: %d %d, want 147 44", x, y);

    /* click segment 2: travel, settle, aims only at rest */
    reset(); ev(EV_MOUSE_MOVE, 147, 44, 0); ev(EV_MOUSE, 147, 44, 0); ev(EV_MOUSE_UP, 147, 44, 0);
    if (g_sel != 2) FAIL("segmented-glide: click did not select (sel %d)", g_sel);
    int prev = 22, mono = 1, jumped = 0, frames = 0, last_x = 22, moved_frames = 0;
    reset();
    W.nout = 0;
    for (int i = 0; i < 40; i++) {
        step(16, 1);
        th = rr_find(50, 24, thumb);
        if (!th) continue;
        if (th->x < prev) mono = 0;
        if (th->x - prev > 90) jumped = 1;
        if (th->x != prev) moved_frames++;
        prev = last_x = th->x;
        frames = i + 1;
        if (th->x == 122 && !aui_anim_active()) break;
    }
    if (last_x != 122) FAIL("segmented-glide: thumb ended at %d, want 122", last_x);
    if (!mono || jumped || moved_frames < 3) FAIL("segmented-glide: thumb jumped or reversed (mono %d jumped %d frames %d)", mono, jumped, moved_frames);
    if (frames * 16 > 200) FAIL("segmented-glide: %d ms to settle, want <= ~160", frames * 16);
    step(16, 2);
    if (aui_anim_active() || aui_anim_wait() != 0) FAIL("segmented-glide: still requesting frames after settling");
    if (!aim_last("seg-1", &x, &y) || x != 20 + 2 + 50 + 25) FAIL("aims: seg-1 not republished at rest");
    if (!bad) printf("ok   segmented %s %d%%: geometry, E1, no glass, glide %d ms\n", dark ? "dark" : "light", pct, frames * 16);
}

/* aims must not appear while the thumb is in flight */
static void case_segmented_aims(int arg)
{
    (void)arg;
    setup(100, 0);
    g_sel = 0; cur = draw_seg;
    reset(); frame(); step(16, 2);
    ev(EV_MOUSE_MOVE, 100, 44, 0); ev(EV_MOUSE, 100, 44, 0); ev(EV_MOUSE_UP, 100, 44, 0);   /* segment 1 */
    reset();
    W.ms += 16; frame();                 /* one frame into the glide */
    if (W.out[0] && strstr(W.out, "aim seg-")) FAIL("aims: published while the thumb moves: %s", W.out);
    int moving = aui_anim_active();
    if (!moving) FAIL("aims: the glide was not in flight on the first frame after the click");
    if (!bad) printf("ok   segmented aims: none while the thumb moves\n");
}

static void case_reduced(int arg)
{
    (void)arg;
    setup(100, 0); W.reduce_m = 1;
    g_sel = 0; cur = draw_seg;
    reset(); frame(); step(16, 2);
    ev(EV_MOUSE_MOVE, 147, 44, 0); ev(EV_MOUSE, 147, 44, 0); ev(EV_MOUSE_UP, 147, 44, 0);
    reset(); W.ms += 16; frame();
    const struct rrec *th = rr_find(50, 24, 0xFFFFFF);
    if (!th || th->x != 122 || aui_anim_active()) FAIL("segmented-glide reduced motion: thumb at %d (want 122 in one frame)", th ? th->x : -1);
    if (!bad) printf("ok   segmented reduced motion: snaps\n");
}

static void case_rt(int arg)
{
    (void)arg;
    setup(100, 0); W.reduce_t = 1;
    g_sel = 1; cur = draw_seg;
    reset(); frame(); step(16, 3);
    if (W.nglass || W.nbd) FAIL("rt: %d GLASS2 %d BACKDROP with Reduce Transparency, want 0 and 0", W.nglass, W.nbd);
    if (!bad) printf("ok   reduce transparency: 0 GLASS2, 0 BACKDROP\n");
}

static const char *const TABS[3] = { "进程", "内存", "磁盘" };
static int g_tab;
static void draw_tabs(void) { aui_aim_prefix("tab"); aui_tabs(0, 10, 400, TABS, 3, &g_tab); }

static void case_tabs(int arg)
{
    (void)arg;
    setup(100, 0);
    g_tab = 1; cur = draw_tabs;
    reset(); frame(); step(16, 2);
    /* 26 + 24 = 50, 3 segments + 4 = 154, centred in 400: x 123 */
    const struct rrec *tr = rr_find(154, 28, aui_tok()->fill_control);
    if (!tr || tr->x != 123 || tr->y != 10) FAIL("tabs-geometry: %s x %d y %d, want a 154x28 track at 123,10", tr ? "found" : "none", tr ? tr->x : 0, tr ? tr->y : 0);
    int x, y;
    if (!aim_get("tab-2", &x, &y) || x != 123 + 2 + 100 + 25 || y != 24) FAIL("aims tab-2: %d %d, want 250 24", x, y);
    if (!bad) printf("ok   tabs: segmented h 28, centred\n");
}

static const int ICONS[3] = { GICON_FOLDER, GICON_GRID, GICON_GEAR };
static void draw_segicons(void) { aui_segmented_icons(10, 10, 0, 28, ICONS, 3, &g_sel); }
static void case_segicons(int arg)
{
    (void)arg;
    setup(100, 0); g_sel = 1; cur = draw_segicons;
    reset(); frame(); step(16, 2);
    if (!rr_find(112, 28, aui_tok()->fill_control)) FAIL("segmented-icons: want a 36*3+4 = 112 wide track");
    const struct irec *i1 = ic_find(GICON_GRID, 16);
    if (!i1 || i1->x != 10 + 2 + 36 + 10 || i1->y != 16) FAIL("segmented-icons: grid icon at %d,%d, want 58,16 (16 pt centred in its 36 segment)", i1 ? i1->x : -1, i1 ? i1->y : -1);
    if (!bad) printf("ok   segmented icons: 36 pt segments\n");
}

/* ------------------------------------------------------------- lists */
static struct aui_col g_cols[4];
static const char *g_cells[512];
static int g_nrow, g_nc, g_scroll, g_rsel[2], g_tsel = 1;
static struct aui_sort g_sort;
static unsigned g_flags;
static int g_rh;
static void draw_table(void)
{
    aui_aim_prefix("row");
    aui_table_ex(0, 0, 400, 200, g_cols, g_nc, g_cells, g_nrow, (g_flags & AUI_TBL_RANGE) ? g_rsel : &g_tsel,
                 &g_scroll, &g_sort, g_flags, g_rh);
}
static void fill_cells(int nc, int nrow)
{
    g_nc = nc; g_nrow = nrow;
    static char names[64][16];
    for (int i = 0; i < nrow; i++)
        for (int c = 0; c < nc; c++) {
            if (c == 0) { snprintf(names[i], sizeof names[i], "项目%d", i); g_cells[i * nc + c] = names[i]; }
            else g_cells[i * nc + c] = "1";
        }
    g_cols[0] = (struct aui_col){ "名称", 160, 0 };
    g_cols[1] = (struct aui_col){ "大小", 80, AUI_COL_NUM };
    g_cols[2] = (struct aui_col){ "种类", 80, 0 };
    g_cols[3] = (struct aui_col){ "日期", 80, AUI_COL_NUM };
}

static void case_pill(int arg)
{
    int unfocused = arg & 1, rh26 = arg & 2;
    setup(100, 0);
    fill_cells(3, 5); g_tsel = 1; g_scroll = 0; g_flags = 0; g_rh = rh26 ? 26 : 0; memset(&g_sort, 0, sizeof g_sort);
    cur = draw_table;
    const struct logit_tokens *T = aui_tok();
    int rh = rh26 ? 26 : 28;
    if (unfocused) ev(EV_WINDOW_FOCUS, 0, 0, 0);
    reset(); frame(); step(16, 6);
    /* body y 28; row 1 at 28 + rh; the pill inset 6, y + 1, h rh - 2, r 6, over the table's 400 width */
    const struct rrec *p = rr_find(388, rh - 2, unfocused ? T->fill_inactive_sel : T->accent);
    if (!p) FAIL("list-pill%s: no 388x%d rrect in %s", rh26 ? " row_h 26" : "", rh - 2, unfocused ? "fill.inactiveSel" : "accent");
    else if (p->x != 6 || p->y != 28 + rh + 1 || p->r != 6)
        FAIL("list-pill%s: x %d y %d r %d, want 6, %d, 6", rh26 ? " row_h 26" : "", p->x, p->y, p->r, 28 + rh + 1);
    if (rh26 && p) printf("ok   row-h-26: pill h %d at row y + 1\n", p->h);
    const struct trec *t = tx_find("项目1");
    unsigned want = unfocused ? T->label_primary : T->label_on_accent;
    if (!t || t->col != want) FAIL("list-pill text: %06x, want %06x (%s)", t ? t->col : 0, want, unfocused ? "label.primary" : "label.onAccent");
    /* no outer box: nothing 400 wide with a stroke radius, nor a panel behind the body */
    for (int i = 0; i < W.np; i++) if (W.pa[i].stroke && W.pa[i].x1 - W.pa[i].x0 >= 396 && W.pa[i].y1 - W.pa[i].y0 >= 100) FAIL("list-flat: a %d wide stroked outer box", W.pa[i].x1 - W.pa[i].x0);
    if (!bad) printf("ok   list pill%s: inset 6, r 6, %s\n", rh26 ? " (row_h 26)" : "", unfocused ? "fill.inactiveSel + primary" : "accent + onAccent");
}

static void case_merge(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(3, 6); g_flags = AUI_TBL_RANGE; g_rh = 0; g_rsel[0] = 1; g_rsel[1] = 3; g_scroll = 0; memset(&g_sort, 0, sizeof g_sort);
    cur = draw_table;
    reset(); frame(); step(16, 6);
    /* rows 1..3: one block, 3 * 28 - 2 = 82 high from row 1 + 1 */
    const struct rrec *p = rr_find(388, 82, aui_tok()->accent);
    if (!p || p->y != 28 + 28 + 1 || p->r != 6) FAIL("list-merge: want one 388x82 block at y 57 r 6 (got %s)", p ? "another rect" : "none");
    int n = 0;
    for (int i = 0; i < W.nrr; i++) n += W.rr[i].col == aui_tok()->accent && W.rr[i].w == 388;
    if (n != 1) FAIL("list-merge: %d accent blocks, want exactly 1", n);
    if (!bad) printf("ok   list merge: one block over a 3 row range\n");
}

static void case_header(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(3, 4); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = -1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    g_cols[1].flags = AUI_COL_NUM;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    const struct logit_tokens *T = aui_tok();
    const struct trec *h = tx_find("名称");
    if (!h || h->px != 12 || h->weight != 500 || h->col != T->label_secondary) FAIL("table-header: 名称 %d/%d col %06x, want 12/500 label.secondary", h ? h->px : 0, h ? h->weight : 0, h ? h->col : 0);
    if (h && (h->x24 / 256) != 12) FAIL("table-header: first column text at x %d, want 12 (the row text x)", h->x24 / 256);
    const struct trec *r0 = tx_find("项目0");
    if (h && r0 && h->x24 != r0->x24) FAIL("table-header: header x %d != row text x %d", h->x24, r0->x24);
    const struct trec *hn = tx_find("大小");
    if (!hn || (hn->x24 / 256) + measure("大小", 6, 12) < 160 + 80 - 9) FAIL("table-header: numeric header not right-aligned (x %d)", hn ? hn->x24 / 256 : 0);
    /* the header underline: a 1 dev strong hairline on the header's last row */
    int line = 0;
    for (int i = 0; i < W.np; i++) if (W.pa[i].argb == T->sep_strong && W.pa[i].x1 - W.pa[i].x0 >= 399 && W.pa[i].y1 - W.pa[i].y0 <= 1) line = 1;
    if (!line) FAIL("table-header: no 1 dev sep.strong underline");
    /* the last column fills: its text may start at 160 + 80 + 8 */
    const struct trec *last = tx_find("种类");
    if (!last || last->x24 / 256 != 240 + 8) FAIL("table-header: last column header at x %d, want 248", last ? last->x24 / 256 : 0);
    if (!bad) printf("ok   table header: 12/500, aligned, underline, numeric right\n");
}

static void case_sort(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(3, 4); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = -1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    if (ic_count(GICON_CHEVRON_U) + ic_count(GICON_CHEVRON_D)) FAIL("sort-chevron: a chevron with no sorted column");
    click(100, 14);                     /* header of column 0 */
    step(16, 2);
    const struct logit_tokens *T = aui_tok();
    if (g_sort.col != 0 || g_sort.desc != 0) FAIL("sort-chevron: first click -> col %d desc %d, want 0 ascending", g_sort.col, g_sort.desc);
    const struct irec *c1 = ic_find(GICON_CHEVRON_U, 10);
    if (!c1 || c1->col != T->label_tertiary) FAIL("sort-chevron: ascending is CHEVRON_U 10 label.tertiary (%s)", c1 ? "wrong colour" : "missing");
    else {
        int lw = measure("名称", 6, 12);
        if (c1->x != 12 + lw + 4) FAIL("sort-chevron: chevron at x %d, want 4 after the label (%d)", c1->x, 12 + lw + 4);
    }
    click(100, 14);
    step(16, 2);
    if (g_sort.col != 0 || g_sort.desc != 1) FAIL("sort-chevron: second click -> desc %d, want 1", g_sort.desc);
    if (!ic_find(GICON_CHEVRON_D, 10) || ic_find(GICON_CHEVRON_U, 10)) FAIL("sort-chevron: descending is CHEVRON_D alone");
    {   /* numeric column 1 (x 160..240): chevron 4 after its right-aligned label (design 4.8) */
        click(200, 14); step(16, 2);
        const struct irec *cn = ic_find(GICON_CHEVRON_U, 10);
        int want = 160 + 80 - 22 + 4;
        if (!cn) FAIL("sort-chevron-num: no chevron on a sorted numeric column");
        else if (cn->x != want) FAIL("sort-chevron-num: chevron at x %d, want %d (4 after the label)", cn->x, want);
    }
    click(300, 14);                     /* column 2 (x 240..) starts ascending */
    step(16, 2);
    if (g_sort.col != 2 || g_sort.desc != 0) FAIL("sort-chevron: other column -> col %d desc %d, want 2 ascending", g_sort.col, g_sort.desc);
    if (!bad) printf("ok   sort chevron: U, D, new column ascends\n");
}

static void case_zebra(int arg)
{
    int nc = arg;
    setup(100, 0);
    fill_cells(nc, 6); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = -1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    int z = 0;
    for (int i = 0; i < W.nrect; i++) z += W.rect[i].col == aui_tok()->fill_zebra && W.rect[i].w == 400;
    int surf = 0, hdr = 0;
    for (int i = 0; i < W.nrect; i++) {
        surf += W.rect[i].col == aui_tok()->bg_content && W.rect[i].w == 400 && W.rect[i].h == 200;
        hdr += W.rect[i].col == aui_tok()->bg_content && W.rect[i].w == 400 && W.rect[i].h == 28;
    }
    if (surf != 1 || hdr) FAIL("table-surface: %d full-widget bg.content fills (want 1), %d header-only fills (want 0): header and body are one surface", surf, hdr);
    if (nc >= 4 && z != 3) FAIL("zebra-rule: %d columns drew %d zebra rows, want 3 (odd rows of 6)", nc, z);
    if (nc < 4 && z != 0) FAIL("zebra-rule: %d columns drew %d zebra rows, want 0", nc, z);
    if (!bad) printf("ok   zebra %d columns: %d rows\n", nc, z);
}

static void case_slide(int arg)
{
    int nrow = arg;                     /* 5: slides; 40: snaps */
    setup(100, 0);
    fill_cells(2, nrow); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = -1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    const struct trec *a = tx_find("项目0");
    int y0 = a ? a->y24 : 0;
    /* reverse the first two rows: swap their pointers */
    const char *t0 = g_cells[0], *t1 = g_cells[2];
    g_cells[0] = t1; g_cells[2] = t0;
    reset(); W.ms += 16; frame();
    a = tx_find("项目0");
    int ya = a ? a->y24 : 0;
    int first = ya;
    if (nrow <= 30) {
        int travelled = 0, mono = 1, prev = first;
        for (int i = 0; i < 40; i++) {
            step(16, 1);
            a = tx_find("项目0");
            if (!a) break;
            if (a->y24 < prev) mono = 0;
            if (a->y24 != prev) travelled++;
            prev = a->y24;
            if (!aui_anim_active()) break;
        }
        int dy = prev - y0;
        if (travelled < 3 || !mono) FAIL("resort-slide: row 项目0 travelled %d frames (mono %d), want a slide", travelled, mono);
        if (dy < 28 * 256 - 400 || dy > 28 * 256 + 400) FAIL("resort-slide: ended %d (24.8) below start, want one row (7168)", dy);
        if (first - y0 > 20 * 256) FAIL("resort-slide: first frame already %d pt away: it snapped", (first - y0) / 256);
        if (!bad) printf("ok   re-sort: %d rows slide in %d frames\n", nrow, travelled);
    } else {
        if (first - y0 != 28 * 256 || aui_anim_active()) FAIL("resort-slide: %d rows must snap (moved %d, active %d)", nrow, (first - y0) / 256, aui_anim_active());
        if (!bad) printf("ok   re-sort: %d rows snap\n", nrow);
    }
}

static void case_empty(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(3, 5); g_flags = AUI_TBL_EMPTY; g_rh = 0; g_scroll = 0; g_tsel = 1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    if (tx_find("项目1") || tx_find("项目0")) FAIL("table-empty: rows drawn under AUI_TBL_EMPTY");
    if (!tx_find("名称")) FAIL("table-empty: the header is gone");
    if (!bad) printf("ok   AUI_TBL_EMPTY clears the body\n");
}

static void case_collapse(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(2, 5); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = -1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 3);
    const struct trec *a = tx_find("项目3");
    int y0 = a ? a->y24 : 0;
    for (int i = 2; i < 4; i++) { g_cells[i * 2] = g_cells[(i + 1) * 2]; g_cells[i * 2 + 1] = g_cells[(i + 1) * 2 + 1]; }
    g_nrow = 4;                         /* 项目2 removed: 项目3 and 项目4 move up one row */
    reset(); W.ms += 16; frame();
    a = tx_find("项目3");
    if (!a) { FAIL("resort-slide collapse: row 项目3 not drawn"); return; }
    int first = a->y24, ms = 16;
    if (y0 - first > 20 * 256) FAIL("resort-slide collapse: 项目3 already %d pt up on the first frame: it snapped", (y0 - first) / 256);
    for (int i = 0; i < 40; i++) { step(16, 1); ms += 16; if (!aui_anim_active()) break; }
    a = tx_find("项目3");
    if (!a || y0 - a->y24 < 28 * 256 - 400 || y0 - a->y24 > 28 * 256 + 400) FAIL("resort-slide collapse: 项目3 moved %d, want one row (7168)", a ? y0 - a->y24 : 0);
    if (ms > 160) FAIL("resort-slide collapse: %d ms, want ~140", ms);
    if (!bad) printf("ok   removed row: gap collapses in %d ms\n", ms);
}

static void case_row_aims(int arg)
{
    (void)arg;
    setup(100, 0);
    fill_cells(3, 5); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = 0; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 8);
    int x, y;
    if (!aim_last("row-3", &x, &y) || x != 200 || y != 28 + 3 * 28 + 14) FAIL("aims row-3: %d %d, want 200 126", x, y);
    /* selecting another row: the pill glides; no aim while it moves, aims after */
    click(100, 28 + 4 * 28 + 5);
    reset(); W.ms += 16; frame();
    if (strstr(W.out, "aim row-")) FAIL("aims: rows published while the pill glides");
    step(16, 20);
    if (g_tsel != 4) FAIL("list-pill: click selected %d, want 4", g_tsel);
    if (aui_anim_active()) FAIL("list-pill: still animating after 20 frames");
    reset(); step(16, 1);
    if (W.out[0]) FAIL("aims: an unchanged settled frame printed %d bytes", W.nout);
    if (!bad) printf("ok   row aims: new centres, quiet while the pill moves\n");
}

/* --------------------------------------------------------- document tabs */
static struct aui_doctab g_dt[4];
static int g_ndt, g_active;
static unsigned g_dtflags;
static int g_dtret;
static void draw_dt(void) { aui_aim_prefix("dtab"); int r = aui_doctabs(0, 0, 800, g_dt, g_ndt, &g_active, g_dtflags); if (r) g_dtret = r; }
static void dt_init(void)
{
    g_dt[0] = (struct aui_doctab){ "首页", GICON_GLOBE, 0, 11 };
    g_dt[1] = (struct aui_doctab){ "文档", GICON_DOC, AUI_DTAB_UNSAVED, 12 };
    g_dt[2] = (struct aui_doctab){ "加载中", GICON_GLOBE, AUI_DTAB_LOADING, 13 };
    g_ndt = 3; g_active = 0; g_dtflags = 0;
}

static void case_doctabs(int arg)
{
    int dark = arg;
    setup(100, dark); dt_init(); cur = draw_dt;
    const struct logit_tokens *T = aui_tok();
    reset(); frame(); step(16, 12);
    /* n 3, w 800: avail 752 / 3 = 250 -> 220. First tab at x 8. */
    int found = 0;
    for (int i = 0; i < W.np; i++)
        if (W.pa[i].argb == (0xFF000000u | T->bg_content) && W.pa[i].x0 == 8 && W.pa[i].x1 == 8 + 220 && W.pa[i].y0 == 0 && W.pa[i].y1 == 30) found = 1;
    if (!found) FAIL("doctabs-active: no bg.content path 220x30 at 8,0 (r 8 top, square bottom)");
    int strip = 0;
    for (int i = 0; i < W.nrect; i++) strip += W.rect[i].col == T->bg_toolbar && W.rect[i].w == 800 && W.rect[i].h == 30;
    if (!strip) FAIL("doctabs-active: no 800x30 bg.toolbar strip");
    const struct irec *cl = ic_find(GICON_CLOSE, 8);
    if (!cl || cl->x != 8 + 220 - 22 + 4 || cl->y != 7 + 4) FAIL("doctabs-icons: CLOSE 8 at %d,%d on the active tab, want %d,11", cl ? cl->x : -1, cl ? cl->y : -1, 8 + 220 - 22 + 4);
    const struct irec *pl = ic_find(GICON_PLUS, 14);
    if (!pl || pl->x != 8 + 660 + 4 + 5) FAIL("doctabs-icons: PLUS 14 at %d, want %d", pl ? pl->x : -1, 8 + 660 + 4 + 5);
    for (int i = 0; i < W.nt; i++) if (!strcmp(W.tx[i].s, "x") || !strcmp(W.tx[i].s, "×") || !strcmp(W.tx[i].s, "X")) FAIL("doctabs-icons: an 'x' text close button");
    /* tab 0: icon at x + 10; tab 2 loading: spinner arc, no icon at its x + 10 */
    int g0 = 0, g2 = 0;
    for (int i = 0; i < W.ni; i++) { g0 += W.ic[i].id == GICON_GLOBE && W.ic[i].x == 8 + 10; g2 += W.ic[i].id == GICON_GLOBE && W.ic[i].x == 8 + 440 + 10; }
    if (!g0 || g2) FAIL("doctabs-icons: favicon %d on tab 0 (want 1), %d on the loading tab (want 0)", g0, g2);
    /* the unsaved dot: 6 pt label.secondary circle where the close button goes on tab 1 */
    const struct rrec *dot = rr_find(6, 6, T->label_secondary);
    if (!dot || dot->x != 8 + 220 + 220 - 22 + 5) FAIL("doctabs-icons: unsaved dot %s at x %d, want a 6x6 label.secondary dot at %d", dot ? "found" : "missing", dot ? dot->x : -1, 8 + 440 - 22 + 5);
    const struct trec *tt = tx_find("首页");
    if (!tt || tt->px != 13 || tt->weight != 500 || tt->col != T->label_primary || tt->x24 / 256 != 8 + 30) FAIL("doctabs-active: title 13/500 primary at tab x + 30 (got %d/%d x %d)", tt ? tt->px : 0, tt ? tt->weight : 0, tt ? tt->x24 / 256 : 0);
    const struct trec *t2 = tx_find("文档");
    if (!t2 || t2->weight != 400 || t2->col != T->label_secondary) FAIL("doctabs-active: inactive title must be 13/400 label.secondary");
    int x, y;
    if (!aim_get("dtab-1", &x, &y) || x != 8 + 220 + 110 || y != 15) FAIL("aims dtab-1: %d %d, want 338 15", x, y);
    if (!aim_get("dtab-3", &x, &y) || x != 8 + 660 + 4 + 12) FAIL("aims dtab-3 (the + button): %d, want 684", x);
    if (!bad) printf("ok   doctabs %s: active fused, CLOSE/PLUS icons, dot, spinner, aims\n", dark ? "dark" : "light");
}

static void case_dt_actions(int arg)
{
    (void)arg;
    setup(100, 0); dt_init(); cur = draw_dt;
    reset(); frame(); step(16, 12);
    ev(EV_MOUSE_MOVE, 300, 15, 0); g_dtret = 0;
    ev(EV_MOUSE, 300, 15, 0); ev(EV_MOUSE_UP, 300, 15, 0);
    if (AUI_DT_ACTION(g_dtret) != AUI_DT_ACTIVATE || AUI_DT_INDEX(g_dtret) != 1 || g_active != 1)
        FAIL("doctabs-actions: click on tab 1 -> action %d index %d active %d", AUI_DT_ACTION(g_dtret), AUI_DT_INDEX(g_dtret), g_active);
    /* tab 1 is active now: its close button at 8 + 220 + 220 - 22 + 8 = 434, y 15 */
    ev(EV_MOUSE_MOVE, 434, 15, 0); g_dtret = 0;
    ev(EV_MOUSE, 434, 15, 0); ev(EV_MOUSE_UP, 434, 15, 0);
    if (AUI_DT_ACTION(g_dtret) != AUI_DT_CLOSE || AUI_DT_INDEX(g_dtret) != 1) FAIL("doctabs-actions: close -> action %d index %d", AUI_DT_ACTION(g_dtret), AUI_DT_INDEX(g_dtret));
    ev(EV_MOUSE_MOVE, 684, 15, 0); g_dtret = 0;
    ev(EV_MOUSE, 684, 15, 0); ev(EV_MOUSE_UP, 684, 15, 0);
    if (AUI_DT_ACTION(g_dtret) != AUI_DT_NEW) FAIL("doctabs-actions: + -> action %d", AUI_DT_ACTION(g_dtret));
    if (!bad) printf("ok   doctabs actions: activate, close, new\n");
}

static void case_dt_springs(int arg)
{
    (void)arg;
    setup(100, 0); dt_init(); cur = draw_dt;
    reset(); frame(); step(16, 12);
    /* open a fourth tab: avail 752 / 4 = 188; it springs from 0 */
    g_dt[3] = (struct aui_doctab){ "新标签页", GICON_GLOBE, 0, 14 }; g_ndt = 4;
    reset(); W.ms += 16; frame();
    const struct trec *t = tx_find("新标签页");
    int first_present = t != 0, prevx = -1, grow = 0, settled_frames = 0;
    for (int i = 0; i < 30; i++) {
        step(16, 1);
        const struct irec *pl = ic_find(GICON_PLUS, 14);
        if (!pl) continue;
        if (prevx >= 0 && pl->x != prevx) grow++;
        prevx = pl->x;
        settled_frames = i + 1;
        if (!aui_anim_active()) break;
    }
    if (grow < 2 || settled_frames * 16 > 240) FAIL("doctabs-springs: + button moved in %d frames over %d ms, want a spring of < 240 ms", grow, settled_frames * 16);
    (void)first_present;
    /* close tab 3 (the last key): its title goes on collapsing */
    g_ndt = 3;
    int widths = 0, last = 0;
    reset(); W.ms += 16; frame();
    const struct irec *pl = ic_find(GICON_PLUS, 14);
    last = pl ? pl->x : 0;
    for (int i = 0; i < 30; i++) { step(16, 1); pl = ic_find(GICON_PLUS, 14); if (pl && pl->x != last) widths++; if (pl) last = pl->x; if (!aui_anim_active()) break; }
    if (widths < 2) FAIL("doctabs-springs: closing did not collapse over time (%d changes)", widths);
    if (last != 8 + 660 + 4 + 5) FAIL("doctabs-springs: after collapsing + ended at %d, want %d", last, 8 + 660 + 4 + 5);
    if (!bad) printf("ok   doctabs springs: open from 0, collapse closed\n");
}

static void case_scale_geometry(int arg)
{
    int pct = arg;
    setup(pct, 0);
    fill_cells(3, 5); g_flags = 0; g_rh = 0; g_scroll = 0; g_tsel = 1; memset(&g_sort, 0, sizeof g_sort); g_sort.col = -1;
    cur = draw_table;
    reset(); frame(); step(16, 6);
    const struct rrec *p = rr_find(388, 26, aui_tok()->accent);
    if (!p || p->x != 6 || p->y != 57) FAIL("scales: %d %% pill at %d,%d, want 6,57 in points", pct, p ? p->x : -1, p ? p->y : -1);
    if (!bad) printf("ok   scales %d%%: the same point geometry\n", pct);
}

/* ===================================================================== */
static int failures;
static void run(const char *name, void (*fn)(int), int arg)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { bad = 0; fn(arg); fflush(stdout); _exit(bad ? 1 : 0); }
    int st = 0;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st)) { printf("FAIL %s crashed (signal %d)\n", name, WIFSIGNALED(st) ? WTERMSIG(st) : 0); failures++; }
    else if (WEXITSTATUS(st)) failures++;
}

int main(void)
{
    run("segmented light", case_segmented, 0);
    run("segmented dark", case_segmented, 1);
    run("segmented 125", case_segmented, 2);
    run("segmented 150", case_segmented, 4);
    run("segmented aims", case_segmented_aims, 0);
    run("segmented reduced", case_reduced, 0);
    run("rt", case_rt, 0);
    run("tabs", case_tabs, 0);
    run("segmented icons", case_segicons, 0);
    run("pill", case_pill, 0);
    run("pill unfocused", case_pill, 1);
    run("pill row_h 26", case_pill, 2);
    run("merge", case_merge, 0);
    run("header", case_header, 0);
    run("sort", case_sort, 0);
    run("zebra 4", case_zebra, 4);
    run("zebra 3", case_zebra, 3);
    run("slide", case_slide, 5);
    run("snap", case_slide, 40);
    run("collapse", case_collapse, 0);
    run("empty", case_empty, 0);
    run("row aims", case_row_aims, 0);
    run("doctabs light", case_doctabs, 0);
    run("doctabs dark", case_doctabs, 1);
    run("doctab actions", case_dt_actions, 0);
    run("doctab springs", case_dt_springs, 0);
    run("scale 125", case_scale_geometry, 125);
    run("scale 150", case_scale_geometry, 150);
    if (failures) { printf("FAIL ui-aui-lists: %d case(s) failed\n", failures); return 1; }
    printf("ui-aui-lists: all cases passed\n");
    return 0;
}
