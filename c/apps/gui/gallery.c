#include "aui.h"

/* ============================================================================
 * Gallery -- every aui widget, in every state, on one screen.
 *
 * This is the demo AND the regression test. tests/qmp/qmp_gallery.py drives it
 * over QMP and asserts against the pixels: that a corner is anti-aliased (a
 * gradient of coverage, not two colours), that hovering changes a control, that
 * Tab moves a focus ring, that a shadow exists under a card. A widget that
 * renders wrong should fail a test rather than wait for somebody to notice.
 *
 * Each tab paints a KNOWN COLOUR PROBE at a known place -- a small rect in an
 * unmistakable colour -- so the driver can find the page it is looking at
 * without OCR and without hard-coding a layout it cannot see.
 * ========================================================================== */

#define WINW 720
#define WINH 560

/* Probe colours. Deliberately odd values that appear nowhere else in the theme,
 * so PPM.find_color() cannot confuse them with chrome. */
#define PROBE_X 4
#define PROBE_Y 4
#define PROBE_W 6
#define PROBE_H 6

enum { T_CONTROLS, T_SHAPES, T_DATA, T_OVERLAY, NTAB };
static const char *const tabs[NTAB] = { "Controls", "Shapes", "Data", "Overlays" };
static int tab;

/* ---- controls page state ---- */
static int cb_a = 1, cb_b, cb_c = 1;
static int radio_g = 1;
static int sw_a = 1, sw_b;
static int slider_v = 42, slider_w = 80;
static int seg_v = 1;
static int sel_v = 2;
static char field_a[48] = "editable text";
static char field_b[48] = "";
static int  prog_v = 65;

/* ---- data page state ---- */
static int list_sel, list_scroll;
static int tab_sel, tab_scroll;
static const char *const fruit[12] = {
    "Anchor", "Bitmap", "Compositor", "Damage", "Elevation", "Framebuffer",
    "Glyph", "Hairline", "Immediate", "Jaggies", "Kerning", "Layout"
};
static const char *const cols[3] = { "Widget", "State", "Cost" };
/* Sums to 314, which is the table's inner width (688-360 = 328) minus the two
 * border pixels and the 12-point scrollbar gutter. A column set that overflows
 * is ellipsised rather than clipped, but "Cost" reading as "9 c" is still the
 * app's arithmetic being wrong, not the widget's. */
static const int colw[3] = { 150, 96, 68 };
static const char *const cells[8 * 3] = {
    "Button",    "hover",    "9 calls",
    "Checkbox",  "on",       "7 calls",
    "Slider",    "drag",     "14 calls",
    "Toggle",    "on",       "12 calls",
    "Card",      "elev 2",   "8 calls",
    "Dropdown",  "open",     "deferred",
    "Table",     "scroll",   "per row",
    "Dialog",    "modal",    "1 scrim",
};

/* ---- overlay page state ---- */
static int dlg_open, dlg_answer = -1;
static int menu_mi = -1, menu_ii = -1;
static const char *const m_file[] = { "New", "Open", "-", "Close", 0 };
static const char *const m_edit[] = { "Cut", "Copy", "Paste", 0 };
static const char *const m_view[] = { "Zoom in", "Zoom out", "Actual size", 0 };
static const char *const *const menus[3] = { m_file, m_edit, m_view };
static const char *const mtitles[3] = { "File", "Edit", "View" };

static void itoa_(int v, char *b)
{
    char t[12]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    int p = 0; if (neg) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

static void probe(int r, int g, int b)
{ aui_fill(PROBE_X, PROBE_Y, PROBE_W, PROBE_H, rgb(r, g, b)); }

/* -------------------------------------------------------------- controls */
static void page_controls(int x, int y, int w)
{
    probe(255, 0, 128);
    (void)w;
    int col2 = x + 350;

    aui_text_sz(x, y, "Buttons", AUI_MUTED, AUI_FS_LABEL);
    y += 20;
    aui_button_ex(x,       y, 96, AUI_H_CTL, "Primary",   AUI_V_PRIMARY,   1);
    aui_button_ex(x + 104, y, 96, AUI_H_CTL, "Secondary", AUI_V_SECONDARY, 1);
    aui_button_ex(x + 208, y, 76, AUI_H_CTL, "Ghost",     AUI_V_GHOST,     1);
    y += AUI_H_CTL + 8;
    aui_button_ex(x,       y, 96, AUI_H_CTL, "Danger",    AUI_V_DANGER,    1);
    aui_button_ex(x + 104, y, 96, AUI_H_CTL, "Disabled",  AUI_V_SECONDARY, 0);
    aui_button(x + 208, y, 76, AUI_H_CTL, "Glass");
    aui_tooltip("the original glass pill, unchanged");
    y += AUI_H_CTL + 8;
    aui_icon_button(x,      y, 32, GICON_FOLDER, 1);
    aui_icon_button(x + 40, y, 32, GICON_GLOBE,  1);
    aui_icon_button(x + 80, y, 32, GICON_CHART,  0);

    y += 44;
    aui_text_sz(x, y, "Selection", AUI_MUTED, AUI_FS_LABEL);
    y += 20;
    aui_checkbox(x, y, "Checked", &cb_a);
    aui_checkbox(x + 140, y, "Unchecked", &cb_b);
    y += 26;
    aui_checkbox_ex(x, y, "Disabled on", &cb_c, 0);
    y += 30;
    aui_radio(x, y, "Alpha", &radio_g, 0);
    aui_radio(x + 100, y, "Beta", &radio_g, 1);
    aui_radio(x + 190, y, "Gamma", &radio_g, 2);
    y += 30;
    aui_toggle(x, y, &sw_a, 1);
    aui_label(x + 56, y + 4, "On", AUI_TEXT);
    aui_toggle(x + 120, y, &sw_b, 1);
    aui_label(x + 176, y + 4, "Off", AUI_TEXT);

    /* second column */
    int y2 = y - 200;
    aui_text_sz(col2, y2, "Values", AUI_MUTED, AUI_FS_LABEL);
    y2 += 22;
    aui_slider(col2, y2, 220, &slider_v, 0, 100);
    char nb[12]; itoa_(slider_v, nb);
    aui_label(col2 + 232, y2 + 3, nb, AUI_TEXT);
    y2 += 30;
    aui_slider(col2, y2, 220, &slider_w, 0, 100);
    y2 += 34;
    aui_progress(col2, y2, 220, prog_v);
    y2 += 18;
    aui_progress(col2, y2, 220, -1);            /* indeterminate */
    y2 += 24;
    aui_spinner(col2 + 14, y2 + 14, 12);
    aui_label(col2 + 36, y2 + 8, "working", AUI_MUTED);

    y2 += 42;
    aui_text_sz(col2, y2, "Text", AUI_MUTED, AUI_FS_LABEL);
    y2 += 20;
    aui_textfield_ex(col2, y2, 220, field_a, sizeof field_a, "type here", 1);
    y2 += 36;
    aui_textfield_ex(col2, y2, 220, field_b, sizeof field_b, "placeholder", 1);
    y2 += 36;
    aui_segmented(col2, y2, 220, 28, (const char *const[]){ "Day", "Week", "Year" }, 3, &seg_v);
    y2 += 38;
    aui_select(col2, y2, 220, fruit, 6, &sel_v);
}

/* ---------------------------------------------------------------- shapes */
static void page_shapes(int x, int y, int w)
{
    probe(0, 255, 128);
    aui_text_sz(x, y, "Anti-aliased shapes over the blit primitive", AUI_MUTED, AUI_FS_LABEL);
    y += 24;

    /* The AA proof, at a size a screenshot can measure: a big radius means a
     * long arc, and a long arc on a hard-edged rasterizer is a visible
     * staircase. tests/qmp/qmp_gallery.py counts distinct tones along it. */
    aui_round(x, y, 120, 90, 24, AUI_ACCENT);
    aui_text_sz(x, y + 96, "round r=24", AUI_MUTED, AUI_FS_CAPTION);

    aui_stroke(x + 136, y, 120, 90, 24, 3, AUI_ACCENT);
    aui_text_sz(x + 136, y + 96, "stroke t=3", AUI_MUTED, AUI_FS_CAPTION);

    aui_vgrad_round(x + 272, y, 120, 90, 24, aui_hsl(210, 90, 62), aui_hsl(280, 90, 52));
    aui_text_sz(x + 272, y + 96, "gradient", AUI_MUTED, AUI_FS_CAPTION);

    aui_circle(x + 452, y + 45, 45, aui_hsl(150, 70, 48));
    aui_text_sz(x + 408, y + 96, "circle", AUI_MUTED, AUI_FS_CAPTION);

    y += 128;
    aui_text_sz(x, y, "Alpha compositing (no alpha-rect syscall exists)", AUI_MUTED, AUI_FS_LABEL);
    y += 22;
    aui_vgrad(x, y, 260, 70, aui_hsl(24, 90, 58), aui_hsl(340, 80, 52));
    for (int i = 0; i < 5; i++)
        aui_round_a(x + 12 + i * 48, y + 12, 44, 46, 12, rgb(255, 255, 255), 40 + i * 44);
    aui_text_sz(x, y + 76, "20% .. 100% white over a gradient", AUI_MUTED, AUI_FS_CAPTION);

    /* elevation ladder */
    for (int e = 0; e <= 3; e++) {
        int cx = x + 300 + e * 100;
        aui_shadow(cx, y + 6, 76, 58, AUI_R_LG, e);
        aui_round(cx, y + 6, 76, 58, AUI_R_LG, AUI_SURFACE);
        aui_stroke(cx, y + 6, 76, 58, AUI_R_LG, 1, AUI_BORDER);
        char nb[8]; itoa_(e, nb);
        aui_text_in(aui_r(cx, y + 6, 76, 58), nb, AUI_TEXT, AUI_FS_TITLE, AUI_ALIGN_CENTER);
    }
    aui_text_sz(x + 300, y + 76, "elevation 0..3 (8-slice shadows)", AUI_MUTED, AUI_FS_CAPTION);

    y += 110;
    aui_text_sz(x, y, "Badges + glass", AUI_MUTED, AUI_FS_LABEL);
    y += 22;
    aui_badge(x, y, "success", AUI_SUCCESS);
    aui_badge(x + 84, y, "warning", AUI_WARNING);
    aui_badge(x + 176, y, "error", AUI_ERROR);
    aui_badge(x + 250, y, "accent", AUI_ACCENT);
    aui_glass(x + 340, y - 6, w - 340 - 20, 40, AUI_R_LG);
    aui_text_sz(x + 356, y + 2, "liquid glass over the page", AUI_TEXT, AUI_FS_LABEL);
}

/* ------------------------------------------------------------------ data */
static void page_data(int x, int y, int w)
{
    probe(255, 200, 0);
    aui_text_sz(x, y, "Scrollable list (wheel, arrows, Home/End)", AUI_MUTED, AUI_FS_LABEL);
    aui_text_sz(x + 340, y, "Table with headers", AUI_MUTED, AUI_FS_LABEL);
    y += 20;
    aui_list(x, y, 300, 240, fruit, 12, &list_sel, &list_scroll);
    aui_table(x + 340, y, w - 340 - 20, 240, cols, colw, 3, cells, 8, &tab_sel, &tab_scroll);

    y += 254;
    aui_card(x, y, w - 40, 92, AUI_ELEV_1);
    aui_text_sz(x + 16, y + 12, "Card", AUI_TEXT, AUI_FS_TITLE);
    aui_text_sz(x + 16, y + 42, "An elevated surface: shadow, radius, hairline, all from tokens.",
                AUI_MUTED, AUI_FS_LABEL);
    aui_text_sz(x + 16, y + 62, "Nothing here needed a kernel drawing call that did not already exist.",
                AUI_MUTED, AUI_FS_LABEL);
}

/* -------------------------------------------------------------- overlays */
static void page_overlay(int x, int y, int w)
{
    probe(0, 160, 255);
    aui_menubar(x, y, w - 40, mtitles, menus, 3, &menu_mi, &menu_ii);
    y += 40;
    aui_text_sz(x, y, "Menus, tooltips, modal dialogs", AUI_MUTED, AUI_FS_LABEL);
    y += 24;
    if (aui_button_ex(x, y, 150, AUI_H_CTL + 4, "Open dialog", AUI_V_PRIMARY, 1)) dlg_open = 1;
    aui_button_ex(x + 166, y, 150, AUI_H_CTL + 4, "Hover me", AUI_V_SECONDARY, 1);
    aui_tooltip("a tooltip, after the pointer settles");

    y += 46;
    if (menu_mi >= 0) {
        aui_label(x, y, "menu chosen:", AUI_MUTED);
        char nb[12]; itoa_(menu_mi * 100 + menu_ii, nb);
        aui_label(x + 110, y, nb, AUI_TEXT);
    }
    if (dlg_answer >= 0) {
        aui_label(x, y + 22, "dialog answer:", AUI_MUTED);
        char nb[12]; itoa_(dlg_answer, nb);
        aui_label(x + 110, y + 22, nb, AUI_TEXT);
    }

    if (dlg_open) {
        aui_dialog_begin("Delete the volume?", 380, 170);
        aui_text_sz(AUI_SP(5), AUI_SP(3), "This cannot be undone. The scrim behind",
                    AUI_TEXT, AUI_FS_LABEL);
        aui_text_sz(AUI_SP(5), AUI_SP(3) + 20, "this sheet swallows clicks meant for the page.",
                    AUI_TEXT, AUI_FS_LABEL);
        int p = aui_dialog_buttons((const char *const[]){ "Delete", "Cancel" }, 2);
        if (p >= 0) { dlg_answer = p; dlg_open = 0; }
        aui_dialog_end();
    }
}

/* ---- cost, measured on the machine ----
 * This toolkit is linked into every app and drawn every frame, so its cost is
 * not something to estimate. Each page reports the wall time of its own repaint
 * over the serial console (fd 1), in microseconds, from CLOCK_MONOTONIC --
 * monotonic_ms() steps in 10 ms and cannot see a frame at all.
 * tests/boot/run-aui-bench.sh reads these lines. */
static unsigned long long fr_sum, fr_n, fr_max;
static unsigned last_report;

static void report_cost(unsigned long long us)
{
    fr_sum += us; fr_n++;
    if (us > fr_max) fr_max = us;
    unsigned now = (unsigned)monotonic_ms();
    if (!last_report) { last_report = now; return; }
    if (now - last_report < 2000 || fr_n == 0) return;
    last_report = now;
    char b[96]; int p = 0;
    const char *pfx = "[aui] page ";
    while (*pfx) b[p++] = *pfx++;
    b[p++] = (char)('0' + tab);
    const char *m = " frames="; while (*m) b[p++] = *m++;
    char t[24]; itoa_((int)fr_n, t); for (int i = 0; t[i]; i++) b[p++] = t[i];
    m = " avg_us="; while (*m) b[p++] = *m++;
    itoa_((int)(fr_sum / fr_n), t); for (int i = 0; t[i]; i++) b[p++] = t[i];
    m = " max_us="; while (*m) b[p++] = *m++;
    itoa_((int)fr_max, t); for (int i = 0; t[i]; i++) b[p++] = t[i];
    b[p++] = '\n';
    sys_write(1, b, p);
    fr_sum = 0; fr_n = 0; fr_max = 0;
}

static void frame(void)
{
    unsigned long long t0 = monotonic_ns();
    aui_begin(AUI_BG);
    aui_heading(AUI_PAD, 10, "aui gallery", AUI_TEXT);
    aui_text_sz(AUI_PAD + aui_text_w("aui gallery", AUI_FS_TITLE) + 12, 16,
                "immediate-mode widgets over gui_*", AUI_MUTED, AUI_FS_LABEL);
    aui_tabs(AUI_PAD, 44, WINW - 2 * AUI_PAD, tabs, NTAB, &tab);

    int x = AUI_PAD, y = 92, w = WINW - 2 * AUI_PAD;
    switch (tab) {
    case T_CONTROLS: page_controls(x, y, w); break;
    case T_SHAPES:   page_shapes(x, y, w);   break;
    case T_DATA:     page_data(x, y, w);     break;
    default:         page_overlay(x, y, w);  break;
    }
    aui_end();
    report_cost((monotonic_ns() - t0) / 1000);
}

void app_main(void)
{
    gui_create("Gallery", WINW, WINH);
    aui_set_size(WINW, WINH);
    frame();

    struct logit_event e;
    unsigned last_anim = 0;
    for (;;) {
        int drew = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            aui_feed(&e);
            /* Hover IS a repaint, which is the whole reason the toolkit can have
             * hover states at all -- but aui_want_repaint() says whether the
             * motion could change anything, so idle motion over the wallpaper of
             * the window costs nothing. */
            if (aui_want_repaint()) { frame(); drew = 1; }
            aui_feed_done();
        }
        /* Animation tick: the spinner and the indeterminate bar have to advance
         * without input. 20 Hz is enough to look continuous and cheap enough not
         * to fight the compositor for the frame. */
        unsigned now = (unsigned)monotonic_ms();
        if (!drew && now - last_anim >= 50) { last_anim = now; frame(); }
        wait_idle(100);   /* was sys_yield(): a spin. input-driven */
    }
}
