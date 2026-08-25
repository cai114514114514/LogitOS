/* c/lib/media/avi.h -- RIFF/AVI demuxer, entry points.
 *
 * WIRED. media_sniff()/media_open() (c/lib/media/demux.c) dispatch to
 * avi_sniff()/avi_parse() as MEDIA_CONT_AVI -- a plain player only needs
 * media_open(), never avi_open() by name; avi_open() below still exists as
 * a direct entry point (tests use it to cross-check the generic path, see
 * tests/unit/container_test.c's verify-generic mode / tests/containers.mk).
 * avi_open() builds a fully functional `mdemux` the same way media_open()
 * does for MP4/Matroska, so every generic entry point in media.h
 * (media_read, media_select, media_get_sample, media_seek,
 * media_annexb_headers, media_to_annexb, media_close, ...) already works on
 * the result -- they are written purely in terms of `mdemux`/`mtrack`/
 * `msample` (media_int.h), which this file includes but does not modify.
 *
 * See the top comment of avi.c for the on-disk contract this implements.
 */
#ifndef LOGIT_AVI_H
#define LOGIT_AVI_H

#include "media_int.h"

/* By content: "RIFF" + a 4-byte little-endian size + "AVI ". */
int avi_sniff(const uint8_t *data, long len);

/* Same shape as mp4_parse()/mkv_parse(): parse into an mdemux the caller
 * already allocated and initialised (data/len/movie_timescale/... set,
 * ntracks==0). Returns MEDIA_OK or a MEDIA_ERR_*. */
int avi_parse(mdemux *m);

/* Convenience wrapper mirroring media_open(), for callers (tests, and later
 * demux.c) that just want a working mdemux from an AVI buffer without
 * hand-rolling the calloc+parse+md_finish_track dance. */
mdemux *avi_open(const uint8_t *data, long len, int *err);

#endif /* LOGIT_AVI_H */
