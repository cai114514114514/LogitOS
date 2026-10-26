/* The four golden scenes. Each is drawn like an app frame -- aui_begin ... aui_end
 * through the real ring-3 aui.c -- at 100/125/150/200%, in a window of at most
 * 160x120 points.
 *
 * Pointer state is global to the toolkit (one pointer, so one hovered and one
 * pressed widget per frame). S1 wants default, hover, pressed and disabled
 * buttons side by side, so it draws the scene in several frames the way an app
 * would -- pointer over button 2, pointer down on button 3, a click into the
 * text field, pointer parked -- and the golden is the LAST frame with the
 * hover and pressed buttons' pixels (button rect plus its focus-ring margin)
 * carried over from the frames that showed them. That composition is the
 * harness's, not the toolkit's, and is the only place it exists. */
#include <stdlib.h>
#include <string.h>

#include "aui.h"
#include "aui_host.h"

const int aui_host_pcts[AUI_HOST_NPCT] = { 100, 125, 150, 200 };

static const char *const g_name[AUI_HOST_NSCENES] = {
    "s1_controls", "s2_rows", "s3_glass", "s4_sheet"
};
const char *aui_host_scene_name(int s) { return g_name[s]; }
void aui_host_scene_size(int s, int *w, int *h) { (void)s; *w = 160; *h = 120; }

static void ev(int type, int x, int y, int button)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = x; e.b = y; e.button = button;
    aui_feed(&e);
}
static void ev_done(void) { aui_feed_done(); }

/* ----------------------------------------------------------------- S1 */

static int s1_tog = 1, s1_chk = 1, s1_seg = 1, s1_val = 40;
static char s1_buf[16] = "hi";

enum { S1_B1X = 4, S1_B2X = 44, S1_B3X = 84, S1_B4X = 124, S1_BY = 2, S1_BW = 32, S1_BH = 20 };

static void s1_draw(struct aui_host *h)
{
    static const char *const seg[3] = { "A", "B", "C" };
    (void)h;
    aui_begin(AUI_BG);
    aui_button(S1_B1X, S1_BY, S1_BW, S1_BH, "Ok");
    aui_button(S1_B2X, S1_BY, S1_BW, S1_BH, "Hov");
    aui_button(S1_B3X, S1_BY, S1_BW, S1_BH, "Prs");
    aui_button_ex(S1_B4X, S1_BY, S1_BW, S1_BH, "Off", AUI_V_SECONDARY, 0);
    aui_toggle(4, 26, &s1_tog, 1);
    aui_checkbox(56, 29, "Chk", &s1_chk);
    /* chevron row */
    aui_label(112, 30, "Row", AUI_TEXT);
    aui_text_sz(146, 30, "\xE2\x80\xBA", AUI_TEXT, 13);
    aui_hairline(96, 50, 60);
    aui_segmented(4, 52, 152, 20, seg, 3, &s1_seg);
    aui_textfield(4, 76, 100, s1_buf, (int)sizeof s1_buf);
    aui_slider(110, 79, 46, &s1_val, 0, 100);
    aui_end();
}

/* Save / restore a point rect of the surface (device pixels via the same
 * scaling the transport uses). */
struct grab { int x0, y0, w, h; uint32_t *px; };
static void grab_take(struct aui_host *h, struct grab *g, int x, int y, int w, int hh)
{
    int x0 = aui_host_pt(x, h->pct), y0 = aui_host_pt(y, h->pct);
    int x1 = aui_host_pt(x + w, h->pct), y1 = aui_host_pt(y + hh, h->pct);
    if (x1 > h->w) x1 = h->w;
    if (y1 > h->h) y1 = h->h;
    g->x0 = x0; g->y0 = y0; g->w = x1 - x0; g->h = y1 - y0;
    g->px = malloc((size_t)g->w * (size_t)g->h * 4);
    for (int j = 0; j < g->h; j++)
        memcpy(g->px + (size_t)j * g->w, h->px + (size_t)(y0 + j) * h->w + x0, (size_t)g->w * 4);
}
static void grab_put(struct aui_host *h, struct grab *g)
{
    for (int j = 0; j < g->h; j++)
        memcpy(h->px + (size_t)(g->y0 + j) * h->w + g->x0, g->px + (size_t)j * g->w, (size_t)g->w * 4);
    free(g->px);
}

static void scene_s1(struct aui_host *h)
{
    struct grab hov, prs;
    /* frame 1: pointer hovers button 2 */
    aui_host_clock_set(1000);
    ev(EV_MOUSE_MOVE, S1_B2X + S1_BW / 2, S1_BY + S1_BH / 2, 0);
    s1_draw(h); ev_done();
    grab_take(h, &hov, S1_B2X - 5, 0, S1_BW + 10, S1_BH + 8);
    /* frame 2: button 3 pressed (pointer down, not yet released) */
    aui_host_clock_set(1500);
    ev(EV_MOUSE, S1_B3X + S1_BW / 2, S1_BY + S1_BH / 2, EV_BTN_LEFT);
    s1_draw(h); ev_done();
    grab_take(h, &prs, S1_B3X - 5, 0, S1_BW + 10, S1_BH + 8);
    ev(EV_MOUSE_UP, -100, -100, EV_BTN_LEFT);
    s1_draw(h); ev_done();
    /* frame 3: click into the text field (focus, caret) */
    aui_host_clock_set(2000);
    ev(EV_MOUSE, 50, 90, EV_BTN_LEFT);
    s1_draw(h); ev_done();
    ev(EV_MOUSE_UP, -100, -100, EV_BTN_LEFT);
    /* the golden frame: pointer parked off the window */
    aui_host_clock_set(2100);
    ev(EV_MOUSE_MOVE, -100, -100, 0);
    s1_draw(h); ev_done();
    grab_put(h, &hov);
    grab_put(h, &prs);
}

/* ----------------------------------------------------------------- S2 */

static void scene_s2(struct aui_host *h)
{
    static const struct { const char *s; int px; } row[5] = {
        { "Inbox", 13 },
        { "A considerably longer subject line that must ellipsize", 13 },
        { "Mixed 17px title", 17 },
        { "Another very long seventeen pixel label here", 17 },
        { "\xE7\x9F\xAD\xE3\x81\x84\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\xA9\xE3\x83\x99\xE3\x83\xAB wide", 13 },
    };
    (void)h;
    aui_host_clock_set(1000);
    ev(EV_MOUSE_MOVE, -100, -100, 0);
    aui_begin(AUI_BG);
    for (int i = 0; i < 5; i++) {
        int y = 4 + i * 22;
        unsigned fg = AUI_TEXT;
        if (i == 1) { aui_round(4, y, 152, 20, 6, AUI_ACCENT); fg = AUI_ACCENT_TEXT; }
        aui_text_ellipsis(10, y + 2, 118, row[i].s, fg, row[i].px);
        aui_text_sz(138, y + 3, "12", fg, 13);
        if (i < 4) aui_hairline(8, y + 21, 144);
    }
    aui_end();
    ev_done();
}

/* ----------------------------------------------------------------- S3 */

static void scene_s3(struct aui_host *h)
{
    (void)h;
    aui_host_clock_set(1000);
    ev(EV_MOUSE_MOVE, -100, -100, 0);
    aui_begin(AUI_BG);
    aui_vgrad(0, 0, 160, 120, rgb(255, 120, 60), rgb(40, 60, 200));
    aui_fill(0, 38, 160, 6, rgb(255, 255, 255));
    aui_fill(52, 0, 6, 120, rgb(20, 20, 20));
    aui_circle(30, 92, 18, rgb(250, 230, 80));
    aui_glass(8, 8, 100, 52, 14);
    aui_label(22, 28, "Glass", AUI_TEXT);
    aui_shadow_ex(76, 70, 76, 40, 10, 4, 12, 90);
    aui_round(76, 70, 76, 40, 10, rgb(250, 250, 250));
    aui_label(86, 82, "Card", rgb(20, 20, 20));
    aui_end();
    ev_done();
}

/* ----------------------------------------------------------------- S4 */

static void s4_draw(void)
{
    static const char *const btn[1] = { "Done" };
    aui_begin(AUI_BG);
    aui_label(8, 8, "Document", AUI_TEXT);
    aui_button(8, 28, 60, 22, "Open");
    aui_text_sz(8, 60, "Background text", AUI_TEXT, 13);
    if (aui_dialog_begin("Sheet", 120, 64)) {
        aui_label(12, 2, "Sheet body", AUI_TEXT);
        aui_dialog_buttons(btn, 1);
        aui_dialog_end();
    }
    aui_end();
}

/* The sheet opens at the first frame and is drawn again 135 ms later: half of
 * AUI_T_SLOW (270 ms) on the fake clock. */
static void scene_s4(struct aui_host *h)
{
    (void)h;
    ev(EV_MOUSE_MOVE, -100, -100, 0);
    aui_host_clock_set(5000);
    s4_draw(); ev_done();
    aui_host_clock_set(5000 + AUI_T_SLOW / 2);
    s4_draw(); ev_done();
}

void aui_host_scene_draw(int scene, struct aui_host *h)
{
    int w, hh;
    aui_host_scene_size(scene, &w, &hh);
    gui_create("aui-host", w, hh);
    aui_set_size(w, hh);
    switch (scene) {
    case 0: scene_s1(h); break;
    case 1: scene_s2(h); break;
    case 2: scene_s3(h); break;
    default: scene_s4(h); break;
    }
}
