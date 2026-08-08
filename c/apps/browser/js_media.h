#ifndef LOGIT_JS_MEDIA_H
#define LOGIT_JS_MEDIA_H

/* Media Source Extensions for this browser: the DOM/JS side of media playback.
 *
 * WHY THIS EXISTS, measured rather than assumed. Driving a headless Chrome at a
 * real bilibili video page: the DASH manifest offers avc1.640033 (H.264 High,
 * level 5.1), hvc1.1.6.L120.90 (HEVC Main), av01.0.08M.08 (AV1) and mp4a.40.2
 * (AAC-LC); the container is SEPARATE video/mp4 and audio/mp4 fragmented
 * streams, and the player fetches 84 .m4s segments and feeds them to one
 * <video> element. Feeding separate audio and video fMP4 segments into one
 * <video> is MSE and nothing else does it -- so every byte-exact decoder in
 * this tree (c/lib/video, c/lib/audio), the demuxers (c/lib/media), the A/V
 * clock and the sound card were unreachable from a web page until this file.
 *
 * AND THE OTHER HALF, which is the strategic one. Chrome took AV1 on that page
 * because Chrome can decode AV1; the manifest offered H.264 High as well. The
 * mechanism by which a client picks is MediaSource.isTypeSupported(). An HONEST
 * isTypeSupported that answers "no" to av01.* and "yes" to avc1.64* makes the
 * site serve H.264 -- which we decode. Claiming a codec we do not have does not
 * get us a picture; it gets us a stream nothing can read, failing far
 * downstream where it looks like a decoder bug. So the table in
 * mse_type_supported() is the browser's promise about itself, every entry is
 * derived from a gate that actually exists in c/lib/{video,audio}, and
 * tests/unit/mse_test.c decodes a real sample of every type it says yes to.
 *
 * WHAT IS HERE
 *   MediaSource      addSourceBuffer/removeSourceBuffer, duration, readyState,
 *                    endOfStream, isTypeSupported, sourceopen/sourceended.
 *   SourceBuffer     appendBuffer, remove, abort, buffered, timestampOffset,
 *                    mode (segments|sequence), updating, updatestart/update/
 *                    updateend/error.
 *   URL.createObjectURL(mediaSource) / revokeObjectURL, and <video src="blob:">.
 *   HTMLMediaElement play/pause/currentTime/duration/buffered/paused/ended/
 *                    volume/muted/readyState/videoWidth/videoHeight/load, and
 *                    loadedmetadata/canplay/play/playing/pause/timeupdate/
 *                    seeking/seeked/waiting/ended/error/durationchange.
 *
 * WHAT IS DELIBERATELY NOT HERE, said out loud rather than discovered later:
 * no MediaSource.setLiveSeekableRange, no SourceBuffer.changeType, no
 * appendWindowStart/End, no TextTrack, no captions, no picture-in-picture, no
 * playbackRate other than 1, no HTMLMediaElement.canPlayType (isTypeSupported
 * is the one a player actually branches on), and remove() evicts a range from
 * `buffered` and from playback without reclaiming its bytes (see msebuf).
 *
 * THE INCREMENTAL RULE. media_open() in c/lib/media wants a whole file and MSE
 * is the opposite of that -- segments arrive over time. The identity this file
 * is built on is that an fMP4 INIT segment followed by media segments IS a
 * valid fragmented MP4, so an append accumulates into one growing buffer and
 * the demuxer is re-opened over the longest prefix that ends on a top-level box
 * boundary. That is why appendBuffer never blocks on a partial box and why a
 * segment split across three network reads behaves exactly like one read. */

#include <stdint.h>

/* ---- the honest codec table ------------------------------------------------
 * `type` is a full MIME with parameters, e.g.
 *     video/mp4; codecs="avc1.640033"
 *     audio/mp4; codecs="mp4a.40.2"
 * Returns 1 only when every codec named is one the linked decoders accept.
 * An empty or absent codecs= parameter is answered 0, exactly as the spec
 * requires: "video/mp4" alone does not say what is inside it. */
int mse_type_supported(const char *type);

/* ---- the platform, injected -----------------------------------------------
 * The OS build wires this to logit.h -- gui_blit, gui_clip, the snd_ calls and
 * monotonic_ns;
 * the host test injects recorders and a clock it steps by hand, which is what
 * makes "the frames arrived in order and in sync" a unit test instead of a
 * QEMU boot with a stopwatch. */
struct media_platform {
    unsigned long long (*now_ns)(void);
    /* Blit `sw`x`sh` RGBA scaled into the device rect (x,y,w,h). */
    void (*blit)(int x, int y, int w, int h,
                 const unsigned char *rgba, int sw, int sh);
    void (*fill)(int x, int y, int w, int h, unsigned rgb);
    void (*clip)(int x, int y, int w, int h);
    void (*flush)(void);
    /* Sound. open returns a handle >= 0 or < 0 when there is no card; the
     * others take it. `played` is frames the CARD has played, which is the
     * master clock -- never frames written. */
    int  (*snd_open)(int rate, int channels);
    int  (*snd_write)(int h, const void *buf, int bytes);
    int  (*snd_avail)(int h);
    long long (*snd_played)(int h);
    void (*snd_close)(int h, int drain);
};
void media_set_platform(const struct media_platform *p);

/* ---- MediaSource / SourceBuffer -------------------------------------------
 * Opaque to the bindings: js_media.c owns the JS objects and their events,
 * this owns the bytes, the demuxer and the decoders. */
typedef struct msource msource;
typedef struct sbuf    sbuf;
typedef struct melem   melem;

enum { MSE_CLOSED = 0, MSE_OPEN = 1, MSE_ENDED = 2 };
enum { MSE_MODE_SEGMENTS = 0, MSE_MODE_SEQUENCE = 1 };

/* Errors an operation can report to the bindings, which turn them into the
 * right DOM exception. */
enum {
    MSE_OK = 0,
    MSE_E_NOTSUPPORTED = -1,   /* NotSupportedError: the type */
    MSE_E_INVALIDSTATE = -2,   /* InvalidStateError: wrong readyState/updating */
    MSE_E_QUOTA        = -3,   /* QuotaExceededError: the buffer cap */
    MSE_E_DECODE       = -4,   /* the appended bytes are not a container we read */
    MSE_E_OOM          = -5
};

msource *mse_new(void);
void     mse_free(msource *ms);
int      mse_state(const msource *ms);
double   mse_duration(const msource *ms);          /* seconds; -1 = unset */
int      mse_set_duration(msource *ms, double sec);
int      mse_end_of_stream(msource *ms, const char *err);  /* err may be NULL */
sbuf    *mse_add_source_buffer(msource *ms, const char *type, int *err);
int      mse_remove_source_buffer(msource *ms, sbuf *sb);
int      mse_sb_count(const msource *ms);
sbuf    *mse_sb_at(const msource *ms, int i);

/* The element attaches when its src is set to this source's object URL. The
 * bindings call mse_attach, which moves the source to MSE_OPEN and is what
 * makes `sourceopen` fire. */
int      mse_attach(msource *ms, melem *el);
void     mse_detach(msource *ms);

/* ---- SourceBuffer ---- */
/* Copies `n` bytes in and parses whatever complete boxes that produced.
 * Returns MSE_OK or a negative MSE_E_*. The append is synchronous here and the
 * `updateend` event is fired by the bindings on the next pump, which is what
 * the spec's asynchronous append step is observably equivalent to. */
int      sb_append(sbuf *sb, const unsigned char *data, long n);
int      sb_remove(sbuf *sb, double start_sec, double end_sec);
int      sb_abort(sbuf *sb);
void     sb_set_mode(sbuf *sb, int mode);
int      sb_mode(const sbuf *sb);
void     sb_set_timestamp_offset(sbuf *sb, double sec);
double   sb_timestamp_offset(const sbuf *sb);
/* `buffered`: the merged ranges of media time this buffer holds, in seconds. */
int      sb_buffered_count(const sbuf *sb);
int      sb_buffered_range(const sbuf *sb, int i, double *start, double *end);
const char *sb_type(const sbuf *sb);
long     sb_bytes(const sbuf *sb);          /* what it is holding, for the UI */

/* ---- the media element ---------------------------------------------------- */
/* Elements are keyed by a small positive integer that the element itself
 * carries in a data- attribute (see the note on struct melem). */
melem *mel_for_key(int key, int create);
void   mel_free_all(void);                  /* on navigation, with the DOM */

int    mel_play(melem *el);
void   mel_pause(melem *el);
int    mel_paused(const melem *el);
int    mel_ended(const melem *el);
int    mel_seeking(const melem *el);
double mel_current_time(const melem *el);
int    mel_seek(melem *el, double sec);
double mel_duration(const melem *el);       /* -1 = unknown */
int    mel_ready_state(const melem *el);    /* HAVE_NOTHING..HAVE_ENOUGH_DATA */
int    mel_network_state(const melem *el);
void   mel_set_volume(melem *el, double v);
double mel_volume(const melem *el);
void   mel_set_muted(melem *el, int m);
int    mel_muted(const melem *el);
int    mel_video_width(const melem *el);
int    mel_video_height(const melem *el);
int    mel_buffered_count(const melem *el);
int    mel_buffered_range(const melem *el, int i, double *start, double *end);
int    mel_error(const melem *el);          /* MediaError.code, 0 = none */
const char *mel_error_message(const melem *el);
int    mel_attach_url(melem *el, const char *url);   /* "blob:..." -> a msource */
void   mel_load(melem *el);

/* Statistics the tests read back instead of trusting the picture: frames shown,
 * frames dropped, and the A/V drift avclock measured. */
/* Events the engine decided happened. The bindings drain these each pump and
 * dispatch the DOM event, so nothing in the engine ever calls into JS -- which
 * is what lets the host test assert on the same bits without a runtime. */
enum {
    MEV_LOADEDMETADATA = 1u << 0,
    MEV_CANPLAY        = 1u << 1,
    MEV_PLAYING        = 1u << 2,
    MEV_TIMEUPDATE     = 1u << 3,
    MEV_WAITING        = 1u << 4,
    MEV_SEEKED         = 1u << 5,
    MEV_ENDED          = 1u << 6,
    MEV_ERROR          = 1u << 7,
    MEV_DURATIONCHANGE = 1u << 8
};
unsigned mel_take_events(melem *el);
melem   *mel_at(int i);                     /* iterate live elements, NULL past the end */

struct mel_stats {
    long long frames_decoded, frames_shown, frames_dropped, resyncs;
    long long drift_mean_ns, drift_max_ns, drift_min_ns;
    long long audio_frames_written;
    long long appends, bytes_appended, reparses;
};
void mel_get_stats(const melem *el, struct mel_stats *out);

/* ---- the object URL registry ---- */
int      mse_object_url(msource *ms, char *out, int max);   /* "blob:logit/7" */
msource *mse_from_object_url(const char *url);
void     mse_revoke_object_url(const char *url);

/* ---- the pump -------------------------------------------------------------
 * One step of every playing element: feed the sound card, decode what is due,
 * decide show/wait/drop against the clock, and paint the frame into the box the
 * painter last reported. Returns the number of elements that produced a new
 * frame (so the caller knows whether anything on screen changed).
 *
 * `mel_pending()` is 1 while anything needs pumping, so an idle page costs a
 * pointer test -- the same contract js_page_pending() has. */
int  media_pump(void);
int  mel_pending(void);

/* ---- the paint hook -------------------------------------------------------
 * browser_paint.c calls this for an IT_VIDEO item, weakly, so a build without
 * this file still links and paints the poster box. Coordinates are DEVICE
 * pixels and the clip is the painter's already-intersected rect. */
struct node;
void media_paint_box(struct node *node, int x, int y, int w, int h,
                     int clip_x, int clip_y, int clip_w, int clip_h);
/* What media_paint_box resolves to. Split out so the engine needs no DOM at
 * all: js_media.c does the node -> key lookup, which is the only place in this
 * feature that touches dom.h. */
void media_paint_key(int key, int x, int y, int w, int h,
                     int clip_x, int clip_y, int clip_w, int clip_h);

/* ---- the JS bindings (js_media.c) ----------------------------------------
 * Declared weak for js_page.c the same way js_webapi/js_platform are: a build
 * without js_media.c links and simply has no MediaSource. */
#ifdef JS_MEDIA_OPTIONAL
#  define MEDIA_FN __attribute__((__weak__))
#else
#  define MEDIA_FN
#endif
struct JSContext;
MEDIA_FN void js_media_install(struct JSContext *ctx);
MEDIA_FN void js_media_close(struct JSContext *ctx);
/* Step the media pump and fire whatever DOM events that produced. Returns the
 * number of observable things that happened (events fired + frames painted). */
MEDIA_FN int  js_media_pump(struct JSContext *ctx);
MEDIA_FN int  js_media_pending(void);

#endif /* LOGIT_JS_MEDIA_H */
