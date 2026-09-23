/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../lib/agent/gui.h"
#include <stdio.h>
#include <string.h>
#include "aui.h"
#include "model.h"

/* The old single-file UI claimed that four tabs and batched writes solved the
 * layout and persistence problems. Correction: its Desktop/Network cards were
 * shorter than their contents, and Apply copied every stale field back. The
 * view now owns layout; the draft writes only fields explicitly edited here.
 * Dark mode remains an immediate system action, labelled as such in the UI. */
#define WINW 820
#define WINH 560
/* Covers the current 96 system + 64 application slots and schema defaults.
 * The inspector also reports truncation rather than silently hiding rows. */
#define MAXKV 256
#define NWP 3
static const char *const tabs[] = {"Appearance", "Desktop", "Network", "Dock", "Mouse & scrolling",
                                 "Date & time", "All settings", "System"};
#define NTAB ((int)(sizeof tabs / sizeof tabs[0]))
static const char *const wp_paths[] = {"/wallpaper.png", "/media/dot.png", "(none)"};
static const char *const wp_labels[] = {"LogitOS wallpaper", "Dots", "System gradient"};
static const unsigned probe_rgb[] = {0xFF0080, 0x00FF80, 0xFFC800, 0xB000FF,
                                    0x00D0D0, 0xFF6000, 0x00A0FF, 0xA0FF20};
static struct desktop_preferences preferences;
static int tab, nav_scroll, last_gen, conflict, close_pending, reset_pending;
static int v_dark, v_reduce_motion, v_reduce_transparency, v_restore, v_dhcp, wp_sel;
static int v_accent_h, v_accent_s, v_accent_l;
static unsigned accent_loaded, edited;
static char v_wallpaper[LOGIT_SET_VALMAX];
static char v_ip[LOGIT_SET_VALMAX], v_mask[LOGIT_SET_VALMAX];
static char v_gw[LOGIT_SET_VALMAX], v_dns[LOGIT_SET_VALMAX];
static char message[160], filter[64];
static int message_error, all_sel, all_scroll, nkv;
static char kv_key[MAXKV][48], kv_val[MAXKV][LOGIT_SET_VALMAX];
static const char *kv_src[MAXKV], *kv_cell[MAXKV * 3];
static int s_eq(const char *a, const char *b) { return !strcmp(a, b); }
static void s_cpy(char *d, const char *s, int cap) { snprintf(d, (unsigned)cap, "%s", s); }
#include "colour.inc"
#include "store.inc"

static void notice(const char *text, int error)
{
    s_cpy(message, text, sizeof message);
    message_error = error;
    char line[192];
    int length = snprintf(line, sizeof line, "[settings] status error=%d %s\n", error, text);
    sys_write(1, line, length);
}
static void change(enum setting_field field)
{
    edited |= 1u << field;
    message[0] = 0;
}
static unsigned preview_accent(void)
{
    /* RGB -> integer HSL -> RGB is lossy. Merely opening Settings must not
     * quantize an existing custom colour, even when another field is saved. */
    return edited & (1u << F_ACCENT) ? accent_now() : accent_loaded;
}
static void load_all(void)
{
    v_dark = sys_ui_dark(-1) > 0;
    v_reduce_motion = setting_int("ui.reduce_motion", 0) != 0;
    v_reduce_transparency = setting_int("ui.reduce_transparency", 0) != 0;
    v_restore = setting_int("desktop.restore_session", 1) != 0;
    v_dhcp = setting_int("net.dhcp", 1) != 0;
    preferences = desktop_preferences_read(setting_int);
    accent_loaded = (unsigned)setting_int("ui.accent", 0x5E96FF);
    rgb_to_hsl(accent_loaded, &v_accent_h, &v_accent_s, &v_accent_l);
    setting_get("ui.wallpaper", v_wallpaper, sizeof v_wallpaper);
    wp_sel = -1; /* An unknown custom path is preserved, not replaced by gradient. */
    for (int i = 0; i < NWP; i++) if (s_eq(wp_paths[i], v_wallpaper)) wp_sel = i;
    setting_get("net.ip", v_ip, sizeof v_ip);
    setting_get("net.mask", v_mask, sizeof v_mask);
    setting_get("net.gw", v_gw, sizeof v_gw);
    setting_get("net.dns", v_dns, sizeof v_dns);
    edited = 0;
    conflict = 0;
    last_gen = setting_gen();
    aui_set_dark(v_dark);
    aui_set_accent(accent_loaded);
    load_kv();
}
static int valid_network(void)
{
    const char *const values[] = {v_ip, v_mask, v_gw, v_dns};
    const char *const errors[] = {"Enter a valid IPv4 address.", "Enter a contiguous IPv4 subnet mask.",
                                  "Enter a valid router address.", "Enter a valid DNS address."};
    for (int i = 0; i < 4; i++) {
        if (v_dhcp && !(edited & (1u << (F_IP + i)))) continue;
        int ok = i == 1 ? settings_mask(values[i]) : settings_ipv4(values[i], 0);
        if (!ok) { notice(errors[i], 1); tab = 2; return 0; }
    }
    return 1;
}
static int apply_all(void)
{
    if (conflict || setting_gen() != last_gen) {
        conflict = 1;
        notice("Settings changed elsewhere. Revert to reload before applying.", 1);
        return 0;
    }
    if (!valid_network()) return 0;
    char accent[16];
    snprintf(accent, sizeof accent, "0x%06X", preview_accent());
    const char *values[F_COUNT] = {
        v_reduce_motion ? "1" : "0", v_reduce_transparency ? "1" : "0",
        accent, wp_sel < 0 ? v_wallpaper : wp_paths[wp_sel],
        v_restore ? "1" : "0", v_dhcp ? "1" : "0", v_ip, v_mask, v_gw, v_dns
    };
    char extra[F_COUNT][16];
#define PREF_VALUE(field, key, label, low, high, value) \
    snprintf(extra[F_##field], sizeof extra[0], "%d", preferences.field); \
    values[F_##field] = extra[F_##field];
    LOGIT_DESKTOP_PREFS(PREF_VALUE)
#undef PREF_VALUE
    if (settings_write(edited, values, setting_set) < 0 || setting_commit() < 0) {
        /* The ABI has no transaction for the live table. An error can leave
         * earlier writes in RAM; retain the draft and report the failure. */
        last_gen = setting_gen();
        notice("Save failed. Changes may be active but are not safely saved.", 1);
        return 0;
    }
    load_all();
    notice("Changes saved", 0);
    return 1;
}
#include "preview.inc"
#include "view.inc"

void app_main(void)
{
    gui_create("Settings", WINW, WINH);
    _sys(SYS_GUI_WIN_MIN, ((long)760 << 16) | WINH, 0, 0);
    aui_set_size(WINW, WINH);
    load_all();
    frame();
    unsigned long long last_poll = monotonic_ns();
    for (;;) {
        int drew = 0;
        struct logit_event event;
        while (poll_event(&event)) {
            if (ag_gui_event(&event)) continue;
            int had_modal = close_pending || reset_pending;
            if (event.type == EV_CLOSE) {
                if (!edited) app_exit(0);
                close_pending = 1;
            }
            if (event.type == EV_RESIZE) aui_set_size(event.a, event.b);
            if (event.type == EV_KEY && event.a == 27 && reset_pending)
                reset_pending = 0;
            aui_feed(&event);
            if (aui_want_repaint() || event.type == EV_CLOSE || event.type == EV_RESIZE) {
                frame(); drew = 1;
            }
            aui_feed_done();
            /* Dialog buttons are evaluated after the scrim was painted. A key
             * has no mouse-release repaint, so cancelling with Escape otherwise
             * leaves a dismissed dialog visible until the next pointer event. */
            if (had_modal && !close_pending && !reset_pending) {
                frame(); drew = 1;
            }
        }
        /* aui_ms() is sampled only by aui_begin(), so it cannot wake an idle
         * settings watcher. Use the live monotonic clock for this deadline. */
        unsigned long long now = monotonic_ns();
        if (now - last_poll >= 250000000ull) {
            last_poll = now;
            if (setting_gen() != last_gen && !conflict) {
                if (edited) { conflict = 1; notice("Settings changed elsewhere. Revert to reload.", 1); }
                else load_all();
                frame(); drew = 1;
            }
        }
        if (!drew && aui_anim_due()) frame();
        int wait = aui_anim_wait();
        int poll = 250 - (int)((monotonic_ns() - last_poll) / 1000000ull);
        if (poll < 1) poll = 1;
        wait_idle(wait > 0 && wait < poll ? wait : poll);
    }
}
const char *ag_gui_context(unsigned *bytes)
{
    static char text[256];
    *bytes = (unsigned)snprintf(text, sizeof text,
        "Settings page=%s; dark=%d; DHCP=%d; edited=%u; conflict=%d", tabs[tab], v_dark, v_dhcp, edited, conflict);
    return text;
}
