/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Window chrome is separate from the codec loops. These primitives use the
 * existing OpenLogit transport; no second rasterizer or UI-specific decoder. */
static char view_name[192];
static int view_image, view_actual, view_redraw, view_dark;
static unsigned ui_bg, ui_panel, ui_text, ui_muted, ui_line, ui_accent;

static void view_theme(void)
{
    view_dark = sys_ui_dark(-1) > 0;
    ui_bg = view_dark ? 0x20232B : 0xF5F6FA;
    ui_panel = view_dark ? 0x292D37 : 0xFFFFFF;
    ui_text = view_dark ? 0xEDF0F7 : 0x242936;
    ui_muted = view_dark ? 0xA0A8B8 : 0x6D7688;
    ui_line = view_dark ? 0x383E4B : 0xE0E4ED;
    ui_accent = 0x658EFF;
}
static void label(int x, int y, int px, unsigned color, const char *text)
{ gui_text_run(x, y, px, 0, color, text, (int)strlen(text)); }

static void clipped_label(int x, int y, int w, int px, unsigned color, const char *text)
{
    gui_clip(x, y, w, px + 6);
    label(x, y, px, color, text);
    gui_clip(0, 0, WINW, WINH);
}
static int inside(int x, int y, int bx, int by, int w, int h)
{ return x >= bx && y >= by && x < bx + w && y < by + h; }

static void button(int x, int y, int w, const char *text, int active)
{
    gui_rrect(x, y, w, 30, 8, active ? ui_accent : ui_panel);
    int tw = text_measure_px(text, (int)strlen(text), 13, 0);
    label(x + (w - tw) / 2, y + 7, 13, active ? 0xFFFFFF : ui_text, text);
}
static void view_clear(void)
{
    view_theme();
    /* A constant neutral stage keeps transparent image colours predictable
     * in either desktop theme, and preserves exact host/guest alpha checks. */
    gui_clear(0x1C1C20);
}
static void blit_fit_src(const unsigned char *src, int iw, int ih)
{
    if (iw <= 0 || ih <= 0) return;
    int w = WINW - 32, h = CONTH;
    int dw = w, dh = (int)((long)w * ih / iw);
    if (dh > h) { dh = h; dw = (int)((long)h * iw / ih); }
    if (view_image && view_actual) { dw = iw; dh = ih; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    gui_clip(16, 76, w, h);
    gui_blit(16 + (w - dw) / 2, 76 + (h - dh) / 2, dw, dh, src, iw, ih);
    gui_clip(0, 0, WINW, WINH);
}
static void blit_fit(int iw, int ih) { blit_fit_src(rgba, iw, ih); }

static const char *hint(void)
{ return g_from_picker ? "Back to library with Esc" : "Esc to close"; }

static void status(const char *text)
{
    gui_rect(0, 0, WINW, 60, ui_bg);
    gui_rect(0, 59, WINW, 1, ui_line);
    /* A chrome-only anchor for the direct-open association gate. The stage
     * keeps the decoded pixels untouched, including transparent corners. */
    gui_rect(0, 0, 6, 6, 0xFD0181);
    button(16, 15, 76, g_from_picker ? "< Library" : "Close", 0);
    clipped_label(110, 13, WINW - 290, 17, ui_text, view_name[0] ? view_name : "Preview");
    label(110, 36, 11, ui_muted, view_image ? "Image viewer" : "Media viewer");
    if (view_image) {
        button(WINW - 154, 15, 62, "Fit", !view_actual);
        button(WINW - 84, 15, 68, "100%", view_actual);
    }
    gui_rect(0, WINH - 48, WINW, 48, ui_bg);
    gui_rect(0, WINH - 48, WINW, 1, ui_line);
    clipped_label(18, WINH - 34, WINW - 36, 12, ui_muted, text);
}

/* Escape unwinds a library item; directly opened files still close. Resize
 * invalidates a held still as well as active playback, avoiding stale pixels
 * in the newly exposed area of a larger window. */
static int pump_events(void)
{
    struct logit_event e;
    int result = PUMP_NONE;
    while (poll_event(&e)) {
        if (ag_gui_event(&e)) continue;
        if (e.type == EV_CLOSE) return PUMP_QUIT;
        if (e.type == EV_RESIZE) {
            view_width = e.a; view_height = e.b; view_redraw = 1;
        }
        if (e.type == EV_THEME) { view_theme(); view_redraw = 1; }
        if (e.type == EV_KEY) {
            if (e.a == 27) result = g_from_picker ? PUMP_BACK : PUMP_QUIT;
            if ((e.a == 8 || e.a == 127) && g_from_picker) result = PUMP_BACK;
            if (view_image && (e.a == '0' || e.a == '1')) {
                view_actual = e.a == '1'; view_redraw = 1;
            }
        }
        if (e.type == EV_MOUSE) {
            if (inside(e.a, e.b, 16, 15, 76, 30))
                result = g_from_picker ? PUMP_BACK : PUMP_QUIT;
            if (view_image && inside(e.a, e.b, WINW - 154, 15, 138, 30)) {
                view_actual = e.a >= WINW - 84; view_redraw = 1;
            }
        }
    }
    return result;
}

static void message_frame(const char *text)
{
    view_clear();
    int x = 48, y = 116, w = WINW - 96;
    gui_rrect(x, y, w, WINH - 232, 16, ui_panel);
    label(x + 28, y + 28, 22, ui_text, "Preview information");
    /* Wrap by measured glyph width. Decoder refusals often exceed a line;
     * clipping the old single line hid the reason at the right window edge. */
    char line[256];
    int pos = 0, row = 0;
    while (text[pos] && row < (WINH - 324) / 24) {
        int n = 0, space = -1;
        while (text[pos + n] && n < 254) {
            line[n] = text[pos + n];
            if (line[n] == ' ') space = n;
            if (text_measure_px(line, n + 1, 14, 0) > w - 56) break;
            n++;
        }
        if (text[pos + n] && space > 0) n = space;
        if (!n) n = 1;
        line[n] = 0;
        label(x + 28, y + 76 + row * 24, 14, ui_muted, line);
        pos += n;
        while (text[pos] == ' ') pos++;
        row++;
    }
    status(hint()); gui_flush(); view_redraw = 0;
}
static int show_message(const char *msg)
{
    view_image = 0;
    message_frame(msg);
    if (g_one_loop) return PUMP_QUIT;
    for (;;) {
        int p = pump_events();
        if (p) return p;
        if (view_redraw) message_frame(msg);
        wait_idle(30);
    }
}
static int fail_with(const char *msg)
{ plog("preview: error %s\n", msg); return show_message(msg); }
