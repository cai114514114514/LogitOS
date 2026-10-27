/* wm_chrome.c -- window chrome.
 *
 * A window's own chrome: the frame (shadow, glass titlebar, the three
 * lights, the title), the titlebar/content seam, the solid strip an animated
 * window's titlebar is minified from, the rounded content group, the
 * titlebar's glass panel and the titlebar's share of the pointer (the
 * lights and the double-click).
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

static int wm_chrome_glass_titlebar(void);
static int titlebar_double_click(int wi, int x, int y);

/* The glass titlebar panel dmg_expand must contain whole, or 0 if this window
 * has no glass on screen this frame. */
static int win_glass_box(const struct win *w, struct drect *p)
{
    int x, y, ww, wh, a;
    if (!w->used) return 0;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a)) return 0;
    if (!win_drawn_direct(w, x, y, ww, wh, a)) return 0;
    if (w->panel) return 0;                           /* no titlebar, no glass */
    if (!wm_chrome_glass_titlebar()) return 0;        /* a solid titlebar samples nothing */
    p->x0 = x; p->y0 = y;
    p->x1 = x + ww; p->y1 = y + TBH + S(10);
    return 1;
}

/* wm_glass_regions' provider: each window's glass titlebar, back to front. */
int wm_chrome_glass_rects(struct wm_glass_region *out, int max)
{
    int n = 0;
    for (int k = 0; k < wm_norder && n < max; k++)
        if (win_glass_box(&wm_wins[wm_order[k]], &out[n].r)) {
            out[n].radius = S(TITLE_GLASS_PT);
            n++;
        }
    return n;
}

/* Is the window titlebar live glass (WM_MAT_TITLE)? Always, today. The hook
 * a solid header (design P10) turns off, for draw_frame and win_glass_box at
 * once, so the panel dmg_expand protects is the panel that is drawn. */
static int wm_chrome_glass_titlebar(void) { return 1; }

/* ---- THE WINDOW IS A ROUNDED SHAPE, AND SO IS ITS CONTENT -------------------
 * The frame is a rounded rect (S(10)), but the canvas is a rectangle: copied
 * square it covered the frame's bottom corners (and a panel's top ones), so the
 * rounding was drawn and then overpainted by the app's pixels. The content blit
 * therefore runs inside an opacity-255 snapshot group (openlogit_display_group.h):
 * begin saves ONLY the four radius x radius corner tiles, the blit draws as it
 * always did, end blends each corner pixel back toward the snapshot by its
 * outside-the-arc coverage. Everything inside the arc, and everything not in a
 * corner tile, is the plain blit, bit for bit.
 *
 * SURFACE CLIP, NOT DISPLAY CLIP: the group's rect is installed with
 * fb_clip_push, because fb_sdk() rebinds the display from the surface clip
 * whenever the fb context is dirty. Between begin and end this file changes no
 * clip at all (the blit and anim_blit_fade only read it), so the display's clip
 * is still the one begin installed when end runs.
 *
 * SCRATCH: snap == NULL is display_scratch(OL_DS_GROUP) of the display of the
 * CURRENT fb context, g_fbctx[gfx_ctx()].disp, so each par_render band worker
 * has its own and none is shared. The group is begun and ended in one call.
 *
 * Radius is 0 (no group, the old square blit) for a zoomed window, which is
 * the desktop's edge, and under -DWM_SQUARE_CONTENT, the guest negative control. */
int win_corner_r(const struct win *w)
{
#ifdef WM_SQUARE_CONTENT
    (void)w; return 0;
#else
    return w->zoomed ? 0 : S(10);
#endif
}
/* A panel has no title bar, so its canvas owns the top corners as well. */
unsigned win_cover_flags(const struct win *w)
{
    return w->panel ? WM_COVER_TOP_ROUND : 0;
}

/* Open the corner group over the content rect (x, y, cw, ch). 1 if open: the
 * caller must call content_group_end after the draw and nothing else changes
 * the surface clip in between. */
int content_group_begin(struct ol_display_group *g, int x, int y, int cw, int ch,
                               int r, unsigned flags)
{
    int top = (flags & WM_COVER_TOP_ROUND) ? r : 0;
    int rad[4] = { top, top, r, r };      /* TL, TR, BR, BL */
    if (cw <= 0 || ch <= 0) return 0;
    if (fb_clip_push(x, y, x + cw, y + ch) != OL_OK) return 0;
    if (ol_display_group_begin(fb_display_cur(), g, x, y, cw, ch, rad, 255, NULL, 0) != OL_OK) {
        fb_clip_pop();
        return 0;
    }
    return 1;
}
void content_group_end(struct ol_display_group *g)
{
    ol_display_group_end(fb_display_cur(), g);
    fb_clip_pop();
}

/* ---------- the window frame ---------- */
/* The three nested constant-alpha bands that used to be the window shadow are
 * gone; fb_shadow() is the one shadow in the system now, and it keeps the
 * perimeter-only property those bands existed for -- see the comment on it in
 * fb.c, which carries their reasoning forward. */

/* THE TITLEBAR/CONTENT SEAM (unit F, item 5). Both draw_frame_body and
 * draw_frame used to fill this hairline THEMSELVES, before their caller's
 * blit_content -- which starts at the exact same row, y+TBH, and is opaque.
 * The hairline was therefore painted and immediately overpainted, every
 * frame, and had never once reached the screen: what actually drew the
 * boundary was the raw jump from the titlebar's glass/gradient to the
 * content surface's flat fill, an accidental edge rather than a deliberate
 * one, and a vibrant material meeting a flat one with literally nothing
 * between them is exactly the "hard step" this item names. One function, and
 * BOTH call sites now invoke it AFTER blit_content -- on top of the
 * content's own first row, which is the only way to draw a pixel that a
 * same-frame full-opacity blit will not immediately erase -- so the pixel
 * that was always intended to be there finally is. */
void draw_titlebar_sep(int x, int y, int ww)
{
    fb_fill_rect(x, y, ww, S(1), g_ui_dark ? rgb(60, 60, 70) : rgb(214, 214, 220));
}

/* The window body (rounded bg + gradient titlebar + traffic lights + title) drawn
 * at an explicit rect, so it can be rendered into an off-screen surface at origin
 * for the open-pop scale animation as well as straight into `back`. */
static void draw_frame_body(int x, int y, int ww, int wh, const char *title, int focused)
{
    fb_round_rect(x, y, ww, wh, S(10), g_ui_dark ? rgb(30, 30, 36) : rgb(250, 250, 252));
    uint32_t tbtop, tbbot;
    if (g_ui_dark) { tbtop = focused ? rgb(60, 60, 70) : rgb(40, 40, 48); tbbot = focused ? rgb(44, 44, 52) : rgb(34, 34, 40); }
    else           { tbtop = focused ? rgb(246, 246, 250) : rgb(250, 250, 252); tbbot = focused ? rgb(226, 227, 234) : rgb(240, 240, 244); }
    fb_round_rect_vgrad(x, y, ww, TBH, S(10), tbtop, tbbot);
    /* NOT the separator -- see draw_titlebar_sep above; the caller draws it
     * after blit_content, or it is covered before it is ever seen. */
    uint32_t off = g_ui_dark ? rgb(80, 80, 90) : rgb(205, 205, 210);
    fb_fill_circle(x + S(16), y + S(15), S(6), focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + S(34), y + S(15), S(6), focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + S(52), y + S(15), S(6), focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(title);
    fb_text(x + (ww - tw) / 2, y + S(7), title, g_ui_dark ? rgb(210, 211, 218) : rgb(70, 70, 78));
}

/* Normal (non-animating) window frame: a LIQUID GLASS titlebar that frosts +
 * refracts the live backdrop behind the window's top. Drawn while `back` still
 * holds the backdrop (the app's content surface is blitted afterwards and covers
 * the body), so the glass samples real content, not the window's own fill. The
 * glass strip runs TITLEBAR_H+10 tall so its rounded bottom corners hide under
 * the content surface; the top corners round the window. (draw_frame_body, with a
 * solid gradient titlebar, is used only by the open-pop temp render, which has no
 * backdrop to sample.) */
void draw_frame(struct win *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    if (!w->zoomed)                       /* a zoom fills the desktop: nothing to cast on */
        wm_cast_shadow(focused ? WMSH_WIN_FOCUSED : WMSH_WIN_UNFOCUSED, x, y, ww, wh, S(10), 256, 255);
    if (w->panel) return;                 /* a panel is its canvas and its shadow */
    uint8_t a = focused ? (g_ui_dark ? 150 : 104) : (g_ui_dark ? 180 : 140);
    /* Zoomed, the titlebar runs on from the menu bar edge to edge, so its top,
     * left and right are CUT like the menu bar's own (no bevel, square): two
     * rounded bars stacked one under the other read as a doubled header. */
    unsigned cut = w->zoomed ? (GLASS_CUT_TOP | GLASS_CUT_LEFT | GLASS_CUT_RIGHT) : 0;
    if (wm_chrome_glass_titlebar())
        wm_glass_panel(WM_MAT_TITLE, x, y, ww, TBH + S(10), TITLE_GLASS_PT, cut, a);
    /* NOT the separator -- see draw_titlebar_sep above; the caller draws it
     * after blit_content, or it is covered before it is ever seen. */
    uint32_t off = g_ui_dark ? rgb(80, 80, 90) : rgb(205, 205, 210);
    fb_fill_circle(x + S(16), y + S(15), S(6), focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + S(34), y + S(15), S(6), focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + S(52), y + S(15), S(6), focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(w->title);
    fb_text(x + (ww - tw) / 2, y + S(7), w->title, g_ui_dark ? rgb(210, 211, 218) : rgb(70, 70, 78));
}

/* ---- the titlebar's input ---------------------------------------------------
 * A left press inside window wi's titlebar (the caller checked that): the
 * three lights, at last all three doing something, then the double-click.
 * The yellow and green ones have been drawn since M8 and had been decoration
 * for as long -- which is worse than absent, because a control that looks
 * like a control and does nothing teaches a user that this desktop's chrome
 * is a picture of a desktop. Returns 1 when a control took the press, 0 for
 * a plain titlebar press, which the caller makes a drag. */
int wm_chrome_click(int wi, int x, int y, int btn)
{
    if (btn != EV_BTN_LEFT) return 0;
    struct win *w = &wm_wins[wi];
    int cx = x - w->x, cy = y - w->y;
    int r2 = S(8) * S(8);
    int dyc = cy - S(15);
    if ((cx - S(16)) * (cx - S(16)) + dyc * dyc <= r2) {
        if (w->kind == WK_APP) wm_enqueue(w, EV_CLOSE, 0, 0);  /* close */
    } else if ((cx - S(34)) * (cx - S(34)) + dyc * dyc <= r2) {
        win_set_min(w, 1);                                  /* minimise */
    } else if ((cx - S(52)) * (cx - S(52)) + dyc * dyc <= r2) {
        win_set_zoom(w, !w->zoomed);                        /* zoom */
    } else if (titlebar_double_click(wi, x, y)) {
        /* Double-click the titlebar: the same toggle as the green
         * light. Two ways in for one action, because the light is
         * discoverable and the double-click is what a hand already
         * trained on another desktop will try first. */
        win_set_zoom(w, !w->zoomed);
    } else {
        return 0;
    }
    return 1;
}

/* Hover over window wi's frame (-1: no window under the pointer): nothing in
 * the titlebar reacts to it yet. */
int wm_chrome_pointer(int wi, int x, int y) { (void)wi; (void)x; (void)y; return 0; }

/* Per-pass idle work for window chrome: none. */
int wm_chrome_tick(uint64_t now) { (void)now; return 0; }

static uint32_t *anim_buf;
static int anim_buf_n;

/* The scratch surface every animated window is rendered into at FULL size
 * before being scaled down. Grown, never shrunk: the alternative is a
 * multi-megabyte kmalloc/kfree pair per frame of every animation. */
static struct surface *anim_scratch(int w, int h)
{
    static struct surface s;
    int need = w * h;
    if (need <= 0) return NULL;
    if (need > anim_buf_n) {
        if (anim_buf) kfree(anim_buf);
        anim_buf = kmalloc((unsigned)need * 4);
        anim_buf_n = anim_buf ? need : 0;
    }
    if (!anim_buf) return NULL;
    s.px = anim_buf; s.w = w; s.h = h;
    s.clip_on = 0;                       /* scratch is unclipped -- see fb.h */
    return &s;
}

/* ---- an animated window is drawn in the TWO PIECES it is made of -----------
 *
 * The obvious implementation -- render the whole window into a scratch surface
 * at full size, then scale that down -- is what the open pop did, and it is
 * affordable there because the pop scales ONE window by 16% for a sixth of a
 * second. Expose moves EVERY window at once, to a quarter of its size, and
 * paying full window area TWICE per window per frame (once to compose the
 * scratch, once to copy the app's canvas into it) made a single animated frame
 * cost more than the compositor's entire steady-state frame.
 *
 * That is not a theory. Measured on this machine, at 1280x800 with four
 * windows, the whole-window version presented THREE of the flight's eighteen
 * frames: the geometry was provably correct on the wire and nobody could see
 * it move. So:
 *
 *   THE CONTENT is the app's retained canvas, and it is scaled STRAIGHT from
 *   there into the destination. There is no intermediate copy to pay for, and
 *   the cost is the destination rectangle -- which is the thing being drawn.
 *   This is also the honest reading of "the surface IS the content": nobody
 *   asks the app to redraw, and nobody copies its pixels twice to avoid it.
 *
 *   THE CHROME is ours, and it is the only part that has to be composed. Only
 *   the TITLEBAR is ever visible (the content covers everything below it, in
 *   the animated path exactly as in the ordinary one), so only a w x TBH strip
 *   is rendered at full size and minified. That is what keeps a thumbnail's
 *   titlebar text the REAL text resampled, instead of the UI font
 *   re-rasterized at four points -- which is the whole reason to compose it at
 *   full size rather than draw it small.
 *
 * Cost per window per frame: from 2*W*H to (dest area + W*TBH). For the
 * Terminal at 900x590 scaled into a 400x260 cell, 1.06 M pixels to 131 K.
 *
 * TBH IS TALL ENOUGH to contain the rounded top corners -- radius S(10) against
 * a S(30) titlebar -- so nothing of the window's outline is lost by cutting the
 * strip there. If TITLEBAR_H ever drops below the corner radius, this strip has
 * to grow with it. */
struct surface *win_chrome_strip(struct win *w, int focused)
{
    int sh = win_tbh(w);
    if (sh > w->h) sh = w->h;
    if (sh < 1) return NULL;
    struct surface *tmp = anim_scratch(w->w, sh);
    if (!tmp) return NULL;
    fb_target(tmp);
    /* The whole body is asked for and the scratch's own height clips it to the
     * strip: the rounded top corners, the titlebar gradient, the three lights
     * and the title are exactly what survives, and they are exactly what shows. */
    draw_frame_body(0, 0, w->w, w->h, w->title, focused);
    fb_target(NULL);
    return tmp;
}

/* Two presses on the SAME window's titlebar, close together in time and in
 * space. The space test earns its place as much as the time one: a fast hand
 * clicking two different controls is not double-clicking either of them.
 *
 * A third click does not read as a second double-click -- the state is cleared
 * on a hit -- because otherwise holding a key down on a mouse and clicking four
 * times would zoom, restore, zoom, restore. */
static int titlebar_double_click(int wi, int x, int y)
{
    static int last_wi = -1, last_x, last_y;
    static uint64_t last_ms;
    uint64_t now = time_mono_ms();
    int slop = S(6);
    int hit = (last_wi == wi && now - last_ms <= (unsigned)wm_desktop_prefs.double_click &&
               x - last_x <= slop && last_x - x <= slop &&
               y - last_y <= slop && last_y - y <= slop);
    last_wi = hit ? -1 : wi;
    last_x = x; last_y = y; last_ms = now;
    return hit;
}
