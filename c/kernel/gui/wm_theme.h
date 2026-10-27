/* wm_theme.h -- theme state: what the other window-manager files use of
 * wm_theme.c. Include wm_internal.h (which includes this), not this file on
 * its own. */
#ifndef LOGIT_WM_THEME_H
#define LOGIT_WM_THEME_H

#include "wm_internal.h"

/* Re-derive whatever the WM keeps about the theme from g_ui_dark and the
 * settings. Called from the prefs tick (wm_run, every half second) and at
 * the end of every theme flip (wm_set_dark). Nothing is derived yet. */
void wm_theme_refresh(void);

#endif /* LOGIT_WM_THEME_H */
