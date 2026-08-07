/* Preview: opens a file and shows what is in it -- a still image or a video.
 *
 * The two halves are deliberately asymmetric, and it is worth saying why.
 *
 * Images are decoded by the KERNEL (SYS_IMG_DECODE): a picture is decoded once
 * and thrown away, so a syscall that hands back RGBA is a fair trade and the
 * codecs already live in c/lib/image.
 *
 * Video is decoded HERE, in ring 3, from c/lib/video. A video is decoded thirty
 * times a second and carries megabytes of reference frames between calls; doing
 * that in the kernel would hold the big lock for the whole of every frame and
 * put a few thousand lines of untrusted-input parser in ring 0. It is also the
 * direction M17 already took the browser's render pipeline: the kernel provides
 * primitives, the app does the work. So this app links the H.264 decoder and
 * mini-libc, and only asks the kernel to put pixels on the screen.
 *
 * Which half runs is decided by sniffing the file, not by its name: an H.264
 * elementary stream starts with an Annex-B start code. A file named .h264 that
 * is really a PNG still opens as a PNG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logit.h"
#include "h264.h"

#define WINW 760
#define WINH 560
#define MAXW 1280            /* up to screen resolution */
#define MAXH 800
static unsigned char rgba[MAXW * MAXH * 4];

/* ------------------------------------------------------------ colour ---- */
static int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* BT.601 studio-swing YUV -> RGB, the conversion the stream is authored for
 * (H.264 carries no colour matrix of its own in these files). Integer only:
 * this app is built -mno-red-zone freestanding and there is no reason to pull
 * in floating point for a per-pixel loop. Chroma is 4:2:0, so each chroma
 * sample covers a 2x2 luma quad -- nearest-neighbour upsampling, which is what
 * a viewer wants and avoids a second full-frame pass. */
static void yuv420_to_rgba(const h264frame *f, unsigned char *dst)
{
    for (int y = 0; y < f->height; y++) {
        const unsigned char *ly = f->y + (long)y * f->stride_y;
        const unsigned char *lu = f->u + (long)(y / 2) * f->stride_c;
        const unsigned char *lv = f->v + (long)(y / 2) * f->stride_c;
        unsigned char *o = dst + (long)y * f->width * 4;
        for (int x = 0; x < f->width; x++) {
            int c = ly[x] - 16;
            int d = lu[x / 2] - 128;
            int e = lv[x / 2] - 128;
            o[0] = (unsigned char)clip8((298 * c + 409 * e + 128) >> 8);
            o[1] = (unsigned char)clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[2] = (unsigned char)clip8((298 * c + 516 * d + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
}

/* ------------------------------------------------------------- layout --- */
/* Aspect-fit w x h into the window, centred; gui_blit scales. */
static void blit_fit(int iw, int ih)
{
    if (iw <= 0 || ih <= 0) return;
    int dw = WINW, dh = (int)((long)WINW * ih / iw);
    if (dh > WINH) { dh = WINH; dw = (int)((long)WINH * iw / ih); }
    gui_blit((WINW - dw) / 2, (WINH - dh) / 2, dw, dh, rgba, iw, ih);
}

/* Returns 1 if the app should quit. Called between frames so a playing video
 * still answers the close button. */
static int pump_events(void)
{
    struct logit_event e;
    while (poll_event(&e)) {
        if (e.type == EV_CLOSE) return 1;
        if (e.type == EV_KEY && e.a == 27) return 1;      /* Esc closes */
    }
    return 0;
}

static void status(const char *s)
{
    gui_text(12, WINH - 6, rgb(150, 150, 158), s);
}

/* --------------------------------------------------------------- file --- */
static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = malloc((unsigned long)n);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (unsigned long)n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out_len = n;
    return b;
}

/* Annex-B start code: 00 00 01 or 00 00 00 01, at the very start of the file. */
static int looks_like_h264(const unsigned char *b, long n)
{
    if (n < 5) return 0;
    if (b[0] == 0 && b[1] == 0 && b[2] == 1) return 1;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) return 1;
    return 0;
}

/* -------------------------------------------------------------- video --- */
/* Decode and display until the stream ends, then start it again. Frames are
 * shown as fast as they decode rather than on a clock: under QEMU's TCG this
 * is well under real time anyway, and a pacing loop that never waits would be
 * decoration. Pacing belongs here once there is hardware that outruns it. */
static void play(const unsigned char *data, long len, const char *name)
{
    char line[96];
    for (;;) {                                   /* loop the clip */
        h264dec *d = h264_open();
        if (!d) { status("out of memory"); return; }
        long off = 0;
        int frames = 0;

        for (;;) {
            h264frame fr;
            int got = 0, used;
            if (off < len) {
                used = h264_decode(d, data + off, (int)(len - off), &fr, &got);
                if (used < 0) {
                    gui_clear(rgb(28, 28, 32));
                    snprintf(line, sizeof line, "%s: corrupt stream at byte %ld", name, off);
                    status(line);
                    gui_flush();
                    h264_close(d);
                    /* Stay up so the message is readable, but keep answering
                     * the close button. */
                    while (!pump_events()) sys_yield();
                    app_exit(0);
                }
                off += used;
                if (!got) {
                    if (pump_events()) { h264_close(d); app_exit(0); }
                    if (off < len) continue;
                }
            }
            if (!got) { if (!h264_flush(d, &fr)) break; }

            if (fr.width > MAXW || fr.height > MAXH) {
                gui_clear(rgb(28, 28, 32));
                snprintf(line, sizeof line, "%dx%d is larger than this viewer handles",
                         fr.width, fr.height);
                status(line);
                gui_flush();
                h264_close(d);
                while (!pump_events()) sys_yield();
                app_exit(0);
            }

            yuv420_to_rgba(&fr, rgba);
            frames++;
            gui_clear(rgb(18, 18, 20));
            blit_fit(fr.width, fr.height);
            snprintf(line, sizeof line, "%s  %dx%d  frame %d", name, fr.width, fr.height, frames);
            status(line);
            gui_flush();
            if (pump_events()) { h264_close(d); app_exit(0); }
        }
        h264_close(d);
        if (frames == 0) { status("no frames in stream"); gui_flush();
                           while (!pump_events()) sys_yield(); app_exit(0); }
    }
}

/* -------------------------------------------------------------- image --- */
static void show_image(const char *path, const char *name)
{
    int iw = 0, ih = 0;
    int ok = img_open(path, rgba, (int)sizeof rgba, &iw, &ih) == 0 && iw > 0 && ih > 0;
    char line[96];
    for (;;) {
        gui_clear(rgb(28, 28, 32));
        if (ok) {
            blit_fit(iw, ih);
            snprintf(line, sizeof line, "%s  %dx%d", name, iw, ih);
            status(line);
        } else {
            gui_text(16, 16, rgb(220, 220, 224), "cannot open this file");
        }
        gui_flush();
        /* A still needs no redraw; block on events instead of spinning. */
        for (;;) {
            if (pump_events()) app_exit(0);
            sys_yield();
        }
    }
}

/* --------------------------------------------------------------- main --- */
static const char *basename_of(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/') s = q + 1;
    return s;
}

void app_main(void)
{
    gui_create("Preview", WINW, WINH);

    char path[128];
    if (!(get_arg(path, sizeof path) > 0 && path[0])) {
        gui_clear(rgb(28, 28, 32));
        gui_text(16, 16, rgb(220, 220, 224), "no file given");
        gui_flush();
        for (;;) { if (pump_events()) app_exit(0); sys_yield(); }
    }
    const char *name = basename_of(path);

    /* Sniff before deciding. Only the video path needs the file in our own
     * memory; the image path lets the kernel do the read as it always has. */
    long len = 0;
    unsigned char *data = read_all(path, &len);
    if (data && looks_like_h264(data, len))
        play(data, len, name);                   /* never returns */
    if (data) free(data);
    show_image(path, name);                      /* never returns */
}
