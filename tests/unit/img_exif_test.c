/* Host test for EXIF orientation.
 *
 * Two assertion modes, chosen per case by the generator:
 *   exact   -- lossless carrier (PNG eXIf / WebP EXIF). The decoded, oriented
 *              RGBA must equal PIL's ImageOps.exif_transpose of the identical
 *              file, byte for byte. This is what pins the eight transforms.
 *   tagonly -- JPEG APP1. JPEG is lossy, so the pixels are not compared with
 *              another decoder; what is asserted is the orientation VALUE the
 *              parser recovers and the resulting dimensions. The transform
 *              itself is the same code the exact cases already proved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (b && *n && fread(b, 1, *n, f) != (size_t)*n) { free(b); b = 0; }
    fclose(f); return b;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/imgexif";
    char path[512]; snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r"); if (!m) { printf("no manifest in %s\n", dir); return 1; }

    char name[128], ext[16], mode[16]; int O, W, H, fails = 0, total = 0;
    while (fscanf(m, "%127s %15s %d %d %d %15s", name, ext, &O, &W, &H, mode) == 6) {
        total++;
        char pp[600];
        snprintf(pp, sizeof pp, "%s/%s.%s", dir, name, ext);
        long pn; unsigned char *pb = slurp(pp, &pn);
        if (!pb) { printf("FAIL %-14s missing %s\n", name, pp); fails++; continue; }

        int got_o = exif_orientation(pb, (int)pn);
        if (got_o != O) { printf("FAIL %-14s orientation %d want %d\n", name, got_o, O); fails++; }

        struct image im;
        if (img_decode(pb, (int)pn, &im) != 0) {
            printf("FAIL %-14s decode error\n", name); fails++; free(pb); continue;
        }
        if (im.w != W || im.h != H) {
            printf("FAIL %-14s oriented size %dx%d want %dx%d\n", name, im.w, im.h, W, H);
            fails++;
        } else if (strcmp(mode, "exact") == 0) {
            char rp[600]; snprintf(rp, sizeof rp, "%s/%s.rgba", dir, name);
            long rn; unsigned char *rb = slurp(rp, &rn);
            if (!rb || rn != (long)W * H * 4) {
                printf("FAIL %-14s expectation missing/short\n", name); fails++;
            } else {
                long diff = 0, first = -1;
                for (long i = 0; i < rn; i++)
                    if (im.rgba[i] != rb[i]) { diff++; if (first < 0) first = i; }
                if (diff) {
                    printf("FAIL %-14s %ld/%ld bytes differ (first px %ld ch %ld: got %d want %d)\n",
                           name, diff, rn, first / 4, first % 4, im.rgba[first], rb[first]);
                    fails++;
                } else printf("ok   %-14s orient=%d %dx%d exact\n", name, O, W, H);
            }
            free(rb);
        } else {
            printf("ok   %-14s orient=%d %dx%d (tag+geometry)\n", name, O, W, H);
        }

        /* The ANIMATED entry point must orient too. It is a separate code path
         * and it silently did not, which showed up as a portrait JPEG coming
         * back landscape from img_decode_anim while img_decode had it right. */
        struct img_anim an;
        if (img_decode_anim(pb, (int)pn, &an) != 0) {
            printf("FAIL %-14s anim decode error\n", name); fails++;
        } else {
            if (an.w != W || an.h != H) {
                printf("FAIL %-14s anim size %dx%d want %dx%d\n", name, an.w, an.h, W, H);
                fails++;
            } else if (an.nframes >= 1 && im.w == W && im.h == H &&
                       memcmp(an.frames[0].rgba, im.rgba, (long)W * H * 4) != 0) {
                printf("FAIL %-14s anim frame 0 != still\n", name); fails++;
            }
            img_anim_free(&an);
        }
        img_free(&im); free(pb);
    }
    fclose(m);
    printf(fails ? "\n%d/%d EXIF cases FAILED\n" : "\nall %d EXIF cases passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
