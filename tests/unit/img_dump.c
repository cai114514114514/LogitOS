/* img_dump -- write out what the image decoders produce, frame by frame.
 *
 * WHO WANTS THIS AND WHY IT IS NOT A DECODER TEST
 * tests/qmp/qmp_preview.py compares Preview's WINDOW against a decode of the
 * same fixture bytes. For stills its reference is PIL, which is independent
 * code and is already the reference test-png / test-img-still / test-img-exif
 * hold the decoders to.
 *
 * For ANIMATIONS PIL is not a usable reference, and the reason is a real
 * disagreement rather than a rounding one: GIF disposal 2 and APNG
 * DISPOSE_OP_BACKGROUND mean "restore the frame's rectangle to TRANSPARENT",
 * which is what browsers do and what c/lib/image/img.h says this system does
 * on purpose -- while PIL restores it to the background COLOUR. On the
 * committed tests/fixtures/image/anim.gif that is 108 pixels of alpha per
 * frame, so a screen comparison against PIL would fail on frames the guest got
 * right. So the animated comparison uses the HOST BUILD of the same decoder,
 * which is exactly the comparison make test-imgcheck already makes between the
 * host and the guest; what pins the decoder itself against PIL is
 * make test-img-anim.
 *
 * Usage: img_dump <file> <outdir> <name>
 * Writes <outdir>/<name>.meta ("w h nframes loops" then a delay per frame) and
 * <outdir>/<name>.<k>.rgba for every frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: img_dump <file> <outdir> <name>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "img_dump: cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)(n > 0 ? n : 1));
    if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)) {
        fprintf(stderr, "img_dump: short read on %s\n", argv[1]); return 1;
    }
    fclose(f);

    struct img_anim a;
    if (img_decode_anim(b, (int)n, &a) != 0) {
        fprintf(stderr, "img_dump: no decoder claimed %s\n", argv[1]);
        return 1;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/%s.meta", argv[2], argv[3]);
    FILE *m = fopen(path, "w");
    if (!m) { fprintf(stderr, "img_dump: cannot write %s\n", path); return 1; }
    fprintf(m, "%d %d %d %d\n", a.w, a.h, a.nframes, a.loops);
    for (int k = 0; k < a.nframes; k++) fprintf(m, "%d\n", a.frames[k].delay_ms);
    fclose(m);

    for (int k = 0; k < a.nframes; k++) {
        snprintf(path, sizeof path, "%s/%s.%d.rgba", argv[2], argv[3], k);
        FILE *o = fopen(path, "wb");
        if (!o) { fprintf(stderr, "img_dump: cannot write %s\n", path); return 1; }
        fwrite(a.frames[k].rgba, 1, (size_t)a.w * (size_t)a.h * 4, o);
        fclose(o);
    }
    printf("img_dump %s -> %s/%s  %dx%d %d frame(s) loops=%d\n",
           argv[1], argv[2], argv[3], a.w, a.h, a.nframes, a.loops);
    img_anim_free(&a);
    free(b);
    return 0;
}
