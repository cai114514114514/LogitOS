/* SPDX-License-Identifier: GPL-3.0-or-later */
static void page_dock(struct aui_rect body)
{
    struct aui_rect row = group(&body, 72);
    toggle_row(row, "Magnify icons on hover", "Make the icon under your pointer larger", &preferences.dock_magnify, F_dock_magnify);
    aim("magnify", row.x + row.w - 24, row.y + 14);
    row = group(&body, 72);
    toggle_row(row, "Animate app launches", "Bounce an icon when its application opens", &preferences.dock_bounce, F_dock_bounce);
    row = group(&body, 72);
    toggle_row(row, "Show running indicators", "Place a dot beneath each open application", &preferences.dock_dots, F_dock_dots);
    aim("indicators", row.x + row.w - 24, row.y + 14);
    row = group(&body, 88);
    toggle_row(row, "Overview hot corner", "Pause in the top-right corner to see open windows", &preferences.hot_corner, F_hot_corner);
    aui_text_sz(body.x, body.y, "Changes take effect after Apply.", AUI_MUTED, AUI_FS_CAPTION);
}
static void page_input(struct aui_rect body)
{
    struct aui_rect row = group(&body, 78);
    toggle_row(row, "Reverse scroll direction", "Reverse vertical wheel scrolling in applications", &preferences.natural_scroll, F_natural_scroll);
    row = group(&body, 152);
    aui_label(row.x, row.y, "Title-bar double-click speed", AUI_TEXT);
    aui_text_sz(row.x, row.y + 24, "Choose how much time can pass between two clicks.", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_slider(row.x + 10, row.y + 58, row.w - 20, &preferences.double_click, 200, 800)) change(F_double_click);
    aui_text_sz(row.x, row.y + 90, "Fast", AUI_MUTED, AUI_FS_CAPTION);
    char value[32];
    snprintf(value, sizeof value, "%d ms", preferences.double_click);
    aui_text_in(aui_r(row.x, row.y + 90, row.w, 20), value, AUI_TEXT, AUI_FS_CAPTION, AUI_ALIGN_CENTER);
    aui_text_sz(row.x + row.w - 28, row.y + 90, "Slow", AUI_MUTED, AUI_FS_CAPTION);
    row = group(&body, 96);
    aui_label(row.x, row.y, "Try a double-click", AUI_TEXT);
    static unsigned long long clicked;
    static int matched;
    if (aui_button_ex(row.x + row.w - 152, row.y + 12, 152, 30, "Click twice", AUI_V_SECONDARY, 1)) {
        unsigned long long now = monotonic_ns();
        matched = clicked && now - clicked <= (unsigned long long)preferences.double_click * 1000000ull;
        clicked = matched ? 0 : now;
    }
    aui_text_sz(row.x, row.y + 32, matched ? "Double-click detected" : "Use the button to test your timing", AUI_MUTED, AUI_FS_CAPTION);
}
static void page_time(struct aui_rect body)
{
    struct aui_rect row = group(&body, 68);
    toggle_row(row, "Use 24-hour time", "Use 00-23 instead of AM and PM", &preferences.clock_24h, F_clock_24h);
    aim("clock-format", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "Show seconds", "Include seconds in the menu-bar clock", &preferences.clock_seconds, F_clock_seconds);
    aim("clock-seconds", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "Show date", "Include year, month and day", &preferences.clock_date, F_clock_date);
    aim("clock-date", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "Show day of week", "Include the weekday beside the time", &preferences.clock_weekday, F_clock_weekday);
    aim("clock-weekday", row.x + row.w - 24, row.y + 14);
    aui_text_sz(body.x, body.y + 6, "These options control the system menu bar.", AUI_MUTED, AUI_FS_CAPTION);
}
static void system_value(struct aui_rect row, const char *name, const char *value)
{
    aui_text_sz(row.x, row.y, name, AUI_MUTED, AUI_FS_CAPTION);
    aui_label(row.x, row.y + 24, value, AUI_TEXT);
}
static void page_system(struct aui_rect body)
{
    char text[96];
    struct aui_rect row = group(&body, 74);
    snprintf(text, sizeof text, "%d x %d points  /  %d%% scaling", screen_w(), screen_h(), ui_scale());
    system_value(row, "Display", text);
    row = group(&body, 74);
    struct logit_meminfo memory;
    if (sys_meminfo(&memory) == 0) {
        snprintf(text, sizeof text, "%llu MiB usable RAM  /  %llu MiB free",
            memory.frames_total * memory.frame_bytes / 1048576,
            memory.frames_free * memory.frame_bytes / 1048576);
    } else s_cpy(text, "Memory information unavailable", sizeof text);
    system_value(row, "Memory", text);
    row = group(&body, 74);
    struct logit_netinfo network;
    if (net_info(&network) > 0)
        snprintf(text, sizeof text, "%u.%u.%u.%u", network.ip >> 24, (network.ip >> 16) & 255,
                 (network.ip >> 8) & 255, network.ip & 255);
    else s_cpy(text, "No active network adapter", sizeof text);
    system_value(row, "Current network address", text);
    row = group(&body, 86);
    system_value(row, "LogitOS", "Native desktop / OpenLogit graphics");
    if (aui_button_ex(row.x + row.w - 116, row.y + 28, 116, 28, "Refresh", AUI_V_SECONDARY, 1))
        notice("System information refreshed", 0);
}
