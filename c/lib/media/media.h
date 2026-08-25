/* c/lib/media/media.h -- public API of the from-scratch container demuxers.
 *
 * The decoders next door (c/lib/video: H.264 and H.265; c/lib/audio: WAV,
 * FLAC, MP3) are byte-exact and useless on their own, because none of them can
 * open a file anybody actually has. A camera, a phone and a web page all emit
 * a CONTAINER -- .mp4, .mkv, .webm -- and an elementary stream is something
 * you only get by running a tool over one first. This library is that tool,
 * built in, so Preview can open the file itself.
 *
 * Two containers:
 *
 *   ISO-BMFF / MP4 (also .mov, .m4v, .m4a)   mp4.c
 *       Boxes; moov/trak/mdia/minf/stbl sample tables; AND fragmented MP4
 *       (moof/traf/trun over mvex/trex defaults), which is what every
 *       streaming site actually delivers. Both shapes produce the same flat
 *       sample index, so nothing above here knows which it read.
 *
 *   Matroska / WebM                          mkv.c
 *       EBML; Segment/Info/Tracks/Cluster; SimpleBlock and BlockGroup, with
 *       all three lacing modes (Xiph, fixed, EBML).
 *
 * WHY THIS IS A RING-3 LIBRARY, NOT A KERNEL ONE
 * The same reason c/lib/video and c/lib/audio are, and more so. A container is
 * the single most attacker-shaped input in this system: it comes off the
 * network, every length in it is a claim by a stranger, and the whole file is
 * a tree of nested lengths that a parser walks with a pointer. It allocates
 * continuously (a sample index is megabytes on a real film). In ring 0 that
 * would be a stranger's byte lengths driving a pointer inside the kernel, with
 * the big lock held. So c/lib/media is filtered out of the kernel's C_SRC
 * exactly like its two neighbours, and consumers link MED_OBJ plus mini-libc.
 *
 * EVERY INPUT BYTE IS UNTRUSTED. A malformed file is a negative return, never
 * a crash, never a read outside the buffer the caller handed in. That claim is
 * gated by make test-demux-fuzz, under ASan+UBSan with -fno-sanitize-recover.
 *
 * THE MEMORY MODEL: `data` must stay valid and unmodified for the life of the
 * demuxer. Samples are returned as pointers INTO it -- a demuxer that copied
 * every sample would double the memory cost of a file for nothing, and this is
 * the same choice adec_open() already made in c/lib/audio.
 *
 * Usage:
 *     int err;
 *     mdemux *m = media_open(buf, len, &err);
 *     for (int i = 0; i < media_track_count(m); i++)
 *         const media_track *t = media_track_info(m, i);   // codec, geometry
 *     media_sample s;
 *     while (media_read(m, &s) == 1) {
 *         // s.data / s.size / s.pts_ns / s.dts_ns / s.keyframe
 *     }
 *     media_close(m);
 */
#ifndef LOGIT_MEDIA_H
#define LOGIT_MEDIA_H

#include <stdint.h>

#define MEDIA_OK               0
#define MEDIA_ERR_CORRUPT     -1   /* the bytes violate the format */
#define MEDIA_ERR_UNSUPPORTED -2   /* a valid file using something we do not do */
#define MEDIA_ERR_OOM         -3
#define MEDIA_ERR_RANGE       -4   /* self-consistent but absurd (guards allocation) */

/* Ceilings. Every one of these is a header field multiplied into an allocation
 * somewhere below, and a header field is a claim, not a fact. */
#define MEDIA_MAX_TRACKS       16
#define MEDIA_MAX_SAMPLES  4000000L    /* ~10 h of 100 fps video */
#define MEDIA_MAX_EXTRADATA    65536
#define MEDIA_MAX_DIM          16384

typedef enum {
    MEDIA_CONT_UNKNOWN = 0,
    MEDIA_CONT_MP4,            /* ISO-BMFF: .mp4 .mov .m4a .m4v, fragmented too */
    MEDIA_CONT_MKV,            /* Matroska and WebM (the same parser) */
    MEDIA_CONT_AVI,            /* RIFF/AVI -- avi.c */
    MEDIA_CONT_TS,             /* MPEG-2 Transport Stream -- ts.c */
    MEDIA_CONT_PS,             /* MPEG Program Stream (.mpg/.vob) -- ps.c */
    MEDIA_CONT_FLV             /* Flash Video -- flv.c */
} media_container;

typedef enum {
    MEDIA_CODEC_UNKNOWN = 0,
    /* video */
    MEDIA_CODEC_H264,
    MEDIA_CODEC_H265,
    MEDIA_CODEC_VP8,
    MEDIA_CODEC_VP9,
    MEDIA_CODEC_AV1,
    MEDIA_CODEC_MPEG4,
    /* audio */
    MEDIA_CODEC_AAC,
    MEDIA_CODEC_MP3,
    MEDIA_CODEC_FLAC,
    MEDIA_CODEC_PCM_S16LE,
    MEDIA_CODEC_PCM_S16BE,
    MEDIA_CODEC_OPUS,
    MEDIA_CODEC_VORBIS,
    MEDIA_CODEC_AC3
} media_codec;

#define MEDIA_TRACK_NONE   0
#define MEDIA_TRACK_VIDEO  1
#define MEDIA_TRACK_AUDIO  2
#define MEDIA_TRACK_OTHER  3   /* subtitles, timecode, data: indexed, never read */

/* How a track's samples are framed. The video decoders take an Annex B byte
 * stream, but MP4 and Matroska both store H.264/H.265 as length-prefixed NAL
 * units with the parameter sets hoisted out into the sample description. That
 * is a CONTAINER fact, so converting it is this library's job and not the
 * player's -- see media_annexb_headers() and media_to_annexb(). */
#define MEDIA_FRAMING_RAW      0   /* the sample bytes are the codec's own */
#define MEDIA_FRAMING_AVCC     1   /* length-prefixed NALs, nal_length_size below */

typedef struct {
    int          index;          /* 0..count-1, this library's numbering */
    int          id;             /* the container's own track id, for messages */
    int          type;           /* MEDIA_TRACK_* */
    media_codec  codec;
    const char  *codec_name;     /* "h264", "mp3", ... never NULL */

    unsigned     timescale;      /* ticks per second of this track's timestamps */
    long long    duration;       /* in ticks; -1 when the container does not say */
    long         nsamples;

    /* video */
    int          width, height;  /* display size the container declares */
    int          framing;        /* MEDIA_FRAMING_* */
    int          nal_length_size;/* 1,2,4 when framing == AVCC */

    /* audio */
    int          rate, channels, bits;

    /* Codec configuration: avcC / hvcC / esds DecoderSpecificInfo /
     * Matroska CodecPrivate, verbatim. Points into the caller's buffer. */
    const uint8_t *extradata;
    int          extradata_len;
} media_track;

typedef struct {
    int            track;        /* index into the track table */
    const uint8_t *data;         /* into the caller's file buffer */
    long           size;
    long long      pts_ticks;    /* in the track's timescale; the container's own */
    long long      dts_ticks;
    long long      pts_ns;       /* normalised, monotonic-clock units */
    long long      dts_ns;
    int            keyframe;
    long long      file_off;     /* where the payload starts in the file */
} media_sample;

typedef struct mdemux mdemux;

/* Identify by CONTENT, never by file name -- the same rule audio_sniff() and
 * Preview's Annex B check already follow. */
media_container media_sniff(const uint8_t *data, long len);
const char     *media_container_name(media_container c);
const char     *media_codec_name(media_codec c);
const char     *media_strerror(int err);

/* `data` must stay valid and unmodified until media_close(). NULL on failure
 * with *err set (err may be NULL). */
mdemux *media_open(const uint8_t *data, long len, int *err);
void    media_close(mdemux *m);

media_container media_kind(const mdemux *m);
int  media_track_count(const mdemux *m);
const media_track *media_track_info(const mdemux *m, int index);
/* First track of a type, or -1. */
int  media_find_track(const mdemux *m, int type);
/* Whole-file duration in nanoseconds, or -1. */
long long media_duration_ns(const mdemux *m);
/* 1 when the file was fragmented (moof/mdat), 0 otherwise. MP4 only. */
int  media_is_fragmented(const mdemux *m);

/* Pull the next sample across all tracks, in decode order (lowest dts first;
 * ties by file offset, which is the interleave the muxer chose). Returns 1 on
 * a sample, 0 at end of file, negative MEDIA_ERR_*. */
int  media_read(mdemux *m, media_sample *out);

/* Restrict media_read to one track (-1 = all). Lets a player pull video and
 * audio through two independent cursors when it wants to. */
int  media_select(mdemux *m, int track_index);

/* Seek to the last keyframe of `track` at or before `ns` and restart every
 * track's cursor there. Returns the pts_ns actually landed on, or negative. */
long long media_seek(mdemux *m, int track, long long ns);

/* Random access into the built index, which is what a player's second cursor
 * wants. index is 0..track.nsamples-1. Returns 1, 0 past the end, or <0. */
int  media_get_sample(const mdemux *m, int track, long index, media_sample *out);

/* --- Annex B conversion (H.264/H.265 out of MP4 or Matroska) --------------
 * media_annexb_headers() writes the parameter sets held in the sample
 * description (avcC SPS/PPS, hvcC VPS/SPS/PPS) as start-code-prefixed NALs;
 * emit them once, before the first sample. media_to_annexb() rewrites one
 * sample's length prefixes as start codes. Both return the number of bytes
 * written, 0 when there is nothing to write, or negative MEDIA_ERR_*; both
 * return the required size (positive) without writing when out is NULL.
 *
 * Concatenating the headers and every sample in decode order yields exactly
 * the byte stream `ffmpeg -c:v copy -bsf:v h264_mp4toannexb` produces, which
 * is how make test-demux-diff checks this. */
long media_annexb_headers(const mdemux *m, int track, uint8_t *out, long max);
long media_to_annexb(const mdemux *m, const media_sample *s, uint8_t *out, long max);

/* --- the A/V clock (avclock.c) --------------------------------------------
 * Two streams with independent timestamps need one clock and a stated policy
 * for the moment decode falls behind -- which on this machine it will, since a
 * from-scratch H.264 decoder under TCG is nowhere near real time.
 *
 * The policy, and the reason:
 *
 *   AUDIO IS THE MASTER when the file has an audio track. A dropped video
 *   frame is invisible; a gap in audio is a click that everybody hears, and
 *   the sound card is a real clock running at a real rate whether or not we
 *   feed it. So audio is never dropped and never stretched: it is written to
 *   the card as fast as the ring will take it, and the card's own play cursor
 *   IS the clock. With no audio track the clock is the monotonic one.
 *
 *   VIDEO IS PACED AGAINST THAT CLOCK, and when it is late the frame is still
 *   DECODED and not displayed. It has to be decoded: a P frame is the
 *   reference for the next one, so skipping decode corrupts everything after
 *   it until the next keyframe. What is skipped is the colour conversion and
 *   the blit, which is where the time actually goes on this machine.
 *
 *   A LATE FRAME IS NOT WAITED FOR. If we are behind, show it now.
 *   AN EARLY FRAME IS WAITED FOR, but never for longer than av_max_sleep_ns:
 *   a wildly wrong timestamp must not park the player for an hour.
 *
 *   WHEN BEHIND BY MORE THAN av_resync_ns the clock is RE-BASED rather than
 *   chased, because a decoder that is permanently 3x too slow will never catch
 *   up and every frame after that would be "late" for ever. Re-basing keeps
 *   video and audio in sync with each other -- which is what a viewer sees --
 *   and lets playback run slow, which is honest. The count of re-bases is
 *   reported, so "it played" and "it played in real time" stay separable.
 *
 * The clock takes `now_ns` as an ARGUMENT rather than reading the machine's,
 * so the whole policy is testable on the host: make test-avsync runs a minute
 * of simulated playback at several decode speeds and reports the drift. */
typedef struct {
    long long start_ns;        /* now_ns at the anchor (no-audio master only) */
    long long anchor_pts_ns;   /* the pts that instant corresponds to */
    long long audio_pts_ns;    /* -1 until the audio path reports; the master */
    long long offset_ns;       /* correction a re-base applied to the master */
    long long max_sleep_ns;
    long long resync_ns;
    long long drop_ns;         /* lateness beyond which the frame is not shown */
    int       max_drop_run;    /* never skip more than this many in a row */
    int       drop_run;
    /* measurements, not settings */
    long long frames_shown, frames_dropped, resyncs;
    long long drift_sum_ns, drift_max_ns, drift_min_ns, drift_n;
    long long last_drift_ns;
    int       started;
    int       have_audio;
} avclock;

/* have_audio selects the master. Defaults: 50 ms drop threshold, 1 s resync,
 * 250 ms maximum sleep. */
void avclock_init(avclock *c, int have_audio);

/* The audio path calls this with the pts of the audio the CARD HAS PLAYED (not
 * the audio written), whenever it knows it. That is the master clock. */
void avclock_audio(avclock *c, long long played_pts_ns);

#define AV_SHOW  0   /* display it now */
#define AV_WAIT  1   /* too early: sleep *sleep_ns, then ask again */
#define AV_DROP  2   /* too late: decode kept it, do not spend the blit on it */

/* What to do with a decoded frame whose presentation stamp is `pts_ns`, at
 * `now_ns`. On AV_WAIT the caller sleeps and calls again with the same frame. */
int avclock_frame(avclock *c, long long pts_ns, long long now_ns, long long *sleep_ns);

/* Signed drift of the last decision: video minus master. Positive = video is
 * ahead of the clock, negative = video is behind. */
long long avclock_drift_ns(const avclock *c);

#endif /* LOGIT_MEDIA_H */
