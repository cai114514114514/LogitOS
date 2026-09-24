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
