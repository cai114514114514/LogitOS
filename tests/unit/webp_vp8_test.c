/* Lossy WebP (VP8 key frame) against libwebp, byte for byte.
 *
 * The corpus and its .ref files come from tests/unit/webp_vp8_gen.py, which
 * encodes with cwebp and decodes the identical bytes with `dwebp -nofancy`.
 * VP8 reconstruction is exactly specified integer arithmetic, so the assertion
 * is equality -- no tolerance, no mean error. A single differing sample means
 * a rule was misread, and the count and position of the differences is printed
 * because "the first mismatch moved" says nothing about whether a change
 * helped.
 *
 * Usage: webp_vp8_test <corpus-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int cases = 0, failures = 0;

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n ? (size_t)n : 1);
    if (!b || (n && fread(b, 1, (size_t)n, f) != (size_t)n)) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

static void run(const char *dir, const char *name, int w, int h)
{
    char pw[512], pr[512];
    snprintf(pw, sizeof pw, "%s/%s.webp", dir, name);
    snprintf(pr, sizeof pr, "%s/%s.ref", dir, name);

    long nw = 0, nr = 0;
    uint8_t *wb = slurp(pw, &nw), *rb = slurp(pr, &nr);
    cases++;
    if (!wb || !rb) {
        printf("FAIL %-14s missing input\n", name);
        failures++;
        free(wb); free(rb);
        return;
    }

    struct image im;
    memset(&im, 0, sizeof im);
    if (img_decode(wb, (int)nw, &im) != 0) {
        printf("FAIL %-14s decode returned -1\n", name);
        failures++;
        free(wb); free(rb);
        return;
    }
    if (im.w != w || im.h != h) {
        printf("FAIL %-14s size %dx%d, want %dx%d\n", name, im.w, im.h, w, h);
        failures++;
        goto done;
    }
    if (nr != (long)w * h * 4) {
        printf("FAIL %-14s reference is %ld bytes, want %ld\n", name, nr, (long)w * h * 4);
        failures++;
        goto done;
    }
    {
        long bad = 0, first = -1;
        int maxd = 0;
        for (long i = 0; i < nr; i++) {
            int d = im.rgba[i] - rb[i];
            if (d) {
                if (first < 0) first = i;
                bad++;
                if (d < 0) d = -d;
                if (d > maxd) maxd = d;
            }
        }
        if (bad) {
            printf("FAIL %-14s %ld/%ld samples differ, maxdelta %d, first at "
                   "px (%ld,%ld) ch %ld\n",
                   name, bad, nr, maxd, (first / 4) % w, (first / 4) / w, first % 4);
            failures++;
        } else {
            printf("ok   %-14s %4dx%-4d exact (%ld samples)\n", name, w, h, nr);
        }
    }
done:
    kfree(im.rgba);
    free(wb);
    free(rb);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/webpvp8";
    char mf[512];
    snprintf(mf, sizeof mf, "%s/manifest.txt", dir);
    FILE *f = fopen(mf, "r");
    if (!f) { printf("cannot open %s -- run webp_vp8_gen.py first\n", mf); return 2; }

    char name[256];
    int w, h;
    while (fscanf(f, "%255s %d %d", name, &w, &h) == 3)
        run(dir, name, w, h);
    fclose(f);

    printf("\n%d lossy-WebP cases, %d failed\n", cases, failures);
    if (!cases) { printf("NO CASES -- the corpus is empty\n"); return 2; }
    return failures ? 1 : 0;
}
