/* ui-aui-form-data: form groups / rows / kv / help, empty state, chips, kbd chips,
 * squircle tiles, ring gauge, sparkline and the multi-segment bar of the real
 * ring-3 aui.c (design 4.13, 4.14, 4.15, 4.16, 3.7.2, 3.7.3) over the host harness.
 *
 * Checks print "FAIL <check> ..."; the exit status is 1 on any failure:
 *   form-row-h        44 pt plain rows, 56 with a subtitle, kv rows 44; contiguous; the card
 *                     height is the sum
 *   form-switch-x     aui_form_row_switch_x == x + w - 16 - 38; the slot is right-aligned at
 *                     inset 16 and vertically centred
 *   form-card         the group card is bg.card, r 12, ONE squircle PATH + its rim; a corner
 *                     pixel is outside the card, the centre is bg.card (light and dark)
 *   form-kv           label right-aligned in the 112 column, value from x + 128
 *   empty-42          the block's centre is at 42 % of the region +-1, centred in x; the
 *                     64 tile is identity @ 36; the primary button click returns 1
 *   chip-on           chip OFF = fill.control; ON = accent.tint over the surface; hover
 *                     changes it; the press returns 1; heights 24 / 28
 *   kbd               h 18, r 4 (corner clear), fill.control
 *   tile              squircle gradient top -> bottom; kind colours from the table
 *   ring-start        the value arc starts at 12 o'clock (-90 deg) and runs clockwise
 *   ring-settle       the smooth spring converges and then requests no frames
 *   spark-damage      a sparkline's damage stays inside its rect, every frame; once the newest
 *                     point has eased in (120 ms) it requests nothing
 *   multibar-gap      2 DEVICE pixels between segments at 100 / 150 / 200 %
 *
 * Every case runs in a forked child (aui.c keeps scale, capabilities and the
 * animation slots in statics) in v2 mode (PATH, SHADOW2, ...) unless it says
 * otherwise. With UI_FORM_DUMP=<dir> the review scenes are written as PPMs.
 *
 * -DUI_FORM_NEGCTL (tests/ui-aui-form-data.mk) lays plain rows out 40 pt high and
 * must fail 'form-row-h' and nothing else. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.c"
#include "aui_host.h"

static int checks, fails;
static void check(int ok, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static struct aui_host *H;
static int g_pct, g_dark, g_v2;

static void setup(int pct, int dark, int v2, int w, int h)
{
    aui_host_handlers_clear();
    H = aui_host_new(w, h, pct);
    g_pct = pct; g_dark = dark; g_v2 = v2;
    H->dark = dark;
    if (v2) aui_host_enable_v2(H);
    aui_host_clock_set(1000);
    gui_create("form-data", w, h);
    aui_set_size(w, h);
    aui_ensure();
    aui_set_dark(dark);
}
#define BGW (aui_ensure(), aui_t.bg_window)
static void frame_begin(void) { aui_begin(BGW); }
static void frame_end(void) { aui_end(); aui_feed_done(); }
static void ev(int type, int x, int y, int button)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = x; e.b = y; e.button = button;
    aui_feed(&e);
}

/* ------------------------------------------------------------- pixels */

static unsigned px_at(int xd, int yd)
{
    if (xd < 0 || yd < 0 || xd >= H->w || yd >= H->h) return 0xFF00FF;
    return H->px[(size_t)yd * (size_t)H->w + (size_t)xd] & 0xFFFFFF;
}
static unsigned px_pt(int x, int y) { return px_at(aui_host_pt(x, g_pct), aui_host_pt(y, g_pct)); }
static int near_c(unsigned a, unsigned b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < 0) d = -d;
        if (d > tol) return 0;
    }
    return 1;
}
/* c over bg at alpha a (0..255) */
static unsigned over(unsigned bg, unsigned c, int a)
{
    unsigned out = 0;
    for (int s = 0; s < 24; s += 8) {
        int b = (int)((bg >> s) & 255), f = (int)((c >> s) & 255);
        out |= (unsigned)((b * (255 - a) + f * a + 127) / 255) << s;
    }
    return out;
}

/* ------------------------------------------------------------- form */

static void t_form(void)
{
    struct aui_rect col = aui_r(20, 16, 260, 0), r1, r2, r3, r4, slot;
    int calls0;
    col.h = aui_form_h(2, 1) + AUI_FORM_ROW_H;
    check(aui_form_h(2, 1) == 2 * 44 + 56, "form-row-h aui_form_h(2,1) = %d @%d", aui_form_h(2, 1), g_pct);
    frame_begin();
    calls0 = (int)H->calls[SYS_GUI_PATH];
    aui_form_group_begin("External", &col);
    {
        int card_paths = (int)H->calls[SYS_GUI_PATH] - calls0;
        /* the squircle fill and its rim: two PATHs (v2); none without PATH (circular fallback) */
        check(g_v2 ? card_paths == 2 : card_paths == 0, "form-card %d PATH call(s) for the card @%d v2=%d",
              card_paths, g_pct, g_v2);
    }
    r1 = aui_form_row("Plain", 0);
    r2 = aui_form_row("With subtitle", "second line");
    r3 = aui_form_row("Another plain", "");
    r4 = aui_form_kv("Key", "Value");
    slot = aui_form_slot(r1, AUI_FORM_SWITCH_W, 22);
    aui_form_group_end();
    check(r1.h == 44, "form-row-h plain row is %d pt, want 44 @%d", r1.h, g_pct);
    check(r2.h == 56, "form-row-h subtitled row is %d pt, want 56 @%d", r2.h, g_pct);
    check(r3.h == 44, "form-row-h empty-subtitle row is %d pt, want 44 @%d", r3.h, g_pct);
    check(r4.h == 44, "form-row-h kv row is %d pt, want 44 @%d", r4.h, g_pct);
    check(r2.y == r1.y + r1.h && r3.y == r2.y + r2.h && r4.y == r3.y + r3.h,
          "form-row-h rows not contiguous: %d %d %d %d", r1.y, r2.y, r3.y, r4.y);
    check(r1.y == 16 + AUI_FORM_LABEL_H + 8, "form-card first row y %d, want %d (label 16 + 8)", r1.y, 16 + 24);
    check(aui_form_row_switch_x(r1) == r1.x + r1.w - 16 - 38, "form-switch-x %d want %d", aui_form_row_switch_x(r1),
          r1.x + r1.w - 16 - 38);
    check(aui_form_row_switch_x(r1) == 20 + 260 - 54, "form-switch-x %d want %d", aui_form_row_switch_x(r1), 20 + 260 - 54);
    check(slot.x == aui_form_row_switch_x(r1) && slot.y == r1.y + (r1.h - 22) / 2 && slot.h == 22 && slot.x + slot.w == r1.x + r1.w - 16,
          "form-switch-x slot %d,%d %dx%d", slot.x, slot.y, slot.w, slot.h);
    check(col.y == 16 + 24 + col.h + 12, "form-card col.y %d after the group, want %d", col.y, 16 + 24 + col.h + 12);
    frame_end();

    /* card pixels: centre = bg.card, the corner pixel is outside the squircle */
    {
        unsigned card = g_dark ? 0x26262D : 0xFFFFFF;
        struct aui_rect cd = aui_r(20, 16 + 24, 260, col.h);
        check(near_c(px_pt(cd.x + cd.w / 2, cd.y + 6), card, 6),
              "form-card centre pixel %06x want %06x @%d", px_pt(cd.x + cd.w / 2, cd.y + 6), card, g_pct);
    }
}

/* the card against the window background, light and dark (bg.window / bg.card) */
static void t_form_colors(void)
{
    struct aui_rect col = aui_r(20, 16, 260, 88);
    unsigned bg = g_dark ? 0x1E1E24 : 0xF4F4F8, card = g_dark ? 0x26262D : 0xFFFFFF;
    frame_begin();
    aui_form_group_begin(0, &col);
    aui_form_row("A", 0);
    aui_form_row("B", 0);
    aui_form_group_end();
    frame_end();
    check(near_c(px_pt(10, 60), bg, 6), "form-card window bg %06x want %06x @%d dark=%d", px_pt(10, 60), bg, g_pct, g_dark);
    check(near_c(px_pt(150, 20), card, 6) , "form-card fill %06x want %06x @%d dark=%d", px_pt(150, 20), card, g_pct, g_dark);
    {   /* a squircle r 12: the corner pixel (1,1) inside the rect is outside the shape */
        unsigned c = px_at(aui_host_pt(20, g_pct) + 1, aui_host_pt(16, g_pct) + 1);
        check(!near_c(c, card, 6), "form-card r12 corner pixel %06x is still card colour @%d", c, g_pct);
    }
}

static void t_kv(void)
{
    struct aui_rect col = aui_r(10, 10, 300, 44), r;
    frame_begin();
    aui_form_group_begin(0, &col);
    r = aui_form_kv("Name", "LogitOS");
    aui_form_group_end();
    frame_end();
    check(r.h == 44 && r.y == 10, "form-kv row %d,%d h %d", r.x, r.y, r.h);
    /* a text pixel in the value column exists at x >= 128, none between the 112 column and 128 */
    {
        int vpix = 0, gap_pix = 0;
        unsigned card = g_dark ? 0x26262D : 0xFFFFFF;
        for (int x = r.x + 113; x < r.x + 127; x++)
            for (int y = r.y + 8; y < r.y + 36; y++)
                if (!near_c(px_pt(x, y), card, 10)) gap_pix++;
        for (int x = r.x + 128; x < r.x + 128 + 60; x++)
            for (int y = r.y + 8; y < r.y + 36; y++)
                if (!near_c(px_pt(x, y), card, 40)) vpix++;
        check(gap_pix == 0, "form-kv %d ink pixels between the 112 label column and the value at 128 @%d", gap_pix, g_pct);
        check(vpix > 0, "form-kv no value ink from x + 128 @%d", g_pct);
    }
}

static void t_help(void)
{
    struct aui_rect col = aui_r(10, 10, 300, 44);
    int bottom;
    frame_begin();
    aui_form_group_begin(0, &col);
    aui_form_row("A", 0);
    aui_form_group_end();
    bottom = 10 + 44;
    check(col.y == bottom + 12, "form-card col.y %d want %d", col.y, bottom + 12);
    aui_form_help("This help footnote sits eight points under the group and never needs more than two lines of text.");
    check(col.y >= bottom + 8 + 16 + 12 && col.y <= bottom + 8 + 32 + 12, "form-help col.y %d after help", col.y);
    frame_end();
}

/* ------------------------------------------------------------- empty state */

static void t_empty(void)
{
    struct aui_rect reg = aui_r(0, 0, 400, 300), blk, btn;
    int ret, pw, cy;
    unsigned id = 0x3D8BF5, bg = BGW;
    const char *body = "A short sentence that says what to do next, long enough to wrap onto a second line.";
    frame_begin();
    ret = aui_empty_state(reg, GICON_FOLDER, id, "No items", body, "New folder", 0);
    frame_end();
    blk = aui_empty_state_rect(reg, body, "New folder", 0);
    cy = blk.y * 2 + blk.h;
    check(abs(cy - 2 * (300 * 42 / 100)) <= 2, "empty-42 centre*2 %d want %d +-2 (block %d h %d) @%d", cy, 2 * (300 * 42 / 100),
          blk.y, blk.h, g_pct);
    check(blk.h == aui_empty_state_h(body, "New folder", 0), "empty-42 block height mismatch");
    check(ret == 0, "empty-42 returned %d without a click", ret);
    {   /* the tile: 64 wide, centred, identity @ 36 over the surface */
        int tx = 200 - 32, ty = blk.y;
        unsigned want = over(bg, id, 36);
        check(near_c(px_pt(tx + 6, ty + 32), want, 8), "empty-42 tile pixel %06x want %06x @%d", px_pt(tx + 6, ty + 32), want, g_pct);
        check(near_c(px_pt(tx - 3, ty + 32), bg, 6), "empty-42 tile not centred (left of it %06x) @%d", px_pt(tx - 3, ty + 32), g_pct);
        check(near_c(px_pt(tx + 64 + 3, ty + 32), bg, 6), "empty-42 tile not centred (right of it %06x) @%d", px_pt(tx + 67, ty + 32), g_pct);
        check(near_c(px_pt(tx + 32, ty - 3), bg, 6), "empty-42 ink above the tile @%d", g_pct);
    }
    /* click the primary button */
    pw = imax(72, aui_text_measure_w("New folder", 13, AUI_W_MEDIUM) + 32);
    btn = aui_r(200 - pw / 2, blk.y + blk.h - 28, pw, 28);
    ev(EV_MOUSE_MOVE, btn.x + btn.w / 2, btn.y + btn.h / 2, 0);
    frame_begin(); aui_empty_state(reg, GICON_FOLDER, id, "No items", body, "New folder", 0); frame_end();
    ev(EV_MOUSE, btn.x + btn.w / 2, btn.y + btn.h / 2, EV_BTN_LEFT);
    frame_begin(); ret = aui_empty_state(reg, GICON_FOLDER, id, "No items", body, "New folder", 0); frame_end();
    check(ret == 1, "empty-42 primary click returned %d @%d", ret, g_pct);
}

/* ------------------------------------------------------------- chip / kbd */

static void t_chip(void)
{
    unsigned bg = BGW, ctl = g_dark ? 0x33333B : 0xE9E9EE;
    unsigned tint = aui_tok()->accent_tint;
    int w0, w1, r1;
    frame_begin();
    w0 = aui_chip_w("Filter", -1, 0, 0);
    aui_chip(10, 10, "Filter", -1, 0, 0);
    aui_chip(10, 50, "Filter", GICON_STAR, 1, AUI_CHIP_LARGE);
    frame_end();
    w1 = aui_chip_w("Filter", -1, 1, 0);
    check(w0 > 30 && w1 > w0 - 1, "chip-on widths %d %d", w0, w1);
    check(near_c(px_pt(10 + 4, 10 + 12), ctl, 6), "chip-on OFF fill %06x want %06x @%d", px_pt(14, 22), ctl, g_pct);
    check(near_c(px_pt(10 + 4, 10 - 3), bg, 6), "chip-on ink above the 24 pt chip @%d", g_pct);
    check(near_c(px_pt(10 + 4, 10 + 24 + 2), bg, 6), "chip-on chip taller than 24 pt @%d", g_pct);
    {
        unsigned want = over(bg, tint & 0xFFFFFF, (int)(tint >> 24));
        check(near_c(px_pt(10 + 4, 50 + 14), want, 6), "chip-on ON fill %06x want accent.tint %06x @%d dark=%d",
              px_pt(14, 64), want, g_pct, g_dark);
        check(near_c(px_pt(10 + 4, 50 + 28 + 2), bg, 6), "chip-on large chip taller than 28 pt @%d", g_pct);
        check(!near_c(px_pt(10 + 14, 50 + 27), bg, 3), "chip-on large chip shorter than 28 pt @%d", g_pct);
    }
    /* hover washes it, a press returns 1 */
    ev(EV_MOUSE_MOVE, 30, 22, 0);
    aui_host_clock_advance(200);
    frame_begin(); aui_chip(10, 10, "Filter", -1, 0, 0); frame_end();
    aui_host_clock_advance(200);
    ev(EV_MOUSE_MOVE, 30, 22, 0);
    frame_begin(); aui_chip(10, 10, "Filter", -1, 0, 0); frame_end();
    check(!near_c(px_pt(14, 22), ctl, 1), "chip-on hover did not change the fill (%06x) @%d", px_pt(14, 22), g_pct);
    ev(EV_MOUSE, 30, 22, EV_BTN_LEFT);
    frame_begin(); r1 = aui_chip(10, 10, "Filter", -1, 0, 0); frame_end();
    check(r1 == 1, "chip-on press returned %d @%d", r1, g_pct);
}

static void t_kbd(void)
{
    unsigned bg = BGW, ctl = g_dark ? 0x33333B : 0xE9E9EE;
    int w;
    frame_begin();
    w = aui_kbd(10, 10, "esc");
    frame_end();
    check(w >= 18, "kbd width %d", w);
    check(near_c(px_pt(10 + 2, 10 + 9), ctl, 6), "kbd fill %06x want %06x @%d", px_pt(12, 19), ctl, g_pct);
    check(near_c(px_pt(10 + 2, 10 - 2), bg, 6) && near_c(px_pt(10 + 2, 10 + 19), bg, 6), "kbd taller than 18 pt @%d", g_pct);
    {   /* r 4: the very corner pixel is outside the chip */
        unsigned c = px_at(aui_host_pt(10, g_pct), aui_host_pt(10, g_pct));
        check(!near_c(c, ctl, 6), "kbd corner pixel %06x is chip colour (radius < 4) @%d", c, g_pct);
    }
}

/* ------------------------------------------------------------- tiles */

static void t_tile(void)
{
    struct aui_tile_colors f = aui_file_kind_colors(AUI_FK_FOLDER), t = aui_file_kind_colors(AUI_FK_TEXT);
    unsigned bg = BGW;
    frame_begin();
    aui_tile(aui_r(10, 10, 64, 64), f.top, f.bottom, -1, f.icon_rgb, f.flags);
    aui_tile(aui_r(100, 10, 64, 64), t.top, t.bottom, -1, t.icon_rgb, t.flags);
    aui_settings_chip(10, 100, 0x5E5CE6, GICON_GEAR);
    frame_end();
    check(f.top == 0x6CB8FF && f.bottom == 0x3D8BF5 && t.top == 0xFFFFFF && t.bottom == 0xEEF0F4 && (t.flags & AUI_TILE_EDGE) &&
              aui_file_kind_colors(AUI_FK_CODE).top == 0xB48CFF && aui_file_kind_colors(AUI_FK_IMAGE).bottom == 0x1FA88A &&
              aui_file_kind_colors(AUI_FK_AUDIO).top == 0xFF7AA2 && aui_file_kind_colors(AUI_FK_VIDEO).bottom == 0xF28A1E &&
              aui_file_kind_colors(AUI_FK_OTHER).bottom == 0x8E8E99,
          "tile kind colours differ from design 3.7.3");
    check(near_c(px_pt(42, 12), f.top, 22), "tile top row %06x want ~%06x @%d", px_pt(42, 12), f.top, g_pct);
    check(near_c(px_pt(42, 71), f.bottom, 22), "tile bottom row %06x want ~%06x @%d", px_pt(42, 71), f.bottom, g_pct);
    check(near_c(px_at(aui_host_pt(10, g_pct), aui_host_pt(10, g_pct)), bg, 4), "tile corner pixel is inside the squircle @%d", g_pct);
    check(!near_c(px_pt(14, 22), bg, 4), "tile left edge missing @%d", g_pct);
    check(!near_c(px_pt(20, 108), bg, 8), "settings chip missing @%d", g_pct);
    check(near_c(px_pt(10 + 22, 100 + 10), bg, 6), "settings chip wider than 20 pt @%d", g_pct);
}

/* ------------------------------------------------------------- ring */

static void ring_frame(int pm)
{
    frame_begin();
    aui_ring_gauge(aui_r(10, 10, 100, 100), pm, 10, 0xE08A00, "25%", "CPU");
    frame_end();
}

static void t_ring(void)
{
    int cx = 60, cy = 60, R = 100 / 2 - (10 + 1) / 2 - 2;
    unsigned track = g_dark ? 0x33333B : 0xE9E9EE, acc = 0xE08A00;
    ring_frame(250);
    check(near_c(px_pt(cx, cy - R), acc, 12), "ring-start 12 o'clock pixel %06x want the arc colour @%d", px_pt(cx, cy - R), g_pct);
    check(near_c(px_pt(cx + R, cy), acc, 40) || near_c(px_pt(cx + R - 1, cy), acc, 40),
          "ring-start 3 o'clock pixel %06x want the arc end @%d", px_pt(cx + R, cy), g_pct);
    check(near_c(px_pt(cx, cy + R), track, 12), "ring-start 6 o'clock pixel %06x want the track %06x @%d", px_pt(cx, cy + R), track, g_pct);
    check(near_c(px_pt(cx - R, cy), track, 12), "ring-start 9 o'clock pixel %06x want the track @%d", px_pt(cx - R, cy), g_pct);
    /* clockwise: just past 12 o'clock on the right is arc, on the left is track */
    check(near_c(px_pt(cx + 20, cy - R + 3), acc, 30), "ring-start not clockwise (right of 12 o'clock %06x) @%d", px_pt(cx + 20, cy - R + 3), g_pct);
    check(near_c(px_pt(cx - 20, cy - R + 3), track, 30), "ring-start not clockwise (left of 12 o'clock %06x) @%d", px_pt(cx - 20, cy - R + 3), g_pct);
}

static void t_ring_settle(void)
{
    int frames = 0, quiet_after = -1;
    ring_frame(0);
    check(!aui_anim_due() && aui_anim_wait() == 0, "ring-settle a fresh ring requests frames");
    for (int i = 0; i < 80; i++) {
        aui_host_clock_advance(16);
        ring_frame(1000);
        frames++;
        if (!aui_anim_wait() && quiet_after < 0) quiet_after = i;
    }
    check(quiet_after >= 1 && quiet_after < 40, "ring-settle spring never went quiet (first quiet frame %d of %d)", quiet_after, frames);
    check(aui_anim_wait() == 0 && !aui_anim_due(), "ring-settle still requests frames after 80 frames");
    {   /* settled at 100 %: the 6 o'clock pixel is arc colour now */
        int R = 100 / 2 - (10 + 1) / 2 - 2;
        check(near_c(px_pt(60, 60 + R), 0xE08A00, 12), "ring-settle full ring pixel %06x @%d", px_pt(60, 60 + R), g_pct);
    }
}

/* ------------------------------------------------------------- sparkline */

static int inside_dev(const struct aui_host_rect *d, struct aui_rect r)
{
    int x0 = aui_host_pt(r.x, g_pct), y0 = aui_host_pt(r.y, g_pct);
    int x1 = aui_host_pt(r.x + r.w, g_pct), y1 = aui_host_pt(r.y + r.h, g_pct);
    return d->x >= x0 && d->y >= y0 && d->x + d->w <= x1 && d->y + d->h <= y1;
}

static void t_spark(void)
{
    struct aui_rect rc = aui_r(20, 20, 120, 48);
    int s[40], n = 12, bad = 0, frames_with_dmg = 0, req_after = 0;
    for (int i = 0; i < 40; i++) s[i] = (i * 37) % 100;
    /* frame 1 draws everything; later frames only change the sparkline */
    frame_begin(); aui_text_sz(20, 90, "CPU", AUI_TEXT, 12); aui_sparkline(rc, s, n, 0x1F9D55); frame_end();
    for (int step = 0; step < 6; step++) {
        n++;
        aui_host_clock_advance(1000);
        for (int f = 0; f < 12; f++) {
            aui_host_stats_reset(H);
            if (f) aui_host_clock_advance(16);
            frame_begin(); aui_text_sz(20, 90, "CPU", AUI_TEXT, 12); aui_sparkline(rc, s, n, 0x1F9D55); frame_end();
            if (H->ndmg) frames_with_dmg++;
            for (int k = 0; k < H->ndmg; k++)
                if (!inside_dev(&H->dmg[k], rc)) {
                    bad++;
                    if (bad < 4) printf("FAIL spark-damage %d,%d %dx%d outside the rect @%d\n", H->dmg[k].x, H->dmg[k].y,
                                        H->dmg[k].w, H->dmg[k].h, g_pct);
                }
        }
        if (aui_anim_wait()) req_after++;
    }
    check(bad == 0, "spark-damage %d damage rect(s) leave the sparkline rect @%d", bad, g_pct);
    check(frames_with_dmg > 0, "spark-damage a new sample produced no damage @%d", g_pct);
    check(req_after == 0, "spark-damage the sparkline kept requesting frames after its ease (%d of 6 samples) @%d", req_after, g_pct);
    /* the line and the fill exist */
    {
        int ink = 0;
        for (int y = rc.y; y < rc.y + rc.h; y++)
            for (int x = rc.x; x < rc.x + rc.w; x += 2)
                if (near_c(px_pt(x, y), 0x1F9D55, 24)) ink++;
        check(ink > 20, "spark-damage no stroke pixels (%d) @%d", ink, g_pct);
    }
}

/* ------------------------------------------------------------- multibar */

static void t_multibar(void)
{
    struct aui_seg segs[4] = { { 50, 0x5E5CE6, "App" }, { 20, 0xE08A00, "Wired" }, { 0, 0x888888, "none" }, { 30, 0x1F9D55, "Cache" } };
    struct aui_rect rc = aui_r(10, 10, 200, 10);
    unsigned bg = BGW;
    int a0[4], a1[4], ok[4], prev = -1, gaps_ok = 1;
    frame_begin();
    aui_multibar(rc, segs, 4);
    frame_end();
    for (int i = 0; i < 4; i++) ok[i] = aui_multibar_span(rc, segs, 4, 0, i, &a0[i], &a1[i]);
    check(!ok[2], "multibar-gap a zero segment has a span");
    for (int i = 0; i < 4; i++) {
        if (!ok[i]) continue;
        if (prev >= 0 && a0[i] - a1[prev] != 2) {
            gaps_ok = 0;
            printf("FAIL multibar-gap segment %d starts %d dev px after segment %d, want 2 @%d\n", i, a0[i] - a1[prev], prev, g_pct);
        }
        prev = i;
    }
    check(gaps_ok, "multibar-gap spans");
    check(a0[0] == aui_host_pt(10, g_pct) && a1[3] == aui_host_pt(210, g_pct),
          "multibar-gap bar spans %d..%d dev, want %d..%d @%d", a0[0], a1[3], aui_host_pt(10, g_pct), aui_host_pt(210, g_pct), g_pct);
    {   /* pixels: the gap is surface, the segments are their colours (mid-height row) */
        int ym = aui_host_pt(15, g_pct);
        for (int i = 0; i < 4; i++) {
            if (!ok[i]) continue;
            int xm = (a0[i] + a1[i]) / 2;
            check(near_c(px_at(xm, ym), segs[i].rgb, 8), "multibar-gap segment %d pixel %06x want %06x @%d", i, px_at(xm, ym), segs[i].rgb, g_pct);
            if (i != 0) {
                int gx = a0[i] - 1, gx2 = a0[i] - 2;
                check(near_c(px_at(gx, ym), bg, 8) && near_c(px_at(gx2, ym), bg, 8), "multibar-gap gap pixels %06x %06x are not the surface @%d",
                      px_at(gx2, ym), px_at(gx, ym), g_pct);
            }
        }
    }
    {   /* h 10: nothing above or below */
        int xm = (a0[1] + a1[1]) / 2;
        check(near_c(px_at(xm, aui_host_pt(10, g_pct) - 2), bg, 6) && near_c(px_at(xm, aui_host_pt(20, g_pct) + 1), bg, 6),
              "multibar-gap taller than 10 pt @%d", g_pct);
        check(near_c(px_at(a0[0], aui_host_pt(10, g_pct)), bg, 12), "multibar-gap square left corner @%d", g_pct);
    }
    {   /* a partial bar leaves the track */
        struct aui_seg one[1] = { { 25, 0x5E5CE6, "x" } };
        unsigned track = g_dark ? 0x33333B : 0xE9E9EE;
        frame_begin(); aui_multibar_total(rc, one, 1, 100); frame_end();
        check(near_c(px_pt(190, 15), track, 8), "multibar-gap partial bar track %06x want %06x @%d", px_pt(190, 15), track, g_pct);
    }
    {   /* legend: swatch + label, height 16 per line */
        int h1, h2;
        frame_begin();
        h1 = aui_multibar_legend(10, 30, 400, segs, 4);
        h2 = aui_multibar_legend(10, 60, 80, segs, 4);
        frame_end();
        check(h1 == 16 && h2 >= 32, "multibar-gap legend heights %d %d", h1, h2);
        check(near_c(px_pt(12, 30 + 8), 0x5E5CE6, 8), "multibar-gap legend swatch %06x @%d", px_pt(12, 38), g_pct);
        check(near_c(px_pt(10, 30 + 3), BGW, 6), "multibar-gap legend swatch taller than 8 @%d", g_pct);
    }
}

/* ------------------------------------------------------------- scenes */

static void scene_form(int w, int h)
{
    struct aui_rect col = aui_r(20, 16, w - 40, 0);
    static int sw1 = 1, sw0 = 0;
    (void)h;
    aui_begin(BGW);
    col.h = aui_form_h(2, 1);
    aui_form_group_begin("通用", &col);
    {
        struct aui_rect r = aui_form_row("深色模式", 0);
        aui_toggle(aui_form_row_switch_x(r), r.y + (r.h - 24) / 2, &sw1, 1);
        r = aui_form_row("自动切换", "跟随日出日落");
        aui_toggle(aui_form_row_switch_x(r), r.y + (r.h - 24) / 2, &sw0, 1);
        aui_form_row("语言", 0);
    }
    aui_form_group_end();
    aui_form_help("更改会立即生效，并保存在此账户下。");
    col.h = aui_form_h(3, 0);
    aui_form_group_begin("关于", &col);
    aui_form_kv("名称", "LogitOS");
    aui_form_kv("版本", "26.10");
    aui_form_kv("内存", "2 GB");
    aui_form_group_end();
    aui_end();
}

static void scene_data(int w, int h)
{
    (void)h;
    static const struct aui_seg segs[4] = { { 50, 0x5E5CE6, "应用 5.0 GB" }, { 20, 0xE08A00, "核心 2.0 GB" },
                                            { 15, 0x1F9D55, "缓存 1.5 GB" }, { 5, 0x8C8C97, "其他" } };
    static const int sp[24] = { 10, 22, 18, 40, 35, 52, 48, 60, 44, 70, 66, 80, 62, 55, 68, 74, 90, 84, 70, 76, 88, 92, 80, 86 };
    aui_begin(BGW);
    aui_empty_state(aui_r(0, 0, w, 260), GICON_FOLDER, 0x3D8BF5, "此文件夹为空", "把文件拖到这里，或点按“新建文件夹”开始。", "新建文件夹", "导入…");
    {
        int x = 20, y = 272;
        aui_chip(x, y, "全部", -1, 1, 0); x += aui_chip_w("全部", -1, 1, 0) + 8;
        aui_chip(x, y, "文档", GICON_DOC, 0, 0); x += aui_chip_w("文档", GICON_DOC, 0, 0) + 8;
        aui_chip(x, y, "图片", GICON_IMAGE, 0, AUI_CHIP_LARGE);
        x = 20; y = 308;
        x += aui_kbd(x, y, "⌘") + 4; x += aui_kbd(x, y, "K") + 12; aui_kbd(x, y, "esc");
        for (int i = 0; i < AUI_FK_COUNT; i++) {
            struct aui_tile_colors c = aui_file_kind_colors((enum aui_file_kind)i);
            aui_tile(aui_r(20 + i * 52, 340, 44, 44), c.top, c.bottom, c.icon, c.icon_rgb, c.flags);
        }
        aui_settings_chip(20, 396, 0x5E5CE6, GICON_GEAR);
        aui_settings_chip(48, 396, 0x1F6FEB, GICON_GLOBE);
        aui_settings_chip(76, 396, 0xD9342A, GICON_CLOCK);
        aui_ring_gauge(aui_r(20, 432, 96, 96), 640, 10, 0x5E5CE6, "64%", "内存");
        aui_ring_gauge(aui_r(128, 432, 96, 96), 280, 8, 0x1F9D55, "28%", "CPU");
        aui_sparkline(aui_r(236, 440, w - 256, 56), sp, 24, 0x5E5CE6);
        aui_multibar(aui_r(20, 544, w - 40, 10), segs, 4);
        aui_multibar_legend(20, 562, w - 40, segs, 4);
    }
    aui_end();
}

static void dump_scenes(const char *dir)
{
    for (int dark = 0; dark < 2; dark++)
        for (int p = 0; p < 2; p++) {
            int pct = p ? 200 : 100;
            for (int sc = 0; sc < 2; sc++) {
                char path[512];
                int w = sc ? 360 : 340, h = sc ? 600 : 400;
                setup(pct, dark, 1, w, h);
                if (sc) scene_data(w, h); else scene_form(w, h);
                snprintf(path, sizeof path, "%s/%s-%s@%d.ppm", dir, sc ? "data" : "form", dark ? "dark" : "light", pct);
                aui_host_write_ppm(H, path);
                aui_host_free(H);
            }
        }
}

/* ------------------------------------------------------------- driver */

typedef void (*case_fn)(void);
static int run_case(const char *name, case_fn fn, int pct, int dark, int v2, int w, int h)
{
    int fds[2];
    pid_t pid;
    ssize_t n;
    int st, total = 0;
    fflush(stdout);
    if (pipe(fds)) return 1;
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        checks = fails = 0;
        setup(pct, dark, v2, w, h);
        fn();
        aui_host_free(H);
        printf("#checks %d %d\n", checks, fails);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    close(fds[1]);
    {
        static char out[1 << 16];
        size_t len = 0;
        char *p;
        while ((n = read(fds[0], out + len, sizeof out - 1 - len)) > 0) len += (size_t)n;
        out[len] = 0;
        p = strstr(out, "#checks ");
        if (p) { int c, f; if (sscanf(p, "#checks %d %d", &c, &f) == 2) total += c; *p = 0; }
        fputs(out, stdout);
    }
    close(fds[0]);
    waitpid(pid, &st, 0);
    checks += total;
    if (!WIFEXITED(st) || WEXITSTATUS(st) > 1) { printf("FAIL %s crashed (status %d)\n", name, st); return 1; }
    return WEXITSTATUS(st);
}

int main(void)
{
    static const int pcts[3] = { 100, 150, 200 };
    const char *dump = getenv("UI_FORM_DUMP");
    int bad = 0;
    for (int dark = 0; dark < 2; dark++)
        for (int p = 0; p < 3; p++) {
            int pct = pcts[p];
            bad += run_case("form", t_form, pct, dark, 1, 320, 240);
            bad += run_case("form-colors", t_form_colors, pct, dark, 1, 320, 240);
            bad += run_case("kv", t_kv, pct, dark, 1, 320, 120);
            bad += run_case("help", t_help, pct, dark, 1, 320, 200);
            bad += run_case("empty", t_empty, pct, dark, 1, 400, 300);
            bad += run_case("chip", t_chip, pct, dark, 1, 200, 120);
            bad += run_case("kbd", t_kbd, pct, dark, 1, 100, 60);
            bad += run_case("tile", t_tile, pct, dark, 1, 200, 160);
            bad += run_case("ring", t_ring, pct, dark, 1, 140, 140);
            bad += run_case("ring-settle", t_ring_settle, pct, dark, 1, 140, 140);
            bad += run_case("spark", t_spark, pct, dark, 1, 200, 120);
            bad += run_case("multibar", t_multibar, pct, dark, 1, 260, 120);
        }
    /* INFO=0 (the legacy kernel): geometry holds, the circular card has no PATH */
    bad += run_case("form-legacy", t_form, 100, 0, 0, 320, 240);
    bad += run_case("empty-legacy", t_empty, 100, 0, 0, 400, 300);
    if (dump) dump_scenes(dump);
    printf("%s ui-aui-form-data: %d checks, %d failed%s\n", fails || bad ? "result:" : "ok  ", checks, fails,
           bad && !fails ? " (a case failed)" : "");
    return fails || bad ? 1 : 0;
}
