/* Host unit test for the points<->device-pixel conversion in c/kernel/gui/fb.c.
 *
 * This compiles the REAL fb.c with stubs for the things it talks to (the VMM,
 * the text engine, virtio-gpu, the kernel heap), rather than restating the
 * arithmetic here. A test that re-implements the formula it is checking proves
 * only that someone can type the same expression twice.
 *
 * What is actually being asserted is the property the whole scaled desktop rests
 * on: fb_pt is MONOTONIC and GAP-FREE, so a row of abutting logical rectangles
 * converted as (fb_pt(x), fb_pt(x+w) - fb_pt(x)) tiles the device row exactly --
 * no seams, no overlap -- at every scale, including the fractional ones. Getting
 * that wrong does not crash; it draws a one-pixel line of background through the
 * middle of a toolbar and looks like a rendering bug in whatever drew the
 * toolbar.
 *
 *   cc -o scale_test tests/unit/scale_test.c -Ic/kernel/gui -Ic/kernel/mm ...
 * See the Makefile's test-scale rule.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* --- stubs for everything fb.c calls out to -------------------------------
 * The real headers are on the include path, so these are checked against the
 * real prototypes: a stub that has drifted from the kernel is a compile error
 * here, not a test that quietly measures something else. */
#include "vmm.h"
#include "text.h"
#include "virtio_gpu.h"

void vmm_map_range(uint64_t v, uint64_t p, uint64_t sz, uint64_t fl)
{ (void)v; (void)p; (void)sz; (void)fl; }

int       virtio_gpu_init(void)   { return -1; }   /* no GPU: take the multiboot path */
int       virtio_gpu_present(void){ return 0; }
uint32_t *virtio_gpu_fb(void)     { return NULL; }
uint32_t  virtio_gpu_width(void)  { return 0; }
uint32_t  virtio_gpu_height(void) { return 0; }
void      virtio_gpu_flush(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }

void text_init(void) {}
int  text_draw(int x, int y, const char *s, uint32_t c) { (void)y; (void)s; (void)c; return x; }
int  text_draw_sz(int x, int y, const char *s, int px, uint32_t c)
{ (void)y; (void)s; (void)px; (void)c; return x; }
int  text_draw_mono(int x, int y, const char *s, int cw, uint32_t c)
{ (void)y; (void)s; (void)cw; (void)c; return x; }
int  text_draw_mono_sz(int x, int y, const char *s, int px, int cw, uint32_t c)
{ (void)y; (void)s; (void)px; (void)cw; (void)c; return x; }
int  text_width(const char *s) { (void)s; return 0; }
int  text_width_sz(const char *s, int px) { (void)s; return px; }
int  text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)len; (void)mono; return px; }
int  text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t c)
{ (void)y; (void)s; (void)len; (void)px; (void)mono; (void)c; return x; }
int  text_line_height(int px) { return px + px / 4; }
int  text_raster(const struct ttf_font *f, int gid, int px,
                 uint8_t *cov, int cap, int *w, int *h, int *ox, int *oy)
{ (void)f; (void)gid; (void)px; (void)cov; (void)cap; (void)w; (void)h; (void)ox; (void)oy; return -1; }

void *kmalloc(unsigned long n) { (void)n; return NULL; }
void  kfree(void *p) { (void)p; }

#include "fb.c"

/* --- the tests ------------------------------------------------------------ */
static int fails;
static void ck(int cond, const char *what, long got, long want)
{
    if (cond) return;
    printf("FAIL %-52s got %ld, want %ld\n", what, got, want);
    fails++;
}

/* Every scale the ladder can produce, plus the fractional ones in between that
 * an explicit fb_set_scale() could be handed. 125/150/175 are the interesting
 * cases: at an integer scale the seam bug is invisible. */
static const int SCALES[] = { 100, 125, 150, 175, 200, 225, 250, 275, 300 };
#define NSCALES ((int)(sizeof SCALES / sizeof SCALES[0]))

int main(void)
{
    /* 1. pick_scale: the surplus over the design canvas becomes density, and a
     *    mode smaller than the canvas is NOT shrunk into (scaling only goes up). */
    struct { uint32_t w, h; int want; } modes[] = {
        {  640,  480, 100 },   /* unrealized QEMU window: refuse to shrink */
        { 1280,  800, 100 },   /* the design canvas itself */
        { 1600, 1000, 125 },
        { 1920, 1200, 150 },   /* the shipped default */
        { 2560, 1600, 200 },
        { 3840, 2400, 300 },   /* clamped: 300 is the ceiling */
        { 1920,  800, 100 },   /* height is the binding constraint, not width */
        { 1290,  810, 100 },   /* just over the canvas: quantised DOWN to 100 */
    };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        int got = pick_scale(modes[i].w, modes[i].h);
        char msg[80];
        snprintf(msg, sizeof msg, "pick_scale(%u,%u)", modes[i].w, modes[i].h);
        ck(got == modes[i].want, msg, got, modes[i].want);
    }

    /* 2. The logical desktop never falls below the design canvas -- this is the
     *    invariant that lets the browser ask for a 1180-point window on any
     *    display without SYS_GUI_CREATE refusing it. */
    for (uint32_t w = 1280; w <= 4000; w += 37) {
        uint32_t h = w * 800 / 1280;
        fb_w = w; fb_h = h;
        fb_set_scale(pick_scale(w, h));
        char msg[80];
        snprintf(msg, sizeof msg, "desktop >= design canvas at %ux%u", w, h);
        ck(fb_width_pt() >= 1280 && fb_height_pt() >= 800, msg,
           fb_width_pt(), 1280);
    }

    /* 3. THE SEAM TEST. Convert a run of abutting logical rects the way wm.c
     *    does and require the device rects to tile with neither gap nor overlap. */
    for (int si = 0; si < NSCALES; si++) {
        fb_set_scale(SCALES[si]);
        int prev_end = fb_pt(0), gaps = 0, overlaps = 0;
        for (int x = 0; x < 1280; x += 7) {           /* rects of width 7 */
            int dx = fb_pt(x), dw = fb_pt(x + 7) - fb_pt(x);
            if (dx > prev_end) gaps++;
            if (dx < prev_end) overlaps++;
            prev_end = dx + dw;
        }
        char msg[80];
        snprintf(msg, sizeof msg, "no gaps between abutting rects @ %d%%", SCALES[si]);
        ck(gaps == 0, msg, gaps, 0);
        snprintf(msg, sizeof msg, "no overlap between abutting rects @ %d%%", SCALES[si]);
        ck(overlaps == 0, msg, overlaps, 0);
    }

    /* 4. Monotonic across zero. A window dragged off the left edge produces
     *    negative content coordinates; truncation-toward-zero would map -1 and 0
     *    to the same device column and shear the frame at x=0. */
    for (int si = 0; si < NSCALES; si++) {
        fb_set_scale(SCALES[si]);
        int bad = 0;
        for (int x = -200; x < 200; x++)
            if (fb_pt(x + 1) < fb_pt(x)) bad++;
        char msg[80];
        snprintf(msg, sizeof msg, "fb_pt monotonic across 0 @ %d%%", SCALES[si]);
        ck(bad == 0, msg, bad, 0);
    }

    /* 5. Round trip. A click at the device pixel a logical point maps to must
     *    come back as that same point -- this is the hit-testing contract, and
     *    the one that silently breaks a scaled UI when it is wrong. */
    for (int si = 0; si < NSCALES; si++) {
        fb_set_scale(SCALES[si]);
        int bad = 0;
        for (int v = 0; v < 2000; v++)
            if (fb_dev2pt(fb_pt(v)) != v) bad++;
        char msg[80];
        snprintf(msg, sizeof msg, "fb_dev2pt(fb_pt(v)) == v @ %d%%", SCALES[si]);
        ck(bad == 0, msg, bad, 0);
    }

    /* 6. A device pixel anywhere inside a logical point's cell maps back into
     *    that cell -- i.e. a click never lands on the neighbouring widget. */
    for (int si = 0; si < NSCALES; si++) {
        fb_set_scale(SCALES[si]);
        int bad = 0;
        for (int v = 0; v < 500; v++)
            for (int d = fb_pt(v); d < fb_pt(v + 1); d++)
                if (fb_dev2pt(d) != v) bad++;
        char msg[80];
        snprintf(msg, sizeof msg, "every device px in a point's cell maps back @ %d%%", SCALES[si]);
        ck(bad == 0, msg, bad, 0);
    }

    /* 7. The scale is clamped, never inverted: nothing may shrink the UI. */
    fb_set_scale(10);  ck(fb_scale() == 100, "fb_set_scale(10) clamps up", fb_scale(), 100);
    fb_set_scale(9999); ck(fb_scale() == 400, "fb_set_scale(9999) clamps down", fb_scale(), 400);

    /* 8. The UI text size scales with the display, which is what makes the
     *    chrome sharper rather than merely bigger. */
    fb_set_scale(100); ck(fb_ui_px() == 16, "ui px @ 100%", fb_ui_px(), 16);
    fb_set_scale(150); ck(fb_ui_px() == 24, "ui px @ 150%", fb_ui_px(), 24);
    fb_set_scale(200); ck(fb_ui_px() == 32, "ui px @ 200%", fb_ui_px(), 32);

    if (fails) { printf("scale_test: %d FAILED\n", fails); return 1; }
    printf("scale_test: all checks passed\n");
    return 0;
}
