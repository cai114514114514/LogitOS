#include "aqua.h"

/* A web browser: address bar + a real rendered page (the kernel does
 * DNS+TCP+TLS+HTTP, parses HTML->DOM, applies CSS, lays out a display list, and
 * paints it via SYS_PAGE_*). This app drives the URL, scrolls the viewport, and
 * navigates on link clicks. */

/* http_get error codes (mirror include/http.h) */
#define HTTP_ERR_URL  -2
#define HTTP_ERR_DNS  -3
#define HTTP_ERR_CONN -4
#define HTTP_ERR_TLS  -5

#define WINW 760
#define WINH 560
#define BARH 30
#define VIEW_H (WINH - BARH - 18)        /* viewport height (below bar, above status) */

static char url[600] = "http://example.com/";
static int  ulen = 19;
static int  scroll;                      /* pixel scroll offset */
static int  ph;                          /* laid-out page height */
static char status[96] = "ready -- edit URL, Enter to load";

static void set_status(const char *s)
{ int i = 0; while (s[i] && i < (int)sizeof status - 1) { status[i] = s[i]; i++; } status[i] = 0; }

static void redraw(int editing);

static void load(const char *u)
{
    set_status("loading...");
    int rc = http_get(u);
    if (rc < 0) {
        set_status(rc == HTTP_ERR_URL  ? "load failed: bad URL (need http:// or https://)" :
                   rc == HTTP_ERR_DNS  ? "load failed: DNS lookup failed" :
                   rc == HTTP_ERR_CONN ? "load failed: could not connect (timed out)" :
                   rc == HTTP_ERR_TLS  ? "load failed: TLS/certificate error" :
                                         "load failed");
        ph = 0; scroll = 0; return;
    }
    if (http_status() != 2) { set_status("error: could not load"); ph = 0; scroll = 0; return; }
    scroll = 0;
    ph = page_height();
    set_status("loaded");
    redraw(0);                      /* paint the text immediately ... */
    if (page_load_images(3) > 0) {  /* ... then fetch a few images and repaint */
        ph = page_height();
        redraw(0);
    }
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    /* address bar */
    gui_rect(0, 0, WINW, BARH, rgb(225, 228, 234));
    gui_rect(10, 5, WINW - 20, 20, rgb(255, 255, 255));
    gui_text(14, 7, rgb(40, 40, 48), url);
    if (editing) gui_rect(14 + ulen * 8, 7, 8, 16, rgb(90, 150, 240));
    /* the page */
    page_render(scroll, 0, BARH, WINW, VIEW_H);
    /* status line */
    gui_rect(0, WINH - 18, WINW, 18, rgb(238, 240, 244));
    gui_text(10, WINH - 16, rgb(110, 110, 120), status);
    gui_flush();
}

void app_main(void)
{
    gui_create("Browser", WINW, WINH);
    redraw(1);
    int editing = 1;

    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_KEY) {
                int k = e.a;
                int maxs = ph - VIEW_H; if (maxs < 0) maxs = 0;
                if      (k == KEY_DOWN) scroll += 40;
                else if (k == KEY_UP)   scroll -= 40;
                else if (k == KEY_PGDN) scroll += VIEW_H - 40;
                else if (k == KEY_PGUP) scroll -= VIEW_H - 40;
                else if (k == KEY_HOME) scroll = 0;
                else if (k == KEY_END)  scroll = maxs;
                else if (k == '\n') { editing = 0; load(url); editing = 1; }
                else if (k == '\b') { if (ulen > 0) url[--ulen] = 0; }
                else if (k >= ' ' && k < 0x7f && ulen < (int)sizeof url - 1) { url[ulen++] = (char)k; url[ulen] = 0; }
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                redraw(editing);
            } else if (e.type == EV_MOUSE) {
                int mx = e.a, my = e.b;              /* window-local */
                if (my >= BARH && my < BARH + VIEW_H) {
                    char nu[256];
                    if (page_hittest(mx, my - BARH, scroll, nu)) {
                        int i = 0; while (nu[i] && i < (int)sizeof url - 1) { url[i] = nu[i]; i++; }
                        url[i] = 0; ulen = i;
                        load(url);
                    }
                    redraw(editing);
                }
            }
        }
        sys_yield();
    }
}
