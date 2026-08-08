/* Host test for the ANIMATED image decoders (GIF, APNG).
 *
 * The assertions are, per case: the frame COUNT, the loop count, and for every
 * frame both its DELAY in milliseconds and its fully composited canvas,
 * byte-for-byte. Composited, because disposal is the part naive decoders get
 * wrong and it is invisible in a frame count -- a decoder that ignores
 * "restore to background" produces exactly the right number of frames, each of
 * which is wrong in the rectangle the previous frame occupied.
 *
 * Both formats are lossless, so there is no tolerance here.
 *
 * Also checks that img_decode (the STILL entry point) on an animated file
 * returns frame 0 at the LOGICAL SCREEN size -- the old GIF decoder returned
 * the first image descriptor's sub-rectangle, which for a real animation is
 * both the wrong size and the wrong picture. */
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

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("     FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/imganim";
    char path[512]; snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r"); if (!m) { printf("no manifest in %s\n", dir); return 1; }

    char name[128], ext[16]; int W, H, NF, LOOPS, total = 0;
    while (fscanf(m, "%127s %15s %d %d %d %d", name, ext, &W, &H, &NF, &LOOPS) == 6) {
        total++;
        char pp[600];
        snprintf(pp, sizeof pp, "%s/%s.%s", dir, name, ext);
        long pn; unsigned char *pb = slurp(pp, &pn);
        if (!pb) { printf("FAIL %-18s missing %s\n", name, pp); fails++; continue; }

        /* expected delays */
        snprintf(pp, sizeof pp, "%s/%s.frames", dir, name);
        FILE *df = fopen(pp, "r");
        int *want_delay = calloc(NF ? NF : 1, sizeof(int));
        for (int i = 0; i < NF && df; i++) if (fscanf(df, "%d", &want_delay[i]) != 1) break;
        if (df) fclose(df);

        struct img_anim a;
        if (img_decode_anim(pb, (int)pn, &a) != 0) {
            printf("FAIL %-18s anim decode error\n", name); fails++; free(pb); free(want_delay); continue;
        }
        printf("     %-18s %s %dx%d %d frames loops=%d\n", name, ext, a.w, a.h, a.nframes, a.loops);
        CHECK(a.w == W && a.h == H, "%s canvas %dx%d want %dx%d", name, a.w, a.h, W, H);
        CHECK(a.nframes == NF, "%s nframes %d want %d", name, a.nframes, NF);
        CHECK(a.loops == LOOPS, "%s loops %d want %d", name, a.loops, LOOPS);

        int nf = a.nframes < NF ? a.nframes : NF;
        for (int k = 0; k < nf; k++) {
            CHECK(a.frames[k].delay_ms == want_delay[k],
                  "%s frame %d delay %dms want %dms", name, k, a.frames[k].delay_ms, want_delay[k]);
            char fp[640]; snprintf(fp, sizeof fp, "%s/%s.%d.rgba", dir, name, k);
            long rn; unsigned char *rb = slurp(fp, &rn);
            if (!rb) { printf("     FAIL %s frame %d expectation missing\n", name, k); fails++; checks++; continue; }
            checks++;
            if (rn != (long)a.w * a.h * 4) {
                printf("     FAIL %s frame %d expectation size %ld\n", name, k, rn); fails++;
            } else {
                long diff = 0, first = -1;
                for (long i = 0; i < rn; i++)
                    if (a.frames[k].rgba[i] != rb[i]) { diff++; if (first < 0) first = i; }
                if (diff)
                    printf("     FAIL %s frame %d: %ld/%ld bytes differ (first px %ld ch %ld: got %d want %d)\n",
                           name, k, diff, rn, first / 4, first % 4,
                           a.frames[k].rgba[first], rb[first]), fails++;
            }
            free(rb);
        }
        img_anim_free(&a);

        /* the still entry point must yield frame 0 at the canvas size */
        char f0[640]; snprintf(f0, sizeof f0, "%s/%s.0.rgba", dir, name);
        long rn0; unsigned char *rb0 = slurp(f0, &rn0);
        struct image im;
        checks++;
        if (img_decode(pb, (int)pn, &im) != 0) {
            printf("     FAIL %s still decode error\n", name); fails++;
        } else {
            if (im.w != W || im.h != H) {
                printf("     FAIL %s still %dx%d want canvas %dx%d\n", name, im.w, im.h, W, H); fails++;
            } else if (rb0 && rn0 == (long)W * H * 4 && memcmp(im.rgba, rb0, rn0) != 0) {
                long d = 0; for (long i = 0; i < rn0; i++) if (im.rgba[i] != rb0[i]) d++;
                printf("     FAIL %s still != frame 0 (%ld bytes)\n", name, d); fails++;
            }
            img_free(&im);
        }
        free(rb0);
        free(pb); free(want_delay);
    }
    fclose(m);
    printf(fails ? "\n%d of %d animation assertions FAILED (%d cases)\n"
                 : "\nall %d animation assertions passed (%d cases)\n",
           fails ? fails : checks, checks, total);
    return fails ? 1 : 0;
}
