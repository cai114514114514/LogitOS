/* ui_aui_nav_test.c -- make test-ui-aui-nav (ui-aui-nav; design 4.1, 4.2,
 * 4.17, 4.18, signature 6.4).
 *
 * The REAL ring-3 aui.c over the aui_host transport (tests/unit/aui_host, v2
 * mode: the real wm_draw2 / wm_backdrop handlers), with a thin recorder in
 * front of it (nav_sys, -DLOGIT_SYS_HOOK=nav_sys): PATH paints and bounding
 * boxes, BACKDROP structs, BACKDROP_CLEAR ids, GROUP opacities, TEXT_RUN2
 * runs, the serial log, and the reduce_transparency / reduce_motion settings.
 * Every case runs in a forked child (aui keeps file-scope state) at 100, 125,
 * 150 and 200 %. Checks, each printing `FAIL <name>: ...`:
 *
 *   sidebar-rect     the solid panel is ONE path: (8, 0, 220, H-8), radius 12
 *   rt-solid         Reduce Transparency: 0 BACKDROP, the bg.sidebar solid
 *   glass-backdrop   focused + BACKDROP_CACHE: exactly 1 BACKDROP, id 1, the
 *                    LOGIT_MAT_SIDEBAR tint, fill_rgb = bg.sidebar
 *   glass-off        cache bit clear, unfocused, or AUI_SB_SOLID: solid, 0 BACKDROP
 *   crossfade        focus lost: BACKDROP opacity climbs to the solid over
 *                    120 ms, then the solid and one BACKDROP_CLEAR(1); back again
 *   pill-geom        inset 6, height 28 (row y + 1), radius 6, accent.tint
 *                    focused / fill.inactiveSel unfocused
 *   single-pill      exactly ONE pill path in every frame of a glide
 *   glide            the pill moves through intermediate rows, monotone; an
 *                    adjacent row is reached in <= 140 ms, then no frame is
 *                    requested once the 3.8 settle rule holds
 *   glide-damage     every motion frame flushes inside old pill U new pill;
 *                    the switch frame inside the two rows' band
 *   glide-after-idle after 2 s idle the first frame already moved (not a jump)
 *   rm-jump          Reduce Motion: the pill is at the target in one frame
 *   hover-damage     moving the hover one row flushes at most the three rows
 *                    around it, never the rest of the frame (constant calls)
 *   toolbar-hairline only while aui_scroll_offset(scroll_id) > 0
 *   toolbar-unfocused ONE opacity group at 217 (85 %)
 *   toolbar-geom     the capsule [< | >] at (248, 5, 56, 28)
 *   inspector        slides on smooth: monotone, settles, then quiet
 *   split            live drag, clamps, release snaps to the nearest 4 pt
 *   width            divider drag live in 180..280, release snaps to 4 pt
 *   keys             up/down, Enter (OPEN), type-to-select with an 800 ms buffer
 *   aims             one aim per row at rest, none while gliding
 *   ellipsis         a long label ends in U+2026 inside its column
 *   collapse         a collapsed section's rows stop drawing and hitting
 *   disclosure       the height opens to full on smooth and settles
 *   search-slot      (12, 10, SW-24, 28), rows from y 50
 *
 * --dump DIR writes review PPMs of the Finder scene (7.1) plus a text sidecar
 * (tests/unit/ui_aui_nav_dump.py draws the real fonts and makes JPEGs);
 * symbols come from the real icons.c in that mode.
 *
 * -DUI_NAV_NEGCTL (aui_nav.inc) paints a second pill on the target row; this
 * binary must then exit 1 with 'FAIL single-pill' and no other check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui_host.h"
#include "logit_pack.h"
#include "aui.h"

void nav_dump_icon(struct aui_host *h, int id, int x, int y, int px, unsigned color);

static int bad;
#define FAILF(name, ...) do { printf("FAIL %s: ", name); printf(__VA_ARGS__); printf(" (at %d%% %s)\n", g_pct, g_dark ? "dark" : "light"); bad = 1; } while (0)
#define CHECK(c, name, ...) do { if (!(c)) FAILF(name, __VA_ARGS__); } while (0)

static int g_pct = 100, g_dark;

/* ------------------------------------------------------------ recorder */
#define NPATH 1024
struct prec { unsigned argb; int kind, stroke; long long x0, y0, x1, y1, fx, fy; };
struct rrec { int x, y, px, weight, alpha; unsigned color; char s[160]; };
static struct {
    int rt, rm, dump;
    FILE *side;
    int npath; struct prec path[NPATH];
    int nbd; struct logit_backdrop2 bd[16];
    int nclr; int clr[16];
    int ngrp; int grp[16]; int gdepth; int gstack[16];
    int nrun; struct rrec run[512];
    char out[32768]; int nout;
} T;

static void t_reset(void) { T.npath = T.nbd = T.nclr = T.ngrp = T.nrun = 0; }

long nav_sys(long n, long a, long b, long c)
{
    struct aui_host *h = aui_host_current;
    switch (n) {
    case SYS_SETTING_GET: {
        const char *k = (const char *)a, *v = 0;
        if (!strcmp(k, "ui.reduce_transparency")) v = T.rt ? "1" : "0";
        else if (!strcmp(k, "ui.reduce_motion")) v = T.rm ? "1" : "0";
        if (!v || c < 2) return -1;
        strcpy((char *)b, v);
        return 1;
    }
    case SYS_WRITE:
        if (a == 1 && c > 0 && T.nout + c < (long)sizeof T.out - 1) {
            memcpy(T.out + T.nout, (const void *)b, (size_t)c);
            T.nout += (int)c; T.out[T.nout] = 0;
        }
        return c;
    case SYS_GUI_PATH:
        if (T.npath < NPATH) {
            const struct logit_path2 *p = (const struct logit_path2 *)a;
            const struct logit_paint2 *pt = (const struct logit_paint2 *)b;
            struct prec *r = &T.path[T.npath++];
            r->argb = pt->argb; r->kind = pt->kind; r->stroke = c ? ((const struct logit_stroke2 *)c)->width : 0;
            r->x0 = r->y0 = 1LL << 40; r->x1 = r->y1 = -(1LL << 40);
            for (unsigned i = 0; i + 1 < p->nxy; i += 2) {
                long long x = p->xy[i], y = p->xy[i + 1];
                if (i == 0) { r->fx = x; r->fy = y; }
                if (x < r->x0) r->x0 = x;
                if (x > r->x1) r->x1 = x;
                if (y < r->y0) r->y0 = y;
                if (y > r->y1) r->y1 = y;
            }
        }
        break;
    case SYS_GUI_BACKDROP:
        if (T.nbd < 16) T.bd[T.nbd++] = *(const struct logit_backdrop2 *)a;
        break;
    case SYS_GUI_BACKDROP_CLEAR:
        if (T.nclr < 16) T.clr[T.nclr++] = (int)a;
        break;
    case SYS_GUI_GROUP_BEGIN: {
        const struct logit_group2 *g = (const struct logit_group2 *)a;
        if (T.ngrp < 16) T.grp[T.ngrp++] = g->opacity;
        if (T.gdepth < 16) T.gstack[T.gdepth++] = g->opacity;
        break;
    }
    case SYS_GUI_GROUP_END:
        if (T.gdepth) T.gdepth--;
        break;
    case SYS_GUI_TEXT_RUN2: {
        const struct logit_run2 *r = (const struct logit_run2 *)a;
        if (T.nrun < 512) {
            struct rrec *q = &T.run[T.nrun++];
            int len = r->len < 0 ? 0 : r->len > 159 ? 159 : r->len;
            q->x = r->x; q->y = r->y; q->px = r->px; q->weight = r->weight; q->alpha = r->alpha; q->color = r->color;
            memcpy(q->s, r->s, (size_t)len); q->s[len] = 0;
        }
        if (T.dump && T.side && h) {
            int len = r->len < 0 ? 0 : r->len, alpha = r->alpha;
            for (int i = 0; i < T.gdepth; i++) alpha = alpha * T.gstack[i] / 255;
            fprintf(T.side, "T %.3f %.3f %.3f %d %06x %d %d %d %d %d %d ",
                    r->x * h->pct / 25600.0, r->y * h->pct / 25600.0, r->px * h->pct / 25600.0,
                    r->weight ? r->weight : 400, r->color & 0xFFFFFF, alpha,
                    h->clip_on, h->clx0, h->cly0, h->clx1, h->cly1);
            for (int i = 0; i < len; i++) fprintf(T.side, "%02x", (unsigned char)r->s[i]);
            fprintf(T.side, "\n");
            h->calls[n]++;
            return 0;
        }
        break;
    }
    case SYS_GUI_ICON:
        if (T.dump && h) {
            int x = aui_host_pt(LOGIT_GUI_ICON_A_X(a), h->pct), y = aui_host_pt(LOGIT_GUI_ICON_A_Y(a), h->pct);
            int id = LOGIT_GUI_ICON_B_ID(b), px = aui_host_pt(LOGIT_GUI_ICON_B_PX(b), h->pct);
            h->calls[n]++;
            nav_dump_icon(h, id, x, y, px, (unsigned)c);
            return 0;
        }
        break;
    default:
        break;
    }
    return aui_host_sys(n, a, b, c);
}

/* ------------------------------------------------------------- world */
static struct aui_host *H;
static int D(int pt) { return aui_host_pt(pt, g_pct); }                 /* points -> device */
static long long D24(long long pt24) { return pt24 * g_pct / 100; }      /* 24.8 pt -> 24.8 dev */
static int Dr(long long pt24) { return (int)((D24(pt24) + 128) >> 8); } /* ... nearest device px */

static void world(int w, int h, int pct, int dark, unsigned extra_info)
{
    g_pct = pct;
    H = aui_host_new(w, h, pct);
    aui_host_enable_v2(H);
    H->info |= extra_info;
    H->dark = dark;
    H->clock_ms = 10000;
    aui_set_size(w, h);
}

static void (*scene_fn)(void);
static void frame(void)
{
    t_reset();
    aui_host_stats_reset(H);
    aui_begin(0);
    scene_fn();
    aui_end();
}
static void step(int ms) { aui_host_clock_advance((unsigned long long)ms); frame(); }
static void feed(int type, int a, int b, int key)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b; e.button = EV_BTN_LEFT;
    if (type == EV_KEY) e.a = key;
    aui_feed(&e);
    frame();
    aui_feed_done();
}
/* Run frames 16 ms apart while aui asks for them; returns ms used (or -1). */
static int settle(int max_ms)
{
    int t = 0;
    while (aui_anim_wait() > 0 || aui_anim_active() > 0) {
        if (t >= max_ms) return -1;
        step(16); t += 16;
    }
    return t;
}

static int D(int pt);
static int Dr(long long pt24);
/* Fill paths of exactly argb; `sb` keeps those inside the sidebar column
 * (x 8..228) and at least 20 pt tall: the pill, not a toolbar wash or a line. */
static int path_ok_(const struct prec *p, unsigned argb, int sb)
{
    if (p->argb != argb || p->stroke) return 0;
    if (!sb) return 1;
    return Dr(p->x0) >= D(8) && Dr(p->x1) <= D(8 + 280) && Dr(p->y1) - Dr(p->y0) >= D(20);
}
static int path_count_in(unsigned argb, int sb)
{
    int n = 0;
    for (int i = 0; i < T.npath; i++) if (path_ok_(&T.path[i], argb, sb)) n++;
    return n;
}
static const struct prec *path_find_in(unsigned argb, int sb)
{
    for (int i = 0; i < T.npath; i++) if (path_ok_(&T.path[i], argb, sb)) return &T.path[i];
    return 0;
}
#define path_count(a) path_count_in((a), 0)
#define path_find(a) path_find_in((a), 0)
#define pill_count(a) path_count_in((a), 1)
#define pill_find(a) path_find_in((a), 1)
static unsigned px_at(int x, int y) { return H->px[(size_t)y * H->w + x] & 0xFFFFFFu; }
static unsigned blend(unsigned under, unsigned argb)
{
    int a = (int)(argb >> 24);
    unsigned o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int s = (int)((argb >> sh) & 255), d = (int)((under >> sh) & 255);
        o |= (unsigned)((s * a + d * (255 - a) + 127) / 255) << sh;
    }
    return o;
}
static int near_rgb(unsigned a, unsigned b, int tol)
{
    for (int sh = 0; sh <= 16; sh += 8) {
        int d = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

/* ------------------------------------------------- the Finder scene (7.1) */
#define FW 1080
#define FH 600
#define SBX 8
#define TBX 236
#define IW 280
static struct {
    int sel, insp, off, disc, hover_row;
    unsigned sbflags;
    int ret[16];
    int iw, dh;
    int split_pos;
} S = { 0, 1, 0, 1, -1, AUI_SB_TINT, {0}, 0, 0, 200 };

/* row index -> sidebar-local y (search off: sections at 20 / 152 / 254) */
static const int ROW_Y[8] = { 42, 72, 102, 174, 204, 276, 306, 336 };

static void fake_capsule(struct aui_rect r, unsigned fill)
{
    int rr = r.h / 2, rad[4] = { rr, rr, rr, rr };
    aui_round4(r, rad, 0xFF000000u | fill);
}

static void finder(void)
{
    const struct logit_tokens *t = aui_tok();
    static const struct { int icon; const char *label; int count; unsigned fl; } rows[8] = {
        { GICON_DOC, "文稿", 0, 0 }, { GICON_IMAGE, "媒体", 24, 0 }, { GICON_GLOBE, "网站", 0, 0 },
        { GICON_STAR, "Logit 主页", 3, 0 }, { GICON_PLUS, "新建项目", 0, AUI_SB_ROW_ACTION },
        { GICON_HOME, "LogitOS 磁盘", 0, 0 }, { GICON_GRID, "应用程序", 0, 0 }, { GICON_DOC, "字体", 0, 0 },
    };
    static const char *secs[3] = { "收藏", "我的项目", "位置" };
    static const int sec_at[3] = { 0, 3, 5 };
    int W = aui_width(), Hh = aui_height(), i, s = 0;
    int iw = aui_inspector_reflow_w(2), cx = TBX, cw = W - TBX - iw;
    S.iw = iw;
    aui_fill(0, 0, W, Hh, t->bg_content);

    /* content first: the list, scrolling under the toolbar */
    aui_scroll_begin(cx, 38, cw, Hh - 38 - 28, &S.off, 40 * 28);
    aui_fill(0, 0, cw, 28, t->bg_content);
    aui_text_w(12, 6, 12, 500, t->label_secondary, "名称");
    aui_text_w(cw - 340 > 220 ? cw - 340 : 220, 6, 12, 500, t->label_secondary, "修改日期");
    aui_hairline_dev(0, 27, cw, 27, t->sep_strong);
    for (i = 0; i < 14; i++) {
        static const char *names[7] = { "项目计划.txt", "设计稿", "图标", "notes.md", "照片", "启动脚本.as", "README" };
        int y = 28 + i * 28;
        if (i == 2) { int rr[4] = { 6, 6, 6, 6 }; aui_round4(aui_r(6, y, cw - 12, 28), rr, 0xFF000000u | aui_mix(t->bg_content, t->accent, 41)); }
        if (i & 1) aui_fill(0, y, cw, 28, i == 2 ? t->bg_content : t->fill_zebra);
        gui_icon(i % 3 == 1 ? GICON_FOLDER : GICON_DOC, cx + 12, 38 - S.off + y + 6, 16, i % 3 == 1 ? 0x1F6FEB : t->label_secondary);
        aui_text_w(36, y + 5, 13, 400, t->label_primary, names[i % 7]);
        aui_text_w(cw - 340 > 220 ? cw - 340 : 220, y + 5, 13, 400, t->label_secondary, i % 2 ? "今天 13:08" : "昨天 18:40");
    }
    aui_scroll_end();
    /* path bar */
    aui_fill(cx, Hh - 28, cw, 28, t->bg_content);
    aui_hairline_dev(cx, Hh - 28, cx + cw, Hh - 28, 0xFF000000u | t->sep_opaque);
    aui_text_w(cx + 12, Hh - 22, 12, 400, t->label_secondary, "LogitOS 磁盘");
    gui_icon(GICON_CHEVRON_R, cx + 12 + 84, Hh - 18, 8, t->label_tertiary);
    aui_text_w(cx + 12 + 98, Hh - 22, 12, 400, t->label_secondary, "文稿");

    /* the sidebar */
    aui_sidebar_begin(1, aui_r(SBX, 0, 220, Hh - 8), S.sbflags);
    for (i = 0; i < 8; i++) {
        if (s < 3 && i == sec_at[s]) { aui_sidebar_section(secs[s], s == 2); s++; }
        S.ret[i] = aui_sidebar_row(rows[i].icon, rows[i].label, rows[i].count, S.sel == i, rows[i].fl);
        if (S.ret[i] == AUI_SB_PICK && !(rows[i].fl & AUI_SB_ROW_ACTION)) S.sel = i;
    }
    aui_sidebar_end();

    /* the toolbar */
    aui_toolbar_begin(aui_r(TBX, 0, W - TBX, 38), t->bg_content, 1);
    aui_tb_group_begin(2);
    aui_tb_icon(GICON_CHEVRON_L, 0);
    aui_tb_icon(GICON_CHEVRON_R, AUI_TB_OFF);
    aui_tb_group_end();
    aui_tb_title("文稿", "12 项");
    if (aui_tb_icon(GICON_SIDEBAR, AUI_TB_TRAIL | (S.insp ? AUI_TB_ON : 0))) S.insp = !S.insp;
    aui_tb_icon(GICON_STAR, AUI_TB_TRAIL);
    {
        struct aui_rect r = aui_tb_trailing(180);
        fake_capsule(r, t->fill_control);
        gui_icon(GICON_SEARCH, r.x + 10, r.y + 7, 14, t->label_tertiary);
        aui_text_w(r.x + 30, r.y + 5, 13, 400, t->label_tertiary, "搜索");
    }
    aui_tb_icon(GICON_PLUS, AUI_TB_TRAIL);
    aui_toolbar_end();

    /* the inspector */
    if (aui_inspector_begin(2, aui_r(W - IW, 38, IW, Hh - 38), S.insp)) {
        struct aui_rect r;
        aui_row(&r, AUI_FILL, 96);
        {
            int rr = 14;
            aui_squircle(aui_r(r.x + (r.w - 64) / 2, r.y + 8, 64, 64), rr, 0xFF000000u | 0x5E5CE6);
            gui_icon(GICON_FOLDER, W - S.iw + 16 + (r.w - 32) / 2, 38 + r.y + 24, 32, 0xFFFFFF);
            aui_text_vc(aui_r(r.x, r.y + 76, r.w, 20), "文稿", 14, 600, t->label_primary, AUI_ALIGN_CENTER);
        }
        aui_inspector_group("信息");
        {
            static const char *k[3] = { "种类", "大小", "修改时间" }, *v[3] = { "文件夹", "12 项", "今天 13:08" };
            for (i = 0; i < 3; i++) {
                aui_row(&r, AUI_FILL, 20);
                aui_text_vc(r, k[i], 13, 400, t->label_secondary, AUI_ALIGN_LEFT);
                aui_text_vc(r, v[i], 13, 400, t->label_primary, AUI_ALIGN_RIGHT);
            }
        }
        aui_inspector_group("标签");
        aui_disclosure(3, "更多信息", &S.disc);
        S.dh = aui_disclosure_h(3, 60);
        if (S.dh > 0) {
            aui_row(&r, AUI_FILL, S.dh);
            aui_clip_push_round(r, 0);
            aui_text_vc(aui_r(r.x + 18, r.y, r.w - 18, 20), "创建于 2025年3月2日", 13, 400, t->label_secondary, AUI_ALIGN_LEFT);
            aui_text_vc(aui_r(r.x + 18, r.y + 20, r.w - 18, 20), "位置 LogitOS 磁盘", 13, 400, t->label_secondary, AUI_ALIGN_LEFT);
            aui_clip_pop();
        }
        aui_inspector_end();
    }
}

/* ------------------------------------------------------ the keys scene */
static const char *KROWS[6] = { "Desktop", "Documents", "Downloads", "Music", "Pictures",
                                "A very long label that cannot fit in the sidebar" };
static struct aui_rect K_slot;
static int K_pick[6];
static void keys_scene(void)
{
    aui_fill(0, 0, aui_width(), aui_height(), aui_tok()->bg_content);
    aui_aim_prefix("side");
    aui_sidebar_begin(4, aui_r(SBX, 0, 220, aui_height() - 8), 0);
    K_slot = aui_sidebar_search_slot();
    for (int i = 0; i < 6; i++) {
        int r = aui_sidebar_row(GICON_DOC, KROWS[i], i == 1 ? 1234 : 0, S.sel == i, 0);
        K_pick[i] = r;
        if (r == AUI_SB_PICK) S.sel = i;
    }
    aui_sidebar_end();
}

static void split_scene(void)
{
    aui_fill(0, 0, aui_width(), aui_height(), aui_tok()->bg_content);
    aui_split(5, aui_r(100, 50, 500, 300), &S.split_pos, 150, 300, 1);
}

/* ------------------------------------------------------------- cases */
static unsigned tint_of(int dark)
{
    const struct logit_material *m = logit_material(LOGIT_MAT_SIDEBAR);
    return dark ? m->tint_d : m->tint_l;
}

static void case_material(int pct)
{
    const struct logit_tokens *t;
    unsigned solid;
    const struct prec *p;
    int dark = g_dark;
    {
        /* glass: focused + cache bit */
        world(FW, FH, pct, dark, LOGIT_GUI2_F_BACKDROP_CACHE);
        scene_fn = finder; S.sel = 0; S.sbflags = AUI_SB_TINT;
        frame(); frame();
        t = aui_tok(); solid = 0xFF000000u | t->bg_sidebar;
        CHECK(T.nbd == 1 && T.bd[0].id == 1, "glass-backdrop", "focused+cache: %d BACKDROP (%llu calls), id %d", T.nbd, H->calls[SYS_GUI_BACKDROP], T.nbd ? T.bd[0].id : -1);
        if (T.nbd == 1) {
            CHECK(T.bd[0].tint_argb == (tint_of(dark) & 0xFFFFFF) && T.bd[0].opacity == (int)(tint_of(dark) >> 24),
                  "glass-backdrop", "tint %06x op %d", T.bd[0].tint_argb, T.bd[0].opacity);
            CHECK(T.bd[0].fill_rgb == t->bg_sidebar, "glass-backdrop", "fill_rgb %06x", T.bd[0].fill_rgb);
        }
        CHECK(path_count(solid) == 0, "glass-backdrop", "solid drawn under glass");

        /* crossfade out: focus lost */
        H->focused = 0;
        {
            int last = (int)(tint_of(dark) >> 24), mono = 1, mid = 0, clr = 0, ms = 0;
            struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WINDOW_FOCUS; e.a = 0;
            aui_host_clock_advance(16); aui_feed(&e); frame(); aui_feed_done(); ms = 16;
            for (;;) {
                for (int i = 0; i < T.nclr; i++) if (T.clr[i] == 1) clr++;
                if (T.nbd == 0) break;
                if (T.bd[0].opacity < last) mono = 0;
                last = T.bd[0].opacity; mid++;
                if (ms > 400) break;
                step(16); ms += 16;
            }
            CHECK(mid >= 3 && mono, "crossfade", "out: %d tinted frames, monotone %d", mid, mono);
            CHECK(ms >= 112 && ms <= 150, "crossfade", "out: solid after %d ms (120 expected)", ms);
            CHECK(clr == 1 && path_count(solid) == 1, "crossfade", "out: %d clears, solid %d", clr, path_count(solid));
            CHECK(settle(200) >= 0, "crossfade", "out: frames still requested");
        }
        /* and back in */
        H->focused = 1;
        {
            int last = 256, mono = 1, n = 0, ms = 0;
            struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WINDOW_FOCUS; e.a = 1;
            aui_host_clock_advance(16); aui_feed(&e); frame(); aui_feed_done();
            for (; ms <= 300; ms += 16) {
                if (T.nbd == 1) {
                    if (T.bd[0].opacity > last) mono = 0;
                    last = T.bd[0].opacity; n++;
                    if (T.bd[0].opacity == (int)(tint_of(dark) >> 24) && T.bd[0].tint_argb == (tint_of(dark) & 0xFFFFFF)) break;
                }
                step(16);
            }
            CHECK(n >= 3 && mono && ms <= 150, "crossfade", "in: %d frames, monotone %d, %d ms", n, mono, ms);
        }
        aui_host_free(H);

        /* RT: solid, zero BACKDROP, even focused with the cache bit */
        world(FW, FH, pct, dark, LOGIT_GUI2_F_BACKDROP_CACHE);
        T.rt = 1; scene_fn = finder;
        frame(); frame();
        t = aui_tok(); solid = 0xFF000000u | t->bg_sidebar;
        CHECK(H->calls[SYS_GUI_BACKDROP] == 0, "rt-solid", "%llu BACKDROP calls", H->calls[SYS_GUI_BACKDROP]);
        CHECK(path_count(solid) == 1, "rt-solid", "solid paths %d", path_count(solid));
        T.rt = 0;
        aui_host_free(H);

        /* cache bit clear / unfocused / AUI_SB_SOLID */
        for (int v = 0; v < 3; v++) {
            world(FW, FH, pct, dark, v == 0 ? 0 : LOGIT_GUI2_F_BACKDROP_CACHE);
            if (v == 1) H->focused = 0;
            S.sbflags = v == 2 ? (AUI_SB_TINT | AUI_SB_SOLID) : AUI_SB_TINT;
            scene_fn = finder; frame(); frame();
            t = aui_tok(); solid = 0xFF000000u | t->bg_sidebar;
            CHECK(H->calls[SYS_GUI_BACKDROP] == 0 && path_count(solid) == 1, "glass-off",
                  "variant %d: %llu BACKDROP, %d solid", v, H->calls[SYS_GUI_BACKDROP], path_count(solid));
            /* the panel: one path, (8, 0, 220, H-8), r 12 */
            p = path_find(solid);
            if (v == 0) {
                CHECK(p && Dr(p->x0) == D(SBX) && Dr(p->y0) == D(0) && Dr(p->x1) == D(SBX + 220) && Dr(p->y1) == D(FH - 8),
                      "sidebar-rect", "panel bbox %d,%d..%d,%d", p ? Dr(p->x0) : -1, p ? Dr(p->y0) : -1, p ? Dr(p->x1) : -1, p ? Dr(p->y1) : -1);
                CHECK(p && Dr(p->fx) - Dr(p->x0) == D(SBX + 12) - D(SBX) && p->fy == p->y0, "sidebar-rect",
                      "radius %d dev, expected %d", p ? Dr(p->fx) - Dr(p->x0) : -1, D(SBX + 12) - D(SBX));
            }
            S.sbflags = AUI_SB_TINT;
            aui_host_free(H);
        }
    }
}

static void check_pill(const char *when, int row, int focused)
{
    const struct logit_tokens *t = aui_tok();
    unsigned col = focused ? t->accent_tint : (0xFF000000u | t->fill_inactive_sel);
    const struct prec *p = pill_find(col);
    int y = ROW_Y[row];
    CHECK(pill_count(col) == 1, "single-pill", "%s: %d pill paths", when, pill_count(col));
    CHECK(p && Dr(p->x0) == D(SBX + 6) && Dr(p->x1) == D(SBX + 220 - 6) && Dr(p->y0) == D(y + 1) && Dr(p->y1) == D(y + 29),
          "pill-geom", "%s: pill %d,%d..%d,%d want %d,%d..%d,%d", when, p ? Dr(p->x0) : -1, p ? Dr(p->y0) : -1,
          p ? Dr(p->x1) : -1, p ? Dr(p->y1) : -1, D(SBX + 6), D(y + 1), D(SBX + 214), D(y + 29));
    CHECK(p && Dr(p->fx) - Dr(p->x0) == D(SBX + 12) - D(SBX + 6), "pill-geom", "%s: radius %d dev", when,
          p ? Dr(p->fx) - Dr(p->x0) : -1);
}

static void case_pill(int pct)
{
    int dark = g_dark;
    {
        world(FW, FH, pct, dark, 0);
        scene_fn = finder; S.sel = 1;
        frame(); frame();
        check_pill("focused", 1, 1);
        H->focused = 0;
        {
            struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WINDOW_FOCUS; e.a = 0;
            aui_feed(&e); frame(); aui_feed_done();
        }
        check_pill("unfocused", 1, 0);
        aui_host_free(H);
    }
}

static int pill_y_dev(void)
{
    const struct prec *p = pill_find(aui_tok()->accent_tint);
    return p ? Dr(p->y0) : -99999;
}

/* One glide from row `from` to row `to` (the app's state changes between
 * frames). arrive: the first frame the pill is on the target device row;
 * quiet: the frame after which nothing more is requested (design 3.8 rule). */
static void glide(int from, int to, int arrive_max, int quiet_max)
{
    int y0 = D(ROW_Y[from] + 1), y1 = D(ROW_Y[to] + 1), prev = y0, mono = 1, mid = 0, ms = 0, ok_dmg = 1, one = 1;
    int arrive = -1, up = y1 < y0;
    int lo = from < to ? from : to, hi = from < to ? to : from;
    int bx0 = D(SBX + 6), bx1 = D(SBX + 214), by0 = D(ROW_Y[lo] + 1), by1 = D(ROW_Y[hi] + 29);
    int first = 1, ok_first = 1;
    S.sel = to;
    step(16); ms = 16;
    /* The frame that switches the selection re-weights two labels, and aui.c's
     * ad_note_text reports a text run from its x to the window's right edge,
     * padded by px / 2 (WIDEN-BEFORE-YOU-NARROW): that frame must stay in the
     * two rows' band (text pad 7) right of the sidebar's left edge. Every
     * later frame only moves the pill: old pill U new pill, exactly. */
    for (int i = 0; i < H->ndmg; i++) {
        const struct aui_host_rect *d = &H->dmg[i];
        if (d->x < D(SBX) || d->y < D(ROW_Y[lo] - 7) || d->y + d->h > D(ROW_Y[hi] + 37)) ok_first = 0;
    }
    for (;;) {
        int y = pill_y_dev();
        if (pill_count(aui_tok()->accent_tint) != 1) one = 0;
        if (up ? y > prev : y < prev) mono = 0;
        if (up ? (y < y0 && y > y1) : (y > y0 && y < y1)) mid++;
        if (y == y1 && arrive < 0) arrive = ms;
        for (int i = 0; i < H->ndmg && !first; i++) {
            const struct aui_host_rect *d = &H->dmg[i];
            if (d->x < bx0 || d->y < by0 || d->x + d->w > bx1 || d->y + d->h > by1) {
                if (ok_dmg) printf("  glide damage %d,%d %dx%d outside %d,%d..%d,%d\n", d->x, d->y, d->w, d->h, bx0, by0, bx1, by1);
                ok_dmg = 0;
            }
        }
        prev = y; first = 0;
        if (!(aui_anim_wait() > 0 || aui_anim_active() > 0)) break;
        if (ms > 800) break;
        step(16); ms += 16;
    }
    CHECK(one, "single-pill", "glide %d->%d: a frame without exactly one pill", from, to);
    CHECK(mono && mid >= 3, "glide", "%d->%d: monotone %d, %d intermediate frames", from, to, mono, mid);
    CHECK(prev == y1, "glide", "%d->%d: ended at %d, target %d", from, to, prev, y1);
    CHECK(arrive >= 0 && arrive <= arrive_max, "glide", "%d->%d: on target after %d ms (<= %d)", from, to, arrive, arrive_max);
    CHECK(ms <= quiet_max, "glide", "%d->%d: settled after %d ms (<= %d)", from, to, ms, quiet_max);
    CHECK(ok_dmg, "glide-damage", "%d->%d: a motion frame flushed outside old pill U new pill", from, to);
    CHECK(ok_first, "glide-damage", "%d->%d: the switch frame flushed outside the two rows", from, to);
    CHECK(aui_anim_wait() == 0, "glide", "%d->%d: still asking for frames", from, to);
    printf("  glide %d->%d at %d%%: on target %d ms, quiet %d ms, %d frames in between\n", from, to, g_pct, arrive, ms, mid);
}

static void case_glide(int pct)
{
    world(FW, FH, pct, g_dark, 0);
    scene_fn = finder; S.sel = 0;
    frame(); frame(); settle(500);
    step(16);
    glide(0, 1, 140, 260);              /* one row: snappy ~120 ms (3.8) */
    step(16);
    glide(1, 6, 200, 360);              /* across two sections */
    /* after a long idle the first frame already moves */
    {
        int y6 = D(ROW_Y[6] + 1), y2 = D(ROW_Y[2] + 1), y;
        aui_host_clock_advance(2000);
        S.sel = 2; frame();
        y = pill_y_dev();
        CHECK(y < y6 && y > y2, "glide-after-idle", "first frame after 2 s idle at %d (from %d to %d)", y, y6, y2);
        settle(500);
    }
    /* a press on a row starts the glide in the frame of the press */
    {
        int y2 = D(ROW_Y[2] + 1), y5 = D(ROW_Y[5] + 1), y;
        aui_host_clock_advance(500);
        feed(EV_MOUSE, SBX + 60, ROW_Y[5] + 15, 0);
        y = pill_y_dev();
        CHECK(S.ret[5] == AUI_SB_PICK, "glide", "press did not pick row 5 (%d)", S.ret[5]);
        CHECK(y > y2 && y < y5, "glide", "press frame pill at %d (from %d to %d)", y, y2, y5);
        feed(EV_MOUSE_UP, SBX + 60, ROW_Y[5] + 15, 0);
        settle(500);
        CHECK(pill_y_dev() == y5 && S.sel == 5, "glide", "after press: pill %d sel %d", pill_y_dev(), S.sel);
    }
    /* an action row does not take the pill */
    {
        feed(EV_MOUSE, SBX + 60, ROW_Y[4] + 15, 0);
        CHECK(S.ret[4] == AUI_SB_PICK && pill_y_dev() == D(ROW_Y[5] + 1), "glide", "action row moved the pill");
        feed(EV_MOUSE_UP, SBX + 60, ROW_Y[4] + 15, 0);
        settle(500);
    }
    /* hover damage: only the hovered row (and the one it left) */
    {
        int ok = 1;
        feed(EV_MOUSE_MOVE, SBX + 60, ROW_Y[0] + 15, 0);
        settle(400);
        feed(EV_MOUSE_MOVE, SBX + 60, ROW_Y[1] + 15, 0);
        settle(400);
        feed(EV_MOUSE_MOVE, SBX + 60, ROW_Y[2] + 15, 0);
        for (int f = 0; f < 20; f++) {
            for (int i = 0; i < H->ndmg; i++) {
                const struct aui_host_rect *d = &H->dmg[i];
                /* the row entered, the row left (fading) and the idle slot's
                 * parked rect one row further back as it takes the fade over */
                if (d->x < D(SBX) || d->x + d->w > D(SBX + 220) || d->y < D(ROW_Y[0]) || d->y + d->h > D(ROW_Y[2] + 30)) {
                    if (ok) printf("  hover damage %d,%d %dx%d (frame %d)\n", d->x, d->y, d->w, d->h, f);
                    ok = 0;
                }
            }
            if (!(aui_anim_wait() > 0)) break;
            step(16);
        }
        CHECK(ok, "hover-damage", "hover flushed outside the washes' rows 0..2");
        CHECK(aui_anim_wait() == 0, "hover-damage", "hover fade never settled");
    }
    aui_host_free(H);
}

static void case_rm(int pct)
{
    /* Reduce Motion: jump */
    world(FW, FH, pct, g_dark, 0);
    T.rm = 1; scene_fn = finder; S.sel = 0;
    frame(); frame();
    S.sel = 7; step(16);
    CHECK(pill_y_dev() == D(ROW_Y[7] + 1) && aui_anim_wait() == 0, "rm-jump", "pill at %d, wait %d", pill_y_dev(), aui_anim_wait());
    T.rm = 0;
    aui_host_free(H);
}

static void case_toolbar(int pct)
{
    int dark = g_dark;
    {
        const struct logit_tokens *t;
        int row, x;
        unsigned on;
        world(FW, FH, pct, dark, 0);
        scene_fn = finder; S.off = 0; S.sel = 0; S.insp = 1;
        frame(); frame(); settle(500);
        t = aui_tok();
        row = D(38) - 1; x = D(700);
        on = blend(t->bg_content, t->sep_hairline);
        CHECK(px_at(x, row) == t->bg_content, "toolbar-hairline", "offset 0: row %06x, want %06x", px_at(x, row), t->bg_content);
        S.off = 40; frame(); step(16); settle(400);
        CHECK(near_rgb(px_at(x, row), on, 1), "toolbar-hairline", "offset 40: row %06x, want %06x", px_at(x, row), on);
        S.off = 0; frame(); step(16); settle(400);
        CHECK(px_at(x, row) == t->bg_content, "toolbar-hairline", "back at 0: row %06x", px_at(x, row));
        CHECK(T.ngrp == 0, "toolbar-unfocused", "focused toolbar opened %d groups", T.ngrp);
        /* the capsule [< | >]: one face path at (248, 5, 56, 28) */
        {
            unsigned face = dark ? 0xFF3A3A43u : (0xFF000000u | t->bg_raised);
            const struct prec *p = path_find(face);
            CHECK(p && Dr(p->x0) == D(248) && Dr(p->y0) == D(5) && Dr(p->x1) == D(304) && Dr(p->y1) == D(33),
                  "toolbar-geom", "capsule %d,%d..%d,%d", p ? Dr(p->x0) : -1, p ? Dr(p->y0) : -1, p ? Dr(p->x1) : -1, p ? Dr(p->y1) : -1);
        }
        H->focused = 0;
        {
            struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WINDOW_FOCUS; e.a = 0;
            aui_feed(&e); frame(); aui_feed_done();
        }
        CHECK(T.ngrp == 1 && T.grp[0] == 217, "toolbar-unfocused", "%d groups, opacity %d", T.ngrp, T.ngrp ? T.grp[0] : -1);
        aui_host_free(H);
    }
}

static void case_inspector(int pct)
{
    world(FW, FH, pct, g_dark, 0);
    scene_fn = finder; S.insp = 1; S.sel = 0;
    frame(); frame();
    CHECK(S.iw == IW, "inspector", "shown: reflow %d", S.iw);
    CHECK(px_at(D(FW - IW + 100), D(560)) == aui_tok()->bg_inspector, "inspector", "panel pixel %06x", px_at(D(FW - IW + 100), D(560)));
    CHECK(px_at(D(FW - IW), D(560)) == aui_tok()->sep_opaque, "inspector", "edge pixel %06x", px_at(D(FW - IW), D(560)));
    S.insp = 0;
    {
        int prev = S.iw, mono = 1, mid = 0, ms = 0;
        step(16);
        for (; ms <= 600; ms += 16) {
            if (S.iw > prev) mono = 0;
            if (S.iw > 0 && S.iw < IW) mid++;
            prev = S.iw;
            if (!(aui_anim_wait() > 0 || aui_anim_active() > 0)) break;
            step(16);
        }
        CHECK(mono && mid >= 5 && S.iw == 0, "inspector", "hide: monotone %d, %d mid frames, end %d", mono, mid, S.iw);
        CHECK(ms <= 520, "inspector", "hide settled after %d ms", ms);
        printf("  inspector hide at %d%%: quiet after %d ms, %d frames in between\n", g_pct, ms, mid);
        CHECK(aui_anim_wait() == 0, "inspector", "quiet after hide");
    }
    S.insp = 1;
    step(16);
    CHECK(settle(600) >= 0 && S.iw == IW, "inspector", "show: end %d", S.iw);
    /* the disclosure opens on smooth */
    S.disc = 0; step(16); settle(600);
    CHECK(S.dh == 0, "disclosure", "closed height %d", S.dh);
    S.disc = 1; step(16);
    {
        int mid = 0, ms = 0;
        for (; ms < 600 && (aui_anim_wait() > 0 || aui_anim_active() > 0); ms += 16) { if (S.dh > 0 && S.dh < 60) mid++; step(16); }
        CHECK(S.dh == 60 && mid >= 3 && aui_anim_wait() == 0, "disclosure", "open: %d, %d mid frames", S.dh, mid);
    }
    aui_host_free(H);
}

static void case_split(int pct)
{
    world(800, 400, pct, g_dark, 0);
    scene_fn = split_scene; S.split_pos = 200;
    frame(); frame();
    feed(EV_MOUSE, 100 + 200, 150, 0);
    feed(EV_MOUSE_MOVE, 100 + 203, 150, 0);
    CHECK(S.split_pos == 203, "split", "live pos %d", S.split_pos);
    feed(EV_MOUSE_MOVE, 100 + 999, 150, 0);
    CHECK(S.split_pos == 300, "split", "clamped pos %d", S.split_pos);
    feed(EV_MOUSE_MOVE, 100 + 203, 150, 0);
    feed(EV_MOUSE_UP, 100 + 203, 150, 0);
    {
        int ms = settle(400);
        CHECK(S.split_pos == 204 && ms >= 0, "split", "released at 203 -> %d after %d ms", S.split_pos, ms);
    }
    /* the 1 dev divider line */
    CHECK(px_at(D(100 + 204), D(200)) == aui_tok()->sep_opaque && px_at(D(100 + 204) + 1, D(200)) == aui_tok()->bg_content,
          "split", "divider pixels %06x %06x", px_at(D(304), D(200)), px_at(D(304) + 1, D(200)));
    aui_host_free(H);
}

static void case_width(int pct)
{
    world(FW, FH, pct, g_dark, 0);
    scene_fn = finder; S.sel = 0;
    frame(); frame();
    CHECK(aui_sidebar_width(1) == 220, "width", "initial %d", aui_sidebar_width(1));
    feed(EV_MOUSE, SBX + 220 + 3, 300, 0);
    feed(EV_MOUSE_MOVE, SBX + 233 + 3, 300, 0);
    CHECK(aui_sidebar_width(1) == 233, "width", "live %d", aui_sidebar_width(1));
    feed(EV_MOUSE_MOVE, SBX + 999, 300, 0);
    CHECK(aui_sidebar_width(1) == 280, "width", "clamped %d", aui_sidebar_width(1));
    feed(EV_MOUSE_MOVE, SBX + 233 + 3, 300, 0);
    feed(EV_MOUSE_UP, SBX + 233 + 3, 300, 0);
    settle(400);
    CHECK(aui_sidebar_width(1) == 232, "width", "released at 233 -> %d", aui_sidebar_width(1));
    feed(EV_MOUSE, SBX + 232 + 3, 300, 0);
    feed(EV_MOUSE_MOVE, SBX + 10, 300, 0);
    feed(EV_MOUSE_UP, SBX + 10, 300, 0);
    settle(400);
    CHECK(aui_sidebar_width(1) == 180, "width", "dragged small -> %d", aui_sidebar_width(1));
    aui_host_free(H);
}

static void case_keys(int pct)
{
    world(600, 400, pct, g_dark, 0);
    ui_aim_set_app("navtest");
    scene_fn = keys_scene; S.sel = 0;
    frame(); frame();
    CHECK(K_slot.x == SBX + 12 && K_slot.y == 10 && K_slot.w == 220 - 24 && K_slot.h == 28, "search-slot",
          "slot %d,%d %dx%d", K_slot.x, K_slot.y, K_slot.w, K_slot.h);
    {
        const struct prec *p = pill_find(aui_tok()->accent_tint);
        CHECK(p && Dr(p->y0) == D(50 + 1) && Dr(p->y1) == D(50 + 29), "search-slot", "row 0 pill at %d..%d, want %d..%d",
              p ? Dr(p->y0) : -1, p ? Dr(p->y1) : -1, D(51), D(79));
    }
    /* aims at rest: one per row */
    {
        char want[32];
        int ok = 1;
        for (int i = 0; i < 6; i++) { snprintf(want, sizeof want, "aim side-%d ", i); if (!strstr(T.out, want)) ok = 0; }
        CHECK(ok, "aims", "missing row aims in '%.200s'", T.out);
    }
    feed(EV_MOUSE, SBX + 60, 50 + 15, 0);            /* focus the sidebar */
    feed(EV_MOUSE_UP, SBX + 60, 50 + 15, 0);
    settle(300);
    T.nout = 0; T.out[0] = 0;
    feed(EV_KEY, 0, 0, KEY_DOWN);
    CHECK(K_pick[1] == AUI_SB_PICK && S.sel == 1, "keys", "down: pick %d sel %d", K_pick[1], S.sel);
    CHECK(!strstr(T.out, "aim side-"), "aims", "aims while gliding: '%.120s'", T.out);
    settle(300);
    feed(EV_KEY, 0, 0, KEY_DOWN); settle(300);
    feed(EV_KEY, 0, 0, KEY_UP); settle(300);
    CHECK(S.sel == 1, "keys", "down, up: sel %d", S.sel);
    feed(EV_KEY, 0, 0, '\n');
    CHECK(K_pick[1] == AUI_SB_OPEN, "keys", "enter: %d", K_pick[1]);
    aui_host_clock_advance(1000);
    feed(EV_KEY, 0, 0, 'm');
    CHECK(S.sel == 3, "keys", "type m: sel %d", S.sel);
    aui_host_clock_advance(100);
    feed(EV_KEY, 0, 0, 'd');                         /* "md": no match, stays */
    aui_host_clock_advance(900);                     /* buffer expires */
    feed(EV_KEY, 0, 0, 'd');
    aui_host_clock_advance(100);
    feed(EV_KEY, 0, 0, 'o');
    aui_host_clock_advance(100);
    feed(EV_KEY, 0, 0, 'w');
    CHECK(S.sel == 2, "keys", "type d,o,w: sel %d (Downloads)", S.sel);
    settle(300);
    /* the long label is cut with U+2026 and stays in its column */
    {
        int found = 0;
        for (int i = 0; i < T.nrun; i++) {
            const char *s = T.run[i].s;
            size_t n = strlen(s);
            if (!strncmp(s, "A very", 6)) {
                found = 1;
                CHECK(n >= 3 && !strcmp(s + n - 3, "\xE2\x80\xA6"), "ellipsis", "'%s' not cut with U+2026", s);
                /* box glyphs: the harness's advance at the device size; the
                 * column is x 40 .. W - 6 - 8 */
                int adv = aui_host_text_advance(s, (int)n, D(14));
                CHECK(adv <= D(SBX + 206) - D(SBX + 40), "ellipsis", "'%s' %d dev wide, column %d", s, adv,
                      D(SBX + 206) - D(SBX + 40));
            }
        }
        CHECK(found, "ellipsis", "long label not drawn");

    }
    aui_host_free(H);
}

static void case_collapse(int pct)
{
    world(FW, FH, pct, g_dark, 0);
    scene_fn = finder; S.sel = 0;
    frame(); frame();
    {
        int n = 0;
        for (int i = 0; i < T.nrun; i++) if (!strcmp(T.run[i].s, "字体")) n++;
        CHECK(n == 1, "collapse", "open: 字体 drawn %d times", n);
    }
    feed(EV_MOUSE, SBX + 30, 254 + 8, 0);            /* the 位置 header */
    feed(EV_MOUSE_UP, SBX + 30, 254 + 8, 0);
    {
        int ms = settle(800), n = 0;
        for (int i = 0; i < T.nrun; i++) if (!strcmp(T.run[i].s, "字体") || !strcmp(T.run[i].s, "应用程序")) n++;
        CHECK(ms >= 0 && n == 0, "collapse", "collapsed: rows drawn %d (settled %d)", n, ms);
        feed(EV_MOUSE, SBX + 60, 306 + 15, 0);
        feed(EV_MOUSE_UP, SBX + 60, 306 + 15, 0);
        CHECK(S.ret[6] == 0 && S.sel == 0, "collapse", "a hidden row was hit (%d)", S.ret[6]);
    }
    aui_host_free(H);
}

/* --------------------------------------------------------------- dump */
/* In its own child, like every case: aui caches the backing scale once per
 * process (aui_scale), so a 200 % scene after a 100 % one would lay out with
 * the stale scale. */
static void dump_one(const char *dir, const char *name, int pct, int dark, void (*prep)(void))
{
    char p[512];
    pid_t pid;
    fflush(stdout);
    pid = fork();
    if (pid != 0) { int st; waitpid(pid, &st, 0); return; }
    g_dark = dark;
    world(FW, FH, pct, dark, 0);
    T.dump = 1;
    scene_fn = finder; S.sel = 0; S.insp = 1; S.off = 0; S.disc = 1; S.sbflags = AUI_SB_TINT;
    frame(); frame(); settle(800);
    if (prep) prep();
    snprintf(p, sizeof p, "%s/%s.txt", dir, name);
    T.side = fopen(p, "w");
    frame();
    if (T.side) fclose(T.side);
    T.side = 0;
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    aui_host_write_ppm(H, p);
    T.dump = 0;
    aui_host_free(H);
    fflush(stdout);
    _exit(0);
}
static void prep_hover(void) { feed(EV_MOUSE_MOVE, SBX + 60, ROW_Y[2] + 15, 0); settle(400); }
static void prep_unfocused(void)
{
    struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WINDOW_FOCUS; e.a = 0;
    H->focused = 0; aui_feed(&e); frame(); aui_feed_done(); settle(400);
}
static void prep_glide(void) { S.sel = 6; step(16); step(16); step(16); aui_host_clock_advance(16); }
static void prep_scrolled(void) { S.off = 60; frame(); step(16); settle(400); }
static void prep_hidden(void) { S.insp = 0; step(16); settle(800); }

static int dump(const char *dir)
{
    static const int pcts[2] = { 100, 200 };
    char n[64];
    for (int k = 0; k < 2; k++)
        for (int dark = 0; dark < 2; dark++) {
            snprintf(n, sizeof n, "finder_%d_%s", pcts[k], dark ? "dark" : "light");
            dump_one(dir, n, pcts[k], dark, prep_hover);
            snprintf(n, sizeof n, "scrolled_%d_%s", pcts[k], dark ? "dark" : "light");
            dump_one(dir, n, pcts[k], dark, prep_scrolled);
        }
    dump_one(dir, "unfocused_100_light", 100, 0, prep_unfocused);
    dump_one(dir, "unfocused_100_dark", 100, 1, prep_unfocused);
    dump_one(dir, "glide_mid_100_light", 100, 0, prep_glide);
    dump_one(dir, "inspector_hidden_100_light", 100, 0, prep_hidden);
    printf("dumped review scenes into %s\n", dir);
    return 0;
}

/* --------------------------------------------------------------- main */
static int run_case(const char *name, void (*fn)(int), int pct, int dark)
{
    pid_t pid;
    int st = 0;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        bad = 0; g_dark = dark;
        fn(pct);
        fflush(stdout);
        _exit(bad ? 1 : 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st)) { printf("CRASH %s at %d%% %s: signal %d\n", name, pct, dark ? "dark" : "light", WTERMSIG(st)); return 2; }
    return WEXITSTATUS(st);
}

int main(int argc, char **argv)
{
    static const int pcts[4] = { 100, 125, 150, 200 };
    static const struct { const char *name; void (*fn)(int); } cases[] = {
        { "material", case_material }, { "pill", case_pill }, { "glide", case_glide },
        { "rm", case_rm }, { "toolbar", case_toolbar }, { "inspector", case_inspector }, { "split", case_split },
        { "width", case_width }, { "keys", case_keys }, { "collapse", case_collapse },
    };
    int worst = 0, n = 0;
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--dump")) return dump(argv[2]);
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++)
        for (int k = 0; k < 4; k++)
            for (int dark = 0; dark < 2; dark++) {
                int r = run_case(cases[c].name, cases[c].fn, pcts[k], dark);
                if (r > worst) worst = r;
                n++;
            }
    if (worst == 2) { printf("FAIL ui-aui-nav: a case crashed\n"); return 2; }
    if (worst) { printf("FAIL ui-aui-nav: checks failed\n"); return 1; }
    printf("ok   ui-aui-nav: %d cases (10 x 100/125/150/200 %% x light/dark): sidebar, pill glide, material, toolbar, split, inspector\n", n);
    return 0;
}
