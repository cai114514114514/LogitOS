/* Clock -- an analog face, drawn with Open Logit.
 *
 * WHAT THIS REPLACES. Three digits and a date, centred by `(240 - tw) / 2`
 * against a window size written into the source three times. It was correct and
 * it was nothing: a clock application on a desktop that has a vector rasterizer
 * and shows no clock.
 *
 * FIRST APP TO REACH PAST THE TOOLKIT, which aui.h:45-51 explicitly asks for --
 * "an app that needs a shape the toolkit does not have should include gfx.h,
 * build a path and fill it, rather than starting a fourth rasterizer". A hand
 * at 07:38 is a rotated quad and there is no axis-aligned primitive for it, so
 * this is that case, and it is worth having one app demonstrate the route.
 * Everything that CAN come from the toolkit still does: the twelve hour marks
 * are aui_round dots, all served from one cached corner tile. The dial itself
 * cannot -- see the note on GFX_MASK_MAX at the drawing site.
 *
 * INTEGER ONLY. This app links crt0 + aui + gfx and no libc -- there is no
 * sin(), and adding one would pull in libm for three hands. A quarter turn in
 * six-degree steps is sixteen numbers; the other three quarters are symmetry.
 *
 * IT RESIZES, which no GUI app in this tree did. The window manager has
 * supported resize for a while (eight edge and corner drags in test-window) and
 * every app ignored EV_RESIZE and kept drawing at its birth size against
 * hardcoded coordinates. Everything here is laid out from aui_width() /
 * aui_height() on the frame it is drawn. */
#include "aui.h"
#include "gfx.h"

/* The face is capped by these two buffers, which are static because this app
 * has no allocator. MASK_MAX is the widest face in DEVICE pixels; the layout
 * converts it back to points at the scale in force, so the dial fills a big
 * window at 100% and simply gets smaller at 200% rather than overrunning. */
#define MASK_MAX  384

static unsigned char cov[MASK_MAX * MASK_MAX];
static unsigned char rgba[MASK_MAX * MASK_MAX * 4];
/* gfx_path_init's capacity counts POINTS and the buffer holds two ints each --
 * declaring ptbuf[N] and then passing N is a two-times overrun that only shows
 * up once a path gets long. Two flattened circles at this radius are a few
 * hundred points, which is what sets the number. */
#define PTCAP 512
static int  ptbuf[PTCAP * 2];
static int  subbuf[16];

/* sin(k * 6 degrees) * 1024, k = 0..15. */
static const short SIN[16] = {
    0, 107, 213, 316, 417, 512, 602, 685, 761, 828, 887, 936, 974, 1002, 1018, 1024
};
static int sn(int k)
{
    k = ((k % 60) + 60) % 60;
    if (k <= 15) return SIN[k];
    if (k <= 30) return SIN[30 - k];
    if (k <= 45) return -SIN[k - 30];
    return -SIN[60 - k];
}
static int cs(int k) { return sn(k + 15); }

static int devlen(int a, int len) { return aui_dev(a + len) - aui_dev(a); }

/* Append one hand to `p`: a quad from `tail` behind the centre out to `tip`,
 * `w` wide, at 1/60th-turn step `k` measured clockwise from twelve. All in
 * device pixels, mask-local. Direction is (sin, -cos) because y runs down; the
 * perpendicular is that turned a quarter, which is (cos, sin).
 *
 * The engine's coordinates are 24.8, not pixels -- and it takes ints either
 * way, so feeding it pixels is not a type error, it draws every hand at one
 * 256th of its size and the face comes up empty. Hence GFX_PX on the way in,
 * and the half-pixel offsets stay in 24.8 so a 3-pixel-wide second hand is
 * actually 1.5 pixels either side of centre rather than 1 and 2. */
static void hand(struct gfx_path *p, int cx, int cy, int k, int tip, int tail, int w)
{
    int dx = sn(k), dy = -cs(k);            /* x1024 */
    int px = cs(k), py = sn(k);             /* x1024, perpendicular */
    int hx = px * w * GFX_ONE / 2048, hy = py * w * GFX_ONE / 2048;
    int tx = GFX_PX(cx) + dx * tip * GFX_ONE / 1024;
    int ty = GFX_PX(cy) + dy * tip * GFX_ONE / 1024;
    int bx = GFX_PX(cx) - dx * tail * GFX_ONE / 1024;
    int by = GFX_PX(cy) - dy * tail * GFX_ONE / 1024;
    gfx_move_to(p, bx + hx, by + hy);
    gfx_line_to(p, tx + hx, ty + hy);
    gfx_line_to(p, tx - hx, ty - hy);
    gfx_line_to(p, bx - hx, by - hy);
    gfx_close(p);
}

/* Rasterize whatever is in `p` and blit it over the face's point rect. The
 * source is generated at exactly the device size of that rect, so the
 * compositor's rescale is the identity and the antialiasing survives 150%.
 *
 * gfx_fill_mask returns 1 for SUCCESS and 0 for a refusal (an overflowed path,
 * a width past the rasterizer's bound). Reading it the other way round -- the
 * C reflex, zero is fine -- draws nothing at all, silently, on every frame,
 * with no error anywhere: the first version of this file had an empty dial and
 * a compiler with nothing to say about it.
 *
 * The clear is the engine's job and already done inside gfx_fill_mask. A hand
 * written `for (i...) cov[i] = 0` here would also be the exact loop gfx.h warns
 * about -- at -O2 clang is entitled to turn it into a call to memset, and this
 * app links crt0 + aui + gfx and no libc, so that is a link error waiting for
 * whoever next changes an optimisation flag. */
static void stamp(struct gfx_path *p, int rule, int fx, int fy, int fs,
                  int dw, int dh, unsigned color)
{
    if (!gfx_fill_mask(p, rule, cov, dw, dh, 0, 0)) return;
    gfx_mask_to_rgba(rgba, cov, dw, dh, color, 255, 0, 0);
    gui_blit(fx, fy, fs, fs, rgba, dw, dh);
}

static void two(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; }

static void draw(const struct logit_time *t)
{
    int W = aui_width(), H = aui_height();
    aui_begin(AUI_BG);

    /* Layout: a square face over a two-line readout, both centred, both sized
     * from the window as it is now. The readout keeps a fixed height so the
     * face takes every point that is left. */
    int pad = AUI_PAD, foot = AUI_FS_TITLE + AUI_FS_LABEL + AUI_SP(5);
    int fs = W - 2 * pad;
    if (H - 2 * pad - foot < fs) fs = H - 2 * pad - foot;
    /* The ceiling is the mask buffer, not a taste, so express it as such: how
     * many POINTS fit in MASK_MAX device pixels at the scale in force. A fixed
     * point cap would waste half the buffer at 100% and overrun it at 300%. */
    int cap = MASK_MAX * 100 / aui_scale();
    if (fs > cap) fs = cap;
    int fx = (W - fs) / 2;
    /* Centre the whole block, not the face alone. Pinning it to the top made
     * every point of a grown window into dead space under the date. */
    int blk = (fs >= 48 ? fs + AUI_SP(3) : 0) + AUI_FS_TITLE + AUI_SP(1) + AUI_FS_LABEL;
    int fy = (H - blk) / 2; if (fy < pad) fy = pad;

    if (fs >= 48) {
        int dw = devlen(fx, fs), dh = devlen(fy, fs);
        int ok = dw > 0 && dh > 0 && dw <= MASK_MAX && dh <= MASK_MAX;

        /* THE DIAL IS NOT aui_round, and the reason is a limit worth knowing:
         * the toolkit's rounded shapes come from gfx_mask_corner, which refuses
         * a tile past GFX_MASK_MAX (72 device px) and returns NULL, and the
         * caller then falls back to a square. A 180-point face wants a 90-point
         * corner, so `aui_round(..., fs/2, ...)` drew a SQUARE clock -- silently,
         * correctly by its own contract, and wrong. Above that size the shape
         * has to come from the engine directly, which is what the mask cache
         * exists to avoid for small repeated corners and is perfectly fine for
         * one big circle a second. Same for the rim: an annulus is two circles
         * under the even-odd rule, and even-odd does not care which way either
         * of them is wound. */
        if (ok) {
            struct gfx_path p;
            int dcx = dw / 2, dcy = dh / 2, dR = (dw < dh ? dw : dh) / 2;
            gfx_path_init(&p, ptbuf, PTCAP, subbuf, 16);
            gfx_path_circle(&p, GFX_PX(dcx), GFX_PX(dcy), GFX_PX(dR - 1));
            stamp(&p, GFX_NONZERO, fx, fy, fs, dw, dh, AUI_SURFACE);

            gfx_path_reset(&p);
            gfx_path_circle(&p, GFX_PX(dcx), GFX_PX(dcy), GFX_PX(dR - 1));
            gfx_path_circle(&p, GFX_PX(dcx), GFX_PX(dcy), GFX_PX(dR - 2));
            stamp(&p, GFX_EVENODD, fx, fy, fs, dw, dh, AUI_BORDER);
        }

        /* Hour marks. Twelve dots rather than twelve rotated bars: a dot is
         * axis-aligned at every angle, so the toolkit can draw it and the mask
         * cache serves all twelve from one tile. The quarters are bigger --
         * that is the whole of "which way up is this face". */
        int cx = fx + fs / 2, cy = fy + fs / 2, R = fs / 2 - AUI_SP(3);
        for (int k = 0; k < 12; k++) {
            int q = (k % 3) == 0;
            int d = q ? AUI_SP(2) : AUI_SP(1) + 1;
            int mx = cx + sn(k * 5) * R / 1024, my = cy - cs(k * 5) * R / 1024;
            aui_round(mx - d / 2, my - d / 2, d, d, d / 2, q ? AUI_TEXT : AUI_MUTED);
        }

        if (ok) {
            int dcx = dw / 2, dcy = dh / 2, dR = (dw < dh ? dw : dh) / 2;
            struct gfx_path p;

            /* Hour and minute share a colour, so they share a mask and a blit.
             * The hour hand advances WITHIN the hour -- 10:59 must not point at
             * ten -- which is why its step is scaled by 60 and divided back. */
            gfx_path_init(&p, ptbuf, PTCAP, subbuf, 16);
            int hk = ((t->hour % 12) * 60 + t->minute) * 5 / 60;
            hand(&p, dcx, dcy, hk, dR * 52 / 100, dR * 12 / 100, dR * 9 / 100);
            hand(&p, dcx, dcy, t->minute, dR * 76 / 100, dR * 12 / 100, dR * 6 / 100);
            stamp(&p, GFX_NONZERO, fx, fy, fs, dw, dh, AUI_TEXT);

            gfx_path_reset(&p);
            hand(&p, dcx, dcy, t->second, dR * 84 / 100, dR * 20 / 100, dR * 3 / 100);
            stamp(&p, GFX_NONZERO, fx, fy, fs, dw, dh, AUI_ACCENT);
        }

        int cap = AUI_SP(2) + 2;
        aui_round(cx - cap / 2, cy - cap / 2, cap, cap, cap / 2, AUI_ACCENT);
        aui_round(cx - 2, cy - 2, 4, 4, 2, AUI_SURFACE);
    }

    char hms[9];
    two(hms + 0, t->hour);   hms[2] = ':';
    two(hms + 3, t->minute); hms[5] = ':';
    two(hms + 6, t->second); hms[8] = 0;
    int ty = fy + (fs >= 48 ? fs : 0) + AUI_SP(3);
    int tw = text_measure_px(hms, 8, AUI_FS_TITLE, 0);
    aui_text_sz((W - tw) / 2, ty, hms, AUI_TEXT, AUI_FS_TITLE);

    char d[11];
    d[0] = '0' + (t->year / 1000) % 10; d[1] = '0' + (t->year / 100) % 10;
    d[2] = '0' + (t->year / 10) % 10;   d[3] = '0' + t->year % 10;
    d[4] = '-'; two(d + 5, t->month); d[7] = '-'; two(d + 8, t->day); d[10] = 0;
    int dwid = text_measure_px(d, 10, AUI_FS_LABEL, 0);
    aui_text_sz((W - dwid) / 2, ty + AUI_FS_TITLE + AUI_SP(1), d, AUI_MUTED, AUI_FS_LABEL);

    aui_end();
}

void app_main(void)
{
    int w = 260, h = 300;
    gui_create("Clock", w, h);
    aui_set_size(w, h);

    struct logit_time t;
    get_time(&t);
    draw(&t);
    int last = t.second;

    for (;;) {
        struct logit_event e;
        int force = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            /* A theme flip and a resize both invalidate every pixel, and
             * neither of them moves the second hand -- so they cannot wait for
             * the once-a-second repaint below or the window sits wrong for up
             * to a second while the pointer is still dragging its edge. */
            if (e.type == EV_RESIZE) { aui_set_size(e.a, e.b); force = 1; }
            if (e.type == EV_THEME)  force = 1;
        }
        get_time(&t);
        if (force || t.second != last) { last = t.second; draw(&t); }
        wait_idle(100);   /* was sys_yield(): a spin. the second hand only needs to be right to within a frame */
    }
}
