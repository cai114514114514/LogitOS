/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SETTINGS_MODEL_H
#define SETTINGS_MODEL_H
#include "logit_desktop_prefs.h"

/* Validate before the first store write: the kernel deliberately accepts raw
 * values and validates on read. Letting an invalid address through would show
 * "Saved" here while the next boot silently used a different address. */
static int settings_ipv4(const char *text, unsigned *value)
{
    unsigned address = 0;
    for (int part = 0; part < 4; part++) {
        unsigned octet = 0;
        int digits = 0;
        const char *start = text;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10 + (unsigned)(*text++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        if (!digits || (digits > 1 && *start == '0')) return 0;
        address = (address << 8) | octet;
        if (part < 3) { if (*text++ != '.') return 0; }
        else if (*text) return 0;
    }
    if (value) *value = address;
    return 1;
}

static int settings_mask(const char *text)
{
    unsigned mask;
    if (!settings_ipv4(text, &mask)) return 0;
    unsigned host = ~mask;
    return (host & (host + 1)) == 0;
}

enum setting_field { F_MOTION, F_TRANSPARENCY, F_ACCENT, F_WALLPAPER, F_RESTORE,
                     F_DHCP, F_IP, F_MASK, F_GW, F_DNS,
#define PREF_ENUM(field, key, label, low, high, value) F_##field,
                     LOGIT_DESKTOP_PREFS(PREF_ENUM)
#undef PREF_ENUM
                     F_COUNT };
static const char *const field_keys[F_COUNT] = {
    "ui.reduce_motion", "ui.reduce_transparency", "ui.accent", "ui.wallpaper", "desktop.restore_session",
    "net.dhcp", "net.ip", "net.mask", "net.gw", "net.dns",
#define PREF_KEY(field, key, label, low, high, value) key,
    LOGIT_DESKTOP_PREFS(PREF_KEY)
#undef PREF_KEY
};

/* This is a patch, not a replacement snapshot. Unedited fields belong to the
 * store and must never be copied back from this window's stale draft. */
static int settings_write(unsigned edited, const char *const values[F_COUNT],
                          int (*write_value)(const char *, const char *, int))
{
    for (int i = 0; i < F_COUNT; i++)
        if ((edited & (1u << i)) && write_value(field_keys[i], values[i], 0) < 0)
            return -1;
    return 0;
}
#endif
