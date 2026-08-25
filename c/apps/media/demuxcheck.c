/* demuxcheck -- open a container and print a digest of exactly what came out.
 *
 * ONE program, built TWICE: once for the host against glibc (make
 * test-demux-diff drives it) and once for LogitOS against mini-libc, where it
 * is /bin/demuxcheck. That is deliberate and it is the whole point. The host
 * build is what gets compared with ffmpeg; the guest build is what proves the
 * demuxer behaves the same inside the OS, where the allocator is mini-libc's
 * arena, the compiler flags are -ffreestanding -msse2 -mno-red-zone, and there
 * is no operating system underneath to fall back on. Two builds of the SAME
 * source printing the SAME lines is a comparison. Two programs would be an
 * assertion.
 *
 *   demuxcheck <file>            the digest
 *   demuxcheck -decode <file>    also decode the video track and print its
 *                                YUV CRC32, which is the end-to-end claim:
 *                                container in, pictures out.
 *
 * Every number printed is a CRC32 over a canonical byte encoding, so the two
 * builds can be compared with cmp(1) and a single wrong sample boundary
 * anywhere changes the line it is on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "h264.h"
#include "h265.h"
#include "audio.h"

/* c/lib/video's mjpeg.c decodes each frame through c/lib/image's img_decode(),
 * which allocates with the kernel heap's names. In ring 3 those names are
 * mini-libc's -- the same two-line shim preview.c:64, browser_rt.c:44 and
 * vidcheck.c carry. VID_OBJ is a wildcard over c/lib/video/*.c, so every
 * consumer of it inherited this the day MJPEG landed; the link lines did not
 * follow, and the break was invisible because the stale binaries on disk were
 * newer than the new objects. See the Makefile note beside this file's rule. */
#include <stdlib.h>
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

/* ---------------------------------------------------------------- crc ---- */
static unsigned crc_table[256];
static void crc_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}
static unsigned crc_feed(unsigned crc, const unsigned char *p, unsigned long n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}
/* Big-endian fixed width, so the digest does not depend on the host's word
 * order or on how printf formats a long long. */
static unsigned crc_u64(unsigned crc, long long v)
{
    unsigned char b[8];
    unsigned long long u = (unsigned long long)v;
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (56 - 8 * i));
    return crc_feed(crc, b, 8);
}

static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = (unsigned char *)malloc((unsigned long)n);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (unsigned long)n, f) != (long)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out_len = n;
    return b;
}

/* ------------------------------------------------------------- decode ---- */
static unsigned crc_plane(unsigned crc, const unsigned char *p, int stride, int w, int h)
{
    for (int y = 0; y < h; y++) crc = crc_feed(crc, p + (long)y * stride, (unsigned long)w);
    return crc;
}

/* Concatenate the parameter sets and every video sample as Annex B, then run
 * it through the matching decoder. Doing it in one buffer rather than feeding
 * sample by sample is what the elementary-stream fixtures next door already
 * prove works, so a mismatch here is the CONTAINER's fault and not the
 * decoder's -- which is the whole reason to build the stream this way. */
static int decode_video(mdemux *m, int ti)
{
    const media_track *t = media_track_info(m, ti);
    if (t->codec != MEDIA_CODEC_H264 && t->codec != MEDIA_CODEC_H265) {
        printf("MEDIA-DECODE %s unsupported\n", t->codec_name);
        return 0;
    }

    long need = media_annexb_headers(m, ti, 0, 0);
    if (need < 0) { printf("MEDIA-DECODE headers err=%ld\n", need); return 1; }
    media_sample s;
    /* Sized by asking, then filled by index -- media_get_sample rather than
     * media_read so this does not depend on where the read cursor was left. */
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
        long n = media_to_annexb(m, &s, 0, 0);
        if (n < 0) { printf("MEDIA-DECODE sample err=%ld\n", n); return 1; }
        need += n;
    }

    unsigned char *buf = (unsigned char *)malloc((unsigned long)need + 8);
    if (!buf) { printf("MEDIA-DECODE oom %ld\n", need); return 1; }
    long at = media_annexb_headers(m, ti, buf, need);
    if (at < 0) { free(buf); printf("MEDIA-DECODE headers err\n"); return 1; }
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
        long n = media_to_annexb(m, &s, buf + at, need - at);
        if (n < 0) { free(buf); printf("MEDIA-DECODE sample err\n"); return 1; }
        at += n;
    }

    unsigned crc = 0xFFFFFFFFu;
    int frames = 0, w = 0, h = 0;

    if (t->codec == MEDIA_CODEC_H264) {
        h264dec *d = h264_open();
        if (!d) { free(buf); printf("MEDIA-DECODE oom\n"); return 1; }
        long off = 0;
        for (;;) {
            h264frame f; int got = 0;
            if (off < at) {
                int used = h264_decode(d, buf + off, (int)(at - off), &f, &got);
                if (used < 0) { h264_close(d); free(buf);
                                printf("MEDIA-DECODE h264 err=%d at=%ld\n", used, off); return 1; }
                off += used;
                if (!got && off < at) continue;
            }
            if (!got) { if (!h264_flush(d, &f)) break; }
            crc = crc_plane(crc, f.y, f.stride_y, f.width, f.height);
            crc = crc_plane(crc, f.u, f.stride_c, (f.width + 1) / 2, (f.height + 1) / 2);
            crc = crc_plane(crc, f.v, f.stride_c, (f.width + 1) / 2, (f.height + 1) / 2);
            w = f.width; h = f.height;
            frames++;
        }
        h264_close(d);
    } else {
        h265dec *d = h265_open();
        if (!d) { free(buf); printf("MEDIA-DECODE oom\n"); return 1; }
        long off = 0;
        for (;;) {
            h265frame f; int got = 0;
            if (off < at) {
                int used = h265_decode(d, buf + off, (int)(at - off), &f, &got);
                if (used < 0) { h265_close(d); free(buf);
                                printf("MEDIA-DECODE hevc err=%d at=%ld\n", used, off); return 1; }
                off += used;
                if (!got && off < at) continue;
            }
            if (!got) { if (h265_flush(d, &f) != 1) break; }
            crc = crc_plane(crc, f.y, f.stride_y, f.width, f.height);
            crc = crc_plane(crc, f.u, f.stride_c, (f.width + 1) / 2, (f.height + 1) / 2);
            crc = crc_plane(crc, f.v, f.stride_c, (f.width + 1) / 2, (f.height + 1) / 2);
            w = f.width; h = f.height;
            frames++;
        }
        h265_close(d);
    }
    free(buf);
    printf("MEDIA-DECODE %s %dx%d frames=%d crc=%08x\n",
           t->codec_name, w, h, frames, crc ^ 0xFFFFFFFFu);
    return 0;
}

/* -------------------------------------------------------------- audio ---- */
/* An audio elementary stream out of a container is the track's configuration
 * bytes followed by its samples -- for FLAC that is literally the "fLaC" magic
 * and the STREAMINFO block from CodecPrivate followed by the frames, which is
 * a playable .flac; for MP3 there is no configuration and the frames are the
 * file. Building it that way and handing the result to c/lib/audio unchanged
 * is the claim being tested: no per-codec repacking, because the container
 * already stores what the codec wants. */
static int decode_audio(mdemux *m, int ti)
{
    const media_track *t = media_track_info(m, ti);
    if (t->codec != MEDIA_CODEC_MP3 && t->codec != MEDIA_CODEC_FLAC) {
        printf("MEDIA-AUDIO %s no decoder\n", t->codec_name);
        return 0;
    }
    int prefix = (t->codec == MEDIA_CODEC_FLAC) ? t->extradata_len : 0;
    long need = prefix;
    media_sample s;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) need += s.size;

    unsigned char *buf = (unsigned char *)malloc((unsigned long)need + 1);
    if (!buf) { printf("MEDIA-AUDIO oom\n"); return 1; }
    if (prefix) memcpy(buf, t->extradata, (size_t)prefix);
    long at = prefix;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
        memcpy(buf + at, s.data, (size_t)s.size);
        at += s.size;
    }

    apcm pcm;
    int rc = audio_decode(buf, at, 0, &pcm);
    if (rc != AUDIO_OK) {
        free(buf);
        printf("MEDIA-AUDIO %s err=%d\n", t->codec_name, rc);
        return 1;
    }
    unsigned crc = crc_feed(0xFFFFFFFFu, (const unsigned char *)pcm.s16,
                            (unsigned long)pcm.frames * (unsigned long)pcm.channels * 2)
                   ^ 0xFFFFFFFFu;
    printf("MEDIA-AUDIO %s rate=%d ch=%d frames=%ld crc=%08x\n",
           t->codec_name, pcm.rate, pcm.channels, pcm.frames, crc);
    audio_pcm_free(&pcm);
    free(buf);
    return 0;
}

/* --------------------------------------------------------------- main ---- */
int main(int argc, char **argv)
{
    int want_decode = 0, argi = 1;
    if (argi < argc && !strcmp(argv[argi], "-decode")) { want_decode = 1; argi++; }
    if (argi >= argc) { printf("usage: demuxcheck [-decode] <file>\n"); return 2; }

    crc_init();
    long len = 0;
    unsigned char *data = read_all(argv[argi], &len);
    if (!data) { printf("demuxcheck: cannot open %s\n", argv[argi]); return 1; }

    int err = 0;
    mdemux *m = media_open(data, len, &err);
    if (!m) { printf("MEDIA-ERR %s (%d)\n", media_strerror(err), err); free(data); return 1; }

    printf("MEDIA container=%s fragmented=%d duration_ns=%lld tracks=%d\n",
           media_container_name(media_kind(m)), media_is_fragmented(m),
           media_duration_ns(m), media_track_count(m));

    unsigned overall = 0xFFFFFFFFu;
    long total = 0;

    for (int i = 0; i < media_track_count(m); i++) {
        const media_track *t = media_track_info(m, i);
        unsigned ecrc = crc_feed(0xFFFFFFFFu, t->extradata, (unsigned long)t->extradata_len)
                        ^ 0xFFFFFFFFu;
        unsigned dcrc = 0xFFFFFFFFu, scrc = 0xFFFFFFFFu;
        media_sample s;
        for (long k = 0; media_get_sample(m, i, k, &s) == 1; k++) {
            dcrc = crc_feed(dcrc, s.data, (unsigned long)s.size);
            scrc = crc_u64(scrc, s.pts_ticks);
            scrc = crc_u64(scrc, s.dts_ticks);
            scrc = crc_u64(scrc, s.size);
            scrc = crc_u64(scrc, s.keyframe);
            total++;
        }
        dcrc ^= 0xFFFFFFFFu; scrc ^= 0xFFFFFFFFu;

        const char *kind = t->type == MEDIA_TRACK_VIDEO ? "video"
                         : t->type == MEDIA_TRACK_AUDIO ? "audio" : "other";
        printf("TRACK %d %s %s %dx%d rate=%d ch=%d timescale=%u nsamples=%ld "
               "framing=%d nls=%d extradata=%d/%08x data=%08x stamps=%08x\n",
               i, kind, t->codec_name, t->width, t->height, t->rate, t->channels,
               t->timescale, t->nsamples, t->framing, t->nal_length_size,
               t->extradata_len, ecrc, dcrc, scrc);

        overall = crc_u64(overall, dcrc);
        overall = crc_u64(overall, scrc);
        overall = crc_u64(overall, ecrc);
    }

    /* media_read's own ordering is a separate claim from the per-track index,
     * so digest it too: a demuxer that indexes correctly and interleaves
     * wrongly plays audio a second late and passes every test above. */
    unsigned ocrc = 0xFFFFFFFFu;
    media_sample s;
    long nread = 0;
    while (media_read(m, &s) == 1) {
        ocrc = crc_u64(ocrc, s.track);
        ocrc = crc_u64(ocrc, s.file_off);
        ocrc = crc_u64(ocrc, s.dts_ns);
        nread++;
    }
    printf("ORDER samples=%ld crc=%08x\n", nread, ocrc ^ 0xFFFFFFFFu);
    printf("DEMUX-CRC %08x %ld samples\n", overall ^ 0xFFFFFFFFu, total);

    int rc = 0;
    if (want_decode) {
        int vi = media_find_track(m, MEDIA_TRACK_VIDEO);
        if (vi < 0) printf("MEDIA-DECODE no video track\n");
        else rc = decode_video(m, vi);
        int ai = media_find_track(m, MEDIA_TRACK_AUDIO);
        if (ai < 0) printf("MEDIA-AUDIO no audio track\n");
        else if (decode_audio(m, ai)) rc = 1;
    }

    media_close(m);
    free(data);
    return rc;
}
