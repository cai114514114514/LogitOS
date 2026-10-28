/* SPDX-License-Identifier: MIT
 * Guest probe for ui-wm-abi (tests/qmp/qmp_ui_abi.py): a plain logit.h client
 * that exercises the shell ABI this item adds. Two builds of this file:
 *
 *   /bin/ui-abi-probe   (default) the symbol sheet window:
 *     - SYS_GUI2_INFO(0)                         -> "UIABI info=<hex>"
 *     - every set C symbol through SYS_GUI_ICON2 at 16 and 20 pt, weights
 *       1..3, on a light and on a dark card, plus the Logit mark at 14..96 pt
 *       and white on the brand-gradient tile (design 3.7.5) -- the review
 *       sheet of the acceptance shots;
 *     - the weight probe: ICON_SUN at 64 pt, weight 1 in box W1 and weight 3
 *       in box W3, black on white                -> "UIABI icon2 rc1=.. rc3=.."
 *     - WINS_SET_TITLEBAR 0x01F2F2F6 (TB_NO_SEP | #F2F2F6), then an undefined
 *       bit (must be -1)                         -> "UIABI tb rc=.. bad=.."
 *     - WINS_SET_SIZE on a normal window (must be -1) -> "UIABI size-normal rc=.."
 *     - key d / l: ui_set_dark_at(1 / 0, 200, 100) -> "UIABI dark-at on=.. rc=.."
 *     - EV_THEME                                 -> "UIABI theme dark=<0|1>"
 *   /bin/ui-abi-panel   (-DUI_ABI_PANEL) a panel, PW1 x PH1, flat card:
 *     - WINS_SET_PANEL on start                  -> "UIABI panel ready w=.. h=.."
 *     - key g: WINS_SET_SIZE(PW2 x PH2)          -> "UIABI panel grow rc=.."
 *     - key s: WINS_SET_SIZE(PW1 x PH1)          -> "UIABI panel shrink rc=.."
 *     - every frame                              -> "UIABI panel frame w=.. h=.."
 *   /bin/ui-abi-panel-neg (-DUI_ABI_PANEL -DUI_ABI_NEGCTL) THE CONTROL: the
 *     same panel, but g and s only print their line and never call
 *     WINS_SET_SIZE, so the driver must report FAIL panel-resize.
 *
 * All drawing is deterministic (no clock, no animation), so two frames of the
 * same size are identical pixels: the driver's stale-pixel diff relies on it.
 * Keys: Esc / close exits. */
#include "logit.h"

/* The probe links no libc. rep-string forms cannot be rewritten into calls
 * to themselves. */
void *memcpy(void *d, const void *s, unsigned long n)
{ void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r; }
void *memset(void *d, int c, unsigned long n)
{ void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }

static void put_s(char *b, int *n, const char *s) { while (*s) b[(*n)++] = *s++; }
static void put_hex(char *b, int *n, unsigned v)
{
    for (int s = 28; s >= 0; s -= 4) b[(*n)++] = "0123456789abcdef"[(v >> s) & 15];
}
static void put_dec(char *b, int *n, long v)
{
    char d[24]; int k = 0;
    if (v < 0) { b[(*n)++] = '-'; v = -v; }
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[(*n)++] = d[--k];
}
/* "UIABI <tag> <k1>=<v1> <k2>=<v2>\n" */
static void say(const char *tag, const char *k1, long v1, const char *k2, long v2)
{
    char b[120]; int n = 0;
    put_s(b, &n, "UIABI "); put_s(b, &n, tag);
    if (k1) { b[n++] = ' '; put_s(b, &n, k1); b[n++] = '='; put_dec(b, &n, v1); }
    if (k2) { b[n++] = ' '; put_s(b, &n, k2); b[n++] = '='; put_dec(b, &n, v2); }
    b[n++] = '\n';
    sys_write(1, b, n);
}

#ifndef UI_ABI_PANEL
/* ------------------------------------------------------------ the sheet */
#define SW 1000
#define SH 640

/* Layout, content points (the driver mirrors W1/W3 and the dark-at point). */
#define GRID_X   24
#define CARD_W   648
#define CARD_H   252
#define LIGHT_Y  20
#define DARK_Y   (LIGHT_Y + CARD_H + 16)
#define COLS     4
#define CELL_W   156
#define CELL_H   34
#define W1_X     712          /* weight probe boxes: 64 pt squares */
#define W3_X     808
#define WP_Y     420
#define WP_PX    64

static int g_dark;

static void rrect(int x, int y, int w, int h, int r, unsigned c)
{
    _sys(SYS_GUI_RRECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
         ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), ((long)r << 24) | (c & 0xFFFFFF));
}

/* The Logit mark tile (3.7.5, 3.1.6): a rounded square (r = 0.225 size)
 * filled with the 135 degree brand gradient, the mark in white on it. */
static void logit_tile(int x, int y, int size)
{
    int r = size * 225 / 1000;
    int x0 = x << 8, y0 = y << 8, x1 = (x + size) << 8, y1 = (y + size) << 8, rr = r << 8;
    static unsigned char verbs[10];
    static int xy[32];
    int nv = 0, nx = 0;
#define PT2(a, b) (xy[nx++] = (a), xy[nx++] = (b))
    verbs[nv++] = LOGIT_VERB_M; PT2(x0 + rr, y0);
    verbs[nv++] = LOGIT_VERB_L; PT2(x1 - rr, y0);
    verbs[nv++] = LOGIT_VERB_Q; PT2(x1, y0); PT2(x1, y0 + rr);
    verbs[nv++] = LOGIT_VERB_L; PT2(x1, y1 - rr);
    verbs[nv++] = LOGIT_VERB_Q; PT2(x1, y1); PT2(x1 - rr, y1);
    verbs[nv++] = LOGIT_VERB_L; PT2(x0 + rr, y1);
    verbs[nv++] = LOGIT_VERB_Q; PT2(x0, y1); PT2(x0, y1 - rr);
    verbs[nv++] = LOGIT_VERB_L; PT2(x0, y0 + rr);
    verbs[nv++] = LOGIT_VERB_Q; PT2(x0, y0); PT2(x0 + rr, y0);
    verbs[nv++] = LOGIT_VERB_Z;
#undef PT2
    struct logit_path2 p = {0};
    p.verbs = verbs; p.xy = xy; p.nverb = (unsigned)nv; p.nxy = (unsigned)nx;
    p.rule = LOGIT_PATH2_NONZERO;
    static const unsigned pos[3] = { 0, 65536 * 55 / 100, 65536 };
    static const unsigned argb[3] = { 0xFF7C5CFFu, 0xFF4F7BFFu, 0xFF38A8FFu };
    struct logit_paint2 q = {0};
    q.kind = LOGIT_PAINT2_LINEAR;
    q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;     /* 135 degrees: top-left -> bottom-right */
    q.nstop = 3; q.stop_pos = pos; q.stop_argb = argb;
    ol_window_path2(&p, &q, 0);
    int m = size * 56 / 100;                        /* glyph at 56 % of the tile */
    gui_icon2(GICON_LOGIT, (x + (size - m) / 2) << 8, (y + (size - m) / 2) << 8, m << 8, 0xFFFFFF, 2);
}

static void card(int y, unsigned bg, unsigned ink)
{
    rrect(GRID_X - 12, y, CARD_W, CARD_H, 12, bg);
    for (int id = GICON_SUN; id <= GICON_LOGIT; id++) {
        int k = id - GICON_SUN, col = k % COLS, row = k / COLS;
        int x = GRID_X + col * CELL_W, cy = y + 14 + row * CELL_H;
        static const int px[2] = { 16, 20 };
        for (int s = 0; s < 2; s++)
            for (int w = 1; w <= 3; w++) {
                gui_icon2(id, x << 8, (cy + (20 - px[s]) / 2) << 8, px[s] << 8, ink, w);
                x += px[s] + 6;
            }
    }
}

static void frame(void)
{
    unsigned info = (unsigned)ol_gui2_info(0);
    gui_clear(rgb(246, 246, 250));
    card(LIGHT_Y, 0xFFFFFF, 0x1D1D23);
    card(DARK_Y, 0x19191E, 0xF2F2F6);
    /* the Logit mark, monochrome, at the sizes the shell uses it */
    static const int ms[5] = { 14, 20, 28, 48, 96 };
    int x = 700;
    for (int i = 0; i < 5; i++) {
        gui_icon2(GICON_LOGIT, x << 8, (40 + (96 - ms[i]) / 2) << 8, ms[i] << 8, 0x1D1D23, 2);
        x += ms[i] + 12;
    }
    /* white on the gradient tile: the About hero (96) and the app icon (64, 32) */
    logit_tile(700, 160, 96);
    logit_tile(812, 176, 64);
    logit_tile(892, 192, 32);
    /* the weight probe: two 64 pt suns, weight 1 and weight 3 */
    rrect(W1_X - 8, WP_Y - 8, WP_PX + 16, WP_PX + 16, 10, 0xFFFFFF);
    rrect(W3_X - 8, WP_Y - 8, WP_PX + 16, WP_PX + 16, 10, 0xFFFFFF);
    long rc1 = gui_icon2(GICON_SUN, W1_X << 8, WP_Y << 8, WP_PX << 8, 0x000000, 1);
    long rc3 = gui_icon2(GICON_SUN, W3_X << 8, WP_Y << 8, WP_PX << 8, 0x000000, 3);
    gui_flush();
    char b[64]; int n = 0;
    put_s(b, &n, "UIABI info="); put_hex(b, &n, info); b[n++] = '\n';
    sys_write(1, b, n);
    say("icon2", "rc1", rc1, "rc3", rc3);
}

void app_main(void)
{
    gui_create("UI ABI Probe", SW, SH);
    g_dark = sys_ui_dark(-1);
    int tb = gui_set_titlebar(0xF2F2F6u | TB_NO_SEP);
    int bad = gui_set_titlebar(0xF2F2F6u | (1u << 27));
    say("tb", "rc", tb, "bad", bad);
    say("size-normal", "rc", gui_panel_size(400, 300), 0, 0);
    frame();
    struct logit_event e;
    for (;;) {
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) app_exit(0);
            if (e.type == EV_KEY && (e.a == 'd' || e.a == 'l')) {
                int on = e.a == 'd';
                say("dark-at", "on", on, "rc", ui_set_dark_at(on, 200, 100));
            }
            if (e.type == EV_KEY && e.a == 'q') say("dark-query", "rc", ui_set_dark_at(-1, 0, 0), 0, 0);
            if (e.type == EV_KEY && e.a == 'r') frame();
            if (e.type == EV_THEME) { g_dark = sys_ui_dark(-1); say("theme", "dark", g_dark, 0, 0); frame(); }
            if (e.type == EV_RESIZE) frame();
        }
        wait_idle(0);
    }
}

#else
/* ------------------------------------------------------------ the panel */
#define PW1 480
#define PH1 200
#define PW2 640
#define PH2 400

static int cw = PW1, ch = PH1;

static void frame(void)
{
    gui_clear(rgb(255, 255, 255));
    gui_rect(0, 0, cw, 44, rgb(236, 236, 241));                 /* a header band */
    gui_icon2(GICON_SPARKLES, 16 << 8, 12 << 8, 20 << 8, 0x5E5CE6, 2);
    gui_rect(16, 60, cw - 32, 1, rgb(228, 228, 234));
    for (int i = 0; 76 + i * 28 + 16 <= ch; i++)
        gui_rect(16, 76 + i * 28, (cw - 32) * (5 - i % 3) / 5, 14, rgb(232, 232, 238));
    gui_flush();
    say("panel frame", "w", cw, "h", ch);
}

static long resize(int w, int h)
{
#ifdef UI_ABI_NEGCTL
    /* THE CONTROL: claim the resize, never ask for it. */
    (void)w; (void)h;
    return 0;
#else
    return gui_panel_size(w, h);
#endif
}

void app_main(void)
{
    gui_create("UI ABI Panel", PW1, PH1);
    long rc = _sys(SYS_GUI_WIN_STATE, WINS_SET_PANEL, 0, 0);
    say("panel ready", "rc", rc, "w", PW1);
    frame();
    struct logit_event e;
    for (;;) {
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) app_exit(0);
            if (e.type == EV_KEY && e.a == 'g') say("panel grow", "rc", resize(PW2, PH2), 0, 0);
            if (e.type == EV_KEY && e.a == 's') say("panel shrink", "rc", resize(PW1, PH1), 0, 0);
            if (e.type == EV_RESIZE) { cw = e.a; ch = e.b; frame(); }
            if (e.type == EV_THEME) frame();
        }
        wait_idle(0);
    }
}
#endif
