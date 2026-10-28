/* aui_overlay.h -- menus, popovers, the in-window menu bar, sheets and alerts.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core); the 「澄」 overlays (design 4.9, 4.10)
 * were added by ui-aui-overlays. */
#ifndef AUI_OVERLAY_H
#define AUI_OVERLAY_H

/* ---------------------------------------------------------------- menus --
 * One menu panel (design 4.9) serves the select popup, the in-window menu
 * bar's dropdowns, context menus and aui_menu_panel(): material 3.2 "menus"
 * (THICK, radius 12), elevation E3, rows of 24, an accent hover pill (inset 5,
 * radius 7), 9-tall separators, a vector CHECK, fixed shortcut cells.
 *
 * An item string is
 *     [flag bytes] ["✓ "] label ["\t" shortcut]
 * "-" alone is a separator. A flag byte is one of AUI_MI_DESTRUCTIVE (error
 * ink), AUI_MI_DISABLED (quaternary ink, no hover, not chosen) or
 * AUI_MI_SUBMENU (a CHEVRON_R at the right). A leading U+2713 (and the space
 * after it) is drawn as the CHECK icon, never as text. The shortcut's leading
 * modifier glyphs (⌘ ⌥ ⇧ ⌃) are right-aligned in a 30-wide cell and the key
 * follows in a 14-wide cell, so every shortcut in a menu lines up. Those glyphs
 * come from text.ttf through the font fallback.
 *
 * Opening: the panel fades in over 180 ms (STANDARD) and settles 4 pt down on
 * AUI_SPRING_SMOOTH. THE MATERIAL IS NOT RE-ISSUED PER ANIMATION FRAME: while
 * it moves the plate is the 3.2 solid (bg.raised) inside one opacity group, and
 * the single GLASS2 is drawn on the frame that settles (and on every later
 * repaint of the open menu). Under Reduce Transparency, or without GLASS2, the
 * plate is the solid throughout. Closing fades over 120 ms (IN_CUBIC). Moving
 * from one menu-bar title to the next swaps the panel without a fade.
 *
 * Aims (single emitter, ui_aim.h): popup-<i> for a select popup or a menu-bar
 * dropdown, context-<i> for a context menu, once the panel has settled. */
#define AUI_MI_DESTRUCTIVE "\x01"
#define AUI_MI_DISABLED    "\x02"
#define AUI_MI_SUBMENU     "\x03"

/* A menu bar: `titles` across the top, each opening `items[i]` (a NULL-
 * terminated array of item strings as above). Writes the chosen menu index
 * into *mi and the item index into *ii, and returns 1. */
int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii);

/* A context menu of `n` items at (x, y) (the current drawing coordinates).
 * Call it with x >= 0 ONCE, on the frame of the right-click, to open it; the
 * panel then draws itself at the end of every frame (like a select popup),
 * flips to stay inside the window, and closes on a choice, a click anywhere
 * else or Escape. Every call returns the item chosen since the last call, once
 * (-1 for none): poll with aui_context_menu(-1, -1, items, n). `items` must
 * stay valid while the menu is open. */
int aui_context_menu(int x, int y, const char *const *items, int n);

/* The panel as a still image: plate, rows and the hover pill on item `hi`
 * (-1 for none) at (x, y), `w` wide (aui_menu_width), with no input and no
 * animation. For an app that lays out its own menu but must look like one. */
int  aui_menu_width(const char *const *items, int n);
int  aui_menu_height(const char *const *items, int n);
void aui_menu_panel(int x, int y, int w, const char *const *items, int n, int hi);

/* -------------------------------------------------------------- popover --
 * A popover (design 4.9): material "popover", radius 14, E3, an arrow 16 x 8
 * with a rounded tip, 6 pt from the anchor. `anchor` is in the current drawing
 * coordinates; `w` x `h` is the WHOLE popover, and the 16 pt padding is inside
 * it: draw into the (w - 32) x (h - 32) space between begin and end, in
 * popover-local coordinates (like aui_dialog_begin). `edge` is the side of the
 * anchor it opens on (AUI_EDGE_*); it flips to the opposite side when it would
 * leave the window and slides along the edge to stay inside. Call it every
 * frame while it is open; it fades in over 180 ms and settles 4 pt on
 * AUI_SPRING_SMOOTH, with the same solid-then-glass rule as the menus.
 *
 * aui_popover_dismissed() answers (once) whether the user clicked outside the
 * popover (the anchor excepted, so a toggle button keeps its own say) or
 * pressed Escape since the last call: the caller then stops calling begin. */
#define AUI_EDGE_TOP    0
#define AUI_EDGE_BOTTOM 1
#define AUI_EDGE_LEFT   2
#define AUI_EDGE_RIGHT  3
int  aui_popover_begin(struct aui_rect anchor, int w, int h, int edge);
void aui_popover_end(void);
int  aui_popover_dismissed(void);

/* ------------------------------------------------------- sheet and alert --
 * Modal sheet (design 4.10). Returns 1 while it is open; draw its content
 * between the two calls, in dialog-local coordinates. The scrim (black @ 46
 * light / @ 89 dark, fading over 180 ms) covers the owner window and swallows
 * clicks behind it. The panel is bg.raised, radius 18, E6, with the dark rim.
 * It has NO title band: the title (title2 17 / 600) is part of the content,
 * left at x 20 in the top 40 pt of the local space (kept so the local origin
 * and existing layouts do not move); the panel is `w` x (`h` + 40). It rises 12
 * pt on a smooth curve while it fades in. */
int  aui_dialog_begin(const char *title, int w, int h);
void aui_dialog_end(void);
/* The standard footer, right-aligned, h 28, 8 apart. labels[0] is the Primary
 * and the RIGHTMOST button; labels[n-1] is the cancel and the leftmost.
 * Returns the index pressed, or -1. Enter picks item 0 (the default), Escape
 * the last one (the cancel). Aims: dialog-<i> (0 = rightmost), dialog-confirm
 * (item 0) and, with more than one button, dialog-cancel (the last). */
int  aui_dialog_buttons(const char *const *labels, int n);

/* Alert (design 4.10, macOS 26 style): 300 wide, bg.raised, radius 18, E6,
 * padding 20. `icon` is a GICON_* drawn 28 on a 48 tint circle (0 = none);
 * the tint is chosen by `flags & AUI_ALERT_KIND` (info = accent). Title
 * (title2, centred), message (callout, label.secondary, wrapped to 260, at
 * most 4 lines), then the buttons: TWO sit side by side, each 126 x 32, the
 * Primary (or, with AUI_ALERT_DESTRUCTIVE, the Destructive) on the right;
 * THREE OR MORE stack full width, 8 apart, the primary on top. buttons[0] is
 * that primary, buttons[n-1] the cancel. Enter = default, Escape = cancel;
 * with AUI_ALERT_DESTRUCTIVE the default is the cancel. Call it every frame
 * while the alert is open; it returns the button index pressed, else -1.
 * Aims: alert-<i> (0 = rightmost / top), alert-confirm (item 0) and, with more
 * than one button, alert-cancel (the last). */
#define AUI_ALERT_DESTRUCTIVE (1u << 0)
#define AUI_ALERT_INFO        (0u << 1)
#define AUI_ALERT_WARNING     (1u << 1)
#define AUI_ALERT_ERROR       (2u << 1)
#define AUI_ALERT_SUCCESS     (3u << 1)
#define AUI_ALERT_KIND        (3u << 1)
int aui_alert(int icon, const char *title, const char *message,
              const char *const *buttons, int n, unsigned flags);

#endif /* AUI_OVERLAY_H */
