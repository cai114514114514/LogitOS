/* wm_overlay.c -- the About / power panel and the Cmd+Tab switcher.
 *
 * The two modal overlays: the About / Shut Down / Restart panel (geometry,
 * buttons, painting and its share of the pointer) and the Cmd+Tab switcher
 * with the most-recently-used order it reads.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

int g_overlay = OV_NONE;
static int g_overlay_btn_hov = -1;   /* -1 none, 0 cancel, 1 the destructive action */

/* MOST RECENTLY USED, for the Cmd+Tab switcher: the focused window moves to
 * the front every time focus is published. Closed windows are filtered out
 * when the list is read, not here -- a slot can be reused, and `used` at read
 * time is the only honest answer to "is this still that window". */
int g_mru[MAXWIN], g_nmru;
void mru_touch(int wi)
{
    int k = 0;
    while (k < g_nmru && g_mru[k] != wi) k++;
    if (k == g_nmru && g_nmru < MAXWIN) g_nmru++;
    if (k >= g_nmru) k = g_nmru - 1;
    for (; k > 0; k--) g_mru[k] = g_mru[k - 1];
    g_mru[0] = wi;
}

#define AB_W S(320)
#define AB_H S(190)
#define CF_W S(300)
#define CF_H S(130)

void overlay_box(struct drect *r)
{
    int w, h;
    switch (g_overlay) {
    case OV_ABOUT:                      w = AB_W; h = AB_H; break;
    case OV_CONFIRM_SHUTDOWN:
    case OV_CONFIRM_RESTART:            w = CF_W; h = CF_H; break;
    default: r->x0 = r->y0 = r->x1 = r->y1 = 0; return;
    }
    r->x0 = (wm_W - w) / 2; r->y0 = (wm_H - h) / 2;
    r->x1 = r->x0 + w;   r->y1 = r->y0 + h;
}

static void overlay_buttons(struct drect *cancel, struct drect *ok)
{
    struct drect p; overlay_box(&p);
    int bw = S(112), bh = S(30), gap = S(14);
    int by = p.y1 - bh - S(16);
    ok->x1 = p.x1 - S(16); ok->x0 = ok->x1 - bw; ok->y0 = by; ok->y1 = by + bh;
    cancel->x1 = ok->x0 - gap; cancel->x0 = cancel->x1 - bw; cancel->y0 = by; cancel->y1 = by + bh;
}

static int overlay_button_at(int x, int y)
{
    if (g_overlay != OV_CONFIRM_SHUTDOWN && g_overlay != OV_CONFIRM_RESTART) return -1;
    struct drect c, o; overlay_buttons(&c, &o);
    if (in_rect(x, y, c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0)) return 0;
    if (in_rect(x, y, o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0)) return 1;
    return -1;
}

/* Dirty wherever it IS, THEN change state -- the same order win_set_min uses
 * and for the same reason: the box a moment ago cannot be re-derived once
 * g_overlay has already moved on. */
/* The panel's shadow is part of what it paints, so it is part of what opening
 * and closing must repaint -- see dirty_menu_dropdown() for what leaving it
 * out did to the menus. */
#define OVERLAY_SH_DY   0
#define OVERLAY_SH_BLUR wm_sh_ext(WMSH_OVERLAY)
static void dirty_overlay(void)
{
    struct drect p; overlay_box(&p);
    dirty_rect(p.x0 - OVERLAY_SH_BLUR, p.y0 - OVERLAY_SH_BLUR + OVERLAY_SH_DY,
               p.x1 - p.x0 + 2 * OVERLAY_SH_BLUR, p.y1 - p.y0 + 2 * OVERLAY_SH_BLUR);
}

void overlay_close(void)
{
    if (g_overlay == OV_NONE) return;
    dirty_overlay();
    g_overlay = OV_NONE; g_overlay_btn_hov = -1;
}

void overlay_open(int kind)
{
    g_overlay = kind; g_overlay_btn_hov = -1;
    dirty_overlay();
}

/* The panel's share of the pointer while it owns it (wm.c's
 * wm_overlay_capture does the rest). Motion: a confirm panel's hovered button. */
int wm_overlay_pointer(int x, int y)
{
    if (g_overlay == OV_CONFIRM_SHUTDOWN || g_overlay == OV_CONFIRM_RESTART) {
        int hb = overlay_button_at(x, y);
        if (hb != g_overlay_btn_hov) {
            g_overlay_btn_hov = hb;
            struct drect p; overlay_box(&p);
            dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
        }
    }
    return 1;
}

/* A press: a button, or a dismissal. */
int wm_overlay_click(int x, int y, int btn)
{
    (void)btn;
    if (g_overlay == OV_ABOUT) {
        overlay_close();                    /* an About box has nothing to click but "away" */
    } else {
        int hb = overlay_button_at(x, y);
        int shutdown = (g_overlay == OV_CONFIRM_SHUTDOWN);
        overlay_close();                    /* dirty + clear state BEFORE the noreturn call */
        if (hb == 1) { if (shutdown) kernel_poweroff(); else kernel_reboot(); }
        /* hb==0 (Cancel) or hb==-1 (click outside): already dismissed above. */
    }
    return 1;
}

/* ---- decimal formatting for the About panel -------------------------------
 * No printf in the kernel's draw path; two tiny helpers, same spirit as fmt2
 * above but unbounded (a frame count or a byte total is not two digits). */
static int str_copy(char *d, const char *s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } return i; }
static int fmt_u(char *d, unsigned long v)
{
    char tmp[24]; int n = 0;
    if (v == 0) { d[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) d[i] = tmp[n - 1 - i];
    return n;
}

void draw_overlay_panel(void)
{
    struct drect p; overlay_box(&p);
    int w = p.x1 - p.x0, h = p.y1 - p.y0;
    wm_cast_shadow(WMSH_OVERLAY, p.x0, p.y0, w, h, S(14), 256, 255);
    wm_glass_panel(WM_MAT_OVERLAY, p.x0, p.y0, w, h, OVERLAY_GLASS_PT, 0, WM_GLASS_ALPHA_KIND);
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    uint32_t dim = g_ui_dark ? rgb(172, 173, 182) : rgb(96, 96, 104);

    if (g_overlay == OV_ABOUT) {
        int tx = p.x0 + S(24), ty = p.y0 + S(20);
        fb_fill_circle(tx + S(7), ty + S(7), S(14), rgb(94, 150, 255));
        fb_text(tx + S(34), ty + S(2), "LogitOS", ink);
        ty += S(34);
        fb_text(tx, ty, "关于本机", ink); ty += S(26);
        char line[64]; int n;
        fb_text(tx, ty, "版本：M27", dim); ty += S(20);
        n = str_copy(line, "内存：");
        n += fmt_u(line + n, (unsigned long)(pmm_total_bytes() / (1024 * 1024)));
        n += str_copy(line + n, " MB"); line[n] = 0;
        fb_text(tx, ty, line, dim); ty += S(20);
        n = str_copy(line, "分辨率：");
        n += fmt_u(line + n, (unsigned long)wm_W);
        n += str_copy(line + n, " x ");
        n += fmt_u(line + n, (unsigned long)wm_H); line[n] = 0;
        fb_text(tx, ty, line, dim); ty += S(20);
        n = str_copy(line, "处理器：");
        n += fmt_u(line + n, (unsigned long)smp_cpu_count()); line[n] = 0;
        fb_text(tx, ty, line, dim);
        return;
    }

    const char *msg = g_overlay == OV_CONFIRM_SHUTDOWN ? "现在关机吗？"
                                                         : "现在重新启动吗？";
    fb_text(p.x0 + (w - fb_text_width(msg)) / 2, p.y0 + S(30), msg, ink);
    struct drect c, o; overlay_buttons(&c, &o);
    int hb = g_overlay_btn_hov;
    uint32_t cbg = hb == 0 ? (g_ui_dark ? rgb(72, 72, 82) : rgb(220, 220, 226))
                           : (g_ui_dark ? rgb(56, 56, 64) : rgb(236, 236, 240));
    fb_round_rect(c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0, S(7), cbg);
    int ctw = fb_text_width("取消");
    fb_text(c.x0 + ((c.x1 - c.x0) - ctw) / 2, c.y0 + S(7), "取消", ink);

    uint32_t okbg = hb == 1 ? rgb(255, 82, 72) : rgb(230, 60, 52);
    fb_round_rect(o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0, S(7), okbg);
    const char *olabel = g_overlay == OV_CONFIRM_SHUTDOWN ? "关机" : "重新启动";
    int otw = fb_text_width(olabel);
    fb_text(o.x0 + ((o.x1 - o.x0) - otw) / 2, o.y0 + S(7), olabel, rgb(255, 255, 255));
}

/* ===========================================================================
 * THE Cmd+Tab SWITCHER ("Fluid" P2)
 *
 * Hold Cmd, press Tab: a panel of the open windows in most-recently-used
 * order, the second one selected; Tab / Shift+Tab move the selection; letting
 * go of Cmd switches to it; Escape cancels. A quick Cmd+Tab -- released before
 * SW_SHOW_MS -- switches without ever drawing the panel, which is what makes
 * one tap flip between the two most recent windows.
 *
 * It needed an event this machine did not have: the RELEASE of Cmd. The
 * keyboard driver dropped every release until key-up existed (wm_key_up,
 * wm_mods_changed), which is why Cmd+Tab used to rotate the z-order one step
 * per press with nothing on screen. The WM loop also re-reads kbd_mods() every
 * pass while the panel is open, so a lost release cannot strand it.
 * ======================================================================== */
#define SW_SHOW_MS 150
struct wm_switcher g_sw;

static void sw_geom(int *cell, int *x0, int *y0, int *w, int *h)
{
    int c = S(100), maxw = wm_W - S(80);
    if (g_sw.n > 0 && g_sw.n * c + S(24) > maxw) c = (maxw - S(24)) / g_sw.n;
    *cell = c;
    *w = g_sw.n * c + S(24);
    *h = c + S(40);
    *x0 = (wm_W - *w) / 2;
    *y0 = (wm_H - *h) / 2;
}

void sw_box(struct drect *r)
{
    if (!g_sw.shown) { r->x0 = r->y0 = r->x1 = r->y1 = 0; return; }
    int c, x0, y0, w, h;
    sw_geom(&c, &x0, &y0, &w, &h);
    int eb = wm_sh_ext(WMSH_OVERLAY);                    /* the shadow too */
    r->x0 = x0 - eb; r->y0 = y0 - eb;
    r->x1 = x0 + w + eb; r->y1 = y0 + h + eb;
}
static void sw_glass_box(struct drect *r)
{
    int c, x0, y0, w, h;
    sw_geom(&c, &x0, &y0, &w, &h);
    r->x0 = x0; r->y0 = y0; r->x1 = x0 + w; r->y1 = y0 + h;
}
/* wm_glass_regions' provider: the About/power panel, then the switcher. */
int wm_overlay_glass_rect(struct wm_glass_region *out, int max)
{
    int n = 0;
    if (g_overlay != OV_NONE && n < max) {
        overlay_box(&out[n].r);
        out[n].radius = S(OVERLAY_GLASS_PT);
        n++;
    }
    if (g_sw.shown && n < max) {
        sw_glass_box(&out[n].r);
        out[n].radius = S(OVERLAY_GLASS_PT);
        n++;
    }
    return n;
}
static void dirty_switcher(void)
{ struct drect r; sw_box(&r); if (r.x1 > r.x0) dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0); }

void draw_switcher(void)
{
    int cell, x0, y0, w, h;
    sw_geom(&cell, &x0, &y0, &w, &h);
    wm_cast_shadow(WMSH_OVERLAY, x0, y0, w, h, S(18), 256, 255);
    wm_glass_panel(WM_MAT_SWITCHER, x0, y0, w, h, OVERLAY_GLASS_PT, 0, WM_GLASS_ALPHA_KIND);
    int isz = cell * 64 / 100;
    for (int i = 0; i < g_sw.n; i++) {
        int cx = x0 + S(12) + i * cell + cell / 2, cy = y0 + S(12) + cell / 2 - S(6);
        if (i == g_sw.sel)
            fb_blend_round_rect(x0 + S(12) + i * cell + S(4), y0 + S(8), cell - S(8), cell - S(2), S(14),
                                g_ui_dark ? 255 : 0, g_ui_dark ? 255 : 0, g_ui_dark ? 255 : 0, g_ui_dark ? 40 : 28);
        struct win *sw = &wm_wins[g_sw.list[i]];
        if (dock_app_count() > 0) dock_tile(dock_slot_for(sw), cx, cy, isz);
        else fb_fill_circle(cx, cy, isz / 2, rgb(94, 150, 255));
    }
    const char *t = wm_wins[g_sw.list[g_sw.sel]].title;
    int tw = fb_text_width(t);
    fb_text(x0 + (w - tw) / 2, y0 + h - S(30), t, g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48));
}

static void sw_close(void)
{
    if (g_sw.shown) dirty_switcher();
    g_sw.open = g_sw.shown = 0;
}

void sw_open(int dir)
{
    int n = 0, seen[MAXWIN] = {0};
    for (int k = 0; k < g_nmru; k++) {
        int wi = g_mru[k];
        if (wi < 0 || wi >= MAXWIN || seen[wi] || !wm_wins[wi].used || wm_wins[wi].kind != WK_APP || wm_wins[wi].panel) continue;
        seen[wi] = 1; g_sw.list[n++] = wi;
    }
    for (int k = wm_norder - 1; k >= 0; k--) {            /* never focused yet: z-order */
        int wi = wm_order[k];
        if (seen[wi] || !wm_wins[wi].used || wm_wins[wi].kind != WK_APP || wm_wins[wi].panel) continue;
        seen[wi] = 1; g_sw.list[n++] = wi;
    }
    if (n < 2) return;
    g_sw.n = n;
    g_sw.sel = dir < 0 ? n - 1 : 1;
    g_sw.open = 1; g_sw.shown = 0;
    g_sw.t0 = anim_stamp();
    kprintf("[wm] switcher open n=%d sel=%d\n", n, g_sw.sel);
}

void sw_step(int dir)
{
    g_sw.sel = (g_sw.sel + (dir < 0 ? g_sw.n - 1 : 1)) % g_sw.n;
    if (g_sw.shown) dirty_switcher();
    kprintf("[wm] switcher step sel=%d\n", g_sw.sel);
}

void sw_commit(void)
{
    int wi = g_sw.list[g_sw.sel];
    sw_close();
    if (wi < 0 || wi >= MAXWIN || !wm_wins[wi].used) return;
    win_set_min(&wm_wins[wi], 0);
    raise_win(wi);
    dirty_full(FULL_ZORDER_SYSTEM);
    kprintf("[wm] switcher commit win=%d '%s'\n", wi, wm_wins[wi].title);
}

void sw_cancel(void)
{
    sw_close();
    kprintf("[wm] switcher cancel\n");
}

/* Called every WM pass: reveal the panel once it has been held long enough to
 * be a browse rather than a tap, and never leave it open without Cmd. */
static void sw_tick(void)
{
    if (!g_sw.open) return;
    if (!(kbd_mods() & EV_MOD_SUPER)) { sw_commit(); return; }
    if (!g_sw.shown && (anim_elapsed(g_sw.t0) >= SW_SHOW_MS || wm_reduce_motion)) {
        g_sw.shown = 1;
        dirty_switcher();
    }
}

/* The overlay's per-pass work: the switcher's reveal and its Cmd watch
 * (sw_tick). Its damage asks for its frames. */
int wm_overlay_tick(uint64_t now)
{
    (void)now;
    sw_tick();
    return 0;
}
