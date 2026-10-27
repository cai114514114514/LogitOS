#ifndef WM_BANDS_H
#define WM_BANDS_H

/* HOW ONE DAMAGE RECTANGLE IS SPLIT ACROSS CORES.
 *
 * The compositor used to build every frame on one core while the others sat
 * halted: measured 2026-08-28, ~95% of the machine idle during a 94 ms frame.
 * A frame is now cut into horizontal BANDS, each rendered start to finish --
 * wallpaper, windows, chrome -- by whichever core claims it (wm.c,
 * render_band).
 *
 * THE ONE RULE THAT MAKES THIS CORRECT WITHOUT A BARRIER: never cut through a
 * glass panel's SAMPLE extent. Liquid glass reads the backdrop already
 * composited beneath it, including a halo outside the panel. If a cut ran
 * through that extent, one core would frost pixels another core had not
 * finished drawing. With every panel and everything it samples inside a
 * single band, each band's pixels depend only on that band's own work, so the
 * parallel frame is the serial frame, pixel for pixel -- a property the
 * guest gate checks by re-rendering serially and comparing (test-wm-par).
 *
 * Bands are balanced by COST, not rows: a glass row costs several times an
 * ordinary one (102 vs 29 ns/px, measured 2026-08-29), so equal rows would put
 * the menu bar's whole price on one core.
 *
 * Pure: integers in, integers out, host-tested by make test-wm-bands. */

struct wm_yrange { int y0, y1; };          /* half-open [y0, y1) */
struct wm_band   { int y0, y1; };

#define WM_BAND_MAX       8
#define WM_BAND_MIN_ROWS 32
#define WM_BAND_COST_ROW   2               /* an ordinary row ...          */
#define WM_BAND_COST_GLASS 7               /* ... and a glass row: 3.5x    */

static inline int wm_band_in(const struct wm_yrange *r, int n, int y)
{
    for (int i = 0; i < n; i++) if (y >= r[i].y0 && y < r[i].y1) return 1;
    return 0;
}

/* May a band boundary fall at row y (the first row of the lower band)? */
static inline int wm_band_cut_ok(const struct wm_yrange *nocut, int nn, int y)
{
#ifdef WM_BANDS_NEGCTL_IGNORE_GLASS
    /* THE CONTROL: cut anywhere. make test-wm-bands-negctl requires the
     * no-cut property to fail with this, and the guest control build
     * (test-wm-par-negctl) requires the pixel comparison to. */
    (void)nocut; (void)nn; (void)y;
    return 1;
#else
    for (int i = 0; i < nn; i++) if (y > nocut[i].y0 && y < nocut[i].y1) return 0;
    return 1;
#endif
}

/* Append the no-cut rows of one GLASS INPUT BOX -- the panel (x0,y0)-(x1,y1)
 * grown by the material's sample halo -- if that box meets the rectangle
 * (rx0,ry0)-(rx1,ry1) being split. This is the one rule for every glass the
 * compositor draws: the WM's own panels (wm.c, par_nocut) and the backdrop
 * regions behind app windows (wm_backdrop.c, wm_backdrop_nocut). A region
 * frosts the layers beneath its window, so a band boundary through its sample
 * extent would frost pixels another core has not drawn yet -- exactly the
 * WM-glass case. Returns the new count; a full list (n == max) is unchanged. */
static inline int wm_bands_nocut_add(struct wm_yrange *nc, int n, int max,
                                     int x0, int y0, int x1, int y1, int halo,
                                     int rx0, int ry0, int rx1, int ry1)
{
    if (x1 + halo > rx0 && x0 - halo < rx1 && y1 + halo > ry0 && y0 - halo < ry1 && n < max) {
        nc[n].y0 = y0 - halo;
        nc[n].y1 = y1 + halo;
        n++;
    }
    return n;
}

/* Split rows [y0, y1) into at most `want` bands (<= WM_BAND_MAX). `nocut`
 * are the rows no boundary may fall strictly inside; `glass` the rows that
 * cost more. Returns the band count; 1 means "render it serially". */
static inline int wm_bands_split(int y0, int y1,
                                 const struct wm_yrange *nocut, int nn,
                                 const struct wm_yrange *glass, int ng,
                                 int want, struct wm_band *out)
{
    out[0].y0 = y0; out[0].y1 = y1;
    if (want > WM_BAND_MAX) want = WM_BAND_MAX;
    if (want < 2 || y1 - y0 < 2 * WM_BAND_MIN_ROWS) return 1;

    long total = 0;
    for (int y = y0; y < y1; y++)
        total += wm_band_in(glass, ng, y) ? WM_BAND_COST_GLASS : WM_BAND_COST_ROW;

    int n = 0, last = y0;
    long acc = 0;
    int y = y0;
    for (int k = 1; k < want; k++) {
        long target = total * k / want;
        while (y < y1 && acc < target) {
            acc += wm_band_in(glass, ng, y) ? WM_BAND_COST_GLASS : WM_BAND_COST_ROW;
            y++;
        }
        /* The nearest legal boundary to y, looking both ways, that leaves both
         * neighbours at least WM_BAND_MIN_ROWS tall. */
        int best = -1;
        for (int d = 0; d < y1 - y0 && best < 0; d++) {
            int c[2] = { y - d, y + d };
            for (int j = 0; j < 2 && best < 0; j++) {
                int cy = c[j];
                if (cy - last < WM_BAND_MIN_ROWS || y1 - cy < WM_BAND_MIN_ROWS) continue;
                if (wm_band_cut_ok(nocut, nn, cy)) best = cy;
            }
        }
        if (best < 0) continue;
        out[n].y0 = last; out[n].y1 = best; n++;
        last = best;
        /* Resynchronise the running cost to the chosen boundary. */
        acc = 0;
        for (int yy = y0; yy < best; yy++)
            acc += wm_band_in(glass, ng, yy) ? WM_BAND_COST_GLASS : WM_BAND_COST_ROW;
        y = best;
    }
    out[n].y0 = last; out[n].y1 = y1; n++;
    return n;
}

#endif
