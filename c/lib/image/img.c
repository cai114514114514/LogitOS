#include "img.h"
#include "kheap.h"

void kfree(void *);
void *kmalloc(unsigned long);

#define MAXDEC 8
static img_detect_fn dets[MAXDEC];
static img_decode_fn decs[MAXDEC];
static img_anim_fn   anis[MAXDEC];
static int ndec;
static int inited;

void img_register_anim(img_detect_fn detect, img_decode_fn decode, img_anim_fn anim)
{
    if (ndec < MAXDEC) { dets[ndec] = detect; decs[ndec] = decode; anis[ndec] = anim; ndec++; }
}

void img_register(img_detect_fn detect, img_decode_fn decode)
{
    img_register_anim(detect, decode, 0);
}

/* WEAK, and this is the one place that needs explaining before touching it.
 * svg.c is no longer in the kernel's C_SRC (2026-08-15, "one rasterizer for
 * every pixel the machine draws" -- see the Makefile's C_SRC comment): it
 * used to carry its own scanline filler and its own double-precision libm,
 * both now gone in favour of building an ordinary gfx_path and handing it to
 * Open Logit, but that rewrite does not make a bytes-in-attacker-shaped-XML
 * parser a thing ring 0 should run, so it stays out. img_init() below is
 * unconditional and compiles into BOTH the kernel and every ring-3 image
 * consumer -- it has no #ifdef to tell which -- so it always calls
 * svg_register(), and the kernel build would fail to LINK without some
 * definition of that symbol. This weak one is that definition: wherever
 * svg.c is also linked (the browser, Preview, imgcheck, the host image
 * tests -- everywhere the Makefile lists svg.c explicitly), its strong
 * svg_register() overrides this and SVG decodes normally. In the kernel,
 * where svg.c is absent, this is what runs: it registers nothing, so
 * img_decode() treats SVG bytes exactly like any other format with no
 * registered decoder -- returns "unsupported", the same path a kernel build
 * already takes for WOFF or AVIF. That is a real, intentional loss of a
 * capability (no kernel caller can decode an .svg any more), not a silent
 * one: see the C_SRC comment for what it costs the wallpaper loader (nothing
 * today -- no on-disk asset and no Settings option ever named an .svg
 * wallpaper) and where the code that would need to change lives. The exact
 * pattern this mirrors is rust_host_shim.c's weak img_register_anim, for the
 * identical reason: a strong definition here would be a `multiple definition
 * of svg_register` in every binary that also links the real one. */
__attribute__((weak)) void svg_register(void) {}

void img_init(void)
{
    if (inited) return;
    inited = 1;
    png_register();
    gif_register();
    jpeg_register();
    svg_register();
    bmp_register();
    ico_register();
    webp_register();
}

int img_decode(const uint8_t *p, int n, struct image *out)
{
    img_init();
    for (int i = 0; i < ndec; i++) {
        if (!dets[i](p, n)) continue;
        int rc = decs[i](p, n, out);
        if (rc != 0) return rc;
        /* Orientation is applied HERE rather than inside each decoder: the tag
         * rides in the container (JPEG APP1, PNG eXIf, WebP EXIF, bare TIFF),
         * not in the pixel codec, and applying it once means no decoder can
         * forget to. Absent/unparseable metadata returns 1 and costs a scan. */
        int o = exif_orientation(p, n);
        if (o > 1 && exif_apply(out, o) != 0) { img_free(out); return -1; }
        return 0;
    }
    return -1;                                         /* no decoder claimed it */
}

int img_decode_anim(const uint8_t *p, int n, struct img_anim *out)
{
    img_init();
    if (!out) return -1;
    out->w = out->h = out->nframes = 0; out->loops = 1; out->frames = 0;
    for (int i = 0; i < ndec; i++) {
        if (!dets[i](p, n)) continue;
        if (anis[i]) {
            if (anis[i](p, n, out) != 0) return -1;
            /* Orientation applies to the animation, frame by frame. Missing
             * this is invisible for GIF/APNG (they carry no EXIF in practice)
             * and wrong for every other format that reaches here through the
             * animated entry point -- which is how a rotated JPEG came back
             * unrotated the first time this path existed. */
            int o = exif_orientation(p, n);
            if (o > 1) {
                for (int k = 0; k < out->nframes; k++) {
                    struct image f = { out->w, out->h, out->frames[k].rgba };
                    if (exif_apply(&f, o) != 0) { img_anim_free(out); return -1; }
                    out->frames[k].rgba = f.rgba;
                    if (k == out->nframes - 1) { out->w = f.w; out->h = f.h; }
                }
            }
            return 0;
        }
        /* Still format: one frame, so callers need no special case. Routed
         * through img_decode so the orientation hook is applied exactly once,
         * in exactly one place. */
        struct image im;
        if (img_decode(p, n, &im) != 0) return -1;
        struct img_frame *f = kmalloc(sizeof *f);
        if (!f) { img_free(&im); return -1; }
        f->delay_ms = 0; f->rgba = im.rgba;
        out->w = im.w; out->h = im.h; out->nframes = 1; out->loops = 1; out->frames = f;
        return 0;
    }
    return -1;
}

void img_free(struct image *im)
{
    if (im && im->rgba) { kfree(im->rgba); im->rgba = 0; }
    if (im) { im->w = im->h = 0; }
}

void img_anim_free(struct img_anim *a)
{
    if (!a) return;
    if (a->frames) {
        for (int i = 0; i < a->nframes; i++)
            if (a->frames[i].rgba) kfree(a->frames[i].rgba);
        kfree(a->frames);
    }
    a->frames = 0; a->nframes = 0; a->w = a->h = 0;
}
