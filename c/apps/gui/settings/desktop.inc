/* SPDX-License-Identifier: GPL-3.0-or-later */
static void page_dock(struct aui_rect body)
{
    struct aui_rect row = group(&body, 72);
    toggle_row(row, "悬停时放大图标", "放大指针下方的图标", &preferences.dock_magnify, F_dock_magnify);
    aim("magnify", row.x + row.w - 24, row.y + 14);
    row = group(&body, 72);
    toggle_row(row, "打开应用时显示动画", "应用打开时让图标弹跳", &preferences.dock_bounce, F_dock_bounce);
    row = group(&body, 72);
    toggle_row(row, "显示运行指示", "在每个已打开的应用下方显示圆点", &preferences.dock_dots, F_dock_dots);
    aim("indicators", row.x + row.w - 24, row.y + 14);
    row = group(&body, 88);
    toggle_row(row, "概览触发角", "指针停在右上角时显示所有窗口", &preferences.hot_corner, F_hot_corner);
    aui_text_sz(body.x, body.y, "点按“应用”后生效。", AUI_MUTED, AUI_FS_CAPTION);
}
static void page_input(struct aui_rect body)
{
    struct aui_rect row = group(&body, 78);
    toggle_row(row, "反转滚动方向", "反转应用中滚轮的上下方向", &preferences.natural_scroll, F_natural_scroll);
    row = group(&body, 152);
    aui_label(row.x, row.y, "标题栏连按速度", AUI_TEXT);
    aui_text_sz(row.x, row.y + 24, "设置两次点按之间允许的间隔。", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_slider(row.x + 10, row.y + 58, row.w - 20, &preferences.double_click, 200, 800)) change(F_double_click);
    aui_text_sz(row.x, row.y + 90, "快", AUI_MUTED, AUI_FS_CAPTION);
    char value[32];
    snprintf(value, sizeof value, "%d 毫秒", preferences.double_click);
    aui_text_in(aui_r(row.x, row.y + 90, row.w, 20), value, AUI_TEXT, AUI_FS_CAPTION, AUI_ALIGN_CENTER);
    aui_text_sz(row.x + row.w - 28, row.y + 90, "慢", AUI_MUTED, AUI_FS_CAPTION);
    row = group(&body, 96);
    aui_label(row.x, row.y, "试一下连按", AUI_TEXT);
    static unsigned long long clicked;
    static int matched;
    if (aui_button_ex(row.x + row.w - 152, row.y + 12, 152, 30, "连按两次", AUI_V_SECONDARY, 1)) {
        unsigned long long now = monotonic_ns();
        matched = clicked && now - clicked <= (unsigned long long)preferences.double_click * 1000000ull;
        clicked = matched ? 0 : now;
    }
    aui_text_sz(row.x, row.y + 32, matched ? "已识别为连按" : "用这个按钮测试你的点按间隔", AUI_MUTED, AUI_FS_CAPTION);
}
static void page_time(struct aui_rect body)
{
    struct aui_rect row = group(&body, 68);
    toggle_row(row, "使用 24 小时制", "用 00-23 代替上午和下午", &preferences.clock_24h, F_clock_24h);
    aim("clock-format", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "显示秒", "在菜单栏时钟中显示秒", &preferences.clock_seconds, F_clock_seconds);
    aim("clock-seconds", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "显示日期", "显示年、月、日", &preferences.clock_date, F_clock_date);
    aim("clock-date", row.x + row.w - 24, row.y + 14);
    row = group(&body, 68);
    toggle_row(row, "显示星期", "在时间旁显示星期几", &preferences.clock_weekday, F_clock_weekday);
    aim("clock-weekday", row.x + row.w - 24, row.y + 14);
    aui_text_sz(body.x, body.y + 6, "这些选项控制系统菜单栏。", AUI_MUTED, AUI_FS_CAPTION);
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
    snprintf(text, sizeof text, "%d × %d 点  /  缩放 %d%%", screen_w(), screen_h(), ui_scale());
    system_value(row, "显示器", text);
    row = group(&body, 74);
    struct logit_meminfo memory;
    if (sys_meminfo(&memory) == 0) {
        snprintf(text, sizeof text, "可用内存 %llu MiB  /  空闲 %llu MiB",
            memory.frames_total * memory.frame_bytes / 1048576,
            memory.frames_free * memory.frame_bytes / 1048576);
    } else s_cpy(text, "无法获取内存信息", sizeof text);
    system_value(row, "内存", text);
    row = group(&body, 74);
    struct logit_netinfo network;
    if (net_info(&network) > 0)
        snprintf(text, sizeof text, "%u.%u.%u.%u", network.ip >> 24, (network.ip >> 16) & 255,
                 (network.ip >> 8) & 255, network.ip & 255);
    else s_cpy(text, "没有活动的网络适配器", sizeof text);
    system_value(row, "当前网络地址", text);
    row = group(&body, 86);
    system_value(row, "LogitOS", "原生桌面 / OpenLogit 图形");
    if (aui_button_ex(row.x + row.w - 116, row.y + 28, 116, 28, "刷新", AUI_V_SECONDARY, 1))
        notice("系统信息已刷新", 0);
}
