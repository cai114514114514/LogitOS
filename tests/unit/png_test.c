/* Host test for the PNG decoder: decode every case from png_gen.py and compare,
 * byte-for-byte, against PIL's ground-truth RGBA.
 * Build: cc -o /tmp/png_test tools/t/png_test.c src/lib/image/{img,png,gif,inflate}.c \
 *           -Isrc/lib/image -Isrc/kernel/mm
 * Run:   python3 tools/t/png_gen.py /tmp/pngtest && /tmp/png_test /tmp/pngtest */
#include <stdio.h>
#include <stdlib.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n); if (b) { if (fread(b, 1, *n, f) != (size_t)*n) { free(b); b = 0; } }
    fclose(f); return b;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/pngtest";
    char path[512]; snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r"); if (!m) { printf("no manifest in %s\n", dir); return 1; }

    char name[128]; int W, H, fails = 0, total = 0;
    while (fscanf(m, "%127s %d %d", name, &W, &H) == 3) {
        total++;
        char pp[512], rp[512];
        snprintf(pp, sizeof pp, "%s/%s.png", dir, name);
        snprintf(rp, sizeof rp, "%s/%s.rgba", dir, name);
        long pn, rn; unsigned char *pb = slurp(pp, &pn); unsigned char *rb = slurp(rp, &rn);
        if (!pb || !rb) { printf("FAIL %-12s missing files\n", name); fails++; free(pb); free(rb); continue; }

        struct image im;
        if (img_decode(pb, (int)pn, &im) != 0) { printf("FAIL %-12s decode error\n", name); fails++; free(pb); free(rb); continue; }
        if (im.w != W || im.h != H) { printf("FAIL %-12s got %dx%d want %dx%d\n", name, im.w, im.h, W, H); fails++; }
        else if (rn != (long)W*H*4) { printf("FAIL %-12s expected-rgba wrong size\n", name); fails++; }
        else {
            int diff = 0; long first = -1;
            for (long i = 0; i < rn; i++) if (im.rgba[i] != rb[i]) { diff++; if (first < 0) first = i; }
            if (diff) { printf("FAIL %-12s %d/%ld bytes differ (first @%ld: got %d want %d)\n",
                               name, diff, rn, first, im.rgba[first], rb[first]); fails++; }
            else printf("ok   %-12s %dx%d\n", name, W, H);
        }
        img_free(&im); free(pb); free(rb);
    }
    fclose(m);
    printf(fails ? "\n%d/%d PNG cases FAILED\n" : "\nall %d PNG cases passed\n", fails ? fails : total, total);
    return fails ? 1 : 0;
}
