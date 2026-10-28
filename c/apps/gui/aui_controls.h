/* aui_controls.h -- tooltip, checkbox, radio, toggle, slider, progress,
 * spinner, badge, separator.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_CONTROLS_H
#define AUI_CONTROLS_H

/* A tooltip that appears next to the widget just drawn, if it is hovered
 * (design 4.15: one dark style in both themes, E2, 450 ms dwell, fades in over
 * t.fadeIn, out over t.fadeOut). aui_tooltip_ex appends `shortcut` (NULL for
 * none) at alpha 160. */
void aui_tooltip(const char *s);
void aui_tooltip_ex(const char *s, const char *shortcut);

/* Checkbox (4.6): 16 x 16, state 0 off, 1 on, 2 mixed (a click on mixed turns it on). */
int aui_checkbox(int x, int y, const char *label, int *state);
int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled);
/* Radio: `value` is this button's value; sets *group to it when chosen. */
int aui_radio(int x, int y, const char *label, int *group, int value);
/* The switch (design 4.6): 38 x 22, no glass, knob on the snappy spring with a
 * 22 x 18 press stretch. Returns 1 when it changed. */
int aui_toggle(int x, int y, int *state, int enabled);

/* Slider: value in [lo,hi]. Drags, clicks-to-position, and takes arrow keys
 * (and Home/End) when focused. Returns 1 when the value changed. */
int aui_slider(int x, int y, int w, int *value, int lo, int hi);
/* Slider flags. With AUI_SLIDER_VALUE the right 44 + 8 points of `w` are a
 * tabular value column (callout, label.secondary, right-aligned) and the track
 * takes the rest. `step` > 0 snaps pointer and key changes to lo + n * step and
 * (with AUI_SLIDER_TICKS) draws a 2 x 6 tick at each one; step <= 0 keys move
 * by (hi - lo) / 50. Shift multiplies a key step by ten; Home / End jump.
 * `lo_cap` / `hi_cap` (NULL for none) sit 6 points under the track ends. */
#define AUI_SLIDER_VALUE    1u
#define AUI_SLIDER_TICKS    2u
#define AUI_SLIDER_DISABLED 4u
int aui_slider_ex(int x, int y, int w, int *value, int lo, int hi, unsigned flags,
                  const char *lo_cap, const char *hi_cap, int step);
/* A determinate bar; pct 0..100 (h 6, eased on the smooth spring). pct < 0 =
 * indeterminate: a 30 % capsule sweeping in 1.2 s, frozen under Reduce Motion. */
void aui_progress(int x, int y, int w, int pct);
void aui_spinner(int cx, int cy, int radius);          /* animated busy ring */
/* The design spinner: diameter d (16 / 24 / 32), a 270 degree arc with a linear
 * tail, one turn per 900 ms. At most one per window. It keeps turning under
 * Reduce Motion (it carries meaning). */
void aui_spinner_ex(int cx, int cy, int d);
/* The stepper (design 4.17): an h 28 capsule of two 24-wide halves, minus and
 * plus, with an optional 44-wide tabular value cell between them. Returns 1
 * when *value changed. Width: 48, or 92 with the value cell. */
int aui_stepper(int x, int y, int *value, int lo, int hi, int step, int show_value);

/* A coloured status pill (legacy: any colour). */
void aui_badge(int x, int y, const char *s, unsigned color);
/* The design badge (4.15): capsule h 18, padding x 6. Returns its width. */
enum { AUI_BADGE_NEUTRAL, AUI_BADGE_ACCENT, AUI_BADGE_SUCCESS, AUI_BADGE_WARNING, AUI_BADGE_ERROR };
int aui_badge_ex(int x, int y, const char *s, int kind);
void aui_separator(int x, int y, int w);

#endif /* AUI_CONTROLS_H */
