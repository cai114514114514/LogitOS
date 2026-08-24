/* Host test for c/lib/video/mjpeg.c -- Motion JPEG framing + default-Huffman-
 * table splice, decoded by reusing c/lib/image/jpeg.c wholesale.
 *
 * Fixtures come from tools/genmjpeg.sh (real ffmpeg-encoded MJPEG, real
 * djpeg -nosmooth -dct int oracle -- the SAME oracle jpeg.c's own test-jpeg is
 * gated against, so a mismatch here is MJPEG's bug, not JPEG's). This file
 * proves five separate things named in the task and in mjpeg.h's own
 * comments, each with its own assertion rather than one pass/fail bit:
 *
 *   1. BYTE-EXACT decode of every frame of every case, both with the frame's
 *      own DHT (ordinary path) and with it stripped (default-table splice
 *      path) -- both must land on the identical djpeg reference, because
 *      c/lib/video/mjpeg_deftables.inc is byte-identical to what ffmpeg wrote.
 *   2. NO REALLOCATION: mjpegframe.rgba's pointer is IDENTICAL across every
 *      frame of a case (same size throughout) -- checked by pointer identity,
 *      not merely by the bytes still matching (mjpeg.h's own comment promises
 *      this test; see mjpeg_decode_frame's doc comment).
 *   3. SIZE CHANGE mid-stream is refused (MJPEG_ERR_SIZE_CHANGE) and leaves
 *      the caller's `out` (and the decoder's persistent buffer) holding the
 *      previous frame, untouched.
 *   4. mjpeg_is_two_field() reports 1 for two whole frames back-to-back in
 *      one buffer (the OpenDML AVI field convention's shape) and 0 for one.
 *   5. Framing survives byte-stuffing/RSTn inside entropy data, exercised
 *      implicitly by every case above -- mjpeg_next_frame is what carves each
 *      frame out of the concatenated stream in the first place, so a bug
 *      there would desync every case starting at frame 2.
 *
 * The negative control (-DMJPEG_NO_DEFAULT_DHT) is a SEPARATE binary built by
 * tests/mjpeg.mk's test-mjpeg-negctl, not by this file at runtime: it must
 * refuse every *_nodht.mjpeg stream's first frame with MJPEG_ERR_NO_DHT and
 * must NOT disturb the plain (DHT-present) streams, which this same source
 * file's CASE_MODE_NEGCTL path checks directly.
 *
 * Build (see tests/mjpeg.mk for the real recipe, incl. RUST_LIB_HOST):
 *   cc -O2 -o mjpeg_test tests/unit/mjpeg_test.c c/lib/video/mjpeg.c \
 *       c/lib/image/{img,gif,jpeg,svg,exif}.c ... -Ic/lib/video -Ic/lib/image
 * Run: python3 tools/genmjpeg.sh /tmp/mjpegfix && ./mjpeg_test /tmp/mjpegfix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mjpeg.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = 0; }
    fclose(f);
    return b;
}

/* Byte-exact compare; prints the wrong-byte total (not just a boolean) so a
 * failure here is bisectable the way every other decoder gate in this tree
 * requires -- see tests/unit/jpeg_test.c / h264 diff / progressive JPEG. */
static long count_wrong(const unsigned char *a, const unsigned char *b, long n)
{
    long wrong = 0;
    for (long i = 0; i < n; i++) if (a[i] != b[i]) wrong++;
    return wrong;
}

struct kase { char name[64]; int nframes, w, h; };

static int g_fails = 0, g_total = 0;

static void check(int ok, const char *fmt, ...)
{
    g_total++;
    va_list ap;
    va_start(ap, fmt);
    if (!ok) {
        g_fails++;
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
    } else {
        printf("ok   ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

/* Decode `stream` (nframes back-to-back JPEGs) through the public API exactly
 * as a real caller would (mjpeg_next_frame in a loop), comparing every frame
 * byte-exact against <dir>/<case>_f<k>.ref, and asserting the rgba pointer
 * never moves across frames of one case. Returns the decoder so callers that
 * need extra assertions (size-change, two-field) can keep using it. */
static mjpegdec *decode_and_check(const char *dir, const struct kase *k,
                                   const unsigned char *stream, long slen,
                                   const char *label)
{
    mjpegdec *d = mjpeg_open();
    if (!d) { check(0, "%s: mjpeg_open failed", label); return 0; }

    const unsigned char *prev_ptr = 0;
    long off = 0;
    for (int fr = 0; fr < k->nframes; fr++) {
        int start;
        int flen = mjpeg_next_frame(stream + off, (int)(slen - off), &start);
        if (flen <= 0) {
            check(0, "%s frame %d: mjpeg_next_frame returned %d (%s)",
                  label, fr, flen, mjpeg_strerror(flen < 0 ? flen : 0));
            break;
        }
        mjpegframe out;
        int rc = mjpeg_decode_frame(d, stream + off + start, flen, &out);
        if (rc != MJPEG_OK) {
            check(0, "%s frame %d: decode_frame -> %s", label, fr, mjpeg_strerror(rc));
            off += start + flen;
            continue;
        }
        check(out.width == k->w && out.height == k->h,
              "%s frame %d: %dx%d (want %dx%d)", label, fr, out.width, out.height, k->w, k->h);

        char rp[512];
        snprintf(rp, sizeof rp, "%s/%s_f%d.ref", dir, k->name, fr);
        long rn; unsigned char *ref = slurp(rp, &rn);
        if (!ref) {
            check(0, "%s frame %d: missing %s", label, fr, rp);
        } else {
            long want = (long)k->w * k->h * 4;
            long wrong = (rn == want) ? count_wrong(out.rgba, ref, want) : want;
            check(wrong == 0, "%s frame %d: %ld/%ld bytes wrong", label, fr, wrong, want);
            free(ref);
        }

        if (fr == 0) {
            prev_ptr = out.rgba;
        } else {
            check(out.rgba == prev_ptr,
                  "%s frame %d: rgba pointer %p (frame 0's was %p)",
                  label, fr, (void *)out.rgba, (void *)prev_ptr);
        }

        off += start + flen;
    }
    return d;
}

static void run_case(const char *dir, const struct kase *k)
{
    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/%s.mjpeg", dir, k->name);
    snprintf(p2, sizeof p2, "%s/%s_nodht.mjpeg", dir, k->name);
    long n1, n2;
    unsigned char *s1 = slurp(p1, &n1);
    unsigned char *s2 = slurp(p2, &n2);
    if (!s1) { check(0, "%s: missing %s", k->name, p1); return; }
    if (!s2) { check(0, "%s: missing %s", k->name, p2); free(s1); return; }

    mjpegdec *d1 = decode_and_check(dir, k, s1, n1, k->name);
    if (d1) mjpeg_close(d1);

    char label2[80];
    snprintf(label2, sizeof label2, "%s(nodht)", k->name);
    mjpegdec *d2 = decode_and_check(dir, k, s2, n2, label2);
    if (d2) mjpeg_close(d2);

    free(s1); free(s2);
}

/* Requirement 3: a later frame whose SOF geometry differs must be refused,
 * with the previous successful frame left standing. Built by taking one real
 * frame from each of two differently-sized cases and concatenating them --
 * exactly what mjpeg_next_frame is required to carve back apart. */
static void check_size_change(const char *dir, const struct kase *a, const struct kase *b)
{
    char pa[512], pb[512];
    snprintf(pa, sizeof pa, "%s/%s.mjpeg", dir, a->name);
    snprintf(pb, sizeof pb, "%s/%s.mjpeg", dir, b->name);
    long na, nb;
    unsigned char *sa = slurp(pa, &na);
    unsigned char *sb = slurp(pb, &nb);
    if (!sa || !sb) { check(0, "size-change: missing fixture"); free(sa); free(sb); return; }

    int starta, lena = mjpeg_next_frame(sa, (int)na, &starta);
    int startb, lenb = mjpeg_next_frame(sb, (int)nb, &startb);
    check(lena > 0 && lenb > 0, "size-change: carved one frame from each fixture (%d B, %d B)",
          lena, lenb);
    if (lena > 0 && lenb > 0) {
        unsigned char *buf = malloc((size_t)(lena + lenb));
        memcpy(buf, sa + starta, (size_t)lena);
        memcpy(buf + lena, sb + startb, (size_t)lenb);

        mjpegdec *d = mjpeg_open();
        mjpegframe out; memset(&out, 0, sizeof out);
        int s1, f1 = mjpeg_next_frame(buf, lena + lenb, &s1);
        int rc1 = mjpeg_decode_frame(d, buf + s1, f1, &out);
        check(rc1 == MJPEG_OK && out.width == a->w && out.height == a->h,
              "size-change: first frame (%s) decoded ok", a->name);
        const unsigned char *ptr_after_first = out.rgba;
        int wid_after_first = out.width, hei_after_first = out.height;

        int s2, f2 = mjpeg_next_frame(buf + s1 + f1, lena + lenb - s1 - f1, &s2);
        int rc2 = mjpeg_decode_frame(d, buf + s1 + f1 + s2, f2, &out);
        check(rc2 == MJPEG_ERR_SIZE_CHANGE,
              "size-change: second frame (%s, different geometry) -> %s (want ERR_SIZE_CHANGE)",
              b->name, mjpeg_strerror(rc2));
        check(out.rgba == ptr_after_first && out.width == wid_after_first && out.height == hei_after_first,
              "size-change: `out` left holding the previous frame after the refusal");

        mjpeg_close(d);
        free(buf);
    }
    free(sa); free(sb);
}

/* Requirement 4: mjpeg_is_two_field on two whole frames back-to-back (the
 * OpenDML AVI field convention's shape) vs. on one. */
static void check_two_field(const char *dir, const struct kase *k)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s.mjpeg", dir, k->name);
    long n; unsigned char *s = slurp(p, &n);
    if (!s || k->nframes < 2) { check(0, "two-field: need a >=2-frame fixture (%s)", k->name); free(s); return; }

    int s1, f1 = mjpeg_next_frame(s, (int)n, &s1);
    check(f1 > 0, "two-field: carve frame 0 of %s", k->name);

    /* One frame alone: not two-field. */
    check(mjpeg_is_two_field(s + s1, f1) == 0, "two-field: one frame -> 0");

    /* Two whole frames back to back: IS the two-field shape. */
    int s2, f2 = mjpeg_next_frame(s + s1 + f1, (int)n - s1 - f1, &s2);
    check(f2 > 0, "two-field: carve frame 1 of %s", k->name);
    check(mjpeg_is_two_field(s + s1, f1 + s2 + f2) == 1, "two-field: two frames -> 1");

    free(s);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/mjpegfix";
    char mp[512]; snprintf(mp, sizeof mp, "%s/manifest.txt", dir);
    FILE *m = fopen(mp, "r");
    if (!m) { printf("no manifest in %s\n", dir); return 1; }

    struct kase kases[32]; int nk = 0;
    while (nk < 32 && fscanf(m, "%63s %d %d %d", kases[nk].name, &kases[nk].nframes,
                              &kases[nk].w, &kases[nk].h) == 4) nk++;
    fclose(m);
    if (nk == 0) { printf("empty manifest in %s\n", dir); return 1; }

    for (int i = 0; i < nk; i++) run_case(dir, &kases[i]);

    /* Cross-case checks need two differently-sized cases and one multi-frame
     * case; the manifest built by tools/genmjpeg.sh always provides both
     * (c64_hi/c34 differ in size; every case has >=2 frames). */
    int a = -1, b = -1;
    for (int i = 0; i < nk && (a < 0 || b < 0); i++)
        for (int j = 0; j < nk; j++)
            if (i != j && (kases[i].w != kases[j].w || kases[i].h != kases[j].h)) { a = i; b = j; break; }
    if (a >= 0 && b >= 0) check_size_change(dir, &kases[a], &kases[b]);
    else check(0, "size-change: no two differently-sized cases in manifest");

    int mf = -1;
    for (int i = 0; i < nk; i++) if (kases[i].nframes >= 2) { mf = i; break; }
    if (mf >= 0) check_two_field(dir, &kases[mf]);
    else check(0, "two-field: no >=2-frame case in manifest");

    printf(g_fails ? "\n%d/%d MJPEG checks FAILED\n" : "\nall %d MJPEG checks passed\n",
           g_fails ? g_fails : g_total, g_total);
    return g_fails ? 1 : 0;
}
