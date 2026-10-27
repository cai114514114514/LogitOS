/* wm_reveal.c -- the theme reveal.
 *
 * The 日夜 reveal (design 5.10): a theme flip that grows the new theme
 * outward in a circle from where it was asked for, instead of swapping the
 * whole screen in one frame. These are the hooks the rest of the WM already
 * calls at the right moments -- before the flip (wm_set_dark), on a window's
 * flush, on full-screen damage, at present and on every pass. No reveal is
 * built yet, so every one answers "no reveal": the flip takes the instant
 * path it always took, and nothing here draws, allocates or holds state.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

int wm_reveal_begin(int cx, int cy)
{
    (void)cx; (void)cy;
    return 0;
}

int wm_reveal_active(void) { return 0; }

void wm_reveal_note_flush(const struct win *w) { (void)w; }

void wm_reveal_present(void) { }

int wm_reveal_tick(uint64_t now) { (void)now; return 0; }
