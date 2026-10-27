/* wm_menubar.h -- the menu bar: what the other window-manager files use of wm_menubar.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_MENUBAR_H
#define LOGIT_WM_MENUBAR_H

#include "wm_internal.h"

/* The bar's box (always glass) and the titles' published layout (device px). */
void menubar_box(struct drect *r);
int wm_menubar_glass_rect(struct wm_glass_region *out, int max);
extern int g_menu_tx[NMENU_MAX], g_menu_tw[NMENU_MAX];
void menu_title_hit(int i, struct drect *r);

/* Painting. */
void draw_menubar(void);

/* Input: the dark-mode switch and a closed title; the IME label's hit box. */
int wm_menubar_click(int x, int y, int btn);
int wm_menubar_pointer(int x, int y);
int wm_menubar_ime_hit(int x, int y);

/* The clock's wake (damages the bar, returns the next deadline) and the per-pass hook. */
uint64_t wm_menubar_next_wake(uint64_t now);
int wm_menubar_tick(uint64_t now);

#endif /* LOGIT_WM_MENUBAR_H */
