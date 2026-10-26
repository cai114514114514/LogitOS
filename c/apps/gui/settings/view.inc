/* SPDX-License-Identifier: GPL-3.0-or-later */
static void aim(const char *name, int x, int y)
{
    /* Test geometry is emitted by the drawing code, in window-local points.
     * The driver observes the real layout instead of maintaining a second one. */
    static struct { const char *name; int x, y; } aims[24];
    for (int i = 0; i < 24; i++) {
        if (aims[i].name && strcmp(aims[i].name, name)) continue;
        if (aims[i].name && aims[i].x == x && aims[i].y == y) return;
        aims[i].name = name; aims[i].x = x; aims[i].y = y;
        /* printf emits fragments on this runtime: WM log lines can splice
         * between them. One write keeps the geometry parseable under load. */
        char line[96];
        int length = snprintf(line, sizeof line, "[settings] aim %s %d %d\n", name, x, y);
        sys_write(1, line, length);
        return;
    }
}
static struct aui_rect group(struct aui_rect *body, int height)
{
    struct aui_rect box = aui_cut_top(body, height);
    aui_round(box.x, box.y, box.w, box.h, 12, AUI_SURFACE);
    aui_stroke(box.x, box.y, box.w, box.h, 12, 1, AUI_BORDER);
    aui_cut_top(body, 12);
    return aui_inset(box, 16, 12);
}
static void toggle_row(struct aui_rect box, const char *title, const char *detail,
                       int *value, enum setting_field field)
{
    aui_label(box.x, box.y, title, AUI_TEXT);
    if (detail) aui_text_sz(box.x, box.y + 22, detail, AUI_MUTED, AUI_FS_CAPTION);
    if (aui_toggle(box.x + box.w - 46, box.y + 2, value, 1)) change(field);
}
static void page_appearance(struct aui_rect body)
{
    int half = (body.w - 12) / 2;
    struct aui_rect preview = aui_cut_top(&body, 102);
    for (int dark = 0; dark < 2; dark++) {
        int x = preview.x + dark * (half + 12);
        if (preview_draw(x, preview.y, half, dark))
            aui_text_sz(x + 12, preview.y + 36, "无法预览", AUI_MUTED, AUI_FS_CAPTION);
        aui_stroke(x, preview.y, half, 88, 8, v_dark == dark ? 2 : 1,
                   v_dark == dark ? AUI_ACCENT : AUI_BORDER);
        aui_badge(x + 10, preview.y + 8, dark ? "深色" : "浅色", dark ? 0xCAD7EA : 0x55749A);
    }
    struct aui_rect row = group(&body, 60);
    aui_label(row.x, row.y, "深色外观", AUI_TEXT);
    aui_text_sz(row.x, row.y + 22, "立即更改你的桌面", AUI_MUTED, AUI_FS_CAPTION);
    aim("dark", row.x + row.w - 24, row.y + 14);
    if (aui_toggle(row.x + row.w - 46, row.y + 2, &v_dark, 1)) {
        /* Do not acknowledge a concurrent change merely because our own theme
         * action also advances the generation counter. */
        int changed_elsewhere = setting_gen() != last_gen;
        sys_ui_dark(v_dark);
        aui_set_dark(v_dark);
        if (changed_elsewhere && edited) conflict = 1;
        else last_gen = setting_gen();
        notice("外观已更新", 0);
    }
    row = group(&body, 154);
    aui_label(row.x, row.y, "强调色", AUI_TEXT);
    static const unsigned colours[] = {0x5088F2, 0x249E91, 0x8D6ADB, 0xD88535, 0xD76C91};
    for (int i = 0; i < 5; i++) {
        int x = row.x + row.w - 5 * 34 + i * 34;
        if (aui_button_ex(x, row.y - 4, 28, 26, "", AUI_V_SECONDARY, 1)) {
            rgb_to_hsl(colours[i], &v_accent_h, &v_accent_s, &v_accent_l);
            change(F_ACCENT);
        }
        aui_circle(x + 14, row.y + 9, 8, colours[i]);
    }
    const char *const labels[] = {"色相", "饱和度", "亮度"};
    int *const values[] = {&v_accent_h, &v_accent_s, &v_accent_l};
    for (int i = 0; i < 3; i++) {
        int y = row.y + 34 + i * 30;
        aui_text_sz(row.x, y + 5, labels[i], AUI_MUTED, AUI_FS_CAPTION);
        if (aui_slider(row.x + 82, y, row.w - 142, values[i], 0, i ? 100 : 359)) change(F_ACCENT);
        char value[16];
        snprintf(value, sizeof value, i ? "%d%%" : "%d", *values[i]);
        aui_text_sz(row.x + row.w - 42, y + 5, value, AUI_MUTED, AUI_FS_CAPTION);
    }
    aui_set_accent(preview_accent());
    row = group(&body, 56);
    /* Both accessibility switches fit in the existing card. Another full row
     * would run under the fixed footer at the minimum 820x560 window size. */
    int half_row = row.w / 2;
    struct aui_rect motion = {row.x, row.y, half_row, row.h};
    struct aui_rect transparency = {row.x + half_row, row.y,
                                    row.w - half_row, row.h};
    toggle_row(motion, "减弱动态效果", "减少动画", &v_reduce_motion, F_MOTION);
    aim("motion", motion.x + motion.w - 24, motion.y + 14);
    toggle_row(transparency, "降低透明度", "玻璃面板改为不透明",
               &v_reduce_transparency, F_TRANSPARENCY);
    aim("transparency", transparency.x + transparency.w - 24, transparency.y + 14);
}
static void page_desktop(struct aui_rect body)
{
    struct aui_rect row = group(&body, 126);
    aui_label(row.x, row.y, "壁纸", AUI_TEXT);
    aui_text_sz(row.x, row.y + 24, "所选壁纸将在重新启动后生效。", AUI_MUTED, AUI_FS_CAPTION);
    if (aui_select(row.x, row.y + 52, row.w, wp_labels, NWP, &wp_sel)) change(F_WALLPAPER);
    if (wp_sel < 0) aui_text_sz(row.x, row.y + 86, "自定壁纸会一直保留，直到你选择其他壁纸。", AUI_MUTED, AUI_FS_CAPTION);
    row = group(&body, 72);
    toggle_row(row, "登录时重新打开窗口", "恢复上次的工作区", &v_restore, F_RESTORE);
    aim("restore", row.x + row.w - 24, row.y + 14);
    /* Keep the animation harness's established serial contract. */
    static int anim_x, anim_y;
    if (anim_x != row.x + row.w - 24 || anim_y != row.y + 14) {
        anim_x = row.x + row.w - 24; anim_y = row.y + 14;
        char line[80];
        int length = snprintf(line, sizeof line, "[settings] anim-toggle %d %d\n", anim_x, anim_y);
        sys_write(1, line, length);
    }
    row = group(&body, 86);
    int count = 0;
    for (int i = 0; i < nkv; i++) if (!strncmp(kv_key[i], "win.", 4)) count++;
    char text[64];
    snprintf(text, sizeof text, "已存储 %d 个窗口位置", count);
    aui_label(row.x, row.y, text, AUI_TEXT);
    aui_text_sz(row.x, row.y + 28, "可在“所有设置”中查看已存储的偏好设置。", AUI_MUTED, AUI_FS_CAPTION);
    row = group(&body, 72);
    toggle_row(row, "恢复浏览器标签页", "Browser 启动时重新打开上次的标签页", &preferences.browser_restore, F_browser_restore);
}
static void page_network(struct aui_rect body)
{
    struct aui_rect row = group(&body, 74);
    toggle_row(row, "自动配置", "下次启动时由 DHCP 分配地址", &v_dhcp, F_DHCP);
    aim("dhcp", row.x + row.w - 24, row.y + 14);
    row = group(&body, 228);
    aui_label(row.x, row.y, "手动配置 IPv4", AUI_TEXT);
    const char *const labels[] = {"IP 地址", "子网掩码", "路由器", "DNS 服务器"};
    const char *const ids[] = {"ip", "mask", "router", "dns"};
    char *const values[] = {v_ip, v_mask, v_gw, v_dns};
    for (int i = 0; i < 4; i++) {
        int y = row.y + 36 + i * 40;
        aui_label(row.x, y + 7, labels[i], v_dhcp ? AUI_DISABLED_TX : AUI_TEXT);
        if (aui_textfield_ex(row.x + 136, y, row.w - 136, values[i], LOGIT_SET_VALMAX, "0.0.0.0", !v_dhcp))
            change((enum setting_field)(F_IP + i));
        aim(ids[i], row.x + 190, y + 14);
    }
    aui_text_sz(body.x, body.y + 4, "网络更改将在重新启动后生效。", AUI_MUTED, AUI_FS_CAPTION);
}
#include "desktop.inc"

static int contains(const char *text, const char *query)
{
    if (!*query) return 1;
    for (; *text; text++) {
        int i = 0;
        while (query[i] && text[i]) {
            int a = text[i], b = query[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            i++;
        }
        if (!query[i]) return 1;
    }
    return 0;
}
static void page_all(struct aui_rect body)
{
    if (aui_textfield_ex(body.x, body.y, body.w, filter, sizeof filter, "搜索键或值", 1))
        all_scroll = all_sel = 0;
    aim("search", body.x + 100, body.y + 14);
    aui_cut_top(&body, 42);
    static const char *cells[MAXKV * 3];
    int count = 0;
    for (int i = 0; i < nkv; i++) {
        if (!contains(kv_key[i], filter) && !contains(kv_val[i], filter)) continue;
        cells[count * 3] = kv_key[i];
        cells[count * 3 + 1] = kv_val[i];
        cells[count * 3 + 2] = kv_src[i];
        count++;
    }
    static const char *const columns[] = {"键", "值", "来源"};
    int widths[] = {(body.w - 100) / 2, (body.w - 100) / 2, 80};
    aui_table(body.x, body.y, body.w, body.h - 66, columns, widths, 3, cells, count, &all_sel, &all_scroll);
    if (!count) aui_label(body.x + 16, body.y + 48, "没有匹配的设置", AUI_MUTED);
    aui_text_sz(body.x, body.y + body.h - 56, "stored = 已存储的值   default = 内建默认值   rejected = 已忽略",
                AUI_MUTED, AUI_FS_CAPTION);
    if (setting_diag() & ~LOGIT_SETD_NOFILE)
        aui_text_sz(body.x, body.y + body.h - 36, "部分设置无法载入，请检查被忽略的值。", AUI_WARNING, AUI_FS_CAPTION);
    else if (nkv == MAXKV)
        aui_text_sz(body.x, body.y + body.h - 36, "已达到显示上限，其余的键可能未列出。", AUI_WARNING, AUI_FS_CAPTION);
    if (aui_button_ex(body.x + body.w - 142, body.y + body.h - 30, 142, 28,
                      "重置所有设置…", AUI_V_DANGER, 1)) reset_pending = 1;
}
static void frame(void)
{
    aui_begin(AUI_BG);
    int width = aui_width(), height = aui_height();
    aui_fill(0, 0, 176, height, AUI_SURFACE_2);
    aui_vhairline(176, 0, height);
    if (gui_app_icon("settings.aex", 16, 22, 40) == 0) {
        aui_text_sz(66, 22, "Settings", AUI_TEXT, 20);
        aui_text_sz(66, 50, "LogitOS", AUI_MUTED, AUI_FS_CAPTION);
    } else {
        aui_heading(20, 22, "Settings", AUI_TEXT);
        aui_text_sz(20, 55, "LogitOS", AUI_MUTED, AUI_FS_CAPTION);
    }
    int previous = tab;
    aui_list(12, 100, 152, 236, tabs, NTAB, &tab, &nav_scroll);
    if (previous != tab) aui_anim_reset();
    aim("navigation", 70, 114);
    aui_text_sz(20, height - 36, "打造你的工作空间", AUI_MUTED, AUI_FS_CAPTION);
    if (qmp_probes_on()) aui_fill(4, 4, 6, 6, probe_rgb[tab]);   /* see qmp_probe.h */
    const char *const descriptions[] = {"让这里更像你自己的空间", "壁纸与工作区",
        "管理网络连接", "应用在程序坞中的显示方式", "让指针用起来更顺手",
        "菜单栏时钟", "搜索并查看偏好设置", "关于这台电脑"};
    aui_heading(204, 20, tabs[tab], AUI_TEXT);
    aui_text_sz(204, 54, descriptions[tab], AUI_MUTED, AUI_FS_CAPTION);
    struct aui_rect body = aui_r(204, 88, width - 232, height - 158);
    switch (tab) {
    case 0: page_appearance(body); break;
    case 1: page_desktop(body); break;
    case 2: page_network(body); break;
    case 3: page_dock(body); break;
    case 4: page_input(body); break;
    case 5: page_time(body); break;
    case 6: page_all(body); break;
    default: page_system(body); break;
    }
    int footer = height - 64;
    aui_hairline(192, footer, width - 208);
    const char *status = message[0] ? message : edited ? "有未存储的更改" : "已是最新";
    aui_text_sz(204, footer + 7, status, message_error ? AUI_ERROR : AUI_MUTED, AUI_FS_CAPTION);
    if (aui_button_ex(width - 116, footer + 30, 88, 28, "应用", AUI_V_PRIMARY, edited && !conflict)) apply_all();
    aim("apply", width - 72, footer + 44);
    if (aui_button_ex(width - 212, footer + 30, 88, 28, "还原", AUI_V_SECONDARY, edited || conflict)) {
        /* Drafts were never pushed to the store. Reloading the disk here would
         * erase another process's uncommitted live values, so just read it. */
        load_all(); notice("已放弃草稿", 0);
    }
    aim("revert", width - 168, footer + 44);
    if (close_pending && aui_dialog_begin("有未存储的更改", 420, 170)) {
        aui_label(20, 54, "关闭前要存储更改吗？", AUI_TEXT);
        const char *const actions[] = {"存储", "不存储", "取消"};
        int action = aui_dialog_buttons(actions, 3);
        if (action == 0) {
            if (apply_all()) app_exit(0);
            close_pending = 0; /* Reveal the inline validation/save error. */
        }
        if (action == 1) app_exit(0);
        if (action == 2) close_pending = 0;
        aui_dialog_end();
    }
    if (reset_pending && aui_dialog_begin("要重置所有设置吗？", 450, 190)) {
        aui_label(20, 54, "这也会移除已存储的应用偏好设置。", AUI_TEXT);
        aui_text_sz(20, 82, "当前草稿将被放弃。", AUI_MUTED, AUI_FS_CAPTION);
        /* Escape chooses the last button in AUI; keep Cancel last as well as
         * the default, by handling destructive confirmation with explicit buttons. */
        if (aui_button_ex(234, 138, 90, 28, "重置", AUI_V_DANGER, 1)) {
            if (setting_reset() < 0) notice("无法重置设置", 1);
            else { load_all(); sys_ui_dark(setting_int("ui.dark", 0)); load_all(); notice("设置已重置", 0); }
            reset_pending = 0;
        }
        if (aui_button_ex(334, 138, 90, 28, "取消", AUI_V_SECONDARY, 1)) reset_pending = 0;
        aui_dialog_end();
    }
    /* GFX preview blits bypass AUI's damage tracking. Include the content area
     * so a new accent is actually presented even when other widgets are idle. */
    aui_end_rect(192, 0, width - 192, height);
}
