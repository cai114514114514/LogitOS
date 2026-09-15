/* SPDX-License-Identifier: MIT
 * Real AUI consumer for scanout/input acceptance. No replacement rasterizer,
 * clock or interpolation here: the same aui.o used by Gallery draws every row.
 */
#include "aui.h"
static int left, outer, inner, short_content, show_nested = 1;
static int tiny;
static unsigned frames;
static void number(char *b, int *n, unsigned v)
{
    char r[12]; int k = 0;
    do { r[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k) b[(*n)++] = r[--k];
}
static void word(char *b, int *n, const char *s)
{ while (*s) b[(*n)++] = *s++; }
static void report(void)
{
    char b[200]; int n = 0;
    word(b,&n,"SCROLL FRAME n="); number(b,&n,++frames);
    word(b,&n," left="); number(b,&n,left);
    word(b,&n," outer="); number(b,&n,outer);
    word(b,&n," inner="); number(b,&n,inner);
    word(b,&n," active="); number(b,&n,aui_anim_active());
    word(b,&n," ms="); number(b,&n,aui_ms());
    b[n++] = '\n'; sys_write(1,b,n);
}
static void rows(int w, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned color = rgb(40 + i * 9, 100 + i * 5, 190 - i * 5);
        aui_fill(0, i * 48, w, 48, color);
        char b[16]; int p = 0; word(b,&p,"Row "); number(b,&p,i); b[p]=0;
        aui_label(24,i*48+12,b,0xffffff);
    }
}
static void frame(void)
{
    aui_begin(AUI_BG);
    aui_heading(20,12,"OpenLogit scrolling",AUI_TEXT);
    aui_label(20,42,"Wheel / drag. Home resets. C changes content. H hides nesting.",AUI_MUTED);
    aui_scroll_begin(20,70,250,220,&left,short_content ? 96 : 960);
    rows(250,short_content ? 2 : 20);
    aui_scroll_end();
    if (show_nested) {
        aui_scroll_begin(320,70,240,220,&outer,600);
        aui_fill(0,0,240,600,0x263445);
        aui_scroll_begin(8,40,205,140,&inner,600);
        rows(205,13);
        aui_scroll_end();
        aui_scroll_end();
    }
    aui_scroll_begin(320,302,120,3,&tiny,20);
    aui_fill(0,0,120,20,0xe83ba7);
    aui_scroll_end();
    aui_label(20,312,"Shared SDK sampling; independent containers",AUI_MUTED);
    aui_end(); report();
}
void app_main(void)
{
    gui_create("OpenLogit Scroll",600,350);
    aui_set_size(600,350); frame();
    struct logit_event e;
    for (;;) {
        int drew = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) app_exit(0);
            if (e.type == EV_KEY) {
                if (e.a == KEY_HOME) left = outer = inner = 0;
                if (e.a == 'c' || e.a == 'C') short_content = !short_content;
                if (e.a == 'h' || e.a == 'H') show_nested = !show_nested;
            }
            aui_feed(&e);
            if (aui_want_repaint()) { frame(); drew = 1; }
            aui_feed_done();
        }
        if (!drew && aui_anim_due()) frame();
        wait_idle(aui_anim_wait());
    }
}
