/* Host test of wm_cover_ok (c/kernel/gui/wm_cover.h): a damage rectangle that
 * touches a rounded window corner is not "covered" by the app canvas. */
#include <stdio.h>
#include "../../c/kernel/gui/wm_cover.h"

static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } } while (0)

static struct drect rc(int x0, int y0, int x1, int y1) { struct drect r = {x0, y0, x1, y1}; return r; }

int main(void)
{
    static const int pct[] = { 100, 125, 150, 200 };
    for (unsigned k = 0; k < sizeof pct / sizeof *pct; k++) {
        int r = 10 * pct[k] / 100, tbh = 30 * pct[k] / 100;
        int x = 100, y = 80, w = 400, h = 300;
        char nm[96];
        /* every bottom corner square, down to a single pixel in its far corner */
        struct drect bl1 = rc(x, y + h - 1, x + 1, y + h);
        struct drect bl2 = rc(x + r - 1, y + h - r, x + r, y + h - r + 1);
        struct drect br1 = rc(x + w - 1, y + h - 1, x + w, y + h);
        struct drect br2 = rc(x + w - r, y + h - r, x + w - r + 1, y + h - r + 1);
        struct drect wide = rc(x, y + tbh, x + w, y + h);       /* whole canvas */
        snprintf(nm, sizeof nm, "corner-cover bl-edge %d%%", pct[k]);  CHECK(nm, !wm_cover_ok(&bl1, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "corner-cover bl-inner %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&bl2, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "corner-cover br-edge %d%%", pct[k]);  CHECK(nm, !wm_cover_ok(&br1, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "corner-cover br-inner %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&br2, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "corner-cover whole canvas %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&wide, x, y, w, h, tbh, r, 0));
        /* a full-height strip down the left side still reaches the corner */
        struct drect strip = rc(x, y + tbh, x + 2, y + h);
        snprintf(nm, sizeof nm, "corner-cover left strip %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&strip, x, y, w, h, tbh, r, 0));

        /* accepted: interior, and rects hugging a corner square without entering it */
        struct drect in  = rc(x + 50, y + tbh + 20, x + w - 50, y + h - 20);
        struct drect bot = rc(x + r, y + h - 1, x + w - r, y + h);        /* bottom edge between the arcs */
        struct drect lft = rc(x, y + tbh, x + 1, y + h - r);              /* left edge above the arc */
        struct drect rgt = rc(x + w - 1, y + tbh, x + w, y + h - r);
        snprintf(nm, sizeof nm, "accept interior %d%%", pct[k]); CHECK(nm, wm_cover_ok(&in, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "accept bottom edge %d%%", pct[k]); CHECK(nm, wm_cover_ok(&bot, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "accept left edge %d%%", pct[k]); CHECK(nm, wm_cover_ok(&lft, x, y, w, h, tbh, r, 0));
        snprintf(nm, sizeof nm, "accept right edge %d%%", pct[k]); CHECK(nm, wm_cover_ok(&rgt, x, y, w, h, tbh, r, 0));
        /* outside the canvas: the title bar is not canvas */
        struct drect tb = rc(x + 50, y, x + 100, y + tbh + 1);
        snprintf(nm, sizeof nm, "reject titlebar rows %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&tb, x, y, w, h, tbh, r, 0));

        /* title bar hidden / panel: canvas owns the top edge, top squares rounded too */
        struct drect tl = rc(x, y, x + 1, y + 1);
        struct drect tr = rc(x + w - r, y + r - 1, x + w, y + r);
        struct drect tmid = rc(x + r, y, x + w - r, y + 1);
        struct drect pin = rc(x + 50, y + 20, x + w - 50, y + h - 20);
        snprintf(nm, sizeof nm, "corner-cover tl hidden-tb %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&tl, x, y, w, h, 0, r, WM_COVER_TOP_ROUND));
        snprintf(nm, sizeof nm, "corner-cover tr hidden-tb %d%%", pct[k]); CHECK(nm, !wm_cover_ok(&tr, x, y, w, h, 0, r, WM_COVER_TOP_ROUND));
        snprintf(nm, sizeof nm, "accept top edge hidden-tb %d%%", pct[k]); CHECK(nm, wm_cover_ok(&tmid, x, y, w, h, 0, r, WM_COVER_TOP_ROUND));
        snprintf(nm, sizeof nm, "accept interior hidden-tb %d%%", pct[k]); CHECK(nm, wm_cover_ok(&pin, x, y, w, h, 0, r, WM_COVER_TOP_ROUND));
    }
    /* radius clamp: a window thinner than 2r has corners meeting, no crash */
    struct drect z = rc(105, 85, 106, 86);
    CHECK("corner-cover clamp", !wm_cover_ok(&z, 100, 80, 20, 12, 0, 50, WM_COVER_TOP_ROUND));
    /* square window (zoomed): everything inside the canvas is covered */
    struct drect sq = rc(100, 330, 101, 380);
    CHECK("square window accepts corner", wm_cover_ok(&sq, 100, 80, 400, 300, 30, 0, 0));
    struct drect empty = rc(5, 5, 5, 9);
    CHECK("empty rect rejected", !wm_cover_ok(&empty, 0, 0, 100, 100, 0, 0, 0));
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("PASS wm_cover_test\n");
    return 0;
}
