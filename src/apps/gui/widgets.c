#include "aui.h"

/* A showcase for the aui immediate-mode toolkit: label, +/- buttons, a checkbox,
 * a text field, and a button that builds a greeting -- all declarative, no raw
 * rect-pushing in the app. */

static int  count = 0;
static int  fancy = 0;
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

static void frame(void)
{
    aui_begin(AUI_BG);
    aui_label(20, 16, "Widget Toolkit", AUI_TEXT);
    aui_label(20, 38, "immediate-mode UI over gui_*", AUI_MUTED);

    char nb[12]; itoa_(count, nb);
    aui_label(20, 78, "Count:", AUI_MUTED);
    aui_label(86, 78, nb, AUI_TEXT);
    if (aui_button(150, 70, 36, 28, "-")) count--;
    if (aui_button(192, 70, 36, 28, "+")) count++;

    aui_checkbox(20, 116, "Fancy mode", &fancy);

    aui_label(20, 156, "Name:", AUI_MUTED);
    aui_textfield(86, 150, 200, name, sizeof name);

    if (aui_button(20, 196, 96, 30, "Greet")) {
        int p = 0; const char *h = fancy ? "Hello, dear " : "Hello, ";
        while (h[p]) { greet[p] = h[p]; p++; }
        for (int i = 0; name[i] && p < 46; i++) greet[p++] = name[i];
        if (p < 47) greet[p++] = '!';
        greet[p] = 0;
    }
    aui_label(20, 240, greet, AUI_ACCENT);
    aui_end();
}

void app_main(void)
{
    gui_create("Widgets", 320, 280);
    frame();                                  /* initial paint */
    struct aqua_event e;
    for (;;) {
        if (!poll_event(&e)) { sys_yield(); continue; }
        if (e.type == EV_CLOSE) app_exit(0);
        aui_feed(&e);
        frame();
        aui_feed_done();
    }
}
