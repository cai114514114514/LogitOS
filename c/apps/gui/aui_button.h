/* aui_button.h -- buttons and the select control.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core); the 「澄」 variants are design 4.3
 * (ui-aui-buttons). */
#ifndef AUI_BUTTON_H
#define AUI_BUTTON_H

/* ---- button variants (design 4.3) ----
 * aui_button_ex takes an enum aui_variant; the 4.3 names alias the legacy
 * enumerators (GLASS -> SECONDARY, GHOST -> PLAIN, DANGER -> DESTRUCTIVE) and
 * add the variants that had no enumerator. A variant may carry AUI_BTN_F_*
 * flags in the bits above 0xFF: `AUI_BTN_DESTRUCTIVE | AUI_BTN_F_CONFIRM`. */
enum {
    AUI_BTN_PRIMARY     = AUI_V_PRIMARY,    /* accent, the one action of a screen   */
    AUI_BTN_SECONDARY   = AUI_V_SECONDARY,  /* the default                          */
    AUI_BTN_PLAIN       = AUI_V_GHOST,      /* no fill; accent.ink label            */
    AUI_BTN_DESTRUCTIVE = AUI_V_DANGER,     /* Plain + error.ink; fill with _F_CONFIRM */
    AUI_BTN_ICON        = 8,                /* 28 (inline 24) capsule, symbol 16    */
    AUI_BTN_TOGGLE_ICON = 9,                /* Icon; ON = accent.tint + accent.ink  */
    AUI_BTN_AI          = 10,               /* Secondary + Logit-gradient stroke    */
    AUI_BTN_POPUP       = 11                /* Secondary, text left, chevron right  */
};
#define AUI_BTN_F_TOOLBAR 0x100   /* Plain: label.primary instead of accent.ink     */
#define AUI_BTN_F_CONFIRM 0x200   /* Destructive: destructive.fill + white label     */
#define AUI_BTN_F_ON      0x400   /* Toggle icon: on                                 */

/* Design 4.3 minimum widths: 64, and 84 for alert and sheet buttons. */
#define AUI_BTN_MIN_W        64
#define AUI_BTN_MIN_W_ALERT  84
/* The width a button wants: label + 2 x 14 padding (12 and a 14 + 6 icon with
 * one), not below AUI_BTN_MIN_W. */
int aui_button_width(const char *label, int has_icon);

/* The default button's face without a widget: for a caller that hit-tests
 * its own buttons (Finder's toolbar) and must look like aui_button(): a
 * Secondary capsule (radius h / 2; `radius` is ignored). */
void aui_button_face(int x, int y, int w, int h, int radius);

/* Buttons. aui_button is a Secondary capsule (it was a glass pill); AUI_V_GLASS
 * draws as Secondary and AUI_V_GHOST as Plain. No button makes a GLASS2 call. */
int aui_button(int x, int y, int w, int h, const char *label);
int aui_button_ex(int x, int y, int w, int h, const char *label,
                  enum aui_variant v, int enabled);
int aui_icon_button(int x, int y, int size, int icon, int enabled);
/* A button with a leading 14 pt GICON_* (icon < 0: none), 12 pt padding. */
int aui_button_icon(int x, int y, int w, int h, const char *label, int icon,
                    enum aui_variant v, int enabled);
/* The Logit AI button; an empty or NULL label draws the icon-only 28 circle. */
int aui_button_ai(int x, int y, int w, int h, const char *label, int enabled);
/* Toggle icon button: returns 1 on a click (the caller flips `on`). */
int aui_icon_toggle(int x, int y, int size, int icon, int on, int enabled);

/* Dropdown. Draws closed (a Popup button); when open it defers its popup to
 * aui_end() so the list paints over everything else in the frame. Returns 1 on
 * a new choice. */
int aui_select(int x, int y, int w, const char *const *items, int n, int *sel);

#endif /* AUI_BUTTON_H */
