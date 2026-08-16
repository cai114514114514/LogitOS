/* c/apps/video/vidbench.c -- decode-throughput benchmark: frames per second,
 * by resolution, for H.264 and H.265, TIMED on whatever machine runs it.
 *
 * The question this file answers: how fast does this OS's own decoder run --
 * on the host when built native (glibc), and on LogitOS itself when built
 * against mini-libc and booted under QEMU TCG. Nobody had measured either
 * before this existed, and every plan for real video playback (bilibili's
 * DASH .m4s, per CLAUDE.md) rests on knowing the answer. TCG is the machine
 * this OS actually runs on, so the guest number is the one that matters; the
 * host number exists only so the RATIO between them is known, which is what
 * lets a future host measurement predict a guest one without booting QEMU
 * every time.
 *
 * Decode-only: the whole file is read into memory before the clock starts (no
 * disk I/O in the timed region -- vidcheck.c/vidcheck265.c already made this
 * call for the same reason: these are small fixtures, not films, and a
 * streaming reader would only add a code path the timing has no interest in)
 * and nothing is displayed (no SYS_GUI_BLIT -- this measures the decoder, not
 * the compositor; c/lib/gfx's own bench-gfx-frame already measures that half
 * separately). Codec is chosen by extension: .h264 drives h264_open/
 * h264_decode/h264_flush, .h265/.hevc drive the H.265 equivalents.
 *
 * Timed with clock_gettime(CLOCK_MONOTONIC). On LogitOS that is
 * SYS_CLOCK_GETTIME (c/kernel/core/ktime.c), which is RDTSC-calibrated --
 * nanosecond resolution, NOT the 10 ms SYS_MONOTONIC_MS tick used elsewhere in
 * the tree (see logit.h's monotonic_ms comment); at a few hundred frames of a
 * few hundred microseconds each, a 10 ms tick would not resolve anything. On
 * the host it is glibc's usual CLOCK_MONOTONIC. Same call, same POSIX clock id
 * (LOGIT_CLOCK_MONOTONIC == 1 == CLOCK_MONOTONIC on Linux -- see
 * include/abi/logit_abi.h's comment on why the ids match on purpose), one
 * source file compiled twice -- so the two numbers in the printed table are a
 * comparison, not two different measurements pretending to be one.
 *
 * A stream this decoder cannot decode (H264_ERR_* / H265_ERR_*, including
 * H264_ERR_OOM/H265_ERR_OOM if the arena is too small for the picture -- see
 * h265.h's own note that 1080p barely fits the 24 MiB default) is reported by
 * name and error code, never silently skipped: the point of this file is a
 * number somebody will use to predict something, and a missing row would be
 * read as "not measured" rather than "failed", which is a worse kind of wrong.
 *
 * MULTIPLE FILES IN ONE ARGV IS SUPPORTED BUT NOT HOW THE GUEST BENCHMARK
 * USES IT (see tests/boot/run-vidbench-test.sh for the full story): running
 * an H.264 stream immediately before an H.265 one, in the SAME process, was
 * found to make the H.265 decode -- which completes in ~4.5 s given a fresh
 * process -- not finish inside a multi-minute budget under QEMU TCG. That is
 * an arena-fragmentation interaction in mini-libc's malloc between one
 * decoder's h264_close() and the next decoder's h265_open(), not a bug in
 * either decoder's own logic (isolated, the guest/host slowdown ratio for
 * H.265 comes out consistent with H.264's). The guest harness invokes this
 * binary once per stream to sidestep it; this file still accepts a multi-file
 * argv (for the host, where the same interaction was never observed) because
 * splitting it there bought nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "h264.h"
#include "h265.h"

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    unsigned char *buf = malloc((unsigned long)len);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, (unsigned long)len, f) != len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

static int is_265(const char *path)
{
    long n = (long)strlen(path);
    if (n >= 5 && strcmp(path + n - 5, ".h265") == 0) return 1;
    if (n >= 5 && strcmp(path + n - 5, ".hevc") == 0) return 1;
    return 0;
}

/* Returns frame count (>=0) and fills w/h/ms, or a negative H264_ERR_*. */
static int bench_264(const unsigned char *buf, long len, int *w, int *h, double *ms)
{
    h264dec *d = h264_open();
    if (!d) return H264_ERR_OOM;
    int frames = 0;
    long off = 0;
    double t0 = now_ms();
    for (;;) {
        h264frame fr;
        int got = 0, used;
        if (off < len) {
            used = h264_decode(d, buf + off, (int)(len - off), &fr, &got);
            if (used < 0) { h264_close(d); return used; }
            off += used;
            if (!got) { if (off < len) continue; }
        }
        if (!got) { if (!h264_flush(d, &fr)) break; }
        if (frames == 0) { *w = fr.width; *h = fr.height; }
        frames++;
    }
    *ms = now_ms() - t0;
    h264_close(d);
    return frames;
}

/* Same shape as bench_264, for the H.265 decoder's slightly different
 * end-of-stream signal (used==0 && !got means "truly done", per
 * vidcheck265.c -- h264_decode never returns 0 used with no frame short of
 * exhausting the buffer, so the two loops are not quite copies of each
 * other). */
static int bench_265(const unsigned char *buf, long len, int *w, int *h, double *ms)
{
    h265dec *d = h265_open();
    if (!d) return H265_ERR_OOM;
    int frames = 0;
    long off = 0;
    double t0 = now_ms();
    for (;;) {
        h265frame fr;
        int got = 0, used;
        if (off < len) {
            used = h265_decode(d, buf + off, (int)(len - off), &fr, &got);
            if (used < 0) { h265_close(d); return used; }
            if (used == 0 && !got) break;
            off += used;
            if (!got) { if (off < len) continue; }
        }
        if (!got) { if (h265_flush(d, &fr) != 1) break; }
        if (frames == 0) { *w = fr.width; *h = fr.height; }
        frames++;
    }
    *ms = now_ms() - t0;
    h265_close(d);
    return frames;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: vidbench <stream.h264|.h265> [more...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        long len = 0;
        unsigned char *buf = read_all(argv[i], &len);
        if (!buf) {
            printf("VIDBENCH-ERR %s: cannot read\n", argv[i]);
            failures++;
            continue;
        }
        int w = 0, h = 0;
        double ms = 0;
        int codec265 = is_265(argv[i]);
        int frames = codec265 ? bench_265(buf, len, &w, &h, &ms)
                               : bench_264(buf, len, &w, &h, &ms);
        free(buf);
        if (frames < 0) {
            printf("VIDBENCH-ERR %s: decode error %d\n", argv[i], frames);
            failures++;
            continue;
        }
        if (frames == 0 || ms <= 0.0) {
            printf("VIDBENCH-ERR %s: 0 frames decoded (nothing to time)\n", argv[i]);
            failures++;
            continue;
        }
        double fps = (double)frames * 1000.0 / ms;
        double mpix_s = fps * (double)w * (double)h / 1.0e6;
        printf("VIDBENCH %s %dx%d frames=%d ms=%.2f fps=%.2f mpix_s=%.2f\n",
               codec265 ? "h265" : "h264", w, h, frames, ms, fps, mpix_s);
    }
    printf("VIDBENCH-DONE failures=%d\n", failures);
    return 0;
}
