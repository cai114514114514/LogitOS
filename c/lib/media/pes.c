/* c/lib/media/pes.c -- see pes.h. */
#include <string.h>
#include "pes.h"

long long pes_decode_ts5(br *b)
{
    const uint8_t *p = br_bytes(b, 5);
    if (!p) return 0;
    return ((long long)(p[0] & 0x0E) << 29) | ((long long)p[1] << 22) |
           ((long long)(p[2] & 0xFE) << 14) | ((long long)p[3] << 7) |
           ((long long)(p[4] & 0xFE) >> 1);
}

int pes_opt_header_mpeg2(br *b, long long *pts, long long *dts,
                          int *has_pts, int *has_dts)
{
    *pts = 0; *dts = 0; *has_pts = 0; *has_dts = 0;
    uint32_t b0 = br_u8(b);
    uint32_t b1 = br_u8(b);
    uint32_t hdr_len = br_u8(b);
    if (!br_ok(b)) return -1;
    br opt = br_sub(b, (long)hdr_len);
    if (!br_ok(&opt)) return -1;               /* claimed length ran off the packet */

    uint32_t ptsdts = (b1 >> 6) & 0x3;
    (void)b0;
    if (ptsdts == 2) {                          /* PTS only: 5 bytes, '0010' marker */
        *pts = pes_decode_ts5(&opt);
        *has_pts = br_ok(&opt);
    } else if (ptsdts == 3) {                   /* PTS then DTS */
        *pts = pes_decode_ts5(&opt);
        *has_pts = br_ok(&opt);
        *dts = pes_decode_ts5(&opt);
        *has_dts = br_ok(&opt);
    }
    /* Anything else in the optional header (ESCR, ES_rate, DSM trick mode,
     * additional copy info, CRC, extension) is inside `opt` and simply never
     * read -- `b` already advanced past all `hdr_len` bytes via br_sub. */
    return 0;
}

int pes_opt_header_mpeg1(br *b, long long *pts, long long *dts,
                          int *has_pts, int *has_dts)
{
    *pts = 0; *dts = 0; *has_pts = 0; *has_dts = 0;
    int guard;
    for (guard = 0; guard < 16; guard++) {
        long save = b->pos;
        uint32_t c = br_u8(b);
        if (!br_ok(b)) return -1;
        if (c != 0xFF) { b->pos = save; break; }
    }
    /* Optional STD buffer scale/size: top 2 bits '01'. */
    {
        long save = b->pos;
        uint32_t c = br_u8(b);
        if (br_ok(b) && (c >> 6) == 0x1) {
            br_u8(b);                           /* the low byte of size */
        } else {
            b->pos = save;
        }
    }
    if (!br_ok(b)) return -1;
    long save = b->pos;
    uint32_t c = br_u8(b);
    if (!br_ok(b)) return -1;
    if (c == 0x0F) return 0;                    /* no timestamp */
    if ((c >> 4) == 0x2) {                       /* '0010': PTS only */
        b->pos = save;
        *pts = pes_decode_ts5(b);
        *has_pts = br_ok(b);
        return br_ok(b) ? 0 : -1;
    }
    if ((c >> 4) == 0x3) {                       /* '0011': PTS, then DTS */
        b->pos = save;
        *pts = pes_decode_ts5(b);
        *has_pts = br_ok(b);
        uint32_t d0 = br_u8(b);
        if (!br_ok(b) || (d0 >> 4) != 0x1) return -1;   /* DTS must read '0001' */
        b->pos--;
        *dts = pes_decode_ts5(b);
        *has_dts = br_ok(b);
        return br_ok(b) ? 0 : -1;
    }
    return -1;                                   /* none of the three legal shapes */
}

media_codec stream_type_codec(unsigned stream_type, int *out_type)
{
    switch (stream_type) {
    case 0x01: case 0x02:                        /* MPEG-1/2 video */
        *out_type = MEDIA_TRACK_VIDEO; return MEDIA_CODEC_UNKNOWN;
    case 0x10:                                    /* MPEG-4 Part 2 video */
        *out_type = MEDIA_TRACK_VIDEO; return MEDIA_CODEC_MPEG4;
    case 0x1B: case 0x20:                         /* H.264 (0x20 = MVC substream) */
        *out_type = MEDIA_TRACK_VIDEO; return MEDIA_CODEC_H264;
    case 0x24:                                    /* HEVC */
        *out_type = MEDIA_TRACK_VIDEO; return MEDIA_CODEC_H265;
    case 0x03: case 0x04:                         /* MPEG-1/2 audio (layer 1/2/3) */
        *out_type = MEDIA_TRACK_AUDIO; return MEDIA_CODEC_MP3;
    case 0x0F: case 0x11:                         /* AAC ADTS / LATM */
        *out_type = MEDIA_TRACK_AUDIO; return MEDIA_CODEC_AAC;
    case 0x81:                                    /* ATSC AC-3 */
        *out_type = MEDIA_TRACK_AUDIO; return MEDIA_CODEC_AC3;
    case 0x06:                                    /* private data: often AC-3 via a
                                                     * registration descriptor this
                                                     * demuxer does not parse -- indexed,
                                                     * not decoded, same as an unknown
                                                     * fourcc elsewhere in this library */
        *out_type = MEDIA_TRACK_OTHER; return MEDIA_CODEC_UNKNOWN;
    default:
        *out_type = MEDIA_TRACK_OTHER; return MEDIA_CODEC_UNKNOWN;
    }
}

int es_h264_has_idr(const uint8_t *p, long n)
{
    long i = 0;
    while (i + 3 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (i + 3 < n && (p[i + 3] & 0x1F) == 5) return 1;
            i += 3; continue;
        }
        if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            if (i + 4 < n && (p[i + 4] & 0x1F) == 5) return 1;
            i += 4; continue;
        }
        i++;
    }
    return 0;
}

int es_h265_has_idr(const uint8_t *p, long n)
{
    long i = 0;
    while (i + 3 < n) {
        long sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) sc = 3;
        else if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        if (sc) {
            long nh = i + sc;
            if (nh < n) {
                unsigned nut = (p[nh] >> 1) & 0x3F;
                if (nut == 19 || nut == 20) return 1;   /* IDR_W_RADL / IDR_N_LP */
            }
            i += sc; continue;
        }
        i++;
    }
    return 0;
}

static const int adts_freq[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0
};

int pes_adts_probe(const uint8_t *p, long n, int *rate, int *channels)
{
    if (n < 7) return 0;
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) return 0;
    unsigned freq_idx = (p[2] >> 2) & 0x0F;
    unsigned chcfg = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    if (adts_freq[freq_idx] == 0) return 0;
    *rate = adts_freq[freq_idx];
    *channels = (int)chcfg;
    return 1;
}
