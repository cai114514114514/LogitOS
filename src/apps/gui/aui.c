#include "aui.h"

/* Text is drawn + measured through the same px path so labels center exactly. */
#define PX 15

static int ev_type, ev_a, ev_b;     /* the event being handled this frame */
static int id_ctr, focus_id;        /* widget ids (call order); textfield focus */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int tw(const char *s)   { return text_measure_px(s, slen(s), PX, 0); }
static void txt(int x, int y, unsigned c, const char *s) { gui_text_run(x, y, PX, 0, c, s, slen(s)); }

static int clicked_in(int x, int y, int w, int h)
{ return ev_type == EV_MOUSE && ev_a >= x && ev_a < x + w && ev_b >= y && ev_b < y + h; }

void aui_feed(const struct aqua_event *e) { ev_type = e->type; ev_a = e->a; ev_b = e->b; }
void aui_feed_done(void) { ev_type = 0; }

void aui_begin(unsigned bg) { id_ctr = 0; gui_clear(bg); }
void aui_end(void) { gui_flush(); }

void aui_panel(int x, int y, int w, int h, unsigned color) { gui_rect(x, y, w, h, color); }
void aui_label(int x, int y, const char *s, unsigned color) { txt(x, y, color, s); }

int aui_button(int x, int y, int w, int h, const char *label)
{
    id_ctr++;
    int hit = clicked_in(x, y, w, h);
    gui_rect(x, y, w, h, hit ? AUI_ACCENT : AUI_FACE);
    gui_rect(x, y, w, 1, rgb(255, 255, 255));           /* top highlight */
    gui_rect(x, y + h - 1, w, 1, rgb(206, 208, 216));   /* bottom edge */
    int lw = tw(label);
    txt(x + (w - lw) / 2, y + (h - PX) / 2 - 1, hit ? rgb(255, 255, 255) : AUI_TEXT, label);
    return hit;
}

int aui_checkbox(int x, int y, const char *label, int *state)
{
    id_ctr++;
    int box = 16;
    if (*state) gui_rect(x, y, box, box, AUI_ACCENT);
    else { gui_rect(x, y, box, box, rgb(206, 208, 216)); gui_rect(x + 1, y + 1, box - 2, box - 2, rgb(255, 255, 255)); }
    txt(x + box + 8, y + 1, AUI_TEXT, label);
    if (clicked_in(x, y, box + 8 + tw(label), box)) { *state = !*state; return 1; }
    return 0;
}

int aui_textfield(int x, int y, int w, char *buf, int cap)
{
    int myid = ++id_ctr, h = 26, ret = 0;
    if (clicked_in(x, y, w, h)) focus_id = myid;
    int foc = (focus_id == myid);

    gui_rect(x, y, w, h, rgb(255, 255, 255));
    unsigned b = foc ? AUI_ACCENT : rgb(206, 208, 216);
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
