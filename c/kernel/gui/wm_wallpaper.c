/* wm_wallpaper.c -- the desktop picture.
 *
 * The wallpaper: decoding ui.wallpaper (or the dusk gradient when there is
 * none) and box-filtering it once into the compositor's `bg`, the generation
 * a wallpaper-derived cache keys on, the picture each theme shows, and the
 * desktop's own right-button menu (none yet).
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

/* Dusk wallpaper: a soft lavender -> mauve -> warm-sand vertical gradient
 * (a macOS-Tahoe-ish dusk sky). Cached once into `bg`. */
static uint32_t bg_color(int x, int y)
{
    (void)x;
    int t = y * 1000 / wm_H;
    uint8_t r, g, b;
    if (t < 550) {
        r = (uint8_t)lerp(140, 198, t, 550); g = (uint8_t)lerp(150, 168, t, 550); b = (uint8_t)lerp(198, 184, t, 550);
    } else {
        int u = t - 550;
        r = (uint8_t)lerp(198, 228, u, 450); g = (uint8_t)lerp(168, 192, u, 450); b = (uint8_t)lerp(184, 162, u, 450);
    }
    return rgb(r, g, b);
}

/* WHICH PICTURE IS THE DESKTOP, BY GENERATION: bumped every time `bg` is
 * re-baked (a decoded picture, or the gradient when there is none) and every
 * time the theme flips, so a cache derived from the wallpaper (a pre-blurred
 * glass backdrop, design P9's C1) can tell a stale copy from a current one by
 * one compare. Written at boot
 * (wm_init) and then only on the WM thread under wm_lock. */
unsigned g_wallpaper_gen;

/* The picture a theme shows. Today both themes show the same one: the
 * ui.wallpaper setting, /wallpaper.png if the user has not chosen one (a
 * night variant, design P12, is the reason this takes `dark`). The string is
 * a buffer of this file's, valid until the next call: its one caller is
 * draw_wallpaper, at boot or on the WM thread under wm_lock, so one buffer. */
static char wm_wallpaper_path_buf[SET_VALLEN];
const char *wm_wallpaper_path_for_theme(int dark)
{
    (void)dark;
    settings_copy_str("ui.wallpaper", "/wallpaper.png", wm_wallpaper_path_buf,
                      sizeof wm_wallpaper_path_buf);
    return wm_wallpaper_path_buf;
}

/* Load /wallpaper.png (Apple Desktop Picture, packed at build), decode via the
 * kernel image codec, and scale-blit it to fill the screen. 0 if absent/bad so
 * the caller falls back to the gradient. */
static int draw_wallpaper(void)
{
    /* --- settings line: the user's wallpaper, /wallpaper.png if they have not
     * chosen one. A path naming a file that does not exist, or one that is not
     * a decodable image, falls through to the gradient below exactly as a
     * missing /wallpaper.png always did -- so there is no wallpaper setting
     * that can produce a broken desktop, only a plain one. */
    const char *wp = wm_wallpaper_path_for_theme(g_ui_dark);
    int sz = vfs_size(wp);
    if (sz <= 0) return 0;
    uint8_t *file = (uint8_t *)kmalloc((unsigned)sz);
    if (!file) return 0;
    int n = vfs_read(wp, file, sz);
    struct image im;
    int ok = (n > 0 && kernel_img_decode(file, n, &im) == 0);
    kfree(file);
    if (!ok) return 0;
    /* Scale to fill, baked ONCE into the cached `bg` (draw_background runs at
     * init and on a mode or size change). A 5K photo shrunk onto a 1280x800
     * screen with the old point sampler aliased; the exact area average (BOX)
     * is paid here, not per frame. Destination is 24.8: W<<8 x H<<8. */
    if (ol_display_blit_rgba_ex(fb_display_cur(), im.rgba, im.w, im.h, im.w * 4,
                                0, 0, im.w, im.h, 0, 0, wm_W << 8, wm_H << 8, 255, OL_FILTER_BOX) != OL_OK)
        fb_blit_rgba(0, 0, wm_W, wm_H, im.rgba, im.w, im.h);      /* scratch alloc failed: point-sampled */
    img_free(&im);
    return 1;
}

/* The wallpaper only -- baked once into `bg`. The menu bar and dock are now
 * composited per-frame on TOP of the windows (draw_menubar/draw_dock) so their
 * real-time blur frosts the live content behind them (true vibrancy), and a
 * window can slide *under* the dock instead of covering it. */
void draw_background(void)
{
    g_wallpaper_gen++;           /* decoded or the gradient, `bg` is re-baked */
    if (!draw_wallpaper())
        for (int y = 0; y < wm_H; y++)
            for (int x = 0; x < wm_W; x++) fb_put(x, y, bg_color(x, y));
}

/* The right button on the bare desktop (no window under it). Nothing yet; the
 * desktop's own menu (design 5.5) lives here. Returns 1 when it took the press. */
int wm_desktop_context(int x, int y) { (void)x; (void)y; return 0; }

/* Per-pass idle work for the wallpaper (pre-decoding the other theme's
 * picture, design P12): none. */
int wm_wallpaper_tick(uint64_t now) { (void)now; return 0; }
