/* c/lib/media/subs.h -- WebVTT and SRT, parsed into one cue model.
 *
 * A container (c/lib/media/{mp4,mkv}.c) hands out elementary streams; this is
 * the elementary stream for TEXT. Both formats a real deployment actually
 * serves -- WebVTT for HLS/DASH and <track>, SRT for almost everything
 * ripped or hand-authored -- collapse to the same thing once parsed: a list
 * of [start,end) time ranges each holding a run of text plus (WebVTT only) a
 * handful of positioning hints. This library produces that list; it renders
 * nothing and lays out nothing.
 *
 * WEBVTT IS IMPLEMENTED AGAINST THE SPEC'S OWN STATE MACHINE, NOT A
 * LINE-BY-LINE GUESS. https://w3c.github.io/webvtt/ section 6 ("WebVTT
 * parser algorithm" / "collect a WebVTT block" / "collect WebVTT cue timings
 * and settings" / "collect a WebVTT timestamp" / "collect WebVTT region
 * settings") is transcribed into subs.c close enough to trace line by line,
 * because the alternative -- split on blank lines, hope -- gets the easy 90%
 * of real files right and silently disagrees with the spec on every case
 * that made it into web-platform-tests: a header line that runs into the
 * first cue with no blank line between them (WEBVTT\n<space>\n00:00...), a
 * cue-timing line whose settings token has its colon in the wrong place, a
 * STYLE block containing a line that LOOKS LIKE a cue timing inside a CSS
 * comment. tests/subs.mk gates against exactly this corpus -- see its header
 * for what is and is not vendored.
 *
 * CARRIED, NOT APPLIED -- same discipline this tree uses for CSS percentage
 * padding (browser_paint) and stroke joins (gfx): this library extracts
 * structure and does no layout. A cue's WebVTT positioning settings (line,
 * position, size, align, vertical, region) are parsed and stored verbatim;
 * nothing here computes a pixel. Cue TEXT is likewise stored RAW -- the tags
 * WebVTT defines inline (<b> <i> <u> <c> <v Speaker> <ruby><rt> and
 * <00:00:05.000> timestamp spans) and SRT's HTML-ish subset (<b> <i> <u>
 * <font color=...>) all round-trip through this parser as plain bytes inside
 * `text`, because turning them into a node tree (WebVTT spec section 6.4) is
 * a renderer's job, not a demuxer's -- exactly the boundary media.h draws
 * for elementary video/audio, extended to text. What this library DOES
 * guarantee is that embedded tags never desynchronise cue-boundary
 * detection: a "-->" inside a payload line, or a line that merely looks like
 * a tag, cannot be mistaken for the next cue's timing line, because that
 * distinction is what the state machine below is FOR.
 *
 * EVERY INPUT BYTE IS UNTRUSTED, same rule as media.h: a malformed cue is
 * SKIPPED AND COUNTED, never fatal, and nothing here reads outside the
 * buffer the caller handed in regardless of what the file claims. Built with
 * -DSUBS_STRICT (the negative control tests/subs.mk exercises), the first
 * malformed cue instead fails the whole parse -- see subs_parse_vtt's header
 * comment in subs.c for exactly which condition that flag changes.
 *
 * WHAT THIS DOES NOT DO, BY NAME (say so rather than let a missing feature
 * be discovered by its absence, per this tree's own rule about undocumented
 * subsystems):
 *   - no cue-text NODE TREE (WebVTT 6.4) -- text is raw bytes, tags included
 *   - no CSS is interpreted from a STYLE block -- it is recognised and
 *     skipped so it cannot be mistaken for cues, and thrown away
 *   - no REGION rendering -- regions are parsed (id/width/lines/anchors/
 *     scroll) and a cue can reference one by id; nothing computes where a
 *     region sits
 *   - WebVTT chapter/metadata tracks, and the (obsolete, pre-standard)
 *     "Region:" header-line syntax regions-old.test exists to reject, are
 *     out of scope the same way an obsolete box format would be for mp4.c
 *   - SRT has no positioning settings in the base format and none are
 *     invented; every SRT cue's subs_settings is exactly the defaults. A
 *     handful of SRT variants carry an X1/X2/Y1/Y2 pixel-box suffix after
 *     the timing line -- deliberately NOT parsed, because turning a pixel
 *     box into the percentage-based fields WebVTT settings use needs the
 *     video's frame dimensions, which are not available at parse time here;
 *     a trailing suffix like that is simply ignored (does not make the cue
 *     malformed) rather than guessed at
 *
 * THE MEMORY MODEL differs from media.h on purpose. A container's samples
 * are megabytes and returned as pointers into the caller's buffer; a
 * subtitle track is kilobytes and every cue is a separately-useful string, so
 * copying cue id/text into an arena the track owns is the cheaper API for
 * every caller (the alternative is the caller tracking one allocation per
 * cue). `data` need not outlive the returned subs_track.
 */
#ifndef LOGIT_SUBS_H
#define LOGIT_SUBS_H

#include <stdint.h>

#define SUBS_OK               0
#define SUBS_ERR_FORMAT       -1   /* not a WebVTT/SRT file: bad/missing signature */
#define SUBS_ERR_OOM          -2
#define SUBS_ERR_RANGE        -3   /* self-consistent but absurd; guards allocation */
#define SUBS_ERR_STRICT       -4   /* -DSUBS_STRICT build, first malformed cue */

/* Ceilings, same reasoning as MEDIA_MAX_* in media.h: every one of these is a
 * value the FILE supplies, and a value the file supplies is a claim, not a
 * fact. */
#define SUBS_MAX_CUES         200000L
#define SUBS_MAX_REGIONS      256
#define SUBS_MAX_TEXT_LEN     65536   /* one cue's payload, bytes */
#define SUBS_MAX_LINE_LEN     16384   /* one physical line, bytes */
#define SUBS_MAX_ID_LEN        1024

typedef enum {
    SUBS_FMT_UNKNOWN = 0,
    SUBS_FMT_VTT,
    SUBS_FMT_SRT
} subs_format;

typedef enum {
    SUBS_ALIGN_START = 0,
    SUBS_ALIGN_CENTER,
    SUBS_ALIGN_END,
    SUBS_ALIGN_LEFT,
    SUBS_ALIGN_RIGHT
} subs_align;               /* cue text alignment; spec default is CENTER */

typedef enum {
    SUBS_LALIGN_START = 0,  /* spec default */
    SUBS_LALIGN_CENTER,
    SUBS_LALIGN_END
} subs_line_align;

typedef enum {
    SUBS_PALIGN_LINE_LEFT = 0,
    SUBS_PALIGN_CENTER,
    SUBS_PALIGN_LINE_RIGHT,
    SUBS_PALIGN_AUTO         /* spec default */
} subs_pos_align;

typedef enum {
    SUBS_VERTICAL_NONE = 0,  /* horizontal text; the common case */
    SUBS_VERTICAL_RL,        /* "vertical:rl" -- growing left */
    SUBS_VERTICAL_LR         /* "vertical:lr" -- growing right */
} subs_vertical;

typedef enum {
    SUBS_SCROLL_NONE = 0,
    SUBS_SCROLL_UP
} subs_scroll;

/* A WebVTT REGION block (spec 6.2). SRT and a cue with no "region:" setting
 * never reference one -- see subs_cue.region below. */
typedef struct {
    char        id[64];          /* "" is a legal, matchable id */
    double      width;           /* percent 0..100, default 100 */
    int         lines;           /* default 3 */
    double      anchor_x, anchor_y;           /* percent, default (0,100) */
    double      viewport_x, viewport_y;       /* percent, default (0,100) */
    subs_scroll scroll;          /* default SUBS_SCROLL_NONE */
} subs_region;

/* WebVTT cue settings (spec 6.3). CARRIED, NOT APPLIED -- see subs.h's top
 * comment. Every "auto" case is a real, distinct value (not merely "0" or
 * "unset"): a renderer needs to tell "the position was never specified" from
 * "the position was specified as 0", and WebVTT itself distinguishes them. */
typedef struct {
    subs_vertical    vertical;          /* default SUBS_VERTICAL_NONE */

    int              snap_to_lines;     /* default true */
    int              line_is_auto;      /* default true: `line` below is unset */
    double           line;              /* line number (any double) or percent 0..100 */
    int              line_is_percent;
    subs_line_align  line_align;        /* default SUBS_LALIGN_START */

    int              position_is_auto;  /* default true */
    double           position;          /* percent 0..100 */
    subs_pos_align   position_align;    /* default SUBS_PALIGN_AUTO */

    double           size;              /* percent 0..100, default 100 */

    subs_align       align;             /* default SUBS_ALIGN_CENTER */

    int              region;            /* index into subs_region_at(), or -1 */
} subs_settings;

typedef struct {
    int64_t        start_ms;
    int64_t        end_ms;         /* NOT guaranteed >= start_ms -- WebVTT's own file-
                                     * parsing tests require accepting end < start as a
                                     * valid (if degenerate) cue; subs_active_at() simply
                                     * never reports such a cue active, which needs no
                                     * special case since start_ms <= t < end_ms cannot
                                     * hold when end_ms <= start_ms */
    const char    *id;              /* cue identifier ("" if none), NUL-terminated */
    const char    *text;            /* raw payload, LF-joined, tags/entities untouched */
    subs_settings  settings;
} subs_cue;

typedef struct subs_track subs_track;

/* Parse. `data` need not outlive the call -- everything a caller needs is
 * copied into the track. Detected/confirmed format is written to *out_fmt if
 * non-NULL. On SUBS_ERR_FORMAT/OOM/STRICT, returns NULL and *out_err (if
 * non-NULL) explains why; the track is otherwise always returned even if
 * every cue in the file was malformed (subs_cue_count() == 0 is not an
 * error -- subs_skipped_count() says whether that is because the file had no
 * cues or because all of them were rejected). */
subs_track *subs_parse(const uint8_t *data, long len, subs_format *out_fmt, int *out_err);
subs_track *subs_parse_vtt(const uint8_t *data, long len, int *out_err);
subs_track *subs_parse_srt(const uint8_t *data, long len, int *out_err);
void        subs_close(subs_track *tr);

subs_format subs_track_format(const subs_track *tr);

/* Cues are stored SORTED by start_ms (stable: a tie keeps file order), which
 * is what makes subs_active_at's binary search valid and is the natural
 * "in order" a player wants regardless of the order cue blocks appeared in
 * the file. */
int              subs_cue_count(const subs_track *tr);
const subs_cue  *subs_cue_at(const subs_track *tr, int index);

int              subs_region_count(const subs_track *tr);
const subs_region *subs_region_at(const subs_track *tr, int index);

/* Cues the file-level parser rejected outright (bad/absent timing line,
 * end < start, a payload past SUBS_MAX_TEXT_LEN) -- not the same count as
 * "cue settings this library ignored", which is not an error at all. */
int              subs_skipped_count(const subs_track *tr);

/* Per-frame lookup: active cues at t_ms, IN START-TIME ORDER, no allocation
 * -- out_idx is a caller-owned array of subs_cue_at() indices, capacity
 * max_out. Returns the true count (snprintf's convention: may exceed
 * max_out, in which case only the first max_out indices were written).
 *
 * NOT a single bsearch. Two cues can overlap, so "the cues active at t" is a
 * stabbing query over intervals, not a point lookup -- see the block above
 * subs_active_at in subs.c for the O(log n + k) construction (a
 * start-sorted array plus a prefix-max-of-end-time array) and, honestly, for
 * what k is bounded by and what it is not. */
int subs_active_at(const subs_track *tr, int64_t t_ms, int *out_idx, int max_out);

#endif
