/* Preview: opens a file and shows what is in it -- a still image, an ANIMATED
 * image, an audio track, a raw video elementary stream, or a REAL CONTAINER
 * with video and audio in it.
 *
 * The halves are deliberately asymmetric, and it is worth saying why.
 *
 * EVERYTHING IS DECODED HERE, IN RING 3, from c/lib/{image,video,audio,media}.
 * The image codecs also live behind SYS_IMG_DECODE, and Preview used to call
 * it -- but that syscall hands back ONE canvas, and an animation is a list of
 * canvases with a delay each. Rather than widen the kernel's image ABI (and
 * put a per-frame decode of attacker-chosen bytes under the big lock, thirty
 * times a second, for the whole length of a clip), this app links the same
 * c/lib/image sources /bin/imgcheck links and calls img_decode_anim directly.
 * That is the direction M17 already took the browser's render pipeline and the
 * direction c/lib/{video,audio,media} were written for: the kernel provides
 * primitives, the app does the work. So this app links the demuxers, both
 * video decoders, the audio decoders and the image codecs, and only asks the
 * kernel to put pixels on the screen and samples in the sound ring.
 *
 * WHICH PATH RUNS IS DECIDED BY SNIFFING THE FILE, NOT BY ITS NAME. An MP4
 * called .h264 plays as an MP4; a PNG called .mp4 opens as a PNG. The order is
 * container, then image, then audio, then elementary stream, and each step
 * asks the library that owns the format rather than a table kept here. (Image
 * before audio is not arbitrary: a JPEG is full of 0xFF bytes and the MP3
 * frame-sync heuristic has to scan for exactly those.)
 *
 * A FILE THIS MACHINE CANNOT DECODE IS NAMED, NOT BLANKED. Every refusal says
 * what the file is and which part of it has no decoder here -- "video is vp9",
 * "audio is opus", "no sound card", "AAC object type 5 (HE-AAC)" -- because a
 * viewer that shows an empty window teaches the user nothing.
 *
 * SYNCHRONISATION. See the long note at the top of c/lib/media/avclock.c for
 * the policy and the reasoning. The two rules this file implements are:
 * audio is written AHEAD OF THE DECODED VIDEO POSITION BY AT MOST AV_LEAD_NS
 * and never further (that is what keeps the two streams together when decode
 * falls behind, which on this machine it does), and a late frame is still
 * decoded but not converted or blitted.
 *
 * ANIMATION TIMING. A frame's delay is a DEADLINE from the start of the loop,
 * never a sleep between frames: sleeping delay_ms per frame adds that frame's
 * decode, convert and blit cost to every delay, so a 1490 ms animation drifts
 * longer every time round. The player prints the declared and the measured
 * duration of each loop on the console, so "it honours the delays" is a number
 * a test can read back rather than something to take on trust
 * (tests/qmp/qmp_preview.py asserts it; make test-preview-negctl removes the
 * wait and requires that assertion to fail).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logit.h"
#include "logit_sniff.h"
#include "img.h"
#include "h264.h"
#include "h265.h"
#include "audio.h"
#include "aac.h"
#include "media.h"

/* c/lib/image allocates through the kernel heap's names. In ring 3 they are
 * mini-libc's -- the same two-line shim tests/unit/imgcheck.c uses to build
 * the identical sources as /bin/imgcheck. */
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

#define WINW 760
#define WINH 560
#define CONTH (WINH - 30)    /* picture area; the status line owns the rest */
#define MAXW 1280            /* up to screen resolution */
#define MAXH 800
static unsigned char rgba[MAXW * MAXH * 4];

/* How far audio may run ahead of the picture. It is the size of the cushion
 * that absorbs a slow frame AND the worst-case A/V error if the pipeline
 * starves hard enough to drain it; make test-avsync measures both. */
#define AV_LEAD_NS   150000000LL
#define ABUF_FRAMES  1024

/* What pump_events() found. `back` exists because a viewer you can only leave
 * by quitting it makes browsing ten files ten launches. */
#define PUMP_NONE 0
#define PUMP_QUIT 1
#define PUMP_BACK 2

static int g_from_picker;    /* 1 when there is a list to go back to */

/* THE MEASUREMENT BUILD. PREVIEW_CLI enters the same player from the SHELL
 * rather than the Dock -- one file named on the command line, one animation
 * loop, then exit -- so tests/boot/run-preview-timing.sh can time it over the
 * serial console with no window manager driving involved. It is the same
 * source and the same loop; only the way in and the way out differ, which is
 * the whole point: a timing number measured on a different player would say
 * nothing about this one. */
#ifdef PREVIEW_CLI
static const int g_one_loop = 1;
#else
static const int g_one_loop = 0;
#endif

/* ------------------------------------------------------------ console ---- */
/* Preview's window is the product; this is the instrument. Every line a test
 * reads back is printed here, prefixed, and flushed -- a GUI app's stdout is
 * the tty, which is the serial console. */
/* ONE write per line, deliberately. printf() through mini-libc's FILE can hand
 * the tty a line in several pieces, and the kernel's own kprintf goes to the
 * same serial port -- so a line assembled in pieces comes back with a
 * `[time]` or a `[sched]` spliced into the middle of a FILE NAME, and a reader
 * that then looks that name up finds nothing. A single SYS_WRITE is atomic
 * against kprintf (both run under the big lock). */
static void plog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    sys_write(1, buf, n);
}

/* ------------------------------------------------------------- colour ---- */
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
/* Aspect-fit w x h into the picture area, centred; gui_blit scales
 * nearest-neighbour, so the source colours survive exactly. */
static void blit_fit_src(const unsigned char *src, int iw, int ih)
{
    if (iw <= 0 || ih <= 0) return;
    int dw = WINW, dh = (int)((long)WINW * ih / iw);
    if (dh > CONTH) { dh = CONTH; dw = (int)((long)CONTH * iw / ih); }
    gui_blit((WINW - dw) / 2, (CONTH - dh) / 2, dw, dh, src, iw, ih);
}
static void blit_fit(int iw, int ih) { blit_fit_src(rgba, iw, ih); }

/* Returns PUMP_*. Called between frames so a playing clip still answers the
 * close button. Esc and the close button quit; Backspace goes back to the
 * list when there is one. */
static int pump_events(void)
{
    struct logit_event e;
    int r = PUMP_NONE;
    while (poll_event(&e)) {
        if (e.type == EV_CLOSE) return PUMP_QUIT;
        if (e.type == EV_KEY) {
            if (e.a == 27) return PUMP_QUIT;
            if ((e.a == 8 || e.a == 127) && g_from_picker) r = PUMP_BACK;
        }
    }
    return r;
}

static void status(const char *s)
{
    gui_text(12, WINH - 8, rgb(150, 150, 158), s);
}

static const char *hint(void)
{
    return g_from_picker ? "Backspace for the list, Esc to quit"
                         : "Esc or the close button to quit";
}

/* Put a message up and stay there until the user dismisses it. Returns PUMP_*
 * so the caller can go back to the list rather than dying on a bad file --
 * "cannot decode this" is a reason to pick another one, not to quit. */
static int show_message(const char *msg)
{
    gui_clear(rgb(28, 28, 32));
    gui_text(16, 30, rgb(230, 230, 236), msg);
    status(hint());
    gui_flush();
    if (g_one_loop) return PUMP_QUIT;      /* the measurement build never waits */
    for (;;) {
        int p = pump_events();
        if (p) return p;
        sys_sleep_ms(20);
    }
}

static int fail_with(const char *msg)
{
    plog("preview: error %s\n", msg);
    return show_message(msg);
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

/* Does an image codec claim these bytes? img_decode_anim answers -1 for both
 * "not mine" and "mine and broken", and those need different messages: a
 * progressive JPEG must say so rather than falling through to the audio
 * sniffer and ending up as "nothing here decodes it". So the magic is checked
 * separately, through the same table the Terminal and /bin/show use.
 *
 * ICO/CUR is the one c/apps/coreutils/logit_sniff.h has no id for, and its
 * first three bytes read as an Annex B start code -- so it is spelled out
 * here. (Reported to that line rather than edited in.) */
static int is_ico(const unsigned char *b, long n)
{
    return n >= 6 && !b[0] && !b[1] && (b[2] == 1 || b[2] == 2) && !b[3] && (b[4] | b[5]);
}
static int is_image_magic(const unsigned char *b, long n)
{
    int k = sniff_id(b, n < SNIFF_PREFIX ? (int)n : SNIFF_PREFIX);
    return sniff_class(k) == SNC_IMAGE || is_ico(b, n);
}

static const char *audio_err_name(int e)
{
    switch (e) {
    case AUDIO_ERR_CORRUPT:     return "the bytes are not a valid stream";
    case AUDIO_ERR_UNSUPPORTED: return "a feature this decoder does not do";
    case AUDIO_ERR_OOM:         return "out of memory";
    case AUDIO_ERR_RANGE:       return "a header field that cannot be true";
    default:                    return "unknown error";
    }
}

/* ------------------------------------------------------ video decoder --- */
/* One object for both codecs. The two APIs were deliberately given the same
 * shape (see the note at the top of h265.h), so the only thing this adds is
 * not writing the player loop twice.
 *
 * NOTE ON MAIN 10. h265frame grew a 16-bit view (y16/u16/v16 + bit_depth) when
 * the HEVC decoder learned 10-bit, and kept y/u/v as an always-populated 8-bit
 * DISPLAY view. A viewer wants exactly that view -- the panel is 8 bits per
 * channel -- so this consumer is unchanged by the widening and reads bit_depth
 * only to say so on the status line. Nothing here should ever be "upgraded" to
 * y16: down-converting is the decoder's job and it already did it. */
typedef struct {
    int        is265;
    h264dec   *d4;
    h265dec   *d5;
    int        w, h, sy, sc, depth;
    const unsigned char *y, *u, *v;
} vdec;

static int vdec_open(vdec *v, int is265)
{
    memset(v, 0, sizeof *v);
    v->is265 = is265;
    v->depth = 8;
    if (is265) { v->d5 = h265_open(); return v->d5 != 0; }
    v->d4 = h264_open(); return v->d4 != 0;
}
static void vdec_close(vdec *v)
{
    if (v->d4) h264_close(v->d4);
    if (v->d5) h265_close(v->d5);
    v->d4 = 0; v->d5 = 0;
}
/* Decode from `p+*off`, advancing *off by whatever was consumed. Returns 1 when
 * a picture came out, 0 when the decoder wants more input, negative on error.
 *
 * THE CALLER MUST KEEP CALLING UNTIL THE BUFFER IS EMPTY, and that is the whole
 * reason this is shaped as a cursor rather than "feed one sample, get one
 * picture". Both decoders emit a picture when they see the START of the NEXT
 * one, so the call that hands over access unit N+1 typically returns picture N
 * having consumed only the first few bytes -- and a loop that then moves on to
 * sample N+2 throws away everything after them. It does not look like a bug:
 * it plays, at half the frame rate, with the timestamps still perfectly paced.
 * (It was one, until the on-device run showed 15 pictures out of 30.) */
static int vdec_pump(vdec *v, const unsigned char *p, long n, long *off)
{
    int got = 0, used;
    if (*off >= n) return 0;
    if (v->is265) {
        h265frame f;
        used = h265_decode(v->d5, p + *off, (int)(n - *off), &f, &got);
        if (used < 0) return used;
        if (got) { v->w = f.width; v->h = f.height; v->sy = f.stride_y;
                   v->sc = f.stride_c; v->y = f.y; v->u = f.u; v->v = f.v;
                   v->depth = f.bit_depth; }
    } else {
        h264frame f;
        used = h264_decode(v->d4, p + *off, (int)(n - *off), &f, &got);
        if (used < 0) return used;
        if (got) { v->w = f.width; v->h = f.height; v->sy = f.stride_y;
                   v->sc = f.stride_c; v->y = f.y; v->u = f.u; v->v = f.v; }
    }
    if (used == 0 && !got) { *off = n; return 0; }   /* no progress: give up on it */
    *off += used;
    return got;
}
static int vdec_flush(vdec *v)
{
    if (v->is265) {
        h265frame f;
        if (h265_flush(v->d5, &f) != 1) return 0;
        v->w = f.width; v->h = f.height; v->sy = f.stride_y; v->sc = f.stride_c;
        v->y = f.y; v->u = f.u; v->v = f.v; v->depth = f.bit_depth;
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
    unsigned char       *es;      /* an assembled stream this object OWNS, or 0 */
    const unsigned char *src;     /* what the decoder reads: `es`, or the file */
    long                 src_len;
    adec          *dec;
    int            handle;    /* the sound stream, or < 0 */
    int            rate, ch;
    long           total_frames;  /* -1 when nothing states it */
    long long      frames_written;
    long long      written_ns;
    int            eof;
    int            peak[2];   /* level meter, decayed per redraw */
    short          buf[ABUF_FRAMES * 2];
    const char    *why;       /* why there is no audio, for the status line */
    char           whybuf[96];
    const char    *fmt;       /* what it turned out to be */
} aplay;

static const char *fmt_name(audio_format f)
{
    const char *n = audio_format_name(f);
    return n ? n : "audio";
}

/* --- assembling an elementary stream out of a container -------------------
 * An audio elementary stream out of a container is the track's configuration
 * bytes followed by its samples. For FLAC that is literally the "fLaC" magic
 * and STREAMINFO from CodecPrivate followed by the frames -- a playable .flac.
 * For MP3 there is no configuration and the frames are the file.
 *
 * AAC and PCM are the two that need a real repack, and both are repacked into
 * a format c/lib/audio already reads rather than into a second decode path:
 *
 *   AAC   MP4 and Matroska store raw_data_blocks with the AudioSpecificConfig
 *         hoisted into the sample description. Putting the seven-byte ADTS
 *         header back on each block yields a file audio_sniff() calls AAC and
 *         adec_open() decodes -- so the container case and a bare .aac file
 *         are the SAME code below this line. The alternative (aac_open_asc
 *         plus a per-sample pull) would be a second pump loop, a second
 *         end-of-stream rule and a second place for the level meter to be
 *         wrong.
 *
 *   PCM   is samples with no framing at all, so it gets a 44-byte WAV header.
 *         Big-endian is byte-swapped on the way in; there is no S16BE path in
 *         wav.c and inventing one here would be a decoder in a viewer. */
static void put32(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put16(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* ADTS from an AudioSpecificConfig. Returns 0, or -1 when the ASC says
 * something aac.c documents as unsupported -- object types 5/29 (HE-AAC) are
 * refused there on purpose, because decoding the core alone is the right
 * samples at half the rate, which sounds nearly right and is wrong. */
static int asc_parse(const unsigned char *asc, int len, int *obj, int *fi, int *cc)
{
    if (len < 2) return -1;
    unsigned bits = ((unsigned)asc[0] << 8) | asc[1];
    int o = (int)(bits >> 11) & 0x1F;
    int f = (int)(bits >> 7) & 0x0F;
    int c = (int)(bits >> 3) & 0x0F;
    if (o == 31) return -1;                 /* 6-bit escape: not AAC-LC anyway */
    if (f > 12 || c > 7) return -1;
    *obj = o; *fi = f; *cc = c;
    return 0;
}

static void adts_header(unsigned char *h, int obj, int fi, int cc, long framelen)
{
    unsigned long fl = (unsigned long)framelen;
    h[0] = 0xFF;
    h[1] = 0xF1;                                     /* MPEG-4, layer 0, no CRC */
    h[2] = (unsigned char)(((obj - 1) << 6) | (fi << 2) | ((cc >> 2) & 1));
    h[3] = (unsigned char)(((cc & 3) << 6) | (unsigned char)((fl >> 11) & 3));
    h[4] = (unsigned char)((fl >> 3) & 0xFF);
    h[5] = (unsigned char)(((fl & 7) << 5) | 0x1F);  /* buffer fullness 0x7FF */
    h[6] = 0xFC;                                     /* 1 raw_data_block */
}

/* How many bytes the assembled stream needs, and the per-sample prefix size. */
static long es_size(mdemux *m, int ti, const media_track *t, int *hdr_per_sample)
{
    long need = 0;
    *hdr_per_sample = 0;
    if (t->codec == MEDIA_CODEC_FLAC) need += t->extradata_len;
    if (t->codec == MEDIA_CODEC_AAC)  *hdr_per_sample = 7;
    if (t->codec == MEDIA_CODEC_PCM_S16LE || t->codec == MEDIA_CODEC_PCM_S16BE)
        need += 44;
    media_sample s;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++)
        need += s.size + *hdr_per_sample;
    return need;
}

static int aplay_assemble(aplay *a, mdemux *m, int ti, const media_track *t)
{
    int obj = 2, fi = 4, cc = 2;
    if (t->codec == MEDIA_CODEC_AAC) {
        if (t->extradata_len < 2) { a->why = "AAC with no configuration"; return -1; }
        if (asc_parse(t->extradata, t->extradata_len, &obj, &fi, &cc) != 0 || obj != 2) {
            snprintf(a->whybuf, sizeof a->whybuf,
                     "AAC object type %d -- only AAC-LC here", obj);
            a->why = a->whybuf;
            return -1;
        }
        if (cc == 0) { a->why = "AAC with a program config element"; return -1; }
    }
    int per;
    long need = es_size(m, ti, t, &per);
    if (need <= 0) { a->why = "the audio track is empty"; return -1; }
    a->es = malloc((unsigned long)need + 1);
    if (!a->es) { a->why = "out of memory"; return -1; }
    a->src = a->es;

    long at = 0;
    if (t->codec == MEDIA_CODEC_FLAC && t->extradata_len > 0) {
        memcpy(a->es, t->extradata, (unsigned long)t->extradata_len);
        at = t->extradata_len;
    }
    if (t->codec == MEDIA_CODEC_PCM_S16LE || t->codec == MEDIA_CODEC_PCM_S16BE) {
        int ch = t->channels > 0 ? t->channels : 2;
        int rate = t->rate > 0 ? t->rate : 44100;
        long pcm = need - 44;
        unsigned char *h = a->es;
        memcpy(h, "RIFF", 4);      put32(h + 4, (unsigned)(36 + pcm));
        memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
        put16(h + 20, 1); put16(h + 22, (unsigned)ch);
        put32(h + 24, (unsigned)rate);
        put32(h + 28, (unsigned)(rate * ch * 2));
        put16(h + 32, (unsigned)(ch * 2)); put16(h + 34, 16);
        memcpy(h + 36, "data", 4); put32(h + 40, (unsigned)pcm);
        at = 44;
    }

    media_sample s;
    for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
        if (per) { adts_header(a->es + at, obj, fi, cc, s.size + per); at += per; }
        memcpy(a->es + at, s.data, (unsigned long)s.size);
        if (t->codec == MEDIA_CODEC_PCM_S16BE)
            for (long i = 0; i + 1 < s.size; i += 2) {
                unsigned char tmp = a->es[at + i];
                a->es[at + i] = a->es[at + i + 1];
                a->es[at + i + 1] = tmp;
            }
        at += s.size;
    }
    a->src_len = at;
    return 0;
}

/* Open the decoder + the sound stream over a->src. Sets a->why and leaves
 * handle < 0 on every failure; never returns without an explanation. */
static void aplay_start(aplay *a)
{
    int err = 0;
    a->dec = adec_open(a->src, a->src_len, &err);
    if (!a->dec) {
        snprintf(a->whybuf, sizeof a->whybuf, "audio: %s", audio_err_name(err));
        a->why = a->whybuf;
        return;
    }
    a->fmt = fmt_name(adec_format(a->dec));
    if (adec_info(a->dec, &a->rate, &a->ch) != AUDIO_OK || a->rate <= 0) {
        a->why = "the audio header does not state a rate";
        adec_close(a->dec); a->dec = 0; return;
    }
    if (a->ch > 2) a->ch = 2;
    a->total_frames = adec_duration_frames(a->dec);

    struct logit_sndinfo si;
    if (snd_info(&si) != 1) { a->why = "no sound card"; return; }
    a->handle = snd_open_s16((unsigned)a->rate, (unsigned)a->ch);
    if (a->handle < 0) a->why = "the sound device would not open";
}

/* A track out of a container. */
static void aplay_open_track(aplay *a, mdemux *m, int ti)
{
    memset(a, 0, sizeof *a);
    a->handle = -1;
    a->total_frames = -1;
    if (ti < 0) { a->why = "no audio track"; return; }
    const media_track *t = media_track_info(m, ti);
    switch (t->codec) {
    case MEDIA_CODEC_MP3: case MEDIA_CODEC_FLAC: case MEDIA_CODEC_AAC:
    case MEDIA_CODEC_PCM_S16LE: case MEDIA_CODEC_PCM_S16BE:
        break;
    default:
        /* Named, not swallowed. Opus, Vorbis and AC-3 have no decoder in this
         * system (c/lib/audio is WAV/FLAC/MP3/AAC) and saying which one it is
         * beats a silent clip. */
        snprintf(a->whybuf, sizeof a->whybuf,
                 "audio is %s -- no decoder for it here", t->codec_name);
        a->why = a->whybuf;
        return;
    }
    if (aplay_assemble(a, m, ti, t) != 0) return;
    aplay_start(a);
}

/* A bare audio file: the bytes ARE the elementary stream. Nothing is copied --
 * adec_open reads `data` in place, and open_path owns it for longer than the
 * decoder lives. `es` stays 0 so aplay_close does not free a borrowed buffer. */
static void aplay_open_file(aplay *a, const unsigned char *data, long len)
{
    memset(a, 0, sizeof *a);
    a->handle = -1;
    a->total_frames = -1;
    a->src = data;
    a->src_len = len;
    aplay_start(a);
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
    if (!a->dec || a->handle < 0 || a->eof) return;
    int bytes_per_frame = a->ch * 2;
    while (a->written_ns < upto_ns) {
        int room = snd_avail(a->handle);
        if (room < ABUF_FRAMES * bytes_per_frame) break;
        long got = adec_read(a->dec, a->buf, ABUF_FRAMES);
        if (got <= 0) { a->eof = 1; break; }
        /* The level meter reads the samples on their way out, which is the
         * only place they exist: nothing else in this app ever looks at the
         * PCM, and a meter fed from anywhere else would be measuring a
         * different signal from the one the card is playing. */
        for (long i = 0; i < got; i++)
            for (int c = 0; c < a->ch; c++) {
                int s = a->buf[i * a->ch + c];
                if (s < 0) s = -s;
                if (s > a->peak[c]) a->peak[c] = s;
            }
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

/* ---------------------------------------------------------- audio UI ---- */
/* An audio-only file still has to put something on the screen, and "nothing"
 * is not it. Format, position against duration, and a level meter reading the
 * samples the card is actually being handed. */
/* Three segments, not a per-pixel loop: every gui_rect is an int 0x80, and a
 * 700-pixel bar drawn a column at a time would cost 700 syscalls per redraw. */
static void meter_bar(int x, int y, int w, int h, int peak)
{
    gui_rect(x, y, w, h, rgb(38, 38, 46));
    int fill = peak > 0 ? (int)((long)w * peak / 32767) : 0;
    if (fill > w) fill = w;
    int g = w * 70 / 100, amb = w * 90 / 100;
    if (fill > 0)   gui_rect(x, y, fill < g ? fill : g, h, rgb(70, 210, 120));
    if (fill > g)   gui_rect(x + g, y, (fill < amb ? fill : amb) - g, h, rgb(240, 200, 80));
    if (fill > amb) gui_rect(x + amb, y, fill - amb, h, rgb(240, 90, 80));
}

static void draw_audio_ui(aplay *a, const char *name, const char *what,
                          long long pos_ns, long long total_ns)
{
    char line[192];
    gui_clear(rgb(18, 18, 22));
    gui_text(24, 60, rgb(235, 235, 240), name);
    snprintf(line, sizeof line, "%s  %d Hz  %d ch", what, a->rate, a->ch);
    gui_text(24, 92, rgb(170, 172, 182), line);

    /* position */
    long long p = pos_ns < 0 ? 0 : pos_ns;
    if (total_ns > 0) {
        snprintf(line, sizeof line, "%lld:%02lld / %lld:%02lld",
                 p / 60000000000LL, (p / 1000000000LL) % 60,
                 total_ns / 60000000000LL, (total_ns / 1000000000LL) % 60);
    } else {
        snprintf(line, sizeof line, "%lld:%02lld", p / 60000000000LL,
                 (p / 1000000000LL) % 60);
    }
    gui_text(24, 150, rgb(220, 222, 230), line);

    int bx = 24, bw = WINW - 48;
    gui_rect(bx, 170, bw, 8, rgb(40, 40, 48));
    if (total_ns > 0) {
        long long f = p > total_ns ? total_ns : p;
        gui_rect(bx, 170, (int)((long long)bw * f / total_ns), 8, rgb(110, 160, 250));
    }

    gui_text(24, 230, rgb(150, 152, 162), "level");
    meter_bar(bx, 244, bw, 16, a->peak[0]);
    if (a->ch > 1) meter_bar(bx, 268, bw, 16, a->peak[1]);
    /* Decay so the bar falls back when the track goes quiet; the peak is
     * re-raised by every pump. */
    for (int c = 0; c < 2; c++) a->peak[c] = a->peak[c] * 3 / 4;

    if (a->why) gui_text(24, 320, rgb(240, 170, 120), a->why);
}

/* Keeping the final frame up with the final numbers is part of the loop's
 * contract, not a separate screen -- hence the forward declaration. */
static int hold_audio_ui(aplay *a, const char *name, const char *what,
                         long long total_ns);

/* Play a decoder that is already open. Returns PUMP_*. */
static int play_audio_loop(aplay *a, const char *name, const char *what,
                           long long total_ns)
{
    char line[192];
    if (total_ns <= 0 && a->total_frames > 0 && a->rate > 0)
        total_ns = (long long)a->total_frames * 1000000000LL / a->rate;

    plog("preview: audio %s fmt=%s rate=%d ch=%d dur_ms=%lld sound=%s\n",
         name, what, a->rate, a->ch, total_ns / 1000000LL,
         a->handle >= 0 ? "yes" : (a->why ? a->why : "no"));

    unsigned long long t0 = monotonic_ns();
    for (;;) {
        int p = pump_events();
        if (p) return p;
        aplay_pump(a, a->written_ns + AV_LEAD_NS);
        long long pos = aplay_played_ns(a);
        if (pos < 0) pos = (long long)(monotonic_ns() - t0);   /* no card: elapsed */
        draw_audio_ui(a, name, what, pos, total_ns);
        snprintf(line, sizeof line, "%s   %s", hint(),
                 a->eof ? "end of track" : "playing");
        status(line);
        gui_flush();
        if (a->eof && (a->handle < 0 || pos >= a->written_ns)) break;
        if (!a->dec) break;
        sys_sleep_ms(50);
    }
    plog("preview: audio %s done played_ms=%lld\n", name,
         (a->handle >= 0 ? aplay_played_ns(a) : 0) / 1000000LL);
    /* Stay on screen with the final numbers rather than vanishing. */
    return hold_audio_ui(a, name, what, total_ns);
}

static int hold_audio_ui(aplay *a, const char *name, const char *what,
                         long long total_ns)
{
    char line[192];
    draw_audio_ui(a, name, what, total_ns, total_ns);
    snprintf(line, sizeof line, "finished -- %s", hint());
    status(line);
    gui_flush();
    if (g_one_loop) return PUMP_QUIT;
    for (;;) {
        int p = pump_events();
        if (p) return p;
        sys_sleep_ms(30);
    }
}

/* ------------------------------------------------------ the container --- */
static int play_container(unsigned char *data, long len, const char *name)
{
    char line[224];
    int err = 0;
    mdemux *m = media_open(data, len, &err);
    if (!m) {
        snprintf(line, sizeof line, "%s: cannot open this container (%s)",
                 name, media_strerror(err));
        return fail_with(line);
    }

    int vi = media_find_track(m, MEDIA_TRACK_VIDEO);
    int ai = media_find_track(m, MEDIA_TRACK_AUDIO);
    if (vi < 0 && ai < 0) {
        media_close(m);
        return fail_with("this file has no video and no audio track");
    }

    const media_track *vt = vi >= 0 ? media_track_info(m, vi) : 0;
    int vplay = vt && (vt->codec == MEDIA_CODEC_H264 || vt->codec == MEDIA_CODEC_H265);
    char vwhy[96];
    vwhy[0] = 0;
    if (vt && !vplay)
        snprintf(vwhy, sizeof vwhy, "video is %s -- no decoder for it here",
                 vt->codec_name);

    aplay au;
    aplay_open_track(&au, m, ai);

    plog("preview: container %s kind=%s video=%s audio=%s dur_ms=%lld\n",
         name, media_container_name(media_kind(m)),
         vt ? vt->codec_name : "none",
         ai >= 0 ? media_track_info(m, ai)->codec_name : "none",
         media_duration_ns(m) / 1000000LL);

    /* Neither half is playable: say so about BOTH, because "no video decoder"
     * on a file that also has no audio decoder is half an answer. */
    if (!vplay && au.handle < 0 && !au.dec) {
        snprintf(line, sizeof line, "%s: %s%s%s", name,
                 vwhy[0] ? vwhy : "no video track",
                 au.why ? "; " : "", au.why ? au.why : "");
        aplay_close(&au);
        media_close(m);
        return fail_with(line);
    }

    /* Video we cannot decode but audio we can: play the audio and say why
     * there is no picture. */
    if (!vplay) {
        long long dur = media_duration_ns(m);
        const char *what = au.fmt ? au.fmt : "audio";
        static char both[224];
        if (vwhy[0]) {
            snprintf(both, sizeof both, "%s%s%s", vwhy,
                     au.why ? "; " : "", au.why ? au.why : "");
            au.why = both;
        }
        int p = play_audio_loop(&au, name, what, dur);
        aplay_close(&au);
        media_close(m);
        return p;
    }

    avclock clk;
    avclock_init(&clk, au.handle >= 0);

    vdec v;
    static unsigned char nalbuf[1 << 20];
    long pend_len = 0, pend_off = 0;

    if (!vdec_open(&v, vt->codec == MEDIA_CODEC_H265)) {
        aplay_close(&au); media_close(m);
        return fail_with("out of memory opening the video decoder");
    }
    /* The parameter sets, once, before anything else: MP4 and Matroska
     * both hoist them out of the samples into the sample description. */
    {
        long hn = media_annexb_headers(m, vi, nalbuf, (long)sizeof nalbuf);
        if (hn > 0) { pend_len = hn; pend_off = 0; }
    }

    long sample = 0, shown = 0;
    long long first_pts = -1;
    int rc = PUMP_NONE;

    /* Presentation stamps, in DECODE order, waiting for the picture they
     * belong to. Both decoders emit pictures a call or more after the access
     * unit that started them, and neither carries a caller token through -- so
     * the player has to remember. A FIFO is exactly right while decode order is
     * display order, which is every stream these decoders accept today (H.264
     * baseline has no B slices, and the HEVC fixtures here are bframes=0). The
     * moment reordering arrives this has to become a per-picture opaque handed
     * through the decoder; there is no way to do it from out here. */
    long long ptsq[64];
    int ptsq_head = 0, ptsq_n = 0;
    long long cur_pts = 0;

    for (;;) {
        rc = pump_events();
        if (rc) break;

        int got = 0;
        if (pend_off < pend_len) {
            got = vdec_pump(&v, nalbuf, pend_len, &pend_off);
            if (got < 0) {
                snprintf(line, sizeof line,
                         "%s: the %s decoder refused this stream (%d) at sample %ld",
                         name, vt->codec_name, got, sample);
                vdec_close(&v); aplay_close(&au); media_close(m);
                return fail_with(line);
            }
            if (!got) continue;                /* consumed, no picture yet */
        } else {
            media_sample s;
            if (media_get_sample(m, vi, sample, &s) == 1) {
                long n = media_to_annexb(m, &s, nalbuf, (long)sizeof nalbuf);
                if (n < 0) {
                    snprintf(line, sizeof line, "%s: corrupt sample %ld", name, sample);
                    vdec_close(&v); aplay_close(&au); media_close(m);
                    return fail_with(line);
                }
                pend_len = n; pend_off = 0;
                if (ptsq_n < (int)(sizeof ptsq / sizeof ptsq[0]))
                    ptsq[(ptsq_head + ptsq_n++) % (int)(sizeof ptsq / sizeof ptsq[0])] = s.pts_ns;
                sample++;
                continue;
            }
            got = vdec_flush(&v);
            if (!got) break;                   /* end of stream */
        }

        /* The picture's presentation time, taken off the head of the queue. */
        if (ptsq_n > 0) {
            cur_pts = ptsq[ptsq_head];
            ptsq_head = (ptsq_head + 1) % (int)(sizeof ptsq / sizeof ptsq[0]);
            ptsq_n--;
        }
        long long pts = cur_pts;
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
            rc = pump_events();
            if (rc) goto done;
        }
        if (what == AV_DROP) continue;

        if (v.w > MAXW || v.h > MAXH) {
            snprintf(line, sizeof line, "%dx%d is larger than this viewer handles",
                     v.w, v.h);
            vdec_close(&v); aplay_close(&au); media_close(m);
            return fail_with(line);
        }
        yuv420_to_rgba(v.y, v.u, v.v, v.sy, v.sc, v.w, v.h, rgba);
        shown++;
        gui_clear(rgb(18, 18, 20));
        blit_fit(v.w, v.h);
        snprintf(line, sizeof line,
                 "%s  %s %dx%d %d-bit  %s  %lld/%llds  shown %ld  dropped %lld  drift %+lldms",
                 name, vt->codec_name, v.w, v.h, v.depth,
                 au.handle >= 0 ? "audio" : (au.why ? au.why : "silent"),
                 pts / 1000000000LL, media_duration_ns(m) / 1000000000LL,
                 shown, clk.frames_dropped, avclock_drift_ns(&clk) / 1000000LL);
        status(line);
        gui_flush();
    }
done:
    /* Let the card finish what it was given rather than cutting it off. */
    if (au.handle >= 0) snd_close(au.handle, 1), au.handle = -1;
    plog("preview: video %s codec=%s %dx%d depth=%d shown=%ld dropped=%lld "
         "resyncs=%lld drift_mean_ms=%lld drift_max_ms=%lld audio=%s\n",
         name, vt->codec_name, v.w, v.h, v.depth, shown, clk.frames_dropped,
         clk.resyncs, clk.drift_n ? clk.drift_sum_ns / clk.drift_n / 1000000LL : 0,
         clk.drift_max_ns / 1000000LL, au.dec ? (au.fmt ? au.fmt : "yes") : "none");
    snprintf(line, sizeof line,
             "%s  finished: %ld pictures, %lld dropped, %lld re-syncs, "
             "mean drift %lldms",
             name, shown, clk.frames_dropped, clk.resyncs,
             clk.drift_n ? clk.drift_sum_ns / clk.drift_n / 1000000LL : 0);
    vdec_close(&v);
    aplay_close(&au);
    media_close(m);
    if (rc) return rc;
    return show_message(line);
}

/* ------------------------------------------- raw elementary streams ----- */
/* An Annex B file has no container, so it has no timestamps either: there is
 * nothing to synchronise to and nothing to pace against except a frame rate
 * the stream may not state. Shown as fast as it decodes, which under TCG is
 * well under real time anyway. */
static int play_annexb(const unsigned char *data, long len, const char *name)
{
    char line[160];
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
    plog("preview: stream %s codec=%s\n", name, is265 ? "hevc" : "h264");

    for (;;) {
        vdec v;
        if (!vdec_open(&v, is265)) return fail_with("out of memory");
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
                                          v.sc=f.stride_c; v.y=f.y; v.u=f.u; v.v=f.v;
                                          v.depth=f.bit_depth; }
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
                    return fail_with(line);
                }
                off += used;
                if (!got) {
                    int p = pump_events();
                    if (p) { vdec_close(&v); return p; }
                    if (off < len) continue;
                }
            } else got = 0;
            if (!got) { if (!vdec_flush(&v)) break; }

            if (v.w > MAXW || v.h > MAXH) {
                vdec_close(&v);
                return fail_with("this picture is larger than the viewer handles");
            }
            yuv420_to_rgba(v.y, v.u, v.v, v.sy, v.sc, v.w, v.h, rgba);
            frames++;
            gui_clear(rgb(18, 18, 20));
            blit_fit(v.w, v.h);
            snprintf(line, sizeof line, "%s  %s %dx%d %d-bit  frame %d   %s",
                     name, is265 ? "hevc" : "h264", v.w, v.h, v.depth, frames, hint());
            status(line);
            gui_flush();
            int p = pump_events();
            if (p) { vdec_close(&v); return p; }
        }
        vdec_close(&v);
        if (frames == 0) return fail_with("no frames in this stream");
        plog("preview: stream %s frames=%d\n", name, frames);
    }
}

/* -------------------------------------------------------------- image --- */
/* A still is an animation with one frame, which is why there is one path.
 *
 * The delays are DEADLINES measured from the start of the loop rather than
 * sleeps between frames -- see the note at the top of this file. */
static int play_image(const unsigned char *data, long len, const char *name)
{
    char line[192];
    struct img_anim a;
    if (img_decode_anim(data, (int)len, &a) != 0) {
        int k = sniff_id(data, len < SNIFF_PREFIX ? (int)len : SNIFF_PREFIX);
        snprintf(line, sizeof line,
                 "%s: %s, and the decoder refused it -- corrupt, or a variant "
                 "this build does not do (progressive JPEG, 16-bit PNG)",
                 name, is_ico(data, len) ? "ICO/CUR icon" : sniff_name(k));
        return fail_with(line);
    }
    if (a.nframes <= 0 || !a.frames || a.w <= 0 || a.h <= 0) {
        img_anim_free(&a);
        return fail_with("that image decoded to nothing");
    }

    long long declared = 0;
    for (int k = 0; k < a.nframes; k++) {
        if (a.frames[k].delay_ms <= 0 && a.nframes > 1) a.frames[k].delay_ms = 100;
        declared += a.frames[k].delay_ms;
    }
    plog("preview: image %s %dx%d frames=%d loops=%d declared_ms=%lld\n",
         name, a.w, a.h, a.nframes, a.loops, declared);

    /* A still: draw once and wait on events instead of spinning. */
    if (a.nframes == 1) {
        gui_clear(rgb(28, 28, 32));
        blit_fit_src(a.frames[0].rgba, a.w, a.h);
        snprintf(line, sizeof line, "%s  %dx%d   %s", name, a.w, a.h, hint());
        status(line);
        gui_flush();
        if (g_one_loop) { img_anim_free(&a); return PUMP_QUIT; }
        for (;;) {
            int p = pump_events();
            if (p) { img_anim_free(&a); return p; }
            sys_sleep_ms(30);
        }
    }

    int loop = 0, rc = PUMP_NONE;
    for (;;) {
        unsigned long long t0 = monotonic_ns();
        long long due = 0, paint_ns = 0;
        for (int k = 0; k < a.nframes; k++) {
            /* Timed, and reported. A frame of a translucent 40x28 APNG scaled
             * to fill the window is 400k pixels through fb_blit_rgba's
             * read-modify-write alpha path, which on this machine costs more
             * than the 100 ms the file asks for -- so a loop CAN legitimately
             * run long. Printing the paint cost is what keeps "the player
             * honours the delays" and "the machine can keep up" separate
             * claims instead of one excuse. */
            unsigned long long p0 = monotonic_ns();
            gui_clear(rgb(28, 28, 32));
            blit_fit_src(a.frames[k].rgba, a.w, a.h);
            snprintf(line, sizeof line, "%s  %dx%d  frame %d/%d  %d ms   %s",
                     name, a.w, a.h, k + 1, a.nframes, a.frames[k].delay_ms, hint());
            status(line);
            gui_flush();
            paint_ns += (long long)(monotonic_ns() - p0);

            due += (long long)a.frames[k].delay_ms * 1000000LL;
#ifndef PREVIEW_NO_ANIM_TIMING
            for (;;) {
                long long left = (long long)t0 + due - (long long)monotonic_ns();
                if (left <= 0) break;
                rc = pump_events();
                if (rc) goto out;
                sys_sleep_ms(left > 10000000LL ? 10 : 1);
            }
#else
            /* make test-preview-negctl: the delays are decoded and then
             * ignored. The animation runs as fast as the loop turns, which is
             * exactly the bug this player exists to not have. */
            (void)due;
#endif
            rc = pump_events();
            if (rc) goto out;
        }
        loop++;
        plog("preview: anim %s loop=%d frames=%d declared_ms=%lld elapsed_ms=%llu "
             "paint_ms=%lld\n",
             name, loop, a.nframes, declared,
             (unsigned long long)((monotonic_ns() - t0) / 1000000ULL),
             paint_ns / 1000000LL);
        if (g_one_loop) break;
        if (a.loops > 0 && loop >= a.loops) break;
    }
    snprintf(line, sizeof line, "%s  %dx%d  %d frames, %lld ms   %s",
             name, a.w, a.h, a.nframes, declared, hint());
    gui_clear(rgb(28, 28, 32));
    blit_fit_src(a.frames[a.nframes - 1].rgba, a.w, a.h);
    status(line);
    gui_flush();
    while (!g_one_loop) {
        rc = pump_events();
        if (rc) break;
        sys_sleep_ms(30);
    }
out:
    img_anim_free(&a);
    return rc;
}

/* -------------------------------------------------------- audio files --- */
static int play_audio_file(unsigned char *data, long len, const char *name)
{
    aplay a;
    aplay_open_file(&a, data, len);
    if (!a.dec) {
        char line[192];
        snprintf(line, sizeof line, "%s: %s", name, a.why ? a.why : "cannot decode");
        aplay_close(&a);
        return fail_with(line);
    }
    int p = play_audio_loop(&a, name, a.fmt ? a.fmt : "audio", -1);
    aplay_close(&a);
    return p;
}

/* --------------------------------------------------------------- open --- */
/* The dispatch. Container, then image, then audio, then elementary stream --
 * see the header note for why that is the order and not another one. */
static int open_path(const char *path, const char *name)
{
    char line[224];
    long len = 0;
    plog("preview: reading %s\n", path);
    unsigned char *data = read_all(path, &len);
    if (!data) {
        snprintf(line, sizeof line, "%s: cannot read this file", name);
        return fail_with(line);
    }
    int kind = sniff_id(data, len < SNIFF_PREFIX ? (int)len : SNIFF_PREFIX);
    plog("preview: open %s bytes=%ld looks=%s\n", name, len, sniff_name(kind));

    int p;
    if (media_sniff(data, len) != MEDIA_CONT_UNKNOWN)      p = play_container(data, len, name);
    else if (is_image_magic(data, len))                    p = play_image(data, len, name);
    else if (audio_sniff(data, len) != AUDIO_FMT_UNKNOWN)  p = play_audio_file(data, len, name);
    else if (looks_like_annexb(data, len))                 p = play_annexb(data, len, name);
    else {
        char hex[40];
        sniff_hex(data, len < 8 ? (int)len : 8, hex, (int)sizeof hex);
        snprintf(line, sizeof line,
                 "%s: %s -- no decoder in this system reads it (starts %s)",
                 name, sniff_name(kind), hex);
        p = fail_with(line);
    }
    free(data);
    return p;
}

/* --------------------------------------------------------------- main --- */
static const char *basename_of(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/') s = q + 1;
    return s;
}

/* Launched from the Dock there is no file to open, so offer what is there:
 * /media is where the shipped clips live. One level of subdirectory too,
 * because the image fixtures ride in /media/img.
 *
 * The list is also PRINTED, with its indices. A keyboard-driven list is the
 * only part of this app a test can steer, and a test that counts Down presses
 * against a directory order it guessed is a test that breaks the day a file is
 * added. So the app says what it is showing. */
#define PICK_MAX 48
static char pick_names[PICK_MAX][80];
static int  pick_n;

static void pick_scan(const char *dir, const char *prefix, int depth)
{
    int cnt = dir_count(dir);
    for (int i = 0; i < cnt && pick_n < PICK_MAX; i++) {
        char b[64];
        int rc = dir_name(dir, i, b);
        if (rc == -1) continue;
        if (rc == -2) {
            if (depth <= 0) continue;
            char sub[160], sp[64];
            snprintf(sub, sizeof sub, "%s/%s", dir, b);
            snprintf(sp, sizeof sp, "%s%s/", prefix, b);
            pick_scan(sub, sp, depth - 1);
            continue;
        }
        snprintf(pick_names[pick_n], sizeof pick_names[0], "%s%s", prefix, b);
        pick_n++;
    }
}

static int pick_from_media(char *out, int outmax)
{
    if (pick_n == 0) pick_scan("/media", "", 1);
    if (pick_n == 0) { show_message("nothing in /media to open"); return 1; }

    static int sel;
    for (int i = 0; i < pick_n; i++)
        plog("preview: pick %d %s\n", i, pick_names[i]);

    for (;;) {
        gui_clear(rgb(28, 28, 32));
        gui_text(16, 40, rgb(230, 230, 236), "Open from /media");
        int top = sel - 20 > 0 ? sel - 20 : 0;
        for (int i = top; i < pick_n && i - top < 22; i++) {
            int row = i - top;
            unsigned c = (i == sel) ? rgb(255, 210, 120) : rgb(190, 190, 198);
            if (i == sel) gui_rect(12, 54 + row * 20, WINW - 24, 20, rgb(48, 48, 56));
            gui_text(24, 68 + row * 20, c, pick_names[i]);
        }
        status("up/down to choose, Enter to open, Esc to quit");
        gui_flush();

        struct logit_event e;
        for (;;) {
            if (poll_event(&e)) {
                if (e.type == EV_CLOSE) return 1;
                if (e.type == EV_KEY) {
                    if (e.a == 27) return 1;
                    if (e.a == KEY_UP && sel > 0) sel--;
                    if (e.a == KEY_DOWN && sel < pick_n - 1) sel++;
                    if (e.a == '\n' || e.a == '\r') {
                        snprintf(out, outmax, "/media/%s", pick_names[sel]);
                        return 0;
                    }
                }
                break;
            }
            sys_sleep_ms(10);
        }
    }
}

#ifdef PREVIEW_CLI
/* One file, one loop, then out -- see the note by g_one_loop. */
int main(int argc, char **argv)
{
    if (argc < 2) { plog("usage: previewplay <file>\n"); return 2; }
    gui_create("Preview", WINW, WINH);
    open_path(argv[1], basename_of(argv[1]));
    return 0;
}
#else
void app_main(void)
{
    gui_create("Preview", WINW, WINH);

    char path[192];
    if (get_arg(path, sizeof path) > 0 && path[0]) {
        g_from_picker = 0;
        open_path(path, basename_of(path));
        app_exit(0);
    }

    g_from_picker = 1;
    for (;;) {
        if (pick_from_media(path, (int)sizeof path)) break;
        if (open_path(path, basename_of(path)) == PUMP_QUIT) break;
    }
    app_exit(0);
}
#endif
