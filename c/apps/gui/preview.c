/* Preview: opens a file and shows what is in it -- a still image, a raw video
 * elementary stream, or a REAL CONTAINER with video and audio in it.
 *
 * The three halves are deliberately asymmetric, and it is worth saying why.
 *
 * Images are decoded by the KERNEL (SYS_IMG_DECODE): a picture is decoded once
 * and thrown away, so a syscall that hands back RGBA is a fair trade and the
 * codecs already live in c/lib/image.
 *
 * Video, audio and the CONTAINER are handled HERE, in ring 3, from
 * c/lib/{video,audio,media}. A video is decoded thirty times a second and
 * carries megabytes of reference frames between calls; doing that in the
 * kernel would hold the big lock for the whole of every frame. A container is
 * worse still: it is a tree of nested lengths written by whoever made the
 * file, and walking it with a pointer is not something to do in ring 0. It is
 * also the direction M17 already took the browser's render pipeline -- the
 * kernel provides primitives, the app does the work. So this app links the
 * demuxers, both video decoders and the audio decoders, and only asks the
 * kernel to put pixels on the screen and samples in the sound ring.
 *
 * WHICH PATH RUNS IS DECIDED BY SNIFFING THE FILE, NOT BY ITS NAME. An MP4
 * called .h264 plays as an MP4; a PNG called .mp4 opens as a PNG.
 *
 * SYNCHRONISATION. See the long note at the top of c/lib/media/avclock.c for
 * the policy and the reasoning. The two rules this file implements are:
 * audio is written AHEAD OF THE DECODED VIDEO POSITION BY AT MOST AV_LEAD_NS
 * and never further (that is what keeps the two streams together when decode
 * falls behind, which on this machine it does), and a late frame is still
 * decoded but not converted or blitted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logit.h"
#include "h264.h"
#include "h265.h"
#include "audio.h"
#include "media.h"

#define WINW 760
#define WINH 560
#define MAXW 1280            /* up to screen resolution */
#define MAXH 800
static unsigned char rgba[MAXW * MAXH * 4];

/* How far audio may run ahead of the picture. It is the size of the cushion
 * that absorbs a slow frame AND the worst-case A/V error if the pipeline
 * starves hard enough to drain it; make test-avsync measures both. */
#define AV_LEAD_NS   150000000LL
#define ABUF_FRAMES  1024

/* ------------------------------------------------------------ colour ---- */
static int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* BT.601 studio-swing YUV -> RGB, the conversion these streams are authored
 * for. Integer only: this app is built -mno-red-zone freestanding and there is
 * no reason to pull floating point into a per-pixel loop. Chroma is 4:2:0, so
 * each chroma sample covers a 2x2 luma quad -- nearest-neighbour upsampling,
 * which is what a viewer wants and avoids a second full-frame pass. */
static void yuv420_to_rgba(const unsigned char *yp, const unsigned char *up,
                           const unsigned char *vp, int sy, int sc,
                           int w, int h, unsigned char *dst)
{
    for (int y = 0; y < h; y++) {
        const unsigned char *ly = yp + (long)y * sy;
        const unsigned char *lu = up + (long)(y / 2) * sc;
        const unsigned char *lv = vp + (long)(y / 2) * sc;
        unsigned char *o = dst + (long)y * w * 4;
        for (int x = 0; x < w; x++) {
            int c = ly[x] - 16;
            int d = lu[x / 2] - 128;
            int e = lv[x / 2] - 128;
            o[0] = (unsigned char)clip8((298 * c + 409 * e + 128) >> 8);
            o[1] = (unsigned char)clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[2] = (unsigned char)clip8((298 * c + 516 * d + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
}

/* ------------------------------------------------------------- layout --- */
/* Aspect-fit w x h into the window, centred; gui_blit scales. */
static void blit_fit(int iw, int ih)
{
    if (iw <= 0 || ih <= 0) return;
    int dw = WINW, dh = (int)((long)WINW * ih / iw);
    if (dh > WINH) { dh = WINH; dw = (int)((long)WINH * iw / ih); }
    gui_blit((WINW - dw) / 2, (WINH - dh) / 2, dw, dh, rgba, iw, ih);
}

/* Returns 1 if the app should quit. Called between frames so a playing video
 * still answers the close button. */
static int pump_events(void)
{
    struct logit_event e;
    while (poll_event(&e)) {
        if (e.type == EV_CLOSE) return 1;
        if (e.type == EV_KEY && e.a == 27) return 1;      /* Esc closes */
    }
    return 0;
}

static void status(const char *s)
{
    gui_text(12, WINH - 6, rgb(150, 150, 158), s);
}

/* Put a message up and stay there until the user closes the window. */
static void die_with(const char *msg)
{
    gui_clear(rgb(28, 28, 32));
    gui_text(16, 28, rgb(220, 220, 224), msg);
    status("Esc or the close button to quit");
    gui_flush();
    while (!pump_events()) sys_yield();
    app_exit(0);
}

/* --------------------------------------------------------------- file --- */
static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = malloc((unsigned long)n);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (unsigned long)n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out_len = n;
    return b;
}

/* Annex-B start code: 00 00 01 or 00 00 00 01, at the very start of the file. */
static int looks_like_annexb(const unsigned char *b, long n)
{
    if (n < 5) return 0;
    if (b[0] == 0 && b[1] == 0 && b[2] == 1) return 1;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) return 1;
    return 0;
}

/* ------------------------------------------------------ video decoder --- */
/* One object for both codecs. The two APIs were deliberately given the same
 * shape (see the note at the top of h265.h), so the only thing this adds is
 * not writing the player loop twice. */
typedef struct {
    int        is265;
    h264dec   *d4;
    h265dec   *d5;
    int        w, h, sy, sc;
    const unsigned char *y, *u, *v;
} vdec;

static int vdec_open(vdec *v, int is265)
{
    memset(v, 0, sizeof *v);
    v->is265 = is265;
    if (is265) { v->d5 = h265_open(); return v->d5 != 0; }
    v->d4 = h264_open(); return v->d4 != 0;
}
static void vdec_close(vdec *v)
{
    if (v->d4) h264_close(v->d4);
    if (v->d5) h265_close(v->d5);
    v->d4 = 0; v->d5 = 0;
}
/* Feed the whole of one access unit; returns 1 if a picture came out, 0 if
 * not, negative on a decode error. */
static int vdec_feed(vdec *v, const unsigned char *p, long n)
{
    long off = 0;
    int got = 0;
    while (off < n && !got) {
        int used;
        if (v->is265) {
            h265frame f;
            used = h265_decode(v->d5, p + off, (int)(n - off), &f, &got);
            if (used < 0) return used;
            if (got) { v->w = f.width; v->h = f.height; v->sy = f.stride_y;
                       v->sc = f.stride_c; v->y = f.y; v->u = f.u; v->v = f.v; }
        } else {
            h264frame f;
            used = h264_decode(v->d4, p + off, (int)(n - off), &f, &got);
            if (used < 0) return used;
            if (got) { v->w = f.width; v->h = f.height; v->sy = f.stride_y;
                       v->sc = f.stride_c; v->y = f.y; v->u = f.u; v->v = f.v; }
        }
        if (used == 0) break;                  /* no progress: stop asking */
        off += used;
    }
    return got;
}
static int vdec_flush(vdec *v)
{
    if (v->is265) {
        h265frame f;
        if (h265_flush(v->d5, &f) != 1) return 0;
        v->w = f.width; v->h = f.height; v->sy = f.stride_y; v->sc = f.stride_c;
        v->y = f.y; v->u = f.u; v->v = f.v;
        return 1;
    }
    h264frame f;
    if (!h264_flush(v->d4, &f)) return 0;
    v->w = f.width; v->h = f.height; v->sy = f.stride_y; v->sc = f.stride_c;
    v->y = f.y; v->u = f.u; v->v = f.v;
    return 1;
}

/* ------------------------------------------------------ audio playout --- */
/* The audio side of the player. Everything about it follows from one rule:
 * write ahead of the picture, but only so far. */
typedef struct {
    unsigned char *es;        /* the elementary stream, assembled once */
    long           es_len;
    adec          *dec;
    int            handle;    /* the sound stream, or < 0 */
    int            rate, ch;
    long long      frames_written;
    long long      written_ns;
    int            eof;
    short          buf[ABUF_FRAMES * 2];
    const char    *why;       /* why there is no audio, for the status line */
} aplay;

/* An audio elementary stream out of a container is the track's configuration
 * bytes followed by its samples. For FLAC that is literally the "fLaC" magic
 * and STREAMINFO from CodecPrivate followed by the frames -- a playable .flac.
 * For MP3 there is no configuration and the frames are the file. No per-codec
 * repacking, because the container already stores what the codec wants. */
static void aplay_open(aplay *a, mdemux *m, int ti)
{
    memset(a, 0, sizeof *a);
    a->handle = -1;
    if (ti < 0) { a->why = "no audio track"; return; }
    const media_track *t = media_track_info(m, ti);
    if (t->codec != MEDIA_CODEC_MP3 && t->codec != MEDIA_CODEC_FLAC) {
        a->why = t->codec_name;              /* named, not silently ignored */
        return;
    }
    struct logit_sndinfo si;
    if (snd_info(&si) != 1) { a->why = "no sound card"; return; }

    int prefix = (t->codec == MEDIA_CODEC_FLAC) ? t->extradata_len : 0;
    long need = prefix;
    media_sample s;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) need += s.size;
    a->es = malloc((unsigned long)need + 1);
    if (!a->es) { a->why = "out of memory"; return; }
    if (prefix) memcpy(a->es, t->extradata, (unsigned long)prefix);
    long at = prefix;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
        memcpy(a->es + at, s.data, (unsigned long)s.size);
        at += s.size;
    }
    a->es_len = at;

    int err = 0;
    a->dec = adec_open(a->es, a->es_len, &err);
    if (!a->dec) { a->why = "audio decode failed"; return; }
    if (adec_info(a->dec, &a->rate, &a->ch) != AUDIO_OK || a->rate <= 0) {
        a->why = "audio format"; adec_close(a->dec); a->dec = 0; return;
    }
    if (a->ch > 2) a->ch = 2;
    a->handle = snd_open_s16((unsigned)a->rate, (unsigned)a->ch);
    if (a->handle < 0) { a->why = "sound device busy"; adec_close(a->dec); a->dec = 0; }
}

static void aplay_close(aplay *a)
{
    if (a->handle >= 0) snd_close(a->handle, 0);
    if (a->dec) adec_close(a->dec);
    if (a->es) free(a->es);
    memset(a, 0, sizeof *a);
    a->handle = -1;
}

/* Top the ring up, but never past `upto_ns` of media time. Never blocks: it
 * writes only what snd_avail() says there is room for, so a full ring simply
 * ends the loop. */
static void aplay_pump(aplay *a, long long upto_ns)
{
    if (a->handle < 0 || a->eof) return;
    int bytes_per_frame = a->ch * 2;
    while (a->written_ns < upto_ns) {
        int room = snd_avail(a->handle);
        if (room < ABUF_FRAMES * bytes_per_frame) break;
        long got = adec_read(a->dec, a->buf, ABUF_FRAMES);
        if (got <= 0) { a->eof = 1; break; }
        int want = (int)got * bytes_per_frame;
        int off = 0;
        while (off < want) {
            int k = snd_write(a->handle, (const char *)a->buf + off, want - off);
            if (k <= 0) break;
            off += k;
        }
        a->frames_written += got;
        a->written_ns = a->frames_written * 1000000000LL / a->rate;
    }
}

/* The master clock: what the CARD has played, not what we have written. */
static long long aplay_played_ns(aplay *a)
{
    if (a->handle < 0) return -1;
    struct logit_sndstate st;
    if (snd_state(a->handle, &st) != 0) return -1;
    return (long long)st.frames_played * 1000000000LL / a->rate;
}

/* ------------------------------------------------------ the container --- */
static void play_container(unsigned char *data, long len, const char *name)
{
    char line[160];
    int err = 0;
    mdemux *m = media_open(data, len, &err);
    if (!m) {
        snprintf(line, sizeof line, "%s: cannot open this container (%s)",
                 name, media_strerror(err));
        die_with(line);
    }

    int vi = media_find_track(m, MEDIA_TRACK_VIDEO);
    int ai = media_find_track(m, MEDIA_TRACK_AUDIO);
    if (vi < 0 && ai < 0) die_with("this file has no video and no audio track");

    const media_track *vt = vi >= 0 ? media_track_info(m, vi) : 0;
    if (vt && vt->codec != MEDIA_CODEC_H264 && vt->codec != MEDIA_CODEC_H265) {
        /* Named, not swallowed. VP9 and AV1 have no decoder in this system and
         * saying so is more use than a blank window. */
        snprintf(line, sizeof line, "%s: video is %s -- no decoder for it here",
                 name, vt->codec_name);
        die_with(line);
    }

    aplay au;
    aplay_open(&au, m, ai);

    avclock clk;
    avclock_init(&clk, au.handle >= 0);

    vdec v;
    if (vt) {
        if (!vdec_open(&v, vt->codec == MEDIA_CODEC_H265)) die_with("out of memory");
        /* The parameter sets, once, before anything else: MP4 and Matroska
         * both hoist them out of the samples into the sample description. */
        long hn = media_annexb_headers(m, vi, 0, 0);
        if (hn > 0) {
            unsigned char *hb = malloc((unsigned long)hn);
            if (hb && media_annexb_headers(m, vi, hb, hn) == hn) vdec_feed(&v, hb, hn);
            free(hb);
        }
    }

    long sample = 0, shown = 0;
    long long first_pts = -1;
    static unsigned char nalbuf[1 << 20];

    for (;;) {
        if (pump_events()) break;

        /* Audio-only file: just keep the ring fed and show the counters. */
        if (!vt) {
            aplay_pump(&au, au.written_ns + AV_LEAD_NS);
            gui_clear(rgb(18, 18, 20));
            snprintf(line, sizeof line, "%s  %s %d Hz %d ch  %lld s played",
                     name, au.why ? au.why : "audio", au.rate, au.ch,
                     aplay_played_ns(&au) / 1000000000LL);
            status(line);
            gui_flush();
            if (au.eof && snd_avail(au.handle) > 0) break;
            sys_sleep_ms(50);
            continue;
        }

        media_sample s;
        int got = 0;
        if (media_get_sample(m, vi, sample, &s) == 1) {
            long n = media_to_annexb(m, &s, nalbuf, (long)sizeof nalbuf);
            sample++;
            if (n < 0) {
                snprintf(line, sizeof line, "%s: corrupt sample %ld", name, sample - 1);
                die_with(line);
            }
            got = vdec_feed(&v, nalbuf, n);
            if (got < 0) {
                snprintf(line, sizeof line,
                         "%s: the %s decoder refused this stream (%d) at sample %ld",
                         name, vt->codec_name, got, sample - 1);
                die_with(line);
            }
            if (!got) continue;                /* needs more input */
        } else {
            got = vdec_flush(&v);
            if (!got) break;                   /* end of stream */
        }

        /* The picture's presentation time. The decoders emit in display order,
         * so the container's pts for THIS sample is not necessarily this
         * picture's -- for streams without reorder they agree, and for the
         * rest the decode-order stamp is close enough to pace with and is
         * what we have. (Carrying pts through the decoder needs an API it
         * does not have; see the report.) */
        long long pts = s.pts_ns;
        if (first_pts < 0) first_pts = pts;

        /* RULE ONE: audio never runs more than AV_LEAD_NS ahead of the picture. */
        aplay_pump(&au, pts - first_pts + AV_LEAD_NS);
        long long played = aplay_played_ns(&au);
        if (played >= 0) avclock_audio(&clk, played + first_pts);

        /* RULE TWO: a late frame is decoded (done, above) but not painted. */
        int what;
        for (;;) {
            long long sleep_ns = 0;
            what = avclock_frame(&clk, pts, (long long)monotonic_ns(), &sleep_ns);
            if (what != AV_WAIT) break;
            if (sleep_ns > 2000000LL) sys_sleep_ms(sleep_ns / 1000000LL);
            else sys_yield();
            aplay_pump(&au, pts - first_pts + AV_LEAD_NS);
            played = aplay_played_ns(&au);
            if (played >= 0) avclock_audio(&clk, played + first_pts);
            if (pump_events()) goto done;
        }
        if (what == AV_DROP) continue;

        if (v.w > MAXW || v.h > MAXH) {
            snprintf(line, sizeof line, "%dx%d is larger than this viewer handles",
                     v.w, v.h);
            die_with(line);
        }
        yuv420_to_rgba(v.y, v.u, v.v, v.sy, v.sc, v.w, v.h, rgba);
        shown++;
        gui_clear(rgb(18, 18, 20));
        blit_fit(v.w, v.h);
        snprintf(line, sizeof line,
                 "%s  %s %dx%d  %s  %lld/%llds  shown %ld  dropped %lld  drift %+lldms",
                 name, vt->codec_name, v.w, v.h,
                 au.handle >= 0 ? "audio" : (au.why ? au.why : "silent"),
                 pts / 1000000000LL, media_duration_ns(m) / 1000000000LL,
                 shown, clk.frames_dropped, avclock_drift_ns(&clk) / 1000000LL);
        status(line);
        gui_flush();
    }
done:
    /* Let the card finish what it was given rather than cutting it off. */
    if (au.handle >= 0) snd_close(au.handle, 1), au.handle = -1;
    snprintf(line, sizeof line,
             "%s  finished: %ld pictures, %lld dropped, %lld re-syncs, "
             "mean drift %lldms",
             name, shown, clk.frames_dropped, clk.resyncs,
             clk.drift_n ? clk.drift_sum_ns / clk.drift_n / 1000000LL : 0);
    if (vt) vdec_close(&v);
    aplay_close(&au);
    media_close(m);
    die_with(line);
}

/* ------------------------------------------- raw elementary streams ----- */
/* An Annex B file has no container, so it has no timestamps either: there is
 * nothing to synchronise to and nothing to pace against except a frame rate
 * the stream may not state. Shown as fast as it decodes, which under TCG is
 * well under real time anyway. */
static void play_annexb(const unsigned char *data, long len, const char *name)
{
    char line[128];
    int is265 = 0;
    /* Distinguish by the first NAL's type. H.264 puts type in the low 5 bits
     * of one byte; HEVC uses a two-byte header with the type in bits 6..1 of
     * the first. A VPS (32) only exists in HEVC. */
    for (long i = 0; i + 5 < len && i < 4096; i++) {
        if (data[i] || data[i+1] || data[i+2] != 1) continue;
        unsigned b = data[i+3];
        if (((b >> 1) & 0x3F) == 32 && (b & 0x81) == 0) { is265 = 1; break; }
        if ((b & 0x1F) == 7 && (b & 0x80) == 0) { is265 = 0; break; }
    }

    for (;;) {
        vdec v;
        if (!vdec_open(&v, is265)) die_with("out of memory");
        long off = 0;
        int frames = 0;
        for (;;) {
            int got;
            if (off < len) {
                /* Hand it the rest of the file; the decoder consumes one
                 * access unit at a time and tells us how much it took. */
                long n = len - off;
                int used;
                if (is265) {
                    h265frame f; int g = 0;
                    used = h265_decode(v.d5, data + off, (int)n, &f, &g);
                    if (used >= 0 && g) { v.w=f.width; v.h=f.height; v.sy=f.stride_y;
                                          v.sc=f.stride_c; v.y=f.y; v.u=f.u; v.v=f.v; }
                    got = g;
                } else {
                    h264frame f; int g = 0;
                    used = h264_decode(v.d4, data + off, (int)n, &f, &g);
                    if (used >= 0 && g) { v.w=f.width; v.h=f.height; v.sy=f.stride_y;
                                          v.sc=f.stride_c; v.y=f.y; v.u=f.u; v.v=f.v; }
                    got = g;
                }
                if (used < 0) {
                    snprintf(line, sizeof line, "%s: corrupt stream at byte %ld", name, off);
                    vdec_close(&v);
                    die_with(line);
                }
                off += used;
                if (!got) { if (pump_events()) { vdec_close(&v); app_exit(0); }
                            if (off < len) continue; }
            } else got = 0;
            if (!got) { if (!vdec_flush(&v)) break; }

            if (v.w > MAXW || v.h > MAXH) {
                vdec_close(&v);
                die_with("this picture is larger than the viewer handles");
            }
            yuv420_to_rgba(v.y, v.u, v.v, v.sy, v.sc, v.w, v.h, rgba);
            frames++;
            gui_clear(rgb(18, 18, 20));
            blit_fit(v.w, v.h);
            snprintf(line, sizeof line, "%s  %dx%d  frame %d", name, v.w, v.h, frames);
            status(line);
            gui_flush();
            if (pump_events()) { vdec_close(&v); app_exit(0); }
        }
        vdec_close(&v);
        if (frames == 0) die_with("no frames in this stream");
    }
}

/* -------------------------------------------------------------- image --- */
static void show_image(const char *path, const char *name)
{
    int iw = 0, ih = 0;
    int ok = img_open(path, rgba, (int)sizeof rgba, &iw, &ih) == 0 && iw > 0 && ih > 0;
    char line[96];
    gui_clear(rgb(28, 28, 32));
    if (ok) {
        blit_fit(iw, ih);
        snprintf(line, sizeof line, "%s  %dx%d", name, iw, ih);
        status(line);
        gui_flush();
        /* A still needs no redraw; block on events instead of spinning. */
        for (;;) { if (pump_events()) app_exit(0); sys_yield(); }
    }
    die_with("cannot open this file");
}

/* --------------------------------------------------------------- main --- */
static const char *basename_of(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/') s = q + 1;
    return s;
}

/* Launched from the Dock there is no file to open, and the Finder can only
 * hand this app the ONE extension it registers. So offer what is there:
 * /media is where the shipped clips live. */
#define PICK_MAX 32
static void pick_from_media(char *out, int outmax)
{
    char names[PICK_MAX][64];
    int n = 0, sel = 0;
    int cnt = dir_count("/media");
    for (int i = 0; i < cnt && n < PICK_MAX; i++) {
        char b[64];
        int rc = dir_name("/media", i, b);
        if (rc == -2 || rc < 0) continue;              /* a directory, or none */
        snprintf(names[n], sizeof names[n], "%s", b);
        n++;
    }
    if (n == 0) die_with("nothing in /media to open");

    for (;;) {
        gui_clear(rgb(28, 28, 32));
        gui_text(16, 30, rgb(230, 230, 236), "Open from /media");
        for (int i = 0; i < n; i++) {
            unsigned c = (i == sel) ? rgb(255, 210, 120) : rgb(190, 190, 198);
            if (i == sel) gui_rect(12, 44 + i * 20, WINW - 24, 20, rgb(48, 48, 56));
            gui_text(24, 58 + i * 20, c, names[i]);
        }
        status("up/down to choose, Enter to open, Esc to quit");
        gui_flush();

        struct logit_event e;
        for (;;) {
            if (poll_event(&e)) {
                if (e.type == EV_CLOSE) app_exit(0);
                if (e.type == EV_KEY) {
                    if (e.a == 27) app_exit(0);
                    if (e.a == KEY_UP && sel > 0) sel--;
                    if (e.a == KEY_DOWN && sel < n - 1) sel++;
                    if (e.a == '\n' || e.a == '\r') {
                        snprintf(out, outmax, "/media/%s", names[sel]);
                        return;
                    }
                }
                break;
            }
            sys_yield();
        }
    }
}

void app_main(void)
{
    gui_create("Preview", WINW, WINH);

    char path[128];
    if (!(get_arg(path, sizeof path) > 0 && path[0]))
        pick_from_media(path, (int)sizeof path);
    const char *name = basename_of(path);

    /* Sniff before deciding. Only the video paths need the file in our own
     * memory; the image path lets the kernel do the read as it always has. */
    long len = 0;
    unsigned char *data = read_all(path, &len);
    if (data) {
        if (media_sniff(data, len) != MEDIA_CONT_UNKNOWN)
            play_container(data, len, name);         /* never returns */
        if (looks_like_annexb(data, len))
            play_annexb(data, len, name);            /* never returns */
        free(data);
    }
    show_image(path, name);                          /* never returns */
}
