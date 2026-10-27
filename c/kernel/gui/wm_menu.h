/* wm_menu.h -- the menu model and dropdowns: what the other window-manager files use of wm_menu.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_MENU_H
#define LOGIT_WM_MENU_H

#include "wm_internal.h"

#define NMENU_MAX (MB_MAX_MENUS + 2)

/* The model the bar shows: its titles, and the open menu (-1: none). */
extern int g_nmenu;
extern char g_menu_title[NMENU_MAX][MB_TITLE_MAX + 1];
extern int g_menu_reported;
extern int g_menu_open;
void menu_model_sync(void);

/* The open dropdown: its box, its glass, painting it, closing it. */
void menu_dropdown_box(struct drect *r);
int wm_menu_glass_rect(struct wm_glass_region *out, int max);
void draw_menu_dropdown(void);
void menu_close(void);

/* Input: Cmd+<key> to an app's menu, a title under the pointer, opening one,
 * and the open menu's share of the pointer (pointer always before click). */
int menu_key(int c);
int menu_title_at(int x, int y);
void menu_open(int i);
int wm_menu_pointer(int x, int y);
int wm_menu_click(int x, int y, int btn);

/* Per-pass hook. */
int wm_menu_tick(uint64_t now);

#endif /* LOGIT_WM_MENU_H */
