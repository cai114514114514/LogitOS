/* c/lib/media/flv.c -- FLV (Flash Video) demuxer.
 *
 * "FLV\x01" + a flags byte (audio/video presence bits, rest reserved-zero) +
 * a 4-byte DataOffset, then a flat sequence of
 *   [u32 PreviousTagSize][u8 TagType][u24 DataSize][u24 Timestamp]
 *   [u8 TimestampExtended][u24 StreamID=0][DataSize bytes of tag data]
 * repeated to EOF. PreviousTagSize is CHECKED against the actual size of the
 * tag before it (11-byte header + DataSize) -- not merely skipped -- which
 * is this format's own built-in redundant length and exactly where "a wrong
 * tag size" (the negative control tools/gencontainers.sh drives) gets
 * caught, the same way AVI's idx1 cross-check catches a flipped dwOffset.
 *
 * TagType 8 = audio, 9 = video, 18 = script data (onMetaData and friends,
 * AMF0-encoded). The script tag is SKIPPED WHOLE by DataSize -- this demuxer
 * needs nothing out of it (duration/dimensions are read from the elementary
 * streams' own configuration, exactly as every other container here does),
 * and DataSize already gives an exact byte count, so there is no reason to
 * write an AMF0 parser just to walk past bytes a length field already
 * bounds. That is "parse only what you need" read literally: need nothing,
 * parse nothing, just skip the right number of bytes.
 *
 * VIDEO (CodecID 7 = AVC only -- see the file-level note on HEVC below):
 * FrameType (1=key) | CodecID, then AVCPacketType (0 = sequence header =
 * an AVCDecoderConfigurationRecord, byte-for-byte what MP4's avcC box holds
 * -- captured as this track's extradata so md_finish_track's existing
 * AVCC-detection rule, extradata[0]==1, applies unchanged, no format-specific
 * code needed there; 1 = NALU data, already length-prefixed exactly like
 * MP4's avcC framing; 2 = end of sequence, no payload), then a signed 24-bit
 * CompositionTime in milliseconds (FLV's ctts equivalent: pts = dts + ct).
 *
 * AUDIO (SoundFormat 10 = AAC only, for the same reason): AACPacketType 0 =
 * AudioSpecificConfig (captured as extradata, and also decoded here just far
 * enough to fill rate/channels -- FLV carries no ADTS sync per frame the way
 * TS does, so the ASC is the only place that information exists), 1 = one
 * raw (non-ADTS) AAC frame.
 *
 * TIMESTAMPS are already integer milliseconds (Timestamp | TimestampExtended
 * << 24, the extended byte forming bits 31-24 for files over ~4.66 hours) --
 * both tracks use timescale 1000, no conversion needed, and pts==dts except
 * for video's CompositionTime offset.
 *
 * WHAT THIS DOES NOT DO, BY NAME: any CodecID/SoundFormat other than AVC/AAC
 * (MP3, PCM, Speex, Nellymoser audio; Sorenson/VP6/Screen video) is
 * recognised as present but not indexed as a decodable track -- the FLV tag
 * stream for that stream simply produces no samples, matching the
 * demuxer's-job-is-the-index-not-the-decode rule used for AVI's unrecognised
 * strf fourccs. Legacy CodecID never defines HEVC; the 2023 "Enhanced RTMP"
 * extension does (a different tag header shape, FourCC-keyed), and is out of
 * scope here -- this is the classic FLV tag format only, which is also what
 * every encoder in this project's own gate corpus writes.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"
#include "flv.h"

static uint32_t rd_be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t rd_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int flv_sniff(const uint8_t *d, long n)
{
    if (!d || n < 9) return 0;
    if (d[0] != 'F' || d[1] != 'L' || d[2] != 'V') return 0;
    return (d[4] & 0xFA) == 0;                    /* reserved bits of TypeFlags are zero */
}

static const int aac_freqs[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0
};

int flv_parse(mdemux *m)
{
    if (!flv_sniff(m->data, m->len)) return MEDIA_ERR_CORRUPT;
    const uint8_t *hdr = m->data;
    uint32_t dataoffset = rd_be32(hdr + 5);
    if (dataoffset < 9 || dataoffset > (uint32_t)m->len) return MEDIA_ERR_CORRUPT;

    br top; br_init(&top, m->data, m->len, 0);
    br_seek(&top, (long)dataoffset);

    int vtrack = -1, atrack = -1;
    long long prev_total = 0;                      /* PreviousTagSize expected before tag 1 */

    while (br_left(&top) >= 4) {
        uint32_t prevsz = br_u32(&top);
        if (!br_ok(&top)) return MEDIA_ERR_CORRUPT;
        if ((long long)prevsz != prev_total) return MEDIA_ERR_CORRUPT;
        if (br_left(&top) < 11) break;              /* trailing PreviousTagSize: clean EOF */

        uint32_t tagtype  = br_u8(&top);
        uint32_t datasize = br_u24(&top);
        uint32_t ts24     = br_u24(&top);
        uint32_t tsext    = br_u8(&top);
        br_u24(&top);                               /* StreamID, always 0 */
        if (!br_ok(&top)) return MEDIA_ERR_CORRUPT;
        long long ts_ms = ((long long)tsext << 24) | (long long)ts24;

        br body = br_sub(&top, (long)datasize);
        if (!br_ok(&body)) return MEDIA_ERR_CORRUPT;   /* DataSize overruns the file: a "wrong tag size" */
        prev_total = 11 + (long long)datasize;

        if (tagtype == 9 && br_left(&body) >= 1) {     /* video */
            uint32_t b0 = br_u8(&body);
            uint32_t frametype = (b0 >> 4) & 0xF;
            uint32_t codecid   = b0 & 0xF;
            if (codecid == 7 && br_left(&body) >= 4) {
                uint32_t pkttype = br_u8(&body);
                const uint8_t *ctb = br_bytes(&body, 3);
                long ct = 0;
                if (ctb) {
                    ct = (long)rd_be24(ctb);
                    if (ct & 0x800000) ct -= 0x1000000;    /* sign-extend 24 bits */
                }
                if (vtrack < 0) {
                    mtrack *t = md_add_track(m);
                    if (!t) return MEDIA_ERR_RANGE;
                    t->t.type = MEDIA_TRACK_VIDEO;
                    t->t.codec = MEDIA_CODEC_H264;
                    t->t.timescale = 1000;
                    t->t.id = 9;
                    vtrack = t->t.index;
                }
                mtrack *t = &m->tr[vtrack];
                if (pkttype == 0) {
                    long n = br_left(&body);
                    const uint8_t *p = br_bytes(&body, n);
                    if (p && n <= MEDIA_MAX_EXTRADATA) { t->t.extradata = p; t->t.extradata_len = (int)n; }
                } else if (pkttype == 1) {
                    long off = body.org + body.pos;
                    long size = br_left(&body);
                    if (size > 0) {
                        int key = (frametype == 1);
                        int rc = md_push(t, ts_ms, ts_ms + ct, off, size, key);
                        if (rc != MEDIA_OK) return rc;
                    }
                }
                /* pkttype == 2 (end of sequence): no payload, nothing to index */
            }
        } else if (tagtype == 8 && br_left(&body) >= 1) {  /* audio */
            uint32_t b0 = br_u8(&body);
            uint32_t soundformat = (b0 >> 4) & 0xF;
            if (soundformat == 10 && br_left(&body) >= 1) {     /* AAC */
                uint32_t pkttype = br_u8(&body);
                if (atrack < 0) {
                    mtrack *t = md_add_track(m);
                    if (!t) return MEDIA_ERR_RANGE;
                    t->t.type = MEDIA_TRACK_AUDIO;
                    t->t.codec = MEDIA_CODEC_AAC;
                    t->t.timescale = 1000;
                    t->t.id = 8;
                    atrack = t->t.index;
                }
                mtrack *t = &m->tr[atrack];
                if (pkttype == 0) {
                    long n = br_left(&body);
                    const uint8_t *p = br_bytes(&body, n);
                    if (p && n <= MEDIA_MAX_EXTRADATA) {
                        t->t.extradata = p; t->t.extradata_len = (int)n;
                        if (n >= 2) {
                            unsigned freq_idx = ((p[0] & 0x07) << 1) | (p[1] >> 7);
                            unsigned chcfg = (p[1] >> 3) & 0x0F;
                            if (freq_idx < 16 && aac_freqs[freq_idx]) t->t.rate = aac_freqs[freq_idx];
                            if (chcfg >= 1 && chcfg <= 8) t->t.channels = (int)chcfg;
                        }
                    }
                } else if (pkttype == 1) {
                    long off = body.org + body.pos;
                    long size = br_left(&body);
                    if (size > 0) {
                        int rc = md_push(t, ts_ms, ts_ms, off, size, 1);
                        if (rc != MEDIA_OK) return rc;
                    }
                }
            }
            /* other SoundFormats (MP3/PCM/Speex/Nellymoser): recognised, not indexed */
        }
        /* tagtype 18 (script/onMetaData) and anything else: `body` already
         * carved out and skipped by DataSize -- nothing more to do. */
    }

    if (m->ntracks == 0) return MEDIA_ERR_CORRUPT;
    return MEDIA_OK;
}

mdemux *flv_open(const uint8_t *data, long len, int *err)
{
    if (err) *err = MEDIA_OK;
    if (!flv_sniff(data, len)) { if (err) *err = MEDIA_ERR_UNSUPPORTED; return 0; }
    mdemux *m = (mdemux *)calloc(1, sizeof *m);
    if (!m) { if (err) *err = MEDIA_ERR_OOM; return 0; }
    m->data = data; m->len = len; m->kind = MEDIA_CONT_FLV;
    m->movie_timescale = 1000;
    m->movie_duration = -1;
    m->selected = -1;

    int e = flv_parse(m);
    if (e == MEDIA_OK && m->ntracks == 0) e = MEDIA_ERR_CORRUPT;
    if (e != MEDIA_OK) { media_close(m); if (err) *err = e; return 0; }
    for (int i = 0; i < m->ntracks; i++) md_finish_track(&m->tr[i]);
    return m;
}
