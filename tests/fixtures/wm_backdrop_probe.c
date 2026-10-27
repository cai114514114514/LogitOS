/* SPDX-License-Identifier: MIT
 * Guest probe for wm-backdrop-regions: a plain logit.h client with ONE sidebar
 * backdrop region (SYS_GUI_BACKDROP through ol_window_backdrop2) and text drawn
 * inside it. The rest of the canvas is one flat colour; the sidebar's fill_rgb
 * is another flat colour, so anything under the sidebar that is not flat came
 * from the desktop behind the window.
 *
 * Prints on the serial console, once per frame:
 *   BACKDROP DONE <seq> info=<hex> rc=<status>
 * (info is SYS_GUI2_INFO(0); rc is the BACKDROP call's status, 0 = OL_OK).
 * Keys (focus the window first): r redraws the frame; Esc / close exits. */
#include "logit.h"

/* The probe links no libc; the gfx objects it carries still reference
 * memcpy/memset. rep-string forms cannot be rewritten into calls to themselves. */
void *memcpy(void *d, const void *s, unsigned long n)
{ void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r; }
void *memset(void *d, int c, unsigned long n)
{ void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }

#define PW 560
#define PH 380
#define SBW 200              /* sidebar width, points */

static unsigned seq;
static void put_hex(char *b, int *n, unsigned v)
{
    for (int s = 28; s >= 0; s -= 4) b[(*n)++] = "0123456789abcdef"[(v >> s) & 15];
}
static void put_dec(char *b, int *n, long v)
{
    char d[24]; int k = 0;
    if (v < 0) { b[(*n)++] = '-'; v = -v; }
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[(*n)++] = d[--k];
}
static void done(unsigned info, long rc)
{
    char b[80]; int n = 0;
    const char *p = "BACKDROP DONE ";
    while (*p) b[n++] = *p++;
    put_dec(b, &n, ++seq);
    p = " info="; while (*p) b[n++] = *p++;
    put_hex(b, &n, info);
    p = " rc="; while (*p) b[n++] = *p++;
    put_dec(b, &n, rc);
    b[n++] = '\n';
    sys_write(1, b, n);
}

static void frame(void)
{
    unsigned info = (unsigned)ol_gui2_info(0);
    gui_clear(rgb(248, 248, 250));
    struct logit_backdrop2 bd = {0};
    bd.id = 0;
    bd.x = 0; bd.y = 0;
    bd.w = SBW << 8; bd.h = PH << 8;
    bd.radius[1] = bd.radius[2] = 0;     /* square: the window's own corners round it */
    bd.tint_argb = 0xF2F3F7;
    bd.opacity = 40;                     /* a light tint: the backdrop stays legible */
    bd.blur_level = 0;                   /* REGULAR */
    bd.fill_rgb = 0xE4E5EA;
    long rc = ol_window_backdrop2(&bd);
    /* app ink on top of the glass: crisp text in the sidebar */
    gui_text(16, 24, rgb(16, 16, 20), "Favorites");
    gui_text(16, 52, rgb(16, 16, 20), "Applications");
    gui_text(16, 80, rgb(16, 16, 20), "Documents");
    gui_text(SBW + 24, 24, rgb(16, 16, 20), "Content");
    gui_flush();
    done(info, rc);
}

void app_main(void)
{
    gui_create("Backdrop Probe", PW, PH);
    frame();
    struct logit_event e;
    for (;;) {
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) app_exit(0);
            if (e.type == EV_KEY && e.a == 'r') frame();
            if (e.type == EV_RESIZE) frame();
        }
        wait_idle(0);
    }
}
