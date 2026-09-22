/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_DESKTOP_PREFS_H
#define LOGIT_DESKTOP_PREFS_H

/* The schema, desktop consumer and Settings editor expand the same list.
 * Defaults must not disagree when a new account has no saved preferences. */
#define LOGIT_DESKTOP_PREFS(X) \
    X(dock_magnify, "desktop.dock_magnify", "Magnify Dock icons", 0, 1, 1) \
    X(dock_bounce, "desktop.dock_bounce", "Animate app launches", 0, 1, 1) \
    X(dock_dots, "desktop.dock_dots", "Show running indicators", 0, 1, 1) \
    X(hot_corner, "desktop.hot_corner", "Overview hot corner", 0, 1, 1) \
    X(clock_24h, "desktop.clock_24h", "Use 24-hour time", 0, 1, 1) \
    X(clock_seconds, "desktop.clock_seconds", "Show seconds", 0, 1, 1) \
    X(clock_date, "desktop.clock_date", "Show date", 0, 1, 1) \
    X(clock_weekday, "desktop.clock_weekday", "Show day of week", 0, 1, 1) \
    X(natural_scroll, "input.natural_scroll", "Reverse scroll direction", 0, 1, 0) \
    X(double_click, "input.double_click_ms", "Title-bar double-click interval", 200, 800, 400) \
    X(browser_restore, "browser.restore_session", "Restore browser tabs", 0, 1, 1)

struct desktop_preferences {
#define PREF_FIELD(field, key, label, low, high, value) int field;
    LOGIT_DESKTOP_PREFS(PREF_FIELD)
#undef PREF_FIELD
};

static inline struct desktop_preferences desktop_preferences_read(int (*read_int)(const char *, int))
{
    struct desktop_preferences prefs;
#define PREF_READ(field, key, label, low, high, value) \
    prefs.field = read_int(key, value); \
    if (prefs.field < low || prefs.field > high) prefs.field = value;
    LOGIT_DESKTOP_PREFS(PREF_READ)
#undef PREF_READ
    return prefs;
}
#endif
