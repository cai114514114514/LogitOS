/* Host test for the from-scratch baseline JPEG decoder (src/lib/image/jpeg.c).
 *
 * JPEG is lossy, so correctness is proven by feeding the IDENTICAL .jpg bytes to
 * BOTH our decoder and libjpeg `djpeg` (-nosmooth -dct int, see jpeg_gen.py) and
 * asserting the two RGB outputs agree to within a tight per-channel tolerance.
 * fail-mode cases (progressive, CMYK) must be REJECTED (img_decode != 0).
 *
 * Build: cc -O2 -o /tmp/jpeg_test tools/t/jpeg_test.c \
 *           src/lib/image/{img,png,gif,jpeg,inflate}.c -Isrc/lib/image -Isrc/kernel/mm
 * Run:   python3 tools/t/jpeg_gen.py /tmp/jpegtest && /tmp/jpeg_test /tmp/jpegtest */
#include <stdio.h>
#include <stdlib.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* Tolerances vs the djpeg reference: same IDCT + box upsampling means the only
 * differences are colour-conversion rounding. Keep them tight. */
#define MAX_TOL  3      /* max per-channel abs diff */
#define MEAN_TOL 0.5    /* mean per-channel abs diff */

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n); if (b) { if (fread(b, 1, *n, f) != (size_t)*n) { free(b); b = 0; } }
    fclose(f); return b;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/jpegtest";
    char path[512]; snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r"); if (!m) { printf("no manifest in %s\n", dir); return 1; }

    char name[128], mode[16]; int W, H, fails = 0, total = 0;
    while (fscanf(m, "%127s %d %d %15s", name, &W, &H, mode) == 4) {
        total++;
        char jp[512]; snprintf(jp, sizeof jp, "%s/%s.jpg", dir, name);
        long jn; unsigned char *jb = slurp(jp, &jn);
        if (!jb) { printf("FAIL %-14s missing .jpg\n", name); fails++; continue; }

        struct image im;
        int rc = img_decode(jb, (int)jn, &im);

        if (mode[0] == 'f') {            /* fail-expected: must be rejected */
            if (rc == 0) { printf("FAIL %-14s decoded an UNSUPPORTED file (should reject)\n", name); fails++; img_free(&im); }
            else printf("ok   %-14s rejected (graceful)\n", name);
            free(jb); continue;
        }

        /* ok-mode: compare against djpeg reference RGBA. */
        char rp[512]; snprintf(rp, sizeof rp, "%s/%s.ref", dir, name);
        long rn; unsigned char *rb = slurp(rp, &rn);
        if (!rb) { printf("FAIL %-14s missing .ref\n", name); fails++; free(jb); continue; }

        if (rc != 0) { printf("FAIL %-14s decode error\n", name); fails++; free(jb); free(rb); continue; }
        if (im.w != W || im.h != H) {
            printf("FAIL %-14s got %dx%d want %dx%d\n", name, im.w, im.h, W, H);
            fails++; img_free(&im); free(jb); free(rb); continue;
        }
        if (rn != (long)W * H * 4) {
            printf("FAIL %-14s reference rgba wrong size\n", name);
            fails++; img_free(&im); free(jb); free(rb); continue;
        }

        long sum = 0; int maxd = 0; int worst = -1;
        for (long px = 0; px < (long)W * H; px++) {
            for (int c = 0; c < 3; c++) {        /* RGB only; alpha is always 255 */
                int a = im.rgba[px * 4 + c], b = rb[px * 4 + c];
                int d = a > b ? a - b : b - a;
                sum += d;
                if (d > maxd) { maxd = d; worst = (int)px; }
            }
        }
        double mean = (double)sum / ((double)W * H * 3);
        if (maxd > MAX_TOL || mean > MEAN_TOL) {
            printf("FAIL %-14s %dx%d  maxd=%d mean=%.3f (px %d) -- exceeds tol\n",
                   name, W, H, maxd, mean, worst);
            fails++;
        } else {
            printf("ok   %-14s %dx%d  maxd=%d mean=%.3f\n", name, W, H, maxd, mean);
        }
        img_free(&im); free(jb); free(rb);
    }
    fclose(m);
    printf(fails ? "\n%d/%d JPEG cases FAILED\n" : "\nall %d JPEG cases passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
