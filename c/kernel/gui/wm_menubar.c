/* wm_menubar.c -- the menu bar.
 *
 * The 24 pt bar along the top: its glass slab and hairline, the logo dot, the
 * menu titles' layout and hit rects, the dark-mode switch, the IME's 中/EN
 * indicator and the clock -- and the bar's share of the pointer (the switch
 * and a closed title) and of the main loop (the clock's half-second wake).
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

static void draw_clock(void);

/* The menu bar is a GLASS PANEL: it frosts whatever is behind it, so it is one
 * of the two things a damage rectangle may not cut in half (see dmg_expand). */
void menubar_box(struct drect *r) { r->x0 = 0; r->y0 = 0; r->x1 = wm_W; r->y1 = MBH; }
/* wm_glass_regions' provider: the bar is always glass. */
int wm_menubar_glass_rect(struct wm_glass_region *out, int max)
{
    if (max < 1) return 0;
    menubar_box(&out->r);
    out->radius = S(MENU_GLASS_PT);
    return 1;
}
/* The clock is the only thing on an idle desktop that changes, and it changes
 * twice a second. That used to be a full-screen recomposite; it is now this. */
static void dirty_menubar(void) { dirty_rect(0, 0, wm_W, MBH); }
/* WM-HOOK (out): see wm.h. The IME's indicator lives in the bar and ime_ui.c
 * must be able to say "it changed" without knowing MBH. */
void wm_damage_menubar(void) { wm_damage(0, 0, wm_W, MBH); }
/* menu_tog_* are DEVICE pixels: they are written here and read by the click
 * handler, which sees device mouse coordinates. Keeping the stored rect in the
 * same space as the thing it is tested against is the whole trick -- the
 * alternative (store points, convert at every comparison) is where a scaled UI
 * with unscaled hit-testing comes from. */
static int menu_tog_x, menu_tog_y, menu_tog_w = 38, menu_tog_h = 18;   /* dark-mode switch */
static int menu_ime_x, menu_ime_w; /* drawn label geometry, also the toggle hit target */
int g_menu_tx[NMENU_MAX], g_menu_tw[NMENU_MAX];          /* device px, published each frame */

void menu_title_hit(int i, struct drect *r)
{
    r->y0 = 0; r->y1 = MBH;
    r->x0 = (i == 0) ? S(8) : g_menu_tx[i] - S(14);   /* item 0's box also covers the logo dot */
    r->x1 = g_menu_tx[i] + g_menu_tw[i] + S(14);
}

/* Reported once per model, the first time its geometry is computed -- same
 * reasoning as "[wm] win N frame ..." and "[wm] expose cell ...": a test
 * driver has no business re-deriving AA-font text width in Python to find
 * where "File" is clickable, and a driver that guessed a pixel offset here is
 * exactly the kind of thing that rots silently the day the font or the title
 * text changes. Since P5 the titles change with focus, so the report does. */
static void menu_bar_layout(void)
{
    int x = S(32);                            /* unchanged from the old hardcoded "LogitOS" x */
    for (int i = 0; i < g_nmenu; i++) {
        g_menu_tx[i] = x;
        g_menu_tw[i] = fb_text_width(g_menu_title[i]);
        x += g_menu_tw[i] + S(44);            /* the old File-Edit gap, kept for the same rhythm */
    }
    if (!g_menu_reported) {
        g_menu_reported = 1;
        for (int i = 0; i < g_nmenu; i++) {
            struct drect t; menu_title_hit(i, &t);
            kprintf("[wm] menu title %d %s x0 %d y0 %d x1 %d y1 %d\n",
                    i, g_menu_title[i], t.x0, t.y0, t.x1, t.y1);
        }
    }
}

/* Frosted menu bar, composited per-frame on top of the windows: real-time blur
 * of whatever is behind it (wallpaper or a window slid up under it). */
void draw_menubar(void)
{
    /* Liquid Glass menu bar (thin -> adaptive edge band) */
    /* Top/left/right are SCREEN edges -- the slab is cut there, not ended, so
     * no bevel (see fb_liquid_glass_cut in fb.h; this call is why it exists).
     * The bottom edge keeps its rim: that hairline is the menu bar's only
     * separation from the wallpaper below it. */
    unsigned mbcut = GLASS_CUT_TOP | GLASS_CUT_LEFT | GLASS_CUT_RIGHT;
    wm_glass_panel(WM_MAT_MENUBAR, 0, 0, wm_W, MBH, MENU_GLASS_PT, mbcut, WM_GLASS_ALPHA_KIND);
    fb_blend_rect(0, MBH - S(1), wm_W, S(1), 0, 0, 0, g_ui_dark ? 70 : 28);  /* hairline */
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    fb_fill_circle(S(16), MBH / 2, S(6), ink);
    menu_bar_layout();                        /* publishes g_menu_tx/tw for the click path */
    for (int i = 0; i < g_nmenu; i++) fb_text(g_menu_tx[i], S(4), g_menu_title[i], ink);
    /* dark-mode toggle switch: track + knob (knob right = dark) */
    menu_tog_w = S(38); menu_tog_h = S(18);
    /* W - 210 until 2026-10-26: the default clock ("周一 2026-10-26  19:22:22")
     * is ~177 points from the right edge, and the switch's end at W - 172 ran
     * into its first letter. 240 leaves ~25 points to the default clock. The
     * QMP gates that click the switch (qmp_damage, qmp_monitor, qmp_repaint,
     * qmp_window) name the same constant. */
    menu_tog_x = wm_W - S(240); menu_tog_y = (MBH - menu_tog_h) / 2;
    if (g_ui_dark) fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(94, 150, 255));
    else           fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(198, 200, 208));
    int kr = menu_tog_h / 2 - S(2);
    int kx = g_ui_dark ? menu_tog_x + menu_tog_w - kr - S(3) : menu_tog_x + kr + S(3);
    fb_fill_circle(kx, menu_tog_y + menu_tog_h / 2, kr, rgb(255, 255, 255));
    /* What the switch is FOR, drawn in its knob: a sun while light, a crescent
     * moon while dark. A bare track and knob beside the clock said nothing
     * about what it would change (release tour, 2026-10-26). The crescent is a
     * disc with a second, offset disc in the knob's own white over it. */
    {
        int cy = menu_tog_y + menu_tog_h / 2, gr = kr * 5 / 8;
        if (gr < 2) gr = 2;
        if (g_ui_dark) {
            fb_fill_circle(kx, cy, gr, rgb(70, 120, 230));
            fb_fill_circle(kx + gr / 2 + 1, cy - gr / 2, gr - 1, rgb(255, 255, 255));
        } else {
            fb_fill_circle(kx, cy, gr, rgb(242, 164, 36));
        }
    }
    /* THE IME INDICATOR, and it exists because of a measurement rather than a
     * taste. Before it, the toggle changed NOTHING a user could see: the state
     * line goes to the serial console, and the candidate bar does not exist
     * until a composition is open (bar_layout returns 0x0 otherwise). Measured
     * 2026-08-28 by injecting the chord over QMP and screendumping either side
     * -- 175 changed pixels of 2,304,000, every one of them the clock.
     *
     * That silence is what made the binding unknowable from inside the machine.
     * The owner reported being certain the toggle was Ctrl+Space; the guest is
     * inert to Ctrl+Space in BOTH states (probed, 0 serial lines and 0 pixels),
     * and what answered was the HOST's own input-source switcher, with an
     * animation. The only key that replied was the wrong one.
     *
     * Drawn from top_visible() because the IME is PER WINDOW (g_on[wi]), so the
     * indicator has to name the window that would receive the next keystroke,
     * not a machine-wide mode this kernel does not have. */
    if (ime_ui_available()) {
        int on = ime_ui_enabled(top_visible());
        const char *tag = on ? "\xe4\xb8\xad" : "EN";   /* U+4E2D; ui.ttf is GB2312 */
        int tw = fb_text_width(tag);
        menu_ime_x = menu_tog_x - S(18) - tw - S(4);
        menu_ime_w = tw + S(8);
        fb_text(menu_ime_x + S(4), S(4), tag,
                on ? (g_ui_dark ? rgb(150, 195, 255) : rgb(24,  86, 200))
                   : (g_ui_dark ? rgb(146, 148, 158) : rgb(122, 124, 134)));
    }
    draw_clock();
}

static int fmt2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; return 2; }
static void draw_clock(void)
{
    static const char *wd[7] = { "周日","周一","周二","周三","周四","周五","周六" };
    struct rtc_time t; frame_rtc(&t);
    char b[48]; int p = 0;
    const char *w = wd[t.weekday % 7];
    if (wm_desktop_prefs.clock_weekday) {
        while (*w) b[p++] = *w++;          /* UTF-8: "周一" is six bytes */
        b[p++] = ' ';
    }
    if (wm_desktop_prefs.clock_date) {
        b[p++] = '0' + (t.year / 1000) % 10;
        b[p++] = '0' + (t.year / 100) % 10;
        b[p++] = '0' + (t.year / 10) % 10;
        b[p++] = '0' + t.year % 10;
        b[p++] = '-'; p += fmt2(b + p, t.month);
        b[p++] = '-'; p += fmt2(b + p, t.day);
        b[p++] = ' '; b[p++] = ' ';
    }
    int hour = wm_desktop_prefs.clock_24h ? t.hour : (t.hour % 12 ? t.hour % 12 : 12);
    p += fmt2(b + p, hour); b[p++] = ':'; p += fmt2(b + p, t.minute);
    if (wm_desktop_prefs.clock_seconds) { b[p++] = ':'; p += fmt2(b + p, t.second); }
    if (!wm_desktop_prefs.clock_24h) { b[p++] = ' '; b[p++] = t.hour < 12 ? 'A' : 'P'; b[p++] = 'M'; }
    b[p] = 0;
    fb_text(wm_W - fb_text_width(b) - S(12), S(4), b, g_ui_dark ? rgb(228, 229, 235) : rgb(40, 40, 46));
}

/* ---- the menu bar's input --------------------------------------------------
 * A press on the bar: the dark-mode switch, then a closed menu title (an open
 * menu owns the pointer through wm_menu_capture before this is asked). The
 * switch is device-pixel geometry published by draw_menubar, the same space
 * as the pointer. Returns 1 when the bar took the press. */
int wm_menubar_click(int x, int y, int btn)
{
    if (btn != EV_BTN_LEFT) return 0;
    if (in_rect(x, y, menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h)) {
        /* The theme flips about the switch's centre: where a reveal grows from. */
        wm_set_dark(!g_ui_dark, menu_tog_x + menu_tog_w / 2, menu_tog_y + menu_tog_h / 2);
        return 1;
    }
    int i = menu_title_at(x, y);
    if (i >= 0) { menu_open(i); return 1; }
    return 0;
}

/* Hover on the bar: nothing reacts to it yet. */
int wm_menubar_pointer(int x, int y) { (void)x; (void)y; return 0; }

/* Is (x,y) on the IME's 中/EN label (its drawn geometry, see draw_menubar)? */
int wm_menubar_ime_hit(int x, int y)
{
    return in_rect(x, y, menu_ime_x, 0, menu_ime_w, MBH);
}

/* The clock's cadence. The bar is damaged every half second (50 ticks of the
 * 100 Hz PIT) so the clock moves; this is that damage, and the deadline it
 * next wants to be called at, in the same timer_ticks() units as `now`. */
uint64_t wm_menubar_next_wake(uint64_t now)
{
    dirty_menubar();
    return now + 50;
}

/* Per-pass idle work for the bar: none. */
int wm_menubar_tick(uint64_t now) { (void)now; return 0; }
