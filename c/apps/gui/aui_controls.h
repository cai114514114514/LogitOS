/* aui_controls.h -- tooltip, checkbox, radio, toggle, slider, progress,
 * spinner, badge, separator.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_CONTROLS_H
#define AUI_CONTROLS_H

/* A tooltip that appears next to the widget just drawn, if it is hovered. */
void aui_tooltip(const char *s);

int aui_checkbox(int x, int y, const char *label, int *state);
int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled);
/* Radio: `value` is this button's value; sets *group to it when chosen. */
int aui_radio(int x, int y, const char *label, int *group, int value);
/* An iOS-style switch. Returns 1 when it changed. */
int aui_toggle(int x, int y, int *state, int enabled);

/* Slider: value in [lo,hi]. Drags, clicks-to-position, and takes arrow keys
 * (and Home/End) when focused. Returns 1 when the value changed. */
int aui_slider(int x, int y, int w, int *value, int lo, int hi);
/* A determinate bar; pct 0..100. pct < 0 = indeterminate (animated stripe). */
void aui_progress(int x, int y, int w, int pct);
void aui_spinner(int cx, int cy, int radius);          /* animated busy ring */

/* A coloured status pill. */
void aui_badge(int x, int y, const char *s, unsigned color);
void aui_separator(int x, int y, int w);

#endif /* AUI_CONTROLS_H */
