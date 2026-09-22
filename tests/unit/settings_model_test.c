/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "../../c/apps/gui/settings/model.h"
static int failures, writes;
static const char *written;
static int record(const char *key, const char *value, int commit)
{
    (void)value;
    if (commit) failures++;
    writes++;
    written = key;
    return 0;
}
static int refuse(const char *key, const char *value, int commit)
{ (void)key; (void)value; (void)commit; return -1; }
static int defaults(const char *key, int value) { (void)key; return value; }
static int broken_values(const char *key, int value) { (void)key; (void)value; return 9000; }
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %s\n", #expr); failures++; } } while (0)
int main(void)
{
    const char *valid[] = {"0.0.0.0", "255.255.255.255", "10.0.2.15"};
    const char *invalid[] = {"", "1", "1.2.3", "1.2.3.", "1.2.3.4.5", "256.2.3.4", "01.2.3.4",
                            "1.2.3.04", "-1.2.3.4", "1.2.3.4 ", "1.2.3.99999999"};
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++) CHECK(settings_ipv4(valid[i], 0));
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) CHECK(!settings_ipv4(invalid[i], 0));
    CHECK(settings_mask("255.255.255.0"));
    CHECK(settings_mask("255.255.255.255"));
    CHECK(settings_mask("0.0.0.0"));
    CHECK(!settings_mask("255.0.255.0"));
    const char *values[F_COUNT] = {"1", "0xABCDEF", "/custom.png", "1", "0", "1.2.3.4", "255.255.255.0", "1.2.3.1", "1.1.1.1"};
    CHECK(settings_write(1u << F_RESTORE, values, record) == 0);
    CHECK(writes == 1 && !strcmp(written, "desktop.restore_session"));
    CHECK(settings_write(0, values, record) == 0 && writes == 1);
    CHECK(settings_write(1u << F_DNS, values, refuse) < 0);
    struct desktop_preferences initial = desktop_preferences_read(defaults);
    struct desktop_preferences bounded = desktop_preferences_read(broken_values);
    CHECK(initial.dock_magnify == 1 && initial.clock_24h == 1 && initial.natural_scroll == 0);
    CHECK(bounded.double_click == 400 && bounded.clock_seconds == 1);
    values[F_clock_seconds] = "0";
    writes = 0;
    CHECK(settings_write(1u << F_clock_seconds, values, record) == 0);
    CHECK(writes == 1 && !strcmp(written, "desktop.clock_seconds"));
    printf("Settings draft: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
