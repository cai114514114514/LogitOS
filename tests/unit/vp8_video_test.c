/* VP8 INTER-frame decoder (rust/src/vp8_inter.rs, feature "vp8-interframe")
 * against ffmpeg's own vp8 decoder, byte for byte.
 *
 * The corpus and its .yuv references come from tests/unit/vp8_video_gen.py,
 * which encodes with ffmpeg/libvpx and decodes the identical IVF bytes with
 * `ffmpeg -vsync 0 ... rawvideo` (see that file's module doc for why -vsync 0
 * is load-bearing and not cosmetic). VP8 reconstruction is exactly specified
 * integer arithmetic, as the key-frame gate (test-webp-vp8) already holds
 * itself to -- no tolerance, no mean error. A single differing sample means a
 * rule was misread.
 *
 * This exercises the code the key-frame gate structurally cannot: motion
 * vector decode, sub-pixel motion compensation (both filter passes), and
 * golden/altref reference bookkeeping including a real invisible (shown=0)
 * alt-ref frame in the altref_hidden case.
 *
 * Usage: vp8_video_test <corpus-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

extern size_t vp8_video_state_size(void);
extern void   vp8_video_init(uint8_t *state);
extern void   vp8_video_free(uint8_t *state);
extern int32_t vp8_video_decode(uint8_t *state, const uint8_t *data, int32_t len);
extern const uint8_t *vp8_video_yuv(const uint8_t *state);
extern int32_t vp8_video_yuv_len(const uint8_t *state);
extern int32_t vp8_video_had_frac_mv(const uint8_t *state);

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

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Returns 1 if the whole case is bit-exact (every shown frame matches every
 * ref byte and the two frame counts agree exactly), 0 otherwise. */
static int run_case(const char *dir, const char *name)
{
    char pi[512], pr[512];
    snprintf(pi, sizeof pi, "%s/%s.ivf", dir, name);
    snprintf(pr, sizeof pr, "%s/%s.yuv", dir, name);

    long ivf_len = 0, ref_len = 0;
    uint8_t *ivf = slurp(pi, &ivf_len);
    uint8_t *ref = slurp(pr, &ref_len);
    cases++;
    if (!ivf || !ref) {
        printf("FAIL %-14s missing input (%s)\n", name, pi);
        failures++;
        free(ivf); free(ref);
        return 0;
    }
    if (ivf_len < 32 || memcmp(ivf, "DKIF", 4) != 0) {
        printf("FAIL %-14s not an IVF file\n", name);
        failures++;
        free(ivf); free(ref);
        return 0;
    }
    uint16_t hdrlen = rd16(ivf + 6);

    uint8_t *state = malloc(vp8_video_state_size());
    vp8_video_init(state);

    long off = hdrlen, ref_off = 0;
    int decoded = 0, shown = 0, mismatched = 0, bytes_wrong = 0;
    int had_error = 0;

    while (off + 12 <= ivf_len) {
        uint32_t fsz = rd32(ivf + off);
        off += 12;
        if (off + (long)fsz > ivf_len) { had_error = 1; break; }
        int32_t r = vp8_video_decode(state, ivf + off, (int32_t)fsz);
        off += fsz;
        decoded++;
        if (r < 0) { had_error = 1; break; }
        if (r == 0) continue; /* hidden altref/golden frame: not shown, no ref bytes to compare */

        shown++;
        int32_t ylen = vp8_video_yuv_len(state);
        const uint8_t *y = vp8_video_yuv(state);
        if (ref_off + ylen > ref_len) { had_error = 1; break; }
        int wrong = 0;
        for (int32_t i = 0; i < ylen; i++) {
            if (y[i] != ref[ref_off + i]) wrong++;
        }
        if (wrong) { mismatched++; bytes_wrong += wrong; }
        ref_off += ylen;
    }

    int frac = vp8_video_had_frac_mv(state);
    vp8_video_free(state);

    int ok = !had_error && mismatched == 0 && ref_off == ref_len && shown > 0;
    if (ok) {
        printf("ok   %-14s %2d decoded, %2d shown, exact, frac_mv=%d\n", name, decoded, shown, frac);
    } else {
        printf("FAIL %-14s decoded=%d shown=%d mismatched_frames=%d bytes_wrong=%d "
               "ref_consumed=%ld/%ld error=%d\n",
               name, decoded, shown, mismatched, bytes_wrong, ref_off, ref_len, had_error);
        failures++;
    }
    free(ivf); free(ref);
    return ok;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/vp8video";
    char mpath[512];
    snprintf(mpath, sizeof mpath, "%s/manifest.txt", dir);
    FILE *m = fopen(mpath, "r");
    if (!m) {
        fprintf(stderr, "vp8_video_test: cannot open %s (run vp8_video_gen.py first)\n", mpath);
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof line, m)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        run_case(dir, line);
    }
    fclose(m);

    printf("\n%d VP8 inter-frame cases, %d failed\n", cases, failures);
    return failures ? 1 : 0;
}
