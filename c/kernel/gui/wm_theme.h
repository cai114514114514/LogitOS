/* wm_theme.h -- theme state: what the other window-manager files use of
 * wm_theme.c. Other WM files include wm_internal.h (which includes this).
 * Self-contained on purpose: wm_theme.c and its host test
 * (tests/unit/ui_wm_abi_theme_test.c) include only this, wm.h and
 * settings.h, so the token derivation is tested exactly as it ships. */
#ifndef LOGIT_WM_THEME_H
#define LOGIT_WM_THEME_H

#include "logit_tokens.h"

/* Re-derive whatever the WM keeps about the theme from g_ui_dark and the
 * settings: today the accent roles of wm_tok() from ui.accent. Called from
 * the prefs tick (wm_run, every half second) and at the end of every theme
 * flip (wm_set_dark). Cheap when nothing changed (one settings read). */
void wm_theme_refresh(void);

/* The design tokens (logit_tokens.h, design 3.1) for the theme in force --
 * LIGHT or DARK by g_ui_dark -- with the accent roles derived from ui.accent
 * (3.1.4) as of the last wm_theme_refresh. Every new chrome colour comes from
 * here (protocol: no new literals in the kernel). Never NULL; do not keep the
 * pointer across frames. */
const struct logit_tokens *wm_tok(void);

#endif /* LOGIT_WM_THEME_H */
