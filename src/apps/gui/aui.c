#include "aui.h"

/* ---------- color tokens + light/dark theme ----------
 * rgb() isn't a constant expression, so the active theme `aui_t` can't be a
 * static initializer; it's filled on the first aui_begin (or aui_set_dark). */
static int theme_dark, theme_inited;
struct aui_theme aui_t;

static void load_light(void)
{
    aui_t.bg          = rgb(244, 245, 248);
    aui_t.surface     = rgb(255, 255, 255);
    aui_t.face        = rgb(232, 234, 240);
    aui_t.text        = rgb(40, 42, 50);
    aui_t.muted       = rgb(140, 144, 154);
    aui_t.border      = rgb(206, 208, 216);
    aui_t.hi          = rgb(255, 255, 255);
    aui_t.accent      = rgb(64, 130, 246);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(52, 199, 89);
    aui_t.warning     = rgb(255, 179, 64);
    aui_t.error       = rgb(255, 69, 58);
    aui_t.focus       = rgb(64, 130, 246);
}
static void load_dark(void)
{
    aui_t.bg          = rgb(28, 28, 32);
    aui_t.surface     = rgb(44, 44, 52);
    aui_t.face        = rgb(58, 58, 68);
    aui_t.text        = rgb(236, 237, 242);
    aui_t.muted       = rgb(146, 148, 160);
    aui_t.border      = rgb(72, 74, 86);
    aui_t.hi          = rgb(84, 86, 98);
    aui_t.accent      = rgb(94, 150, 255);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(48, 209, 88);
    aui_t.warning     = rgb(255, 190, 84);
    aui_t.error       = rgb(255, 92, 82);
    aui_t.focus       = rgb(94, 150, 255);
}
void aui_ensure(void) { if (!theme_inited) { load_light(); theme_inited = 1; } }

void aui_set_dark(int on)
{
    theme_dark = on; theme_inited = 1;
    if (on) load_dark(); else load_light();
}
int aui_is_dark(void) { return theme_dark; }

/* HSL -> packed rgb, integer-only. h:0..359, s,l:0..100. Channels are carried in
 * "percent" (0..100) through the standard piecewise formula, then scaled to 8-bit:
 *   C = (1-|2L-1|)*S,  X = C*(triangular wave over the sextant),  m = L - C/2. */
unsigned aui_hsl(int h, int s, int l)
{
    if (h < 0) h = 0; if (h > 359) h = 359;
    if (s < 0) s = 0; if (s > 100) s = 100;
    if (l < 0) l = 0; if (l > 100) l = 100;
    int a = (l >= 50) ? (2 * l - 100) : (100 - 2 * l);   /* |2L-1| in % */
    int C = (100 - a) * s / 100;                          /* chroma, 0..100 */
    int seg = h / 60, frac = h % 60;                      /* sextant + position */
    int X = (seg & 1) ? C * (60 - frac) / 60 : C * frac / 60;
    int r1 = 0, g1 = 0, b1 = 0;
    switch (seg) {
        case 0: r1 = C; g1 = X; break;
        case 1: r1 = X; g1 = C; break;
        case 2: g1 = C; b1 = X; break;
        case 3: g1 = X; b1 = C; break;
        case 4: r1 = X; b1 = C; break;
        default: r1 = C; b1 = X; break;
    }
    int m = l - C / 2;                                    /* lightness offset, % */
    int R = (r1 + m) * 255 / 100, G = (g1 + m) * 255 / 100, B = (b1 + m) * 255 / 100;
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    return rgb(R, G, B);
}

void aui_set_accent(unsigned color) { aui_t.accent = color; aui_t.focus = color; }

/* Text is drawn + measured through the same px path so labels center exactly. */
#define PX 15

static int ev_type, ev_a, ev_b;     /* the event being handled this frame */
static int id_ctr, focus_id;        /* widget ids (call order); textfield focus */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int tw(const char *s)   { return text_measure_px(s, slen(s), PX, 0); }
static void txt(int x, int y, unsigned c, const char *s) { gui_text_run(x, y, PX, 0, c, s, slen(s)); }

static int clicked_in(int x, int y, int w, int h)
{ return ev_type == EV_MOUSE && ev_a >= x && ev_a < x + w && ev_b >= y && ev_b < y + h; }

void aui_feed(const struct aether_event *e) { ev_type = e->type; ev_a = e->a; ev_b = e->b; }
void aui_feed_done(void) { ev_type = 0; }

void aui_begin(unsigned bg) { aui_ensure(); id_ctr = 0; gui_clear(bg); }
void aui_end(void) { gui_flush(); }

void aui_panel(int x, int y, int w, int h, unsigned color) { gui_rect(x, y, w, h, color); }
void aui_label(int x, int y, const char *s, unsigned color) { txt(x, y, color, s); }

int aui_button(int x, int y, int w, int h, const char *label)
{
    id_ctr++;
    int hit = clicked_in(x, y, w, h);
    gui_rect(x, y, w, h, hit ? AUI_ACCENT : AUI_FACE);
    gui_rect(x, y, w, 1, AUI_HI);                        /* top highlight */
    gui_rect(x, y + h - 1, w, 1, AUI_BORDER);            /* bottom edge */
    int lw = tw(label);
    txt(x + (w - lw) / 2, y + (h - PX) / 2 - 1, hit ? AUI_ACCENT_TEXT : AUI_TEXT, label);
    return hit;
}

int aui_checkbox(int x, int y, const char *label, int *state)
{
    id_ctr++;
    int box = 16;
    if (*state) gui_rect(x, y, box, box, AUI_ACCENT);
    else { gui_rect(x, y, box, box, AUI_BORDER); gui_rect(x + 1, y + 1, box - 2, box - 2, AUI_SURFACE); }
    txt(x + box + 8, y + 1, AUI_TEXT, label);
    if (clicked_in(x, y, box + 8 + tw(label), box)) { *state = !*state; return 1; }
    return 0;
}

int aui_textfield(int x, int y, int w, char *buf, int cap)
{
    int myid = ++id_ctr, h = 26, ret = 0;
    if (clicked_in(x, y, w, h)) focus_id = myid;
    int foc = (focus_id == myid);

    gui_rect(x, y, w, h, AUI_SURFACE);
    unsigned b = foc ? AUI_FOCUS : AUI_BORDER;
    gui_rect(x, y, w, 1, b); gui_rect(x, y + h - 1, w, 1, b);
    gui_rect(x, y, 1, h, b); gui_rect(x + w - 1, y, 1, h, b);

    int n = slen(buf);
    txt(x + 8, y + (h - PX) / 2 - 1, AUI_TEXT, buf);
    if (foc) {
        gui_rect(x + 9 + tw(buf), y + 5, 1, h - 10, AUI_TEXT);   /* caret */
        if (ev_type == EV_KEY) {
            int k = ev_a;
            if (k == '\n') ret = 1;
            else if (k == '\b' || k == 127) { if (n > 0) buf[n - 1] = 0; }
            else if (k >= 32 && k < 127 && n < cap - 1) { buf[n] = (char)k; buf[n + 1] = 0; }
        }
    }
    return ret;
}
