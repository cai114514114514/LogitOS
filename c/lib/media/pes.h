/* c/lib/media/pes.h -- pieces MPEG-TS and MPEG-PS both need, factored out so
 * ts.c and ps.c do not each grow their own slightly-different copy.
 *
 * A PES (Packetized Elementary Stream) packet has the same 6-byte prefix
 * (start code 00 00 01, stream_id, PES_packet_length) in BOTH containers --
 * TS carries it split across 188-byte transport packets, PS carries it
 * mostly whole inside a pack -- and the OPTIONAL header that follows comes in
 * exactly two shapes, ISO/IEC 13818-1 2.4.3.7: the flag-byte form every
 * MPEG-2 stream (and every MPEG-TS stream; TS did not exist before MPEG-2)
 * uses, and the stuffing/marker-nibble form MPEG-1 streams use. Which shape a
 * given PS file uses follows its pack_header version (ps.c decides that);
 * TS is always the MPEG-2 shape.
 *
 * THE ONE THING BOTH SHAPES SHARE, and the reason `decode_ts` is a single
 * function: the 5-byte, 90 kHz timestamp field ('0010'/'0011'/'0001' marker
 * nibble + three 15-or-3-bit chunks of the value, each followed by a
 * marker_bit) is bit-for-bit identical in both PES header styles. Only what
 * comes BEFORE it (a flags byte pair vs. stuffing bytes) differs.
 */
#ifndef LOGIT_PES_H
#define LOGIT_PES_H

#include "media_int.h"

/* Decode one 5-byte 90 kHz timestamp field (PTS or DTS), marker bits and all.
 * Caller has already positioned at its first byte and checked the leading
 * nibble is what it expected ('0010' PTS-only / '0011' PTS-of-pair /
 * '0001' DTS-of-pair) -- this just extracts the 33-bit value. */
long long pes_decode_ts5(br *b);

/* MPEG-2 / TS style optional header: 2 flag bits ('10') + scrambling/
 * priority/alignment/copyright/copy bits, PTS_DTS_flags + five more flag
 * bits, PES_header_data_length, then that many bytes of optional fields (of
 * which only PTS/DTS are extracted here -- ESCR/ES_rate/DSM-trick/
 * additional-copy/CRC/extension are skipped, nothing here needs them).
 * `b` must be positioned right after the 2-byte PES_packet_length field.
 * Returns 0 on success (bad input leaves *has_pts / *has_dts at 0, not a
 * hard failure -- a truncated optional header is still consumed up to
 * PES_header_data_length so the caller's cursor lands on the payload,
 * UNLESS the length itself does not fit, which is corrupt and reported). */
int pes_opt_header_mpeg2(br *b, long long *pts, long long *dts,
                          int *has_pts, int *has_dts);

/* MPEG-1 style: up to 16 stuffing bytes (0xFF), an optional 2-byte STD
 * buffer scale/size field (top bits '01'), then EITHER a lone 0x0F
 * (no timestamp) OR a '0010'-prefixed PTS OR a '0011'-prefixed PTS followed
 * by a '0001'-prefixed DTS. Returns 0 on success, -1 if the marker sequence
 * does not parse as any of the legal shapes (corrupt). */
int pes_opt_header_mpeg1(br *b, long long *pts, long long *dts,
                          int *has_pts, int *has_dts);

/* Elementary-stream_id classification (ISO/IEC 13818-1 Table 2-22). */
#define PES_SID_PROGRAM_STREAM_MAP  0xBC
#define PES_SID_PRIVATE_1           0xBD  /* AC-3 / DVD subpictures / LPCM */
#define PES_SID_PADDING             0xBE
#define PES_SID_PRIVATE_2           0xBF  /* DVD NAV packs: no PTS/DTS, not ES */
#define PES_SID_ECM                 0xF0
#define PES_SID_EMM                 0xF1
#define PES_SID_PROGRAM_STREAM_DIR  0xFF
#define PES_SID_DSMCC               0xF2
#define PES_SID_H222_TYPE_E         0xF8

static inline int pes_is_video(unsigned sid) { return sid >= 0xE0 && sid <= 0xEF; }
static inline int pes_is_audio(unsigned sid) { return sid >= 0xC0 && sid <= 0xDF; }
/* Streams that carry the flag/stuffing optional-header shape at all (every
 * other stream_id's "payload" is not PES-framed the same way -- padding_stream
 * is pure stuffing bytes with NO optional header, private_stream_2 and the
 * PSM/PSD/ECM/EMM tables are single fixed/length-prefixed blocks). */
static inline int pes_has_std_header(unsigned sid)
{
    return sid != PES_SID_PROGRAM_STREAM_MAP && sid != PES_SID_PADDING &&
           sid != PES_SID_PRIVATE_2 && sid != PES_SID_ECM && sid != PES_SID_EMM &&
           sid != PES_SID_PROGRAM_STREAM_DIR && sid != PES_SID_DSMCC &&
           sid != PES_SID_H222_TYPE_E;
}

/* stream_type -> codec, shared by ts.c's PMT and ps.c's program_stream_map.
 * Writes *out_type (MEDIA_TRACK_VIDEO/AUDIO/OTHER) and returns the codec, or
 * MEDIA_CODEC_UNKNOWN for a recognised-as-A/V-but-undecodable or genuinely
 * unrecognised type -- the track is still indexed either way, never dropped,
 * matching the rest of this library's "a demuxer's job is the index, not the
 * decode" rule (mp4.c's unrecognised stsd fourcc, avi.c's unrecognised strf
 * fourcc). */
media_codec stream_type_codec(unsigned stream_type, int *out_type);

/* Best-effort keyframe scan for a fully-reassembled H.264/H.265 Annex B
 * access unit -- used where the container gives no explicit random-access
 * flag (PS; TS uses the adaptation field's random_access_indicator instead,
 * which is the spec-correct signal and does not need this). */
int es_h264_has_idr(const uint8_t *p, long n);
int es_h265_has_idr(const uint8_t *p, long n);

/* First ADTS frame in a raw AAC elementary stream: fills rate/channels/bits
 * from the header's own fields (TS/PS carry no out-of-band AudioSpecificConfig,
 * unlike MP4/FLV, so this is the only place that information exists). Returns
 * 1 and fills *rate / *channels, or 0 if `p` does not start with a plausible
 * ADTS sync. */
int pes_adts_probe(const uint8_t *p, long n, int *rate, int *channels);

#endif /* LOGIT_PES_H */
