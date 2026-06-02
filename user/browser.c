#include "aqua.h"

/* A tiny web browser: address bar + de-tagged page text with clickable links.
 * The kernel does DNS+TCP+HTTP+render (SYS_HTTP_*); this app just drives the
 * URL, paints the text line-by-line with scroll, and navigates on link click. */

/* http_get error codes (mirror include/http.h) */
#define HTTP_ERR_URL  -2
#define HTTP_ERR_DNS  -3
#define HTTP_ERR_CONN -4
#define HTTP_ERR_TLS  -5

#define WINW 600
#define WINH 440
#define BARH 30
#define LINEH 16
#define LEFT 10
#define TOP  (BARH + 8)
#define ROWS ((WINH - TOP - 8) / LINEH)

static char url[600] = "http://example.com/";
static int  ulen = 19;
static char page[32768];
static int  plen;
static int  scroll;             /* first visible line index */
static char status[64] = "ready -- edit URL, Enter to load";

/* line index -> byte offset table (rebuilt after each load) */
static int  line_off[4096];
static int  nlines;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void index_lines(void)
{
    nlines = 0;
    line_off[nlines++] = 0;
    for (int i = 0; i < plen && nlines < 4096; i++)
        if (page[i] == '\n') line_off[nlines++] = i + 1;
}

/* Is this line a link line? Links render as "[n] text"; return n or -1. */
static int line_link(int li)
{
    int o = line_off[li];
    if (o < plen && page[o] == '[') {
        int n = 0, i = o + 1;
        while (i < plen && page[i] >= '0' && page[i] <= '9') n = n * 10 + (page[i++] - '0');
        if (i < plen && page[i] == ']') return n;
    }
    return -1;
}

static void load(const char *u)
{
    int q = 0; const char *m = "loading...";
    while (m[q]) { status[q] = m[q]; q++; } status[q] = 0;

    int rc = http_get(u);
    if (rc < 0) {
        const char *e = rc == HTTP_ERR_URL  ? "load failed: bad URL (need http:// or https://)" :
                        rc == HTTP_ERR_DNS  ? "load failed: DNS lookup failed" :
                        rc == HTTP_ERR_CONN ? "load failed: could not connect (timed out)" :
                        rc == HTTP_ERR_TLS  ? "load failed: TLS/certificate error" :
                                              "load failed";
        int i = 0; while (e[i]) { status[i] = e[i]; i++; } status[i] = 0;
        plen = 0; nlines = 0; scroll = 0; return;
    }
    /* http_get is synchronous in the kernel; status is DONE on return. */
    int st = http_status();
    if (st != 2) {
        const char *e = "error: could not load (TLS? try http:// only)";
        int i = 0; while (e[i]) { status[i] = e[i]; i++; } status[i] = 0;
        plen = 0; nlines = 0; scroll = 0; return;
    }
    plen = 0;
    for (;;) {
        int n = http_read(plen, page + plen, (int)sizeof page - 1 - plen);
        if (n <= 0) break;
        plen += n;
        if (plen >= (int)sizeof page - 1) break;
    }
    page[plen] = 0;
    index_lines();
    scroll = 0;
    int q2 = 0; const char *d = "loaded";
    while (d[q2]) { status[q2] = d[q2]; q2++; } status[q2] = 0;
}

static void redraw(int editing)
{
    gui_clear(rgb(252, 252, 253));
    /* address bar */
    gui_rect(0, 0, WINW, BARH, rgb(225, 228, 234));
    gui_rect(LEFT, 5, WINW - 2 * LEFT, 20, rgb(255, 255, 255));
    gui_text(LEFT + 4, 7, rgb(40, 40, 48), url);
    if (editing) gui_rect(LEFT + 4 + ulen * 8, 7, 8, 16, rgb(90, 150, 240));

    /* page text */
    int y = TOP;
    for (int r = 0; r < ROWS && scroll + r < nlines; r++) {
        int li = scroll + r;
        int o = line_off[li];
        int end = (li + 1 < nlines) ? line_off[li + 1] - 1 : plen;
        char buf[96]; int n = 0;
        for (int i = o; i < end && n < 95; i++) buf[n++] = page[i];
        buf[n] = 0;
        unsigned col = line_link(li) >= 0 ? rgb(40, 90, 210) : rgb(45, 48, 56);
        gui_text(LEFT, y, col, buf);
        y += LINEH;
    }
    /* status line */
    gui_rect(0, WINH - 18, WINW, 18, rgb(238, 240, 244));
    gui_text(LEFT, WINH - 16, rgb(110, 110, 120), status);
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
                int maxs = nlines - ROWS; if (maxs < 0) maxs = 0;
                if (k == KEY_DOWN)      scroll += 3;
                else if (k == KEY_UP)   scroll -= 3;
                else if (k == KEY_PGDN) scroll += ROWS - 1;
                else if (k == KEY_PGUP) scroll -= ROWS - 1;
                else if (k == KEY_HOME) scroll = 0;
                else if (k == KEY_END)  scroll = maxs;
                else if (k == '\n') { editing = 0; load(url); editing = 1; }
                else if (k == '\b') { if (ulen > 0) url[--ulen] = 0; }
                else if (k >= ' ' && k < 0x7f && ulen < (int)sizeof url - 1) { url[ulen++] = (char)k; url[ulen] = 0; }
                if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
                redraw(editing);
            } else if (e.type == EV_MOUSE) {
                int my = e.b;                       /* window-local y */
                if (my >= TOP) {
                    int r = (my - TOP) / LINEH;
                    int li = scroll + r;
                    if (li < nlines) {
                        int ln = line_link(li);
                        if (ln >= 0) {
                            char nu[600];
                            if (http_link(ln, nu, sizeof nu) == 0) {
                                int i = 0; while (nu[i] && i < (int)sizeof url - 1) { url[i] = nu[i]; i++; }
                                url[i] = 0; ulen = i;
                                load(url);
                            }
                        } else {
                            /* click lower half = page down, upper = page up */
                            if (r > ROWS / 2) { if (scroll + ROWS < nlines) scroll += ROWS / 2; }
                            else { scroll -= ROWS / 2; if (scroll < 0) scroll = 0; }
                        }
                        redraw(editing);
                    }
                }
            }
        }
        sys_yield();
    }
}
