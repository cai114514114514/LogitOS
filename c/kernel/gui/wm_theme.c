/* wm_theme.c -- theme state.
 *
 * What the WM derives from the theme and the appearance settings, refreshed
 * on the prefs tick and on every flip: the home of the chrome's colour
 * tokens once they replace the literal pairs in the painters (design P3).
 * Nothing is derived yet; the painters still read g_ui_dark directly.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

void wm_theme_refresh(void) { }
