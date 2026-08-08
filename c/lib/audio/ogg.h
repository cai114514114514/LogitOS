/* c/lib/audio/ogg.h -- Ogg page/packet framing.
 *
 * WHY THIS LIVES IN c/lib/audio AND NOT IN A CONTAINER LIBRARY.  Ogg is not a
 * general container in the way MP4 and Matroska are, and it is not the
 * container line's job. Vorbis and Opus are DEFINED in terms of it: the Vorbis
 * I specification's decode procedure begins with three header packets whose
 * boundaries only Ogg supplies, and Opus's own specification (RFC 7845) is a
 * document about how Opus sits in an Ogg stream. A demuxer that handed over
 * "the Vorbis data" without page boundaries would have handed over nothing
 * usable, because a Vorbis packet has no length field of its own -- the page's
 * segment table IS the length. So the framing belongs with the codecs that
 * cannot be decoded without it.
 *
 * This is a reader over a whole file already in memory, which is the shape
 * every other decoder in this library takes. It is deliberately not a general
 * Ogg implementation: it does not multiplex, does not seek by granule
 * position, and follows exactly one logical stream (the first one it sees).
 * Multiple logical streams in one physical stream, and chained streams, are
 * recognised and reported rather than silently mixed together.
 *
 * The page CRC is checked. That is not decoration either: Ogg's checksum is
 * the only thing standing between a truncated download and a decoder being
 * handed a packet assembled from two unrelated pages.
 */
#ifndef LOGIT_OGG_H
#define LOGIT_OGG_H

#include <stdint.h>

typedef struct oggreader oggreader;

/* `data` must stay valid for the life of the reader: packets point into it
 * when they do not span a page boundary, and into an owned buffer when they
 * do. Returns NULL and sets *err if the first page is not an Ogg page. */
oggreader *ogg_open(const uint8_t *data, long len, int *err);
void       ogg_close(oggreader *o);

/* Next packet of the followed logical stream. Returns 1 and fills the packet pointer and length on
 * success, 0 at end of stream, negative AUDIO_ERR_* on a malformed stream.
 * The returned pointer is valid until the next ogg_packet() call. */
int  ogg_packet(oggreader *o, const uint8_t **pkt, long *len);

/* Granule position of the page the last packet ENDED on, or -1. Vorbis and
 * Opus both use it to trim the final block. */
int64_t ogg_granulepos(const oggreader *o);

/* Serial number of the stream being followed, and the number of distinct
 * serials seen (more than one means a multiplexed or chained file, of which
 * only the first is decoded). */
uint32_t ogg_serial(const oggreader *o);
int      ogg_nstreams(const oggreader *o);

/* Is this plausibly an Ogg file? Content, never file name. */
int  ogg_sniff(const uint8_t *data, long len);

#endif /* LOGIT_OGG_H */
