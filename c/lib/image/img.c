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
