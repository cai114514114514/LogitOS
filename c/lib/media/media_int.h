/* c/lib/media/media_int.h -- shared internals of the demuxers.
 *
 * The whole safety argument of this library rests on one thing: NO PARSER
 * TOUCHES THE FILE BUFFER DIRECTLY. Every read goes through `br`, which owns
 * the base pointer and the length and refuses to move past it, and every
 * sub-box or sub-element is a `br` CARVED OUT of its parent by br_sub(), so a
 * child element that claims to be larger than its parent gets the parent's
 * remaining bytes and not a pointer into whatever follows.
 *
 * A reader that has gone bad stays bad (`bad` is sticky) and returns zeros.
 * That matters more than it looks: it means a parse loop can read ten fields
 * and check once at the end, instead of checking ten times and getting one of
 * them wrong.
 */
#ifndef LOGIT_MEDIA_INT_H
#define LOGIT_MEDIA_INT_H

#include <stdint.h>
#include "media.h"

/* ------------------------------------------------------------ reader ----- */
typedef struct {
    const uint8_t *base;   /* start of this window inside the file */
    long           len;    /* window length */
    long           pos;    /* cursor within the window */
    long           org;    /* window start as a FILE offset (for chunk offsets) */
    int            bad;    /* sticky: something asked for more than there was */
} br;

static inline void br_init(br *b, const uint8_t *data, long len, long file_off)
{
    b->base = data; b->len = len < 0 ? 0 : len; b->pos = 0; b->org = file_off; b->bad = 0;
}

static inline long br_left(const br *b) { return b->bad ? 0 : b->len - b->pos; }
static inline int  br_ok(const br *b)   { return !b->bad; }
static inline long br_tell(const br *b) { return b->org + b->pos; }

static inline void br_fail(br *b) { b->bad = 1; }

/* Move to an absolute position in the window. Out of range is a failure, not a
 * clamp: a box that says "my payload starts at 40" inside a 12-byte box is
 * corrupt, and clamping would silently reinterpret it. */
static inline void br_seek(br *b, long off)
{
    if (off < 0 || off > b->len) { b->bad = 1; return; }
    b->pos = off;
}

static inline void br_skip(br *b, long n)
{
    if (b->bad) return;
    if (n < 0 || n > b->len - b->pos) { b->bad = 1; return; }
    b->pos += n;
}

static inline uint32_t br_u8(br *b)
{
    if (b->bad || b->pos + 1 > b->len) { b->bad = 1; return 0; }
    return b->base[b->pos++];
}

static inline uint32_t br_u16(br *b)
{
    if (b->bad || b->pos + 2 > b->len) { b->bad = 1; return 0; }
    uint32_t v = ((uint32_t)b->base[b->pos] << 8) | b->base[b->pos + 1];
    b->pos += 2; return v;
}

static inline uint32_t br_u24(br *b)
{
    if (b->bad || b->pos + 3 > b->len) { b->bad = 1; return 0; }
    uint32_t v = ((uint32_t)b->base[b->pos] << 16) | ((uint32_t)b->base[b->pos + 1] << 8)
               | b->base[b->pos + 2];
    b->pos += 3; return v;
}

static inline uint32_t br_u32(br *b)
{
    if (b->bad || b->pos + 4 > b->len) { b->bad = 1; return 0; }
    uint32_t v = ((uint32_t)b->base[b->pos] << 24) | ((uint32_t)b->base[b->pos + 1] << 16)
               | ((uint32_t)b->base[b->pos + 2] << 8) | b->base[b->pos + 3];
    b->pos += 4; return v;
}

static inline uint64_t br_u64(br *b)
{
    uint64_t hi = br_u32(b), lo = br_u32(b);
    return (hi << 32) | lo;
}

/* Little-endian, for the few Matroska fields that are (none, in fact -- EBML is
 * big-endian throughout -- but PCM CodecPrivate and RIFF payloads are). */
static inline uint32_t br_u16le(br *b)
{
    if (b->bad || b->pos + 2 > b->len) { b->bad = 1; return 0; }
    uint32_t v = (uint32_t)b->base[b->pos] | ((uint32_t)b->base[b->pos + 1] << 8);
    b->pos += 2; return v;
}

/* A pointer to n bytes at the cursor, advancing past them. NULL if short. */
static inline const uint8_t *br_bytes(br *b, long n)
{
    if (b->bad || n < 0 || n > b->len - b->pos) { b->bad = 1; return 0; }
    const uint8_t *p = b->base + b->pos;
    b->pos += n;
    return p;
}

/* Carve a child window of n bytes at the cursor and advance past it. On
 * failure the child is a zero-length bad reader, so the caller's subsequent
 * reads from it fail rather than reading the parent's tail. */
static inline br br_sub(br *b, long n)
{
    br c;
    const uint8_t *p = br_bytes(b, n);
    if (!p) { br_init(&c, b->base, 0, br_tell(b)); c.bad = 1; return c; }
    br_init(&c, p, n, b->org + (b->pos - n));
    return c;
}

/* ------------------------------------------------- the demuxer object ---- */
/* One growable sample index per track. The samples of one track are contiguous
 * and in decode order; media_read walks all the cursors and picks the lowest
 * dts, which is the interleave a player wants and is independent of how the
 * muxer laid the file out. */
typedef struct {
    long long dts;         /* in track timescale ticks */
    long long pts;
    uint32_t  off;         /* file offset of the payload -- see off_hi */
    uint32_t  off_hi;      /* files over 4 GiB exist; two 32s beat 8 bytes x N */
    uint32_t  size;
    uint32_t  flags;       /* bit 0: keyframe */
} msample;

#define MS_KEY 1u

typedef struct {
    media_track  t;
    msample     *s;
    long         n, cap;
    long         cursor;      /* media_read position */
    long long    delay_ticks; /* encoder priming to subtract (Matroska CodecDelay) */
    long long    lace_ticks;  /* per-frame duration inside a laced Matroska block */
} mtrack;

struct mdemux {
    const uint8_t  *data;
    long            len;
    media_container kind;
    int             fragmented;
    unsigned        movie_timescale;
    long long       movie_duration;    /* in movie_timescale ticks, -1 unknown */
    int             ntracks;
    mtrack          tr[MEDIA_MAX_TRACKS];
    int             selected;          /* -1 = all */
};

/* Grow-and-append. Returns 0 or MEDIA_ERR_*. The cap doubles, so building an
 * index of a million samples is a couple of dozen reallocs and not a million. */
int  md_push(mtrack *t, long long dts, long long pts, long long off,
             long size, int key);
mtrack *md_add_track(mdemux *m);

/* ticks -> ns without overflowing and without floating point: the kernel is
 * -mno-red-zone freestanding and a timestamp is exact integers. */
long long md_ticks_to_ns(long long ticks, unsigned timescale);

/* Fill in codec_name and the framing fields once codec/extradata are known. */
void md_finish_track(mtrack *t);

/* Parsers. Each returns MEDIA_OK or MEDIA_ERR_*, and owns nothing: on failure
 * media_open frees whatever was allocated. */
int mp4_parse(mdemux *m);
int mkv_parse(mdemux *m);

#endif /* LOGIT_MEDIA_INT_H */
