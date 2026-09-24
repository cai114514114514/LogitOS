/* The compositor's band splitter (c/kernel/gui/wm_bands.h), as properties over
 * randomised inputs rather than examples:
 *
 *   COVER     the bands tile [y0, y1) exactly, in order, no gap, no overlap
 *   NO-CUT    no boundary falls strictly inside a no-cut (glass sample) range
 *   MIN       every band of a multi-band split is >= WM_BAND_MIN_ROWS tall
 *   COUNT     never more bands than asked for
 *   BALANCE   on a plain rectangle with no glass, the split is even to within
 *             one minimum band
 *
 * NO-CUT is the one that makes a parallel frame equal the serial one; the
 * control (-DWM_BANDS_NEGCTL_IGNORE_GLASS) removes it and must fail here. */
#include <stdio.h>
#include <stdlib.h>
#include "wm_bands.h"

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; if (fails < 20) printf("FAIL %s\n", what); } }

int main(void)
{
    srand(20260924);
    int cover_bad = 0, nocut_bad = 0, min_bad = 0, count_bad = 0, multi = 0;
    for (int t = 0; t < 50000; t++) {
        int y0 = rand() % 600, y1 = y0 + 1 + rand() % 1200;
        struct wm_yrange nc[6], gl[6];
        int nn = rand() % 6;
        for (int i = 0; i < nn; i++) {
            int a = y0 + rand() % (y1 - y0), h = 10 + rand() % 250;
            nc[i].y0 = a; nc[i].y1 = a + h;
            gl[i].y0 = a + 25; gl[i].y1 = a + h - 25;
            if (gl[i].y1 < gl[i].y0) gl[i].y1 = gl[i].y0;
        }
        int want = 1 + rand() % 6;
        struct wm_band b[WM_BAND_MAX];
        int n = wm_bands_split(y0, y1, nc, nn, gl, nn, want, b);
        if (n > (want < 1 ? 1 : want)) count_bad++;
        if (b[0].y0 != y0 || b[n - 1].y1 != y1) cover_bad++;
        for (int i = 0; i < n; i++) {
            if (b[i].y1 <= b[i].y0) cover_bad++;
            if (i && b[i].y0 != b[i - 1].y1) cover_bad++;
            if (n > 1 && b[i].y1 - b[i].y0 < WM_BAND_MIN_ROWS) min_bad++;
            if (i) for (int k = 0; k < nn; k++)
                if (b[i].y0 > nc[k].y0 && b[i].y0 < nc[k].y1) nocut_bad++;
        }
        if (n > 1) multi++;
    }
    check(cover_bad == 0, "COVER: bands tile the rectangle exactly");
    check(nocut_bad == 0, "NO-CUT: no boundary inside a glass sample range");
    check(min_bad == 0, "MIN: every band at least WM_BAND_MIN_ROWS tall");
    check(count_bad == 0, "COUNT: never more bands than asked for");
    check(multi > 10000, "the property runs actually split (not all serial)");

    /* BALANCE: 1200 plain rows into 4 -> 300 each, give or take. */
    struct wm_band b[WM_BAND_MAX];
    int n = wm_bands_split(0, 1200, 0, 0, 0, 0, 4, b);
    int ok = n == 4;
    for (int i = 0; i < n; i++) if (b[i].y1 - b[i].y0 < 300 - WM_BAND_MIN_ROWS || b[i].y1 - b[i].y0 > 300 + WM_BAND_MIN_ROWS) ok = 0;
    check(ok, "BALANCE: 1200 plain rows split 4 ways near 300 each");

    /* A glass band costs more: the band holding rows 0..200 of glass should be
     * shorter than the plain ones. */
    struct wm_yrange g = { 0, 200 };
    n = wm_bands_split(0, 1200, &g, 1, &g, 1, 3, b);
    check(n == 3 && b[0].y1 - b[0].y0 >= 200 && b[0].y1 - b[0].y0 < b[2].y1 - b[2].y0,
          "COST: the glass rows are not cut, and their band is the shortest in rows");

    /* The menu bar's case: a panel whose sample range is the top 70 rows and a
     * 60-row damage rectangle inside it -- no cut possible, serial. */
    struct wm_yrange mb = { 0, 70 };
    n = wm_bands_split(0, 60, &mb, 1, &mb, 1, 4, b);
    check(n == 1, "a rectangle inside one panel stays one band");

    printf("wm-bands: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
