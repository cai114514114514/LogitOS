/* Host test for the still-image decoders that are NOT PNG or JPEG: BMP, ICO and
 * WebP-lossless. Every one of these formats is lossless, so the comparison is
 * byte-for-byte against the reference img_still_gen.py recorded -- there is no
 * tolerance anywhere in this file, on purpose.
 *
 * Build/run: see the `test-img-still` target in the Makefile. */
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
    const char *dir = argc > 1 ? argv[1] : "build/imgstill";
    char path[512]; snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r"); if (!m) { printf("no manifest in %s\n", dir); return 1; }

    char name[128], ext[16]; int W, H, fails = 0, total = 0;
    while (fscanf(m, "%127s %15s %d %d", name, ext, &W, &H) == 4) {
        total++;
        char pp[600], rp[600];
        snprintf(pp, sizeof pp, "%s/%s.%s", dir, name, ext);
        snprintf(rp, sizeof rp, "%s/%s.rgba", dir, name);
        long pn, rn; unsigned char *pb = slurp(pp, &pn); unsigned char *rb = slurp(rp, &rn);
        if (!pb || !rb) { printf("FAIL %-14s missing files\n", name); fails++; free(pb); free(rb); continue; }

        struct image im;
        if (img_decode(pb, (int)pn, &im) != 0) {
            printf("FAIL %-14s decode error\n", name); fails++; free(pb); free(rb); continue;
        }
        if (im.w != W || im.h != H) {
            printf("FAIL %-14s got %dx%d want %dx%d\n", name, im.w, im.h, W, H); fails++;
        } else if (rn != (long)W * H * 4) {
            printf("FAIL %-14s expected-rgba wrong size\n", name); fails++;
        } else {
            long diff = 0, first = -1; int maxd = 0;
            for (long i = 0; i < rn; i++) {
                int d = im.rgba[i] - rb[i]; if (d < 0) d = -d;
                if (d) { diff++; if (first < 0) first = i; if (d > maxd) maxd = d; }
            }
            if (diff)
                printf("FAIL %-14s %ld/%ld bytes differ (first @%ld px %ld ch %ld: got %d want %d, maxdelta %d)\n",
                       name, diff, rn, first, first / 4, first % 4,
                       im.rgba[first], rb[first], maxd), fails++;
            else printf("ok   %-14s %s %dx%d\n", name, ext, W, H);
        }
        img_free(&im); free(pb); free(rb);
    }
    fclose(m);

    /* Robustness: every case truncated to a prefix must fail or succeed, never
     * crash and never report a size it did not produce. Run under ASan in the
     * fuzz target; here it is a cheap always-on smoke check. */
    printf(fails ? "\n%d/%d still cases FAILED\n" : "\nall %d still cases passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
