/* c/lib/media/flv.h -- FLV (Flash Video) demuxer, entry points.
 *
 * WIRED -- same status as avi.h/ts.h/ps.h: media_sniff()/media_open()
 * dispatch here as MEDIA_CONT_FLV.
 *
 * UNLIKE ts.c/ps.c, THIS FORMAT NEEDS NO REASSEMBLY BUFFER. An FLV tag's
 * data is one contiguous run of file bytes (DataSize is exact and the
 * payload follows immediately), the same property that lets avi.c/mp4.c
 * point samples straight into the caller's buffer -- so flv_open()'s result
 * is closed with the ordinary media_close(), not a format-specific wrapper.
 * A video tag's AVC payload is already length-prefixed NALs (AVCC framing,
 * the same convention MP4's avcC uses), pointed at directly.
 */
#ifndef LOGIT_FLV_H
#define LOGIT_FLV_H

#include "media_int.h"

/* By content: "FLV" + version byte + a flags byte whose top 5 and bit-1 bits
 * are reserved-zero (TypeFlagsReserved) -- checked so an arbitrary file that
 * happens to start "FLV" is not enough by itself. */
int flv_sniff(const uint8_t *data, long len);

int flv_parse(mdemux *m);

mdemux *flv_open(const uint8_t *data, long len, int *err);

#endif /* LOGIT_FLV_H */
