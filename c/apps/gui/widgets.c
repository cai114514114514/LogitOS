#include "aui.h"

/* A showcase for the aui immediate-mode toolkit: label, +/- buttons, checkboxes,
 * a text field, semantic color swatches, and a light/dark theme toggle -- all
 * declarative, no raw rect-pushing in the app. */

static int  count = 0;
static int  fancy = 0;
static int  dark = 0;
static char name[24] = "";
static char greet[48] = "";

static void itoa_(int v, char *b)
{
    char t[12]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    int p = 0; if (neg) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

static void swatch(int x, int y, unsigned c, const char *s)
{
    aui_panel(x, y, 18, 18, c);
    aui_label(x + 24, y + 2, s, AUI_MUTED);
}

static void frame(void)
{
    aui_begin(AUI_BG);
    aui_heading(AUI_PAD, 12, "Widget Toolkit", AUI_TEXT);     /* title-size type */
    aui_label(AUI_PAD, 40, "immediate-mode UI over gui_*", AUI_MUTED);

    char nb[12]; itoa_(count, nb);
    aui_label(20, 78, "Count:", AUI_MUTED);
    aui_label(86, 78, nb, AUI_TEXT);
    if (aui_button(150, 70, 36, 28, "-")) count--;
    if (aui_button(192, 70, 36, 28, "+")) count++;

    aui_checkbox(20, 116, "Fancy mode", &fancy);
    aui_checkbox(180, 116, "Dark mode", &dark);

    aui_label(20, 156, "Name:", AUI_MUTED);
    aui_textfield(86, 150, 200, name, sizeof name);

    if (aui_button(20, 196, 96, 30, "Greet")) {
        int p = 0; const char *h = fancy ? "Hello, dear " : "Hello, ";
        while (h[p]) { greet[p] = h[p]; p++; }
        for (int i = 0; name[i] && p < 46; i++) greet[p++] = name[i];
        if (p < 47) greet[p++] = '!';
        greet[p] = 0;
    }
    aui_label(130, 203, greet, AUI_ACCENT);

    /* semantic swatches laid out with the horizontal stack helper */
    int sx, sy;
    aui_hstack(AUI_PAD, 250, AUI_SP(7));
    aui_next(90, 18, &sx, &sy); swatch(sx, sy, AUI_SUCCESS, "Success");
    aui_next(90, 18, &sx, &sy); swatch(sx, sy, AUI_WARNING, "Warning");
    aui_next(90, 18, &sx, &sy); swatch(sx, sy, AUI_ERROR,   "Error");
    aui_end();
}

void app_main(void)
{
    gui_create("Widgets", 340, 290);
    dark = sys_ui_dark(-1) > 0;               /* reflect the current system theme */
    frame();                                  /* initial paint */
    struct logit_event e;
    for (;;) {
        if (!poll_event(&e)) { sys_yield(); continue; }
        if (e.type == EV_CLOSE) app_exit(0);
        dark = sys_ui_dark(-1) > 0;           /* follow the menu-bar switch too */
        aui_feed(&e);
        frame();                              /* the Dark checkbox may flip `dark` */
        aui_feed_done();
        if (dark != (sys_ui_dark(-1) > 0)) sys_ui_dark(dark);   /* checkbox -> drive the system */
    }
}
