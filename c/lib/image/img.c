#include "img.h"
#include "kheap.h"

void kfree(void *);

#define MAXDEC 8
static img_detect_fn dets[MAXDEC];
static img_decode_fn decs[MAXDEC];
static int ndec;
static int inited;

void img_register(img_detect_fn detect, img_decode_fn decode)
{
    if (ndec < MAXDEC) { dets[ndec] = detect; decs[ndec] = decode; ndec++; }
}

void img_init(void)
{
    if (inited) return;
    inited = 1;
    png_register();
    gif_register();
    jpeg_register();
    svg_register();
}

int img_decode(const uint8_t *p, int n, struct image *out)
{
    img_init();
    for (int i = 0; i < ndec; i++)
        if (dets[i](p, n)) return decs[i](p, n, out);
    return -1;                                         /* no decoder claimed it */
}

void img_free(struct image *im)
{
    if (im && im->rgba) { kfree(im->rgba); im->rgba = 0; }
    if (im) { im->w = im->h = 0; }
}
