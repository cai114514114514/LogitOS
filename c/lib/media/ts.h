/* c/lib/media/ts.h -- MPEG-2 Transport Stream demuxer, entry points.
 *
 * WIRED -- media_sniff()/media_open() (c/lib/media/demux.c) dispatch here as
 * MEDIA_CONT_TS. ts_open() also still exists as a direct entry point (used
 * by tests to cross-check the generic path); either way the result is a
 * fully functional `mdemux`, so every generic entry point in media.h already
 * works on it, WITH ONE DIFFERENCE from every other container in this
 * library -- see the memory-ownership paragraph below and ts_close().
 *
 * MEMORY OWNERSHIP, AND WHY THIS FORMAT IS DIFFERENT. Every other demuxer
 * here (mp4.c, mkv.c, avi.c) has ONE PROPERTY that lets a sample be a bare
 * pointer into the caller's file buffer: a sample's bytes are CONTIGUOUS in
 * the file, because the container puts one whole box/chunk/element per
 * sample. MPEG-TS does not have that property BY DESIGN -- a single video
 * access unit's PES payload is chopped into ~184-byte pieces and interleaved,
 * in the file, with every other PID's packets (PAT, PMT, the audio track,
 * padding). There is no contiguous run of file bytes that IS one frame, so
 * "a sample is a pointer into m->data" and "no per-sample copy" cannot both
 * hold for this format the way media.h's top comment promises for MP4/MKV.
 *
 * The resolution: ts_parse REASSEMBLES every PES payload it demuxes into ONE
 * scratch buffer it allocates (sized to the whole input, which is always
 * enough since the reassembled elementary content is a strict subset of the
 * transport bytes), and at the end POINTS `m->data` AT THAT BUFFER instead of
 * the caller's original one. Every generic media.h entry point (media_read,
 * media_get_sample, media_annexb_headers, media_to_annexb, ...) keeps working
 * unchanged, because none of them care whether `m->data` is the caller's
 * mmap'd file or a scratch buffer this library built -- they only ever do
 * `m->data + sample.off`. The one field this changes the meaning of is
 * media_sample.file_off, documented in media.h as "where the payload starts
 * in the file" -- for a TS-opened mdemux it is instead an offset into this
 * reassembled buffer, NOT a transport-stream file position. There is nowhere
 * else to put "the payload starts here" for a payload that, in the actual
 * file, does not start anywhere as one run of bytes.
 *
 * CONSEQUENCE FOR CALLERS, historical note now that it is fixed: ts_open()'s
 * result used to have to be freed with ts_close(), never the generic
 * media_close() -- the generic one only ever freed each track's sample-index
 * array (m->tr[i].s) and the mdemux struct itself, and never m->data,
 * because every OTHER format's m->data is the caller's own buffer. That is
 * what struct mdemux's `owns_data` field (media_int.h) is for: ts_parse sets
 * it on success, and media_close() now frees `data` when it is set -- so
 * media_open()'s generic dispatch (which calls ts_parse directly, not
 * ts_open()) closes correctly with the ordinary media_close(), proven by
 * tests/unit/container_test.c's verify-generic mode under ASan
 * (test-containers-verify-generic / test-containers-wiring-negctl in
 * tests/containers.mk -- the negctl reverts the flag and requires the leak
 * back, caught by AddressSanitizer specifically). ts_open()/ts_close() still
 * exist and still work the same way as a direct, non-generic entry point;
 * they just no longer need special handling to be safe.
 *
 * WHAT COUNTS AS "CORRUPT" HERE, on purpose: a continuity-counter gap on a
 * PID that is not flagged by its own discontinuity_indicator, and a
 * transport_error_indicator bit anywhere, both fail the whole parse
 * (MEDIA_ERR_CORRUPT) rather than being silently dropped. That mirrors
 * avi.c's idx1 cross-check (a corrupt file is refused, not quietly patched
 * around) and is, in this API, the only channel available to REPORT a
 * transport-level error to the caller at all -- there is no side channel to
 * add a warning flag without editing media_int.h.
 */
#ifndef LOGIT_TS_H
#define LOGIT_TS_H

#include "media_int.h"

/* By content: at least 3 consecutive 0x47 sync bytes at a 188-byte stride
 * (plain TS) or a 192-byte stride starting 4 bytes in (M2TS/BDAV, which
 * prefixes every 188-byte packet with a 4-byte copy-permission/arrival-time
 * header). A single 0x47 is not enough -- it is a legal byte value anywhere. */
int ts_sniff(const uint8_t *data, long len);

/* Same shape as mp4_parse()/mkv_parse()/avi_parse(): parse into an mdemux the
 * caller already allocated and initialised. On success, m->data/m->len are
 * REPLACED to point at an internally-allocated, reassembled buffer -- see
 * the memory-ownership note above. Returns MEDIA_OK or a MEDIA_ERR_*; on
 * failure nothing is left allocated (the scratch buffer is freed before
 * returning), matching every other parser's contract. */
int ts_parse(mdemux *m);

mdemux *ts_open(const uint8_t *data, long len, int *err);

/* MUST be used instead of media_close() for a ts_open() result -- see above. */
void ts_close(mdemux *m);

#endif /* LOGIT_TS_H */
