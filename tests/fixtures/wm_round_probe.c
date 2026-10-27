/* SPDX-License-Identifier: MIT
 * Guest probe for wm-rounded-content: a plain logit.h client whose window is one
 * solid colour, so the compositor's corner shape is the only thing on screen
 * that is not the app's. Keys (focus the window first):
 *   r  fill the whole canvas red   (255,0,0)      and flush
 *   b  fill the whole canvas blue  (0,0,255)      and flush
 *   k  paint the bottom-left and bottom-right corner squares RED and flush
 *      ONLY those two small rects (a damage rect confined to a corner)
 *   Esc / close  exit.
 * Every flush prints "ROUND DONE <seq> <r|b|k>" on the serial console so the
 * driver can wait for the frame instead of sleeping on it. */
#include "logit.h"

/* The probe is not in the agent catalogue, so it links no libc; the gfx objects
 * it carries (GFX_OBJ) still reference memcpy/memset. rep-string forms cannot be
 * rewritten by the optimiser into a call to themselves. */
void *memcpy(void *d, const void *s, unsigned long n)
{ void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r; }
void *memset(void *d, int c, unsigned long n)
{ void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }

#define PW 400
#define PH 300
#define CORNER 40            /* points: larger than the corner radius at 300% */

static unsigned seq;
static void done(char what)
{
    char b[40]; int n = 0;
    const char *p = "ROUND DONE ";
    while (*p) b[n++] = *p++;
    char d[12]; int k = 0; unsigned v = ++seq;
    do { d[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k) b[n++] = d[--k];
    b[n++] = ' '; b[n++] = what; b[n++] = '\n';
    sys_write(1, b, n);
}
static void fill(unsigned color, char what)
{
    gui_clear(color);
    gui_flush();
    done(what);
}
void app_main(void)
{
    gui_create("Round Probe", PW, PH);
    fill(rgb(255, 0, 0), 'r');
    struct logit_event e;
    for (;;) {
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) app_exit(0);
            if (e.type != EV_KEY) continue;
            if (e.a == 'r') fill(rgb(255, 0, 0), 'r');
            else if (e.a == 'b') fill(rgb(0, 0, 255), 'b');
            else if (e.a == 'k') {
                gui_rect(0, PH - CORNER, CORNER, CORNER, rgb(255, 0, 0));
                gui_rect(PW - CORNER, PH - CORNER, CORNER, CORNER, rgb(255, 0, 0));
                gui_flush_rect(0, PH - CORNER, CORNER, CORNER);
                gui_flush_rect(PW - CORNER, PH - CORNER, CORNER, CORNER);
                done('k');
            }
        }
        wait_idle(0);
    }
}
