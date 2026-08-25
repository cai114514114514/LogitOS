/* c/lib/media/ps.c -- MPEG Program Stream demuxer.
 *
 * See ps.h for the memory-ownership story (pair ps_open() with ps_close()).
 *
 * Top level is a flat sequence of start-code-prefixed structures: pack
 * headers (00 00 01 BA, MPEG-1 8-byte or MPEG-2 10-byte-plus-stuffing shape,
 * distinguished by the top bits of the byte right after the start code),
 * an optional system_header (00 00 01 BB, length-prefixed, skipped -- it
 * only bounds buffer sizes, nothing a demuxer's index needs), an optional
 * program_stream_map (00 00 01 BC, ISO/IEC 13818-1 2.5.4) that is this
 * format's PMT equivalent and is PARSED for real (stream_type per
 * elementary_stream_id, via pes.h's shared table -- same one ts.c's PMT
 * uses), PES packets for every other start code, and
 * MPEG_program_end_code (00 00 01 B9) which ends the stream cleanly.
 *
 * PES HEADER STYLE follows the pack header version last seen (MPEG-1 packs
 * use the stuffing/marker-nibble PES header shape, MPEG-2 packs use the
 * flag-byte shape) -- pes.h's two parsers implement both, and share the
 * 5-byte timestamp decode because that part is bit-identical between them.
 *
 * FRAME REASSEMBLY ACROSS PES PACKETS, for video only: a PES packet that
 * carries NO timestamp at all, arriving while a video access unit is
 * already being accumulated, is treated as a CONTINUATION of it rather than
 * a new one -- this is the same rule real DVD-authoring tools and
 * libavformat use, and it is a heuristic (not a bitstream-defined boundary)
 * for exactly the reason avi.c's keyframe scan is one: nothing else in a
 * bare PS PES header says "this is the rest of the previous picture."
 * Audio and private_stream_1 are never split this way by any encoder this
 * project has seen, so each of their PES packets is one complete sample.
 *
 * PRIVATE_STREAM_1 (0xBD) IS REFUSED BY NAME: it is recognised, given its
 * own track (MEDIA_TRACK_OTHER, MEDIA_CODEC_UNKNOWN) so its samples are
 * still indexed and never silently dropped, but never decoded as AC-3 or
 * anything else -- telling AC-3 apart from DVD subpictures or LPCM inside it
 * needs the 1-byte private sub-header this demuxer does not parse, and
 * guessing would be exactly the kind of "plausible small number where there
 * should have been none" this tree's own history warns about.
 *
 * KEYFRAME, for H.264/H.265 video, comes from an IDR NAL scan of the fully
 * reassembled access unit (pes.h) -- PS carries no random-access flag the
 * way TS's adaptation field does. For any other/unrecognised video codec,
 * the first sample only is marked key, the same last-resort avi.c already
 * uses for an AVI file with no index and an unknown video fourcc.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"
#include "pes.h"
#include "ps.h"

int ps_sniff(const uint8_t *d, long n)
{
    return d && n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0xBA;
}

typedef struct { int sid; int track; } sidtrack;
typedef struct {
    int       active;
    long      start_off;
    long      size;
    long long pts, dts;
    int       have_pts, have_dts;
} acc_t;

static int flush_acc(mtrack *t, acc_t *a, uint8_t *scratch, int *seen_key)
{
    if (!a->active) return MEDIA_OK;
    int key;
    if (t->t.type == MEDIA_TRACK_VIDEO) {
        if (t->t.codec == MEDIA_CODEC_H265) key = es_h265_has_idr(scratch + a->start_off, a->size);
        else if (t->t.codec == MEDIA_CODEC_H264) key = es_h264_has_idr(scratch + a->start_off, a->size);
        else key = !*seen_key;
        if (key) *seen_key = 1;
    } else {
        key = 1;
    }
    long long dts = a->have_dts ? a->dts : a->pts;
    long long pts = a->have_pts ? a->pts : dts;
    int rc = md_push(t, dts, pts, a->start_off, a->size, key);
    a->active = 0;
    return rc;
}

int ps_parse(mdemux *m)
{
    long file_len = m->len;
    uint8_t *scratch = (uint8_t *)malloc((size_t)(file_len > 0 ? file_len : 1));
    if (!scratch) return MEDIA_ERR_OOM;
    long wpos = 0;

    sidtrack smap[MEDIA_MAX_TRACKS]; int nsmap = 0;
    acc_t acc[MEDIA_MAX_TRACKS]; memset(acc, 0, sizeof acc);
    int seen_key[MEDIA_MAX_TRACKS]; memset(seen_key, 0, sizeof seen_key);
    int stype_of_sid[256];
    for (int i = 0; i < 256; i++) stype_of_sid[i] = -1;

    int mpeg_version = 2;
    int rc = MEDIA_OK, ended = 0;

    br top; br_init(&top, m->data, file_len, 0);
    while (!ended && br_left(&top) >= 4) {
        uint32_t sc = br_u24(&top);
        if (sc != 0x000001) { rc = MEDIA_ERR_CORRUPT; break; }
        uint32_t code = br_u8(&top);
        if (!br_ok(&top)) { rc = MEDIA_ERR_CORRUPT; break; }

        if (code == 0xBA) {                      /* pack_header */
            uint32_t b0 = br_u8(&top);
            if (!br_ok(&top)) { rc = MEDIA_ERR_CORRUPT; break; }
            if ((b0 >> 6) == 0x1) {               /* MPEG-2: 9 more bytes, low 3 bits of
                                                    * the last are pack_stuffing_length */
                mpeg_version = 2;
                const uint8_t *rest = br_bytes(&top, 9);
                if (!rest) { rc = MEDIA_ERR_CORRUPT; break; }
                int stuff = rest[8] & 0x07;
                if (stuff && !br_bytes(&top, stuff)) { rc = MEDIA_ERR_CORRUPT; break; }
            } else if ((b0 >> 4) == 0x2) {        /* MPEG-1: 7 more bytes, no stuffing field */
                mpeg_version = 1;
                if (!br_bytes(&top, 7)) { rc = MEDIA_ERR_CORRUPT; break; }
            } else {
                rc = MEDIA_ERR_CORRUPT; break;
            }
            continue;
        }
        if (code == 0xB9) { ended = 1; continue; }   /* MPEG_program_end_code */

        if (code == 0xBB) {                       /* system_header: skip by length */
            uint32_t hlen = br_u16(&top);
            if (!br_ok(&top) || !br_bytes(&top, (long)hlen)) { rc = MEDIA_ERR_CORRUPT; break; }
            continue;
        }

        if (code == 0xBC) {                       /* program_stream_map */
            uint32_t maplen = br_u16(&top);
            if (!br_ok(&top)) { rc = MEDIA_ERR_CORRUPT; break; }
            br sec = br_sub(&top, (long)maplen);
            if (!br_ok(&sec)) { rc = MEDIA_ERR_CORRUPT; break; }
            br_u8(&sec);                          /* current_next|reserved|version */
            br_u8(&sec);                          /* reserved|marker_bit */
            uint32_t psil = br_u16(&sec);
            br_skip(&sec, (long)psil);
            uint32_t esml = br_u16(&sec);
            br loop = br_sub(&sec, (long)esml);
            while (br_ok(&loop) && br_left(&loop) >= 4) {
                uint32_t stype = br_u8(&loop);
                uint32_t esid = br_u8(&loop);
                uint32_t esil = br_u16(&loop);
                br_skip(&loop, (long)esil);
                if (!br_ok(&loop)) break;
                stype_of_sid[esid & 0xFF] = (int)stype;
            }
            for (int i = 0; i < m->ntracks; i++) {
                int sid = m->tr[i].t.id;
                if (sid >= 0 && sid < 256 && stype_of_sid[sid] >= 0) {
                    int tt; media_codec c = stream_type_codec((unsigned)stype_of_sid[sid], &tt);
                    m->tr[i].t.type = tt; m->tr[i].t.codec = c;
                }
            }
            continue;
        }

        /* Every other start code is PES-shaped: stream_id (= `code`), a
         * 16-bit length, that many bytes of optional header + payload. */
        uint32_t pktlen = br_u16(&top);
        if (!br_ok(&top)) { rc = MEDIA_ERR_CORRUPT; break; }
        br pkt = br_sub(&top, (long)pktlen);
        if (!br_ok(&pkt)) { rc = MEDIA_ERR_CORRUPT; break; }
        unsigned sid = code;

        if (!pes_is_video(sid) && !pes_is_audio(sid) && sid != PES_SID_PRIVATE_1)
            continue;                             /* padding/private_stream_2/PSD/ECM/EMM: not indexed */

        long long pts = 0, dts = 0; int hp = 0, hd = 0;
        if (pes_has_std_header(sid)) {
            int hr = (mpeg_version == 1) ? pes_opt_header_mpeg1(&pkt, &pts, &dts, &hp, &hd)
                                          : pes_opt_header_mpeg2(&pkt, &pts, &dts, &hp, &hd);
            if (hr != 0) { rc = MEDIA_ERR_CORRUPT; break; }
        }
        if (!br_ok(&pkt)) { rc = MEDIA_ERR_CORRUPT; break; }
        long esn = br_left(&pkt);
        const uint8_t *espay = pkt.base + pkt.pos;

        int ti = -1;
        for (int k = 0; k < nsmap; k++) if (smap[k].sid == (int)sid) { ti = k; break; }
        if (ti < 0) {
            if (m->ntracks >= MEDIA_MAX_TRACKS) continue;
            mtrack *t = md_add_track(m);
            if (!t) { rc = MEDIA_ERR_RANGE; goto fail; }
            t->t.id = (int)sid;
            t->t.timescale = 90000;
            if (sid == PES_SID_PRIVATE_1) {
                t->t.type = MEDIA_TRACK_OTHER; t->t.codec = MEDIA_CODEC_UNKNOWN;
            } else if (stype_of_sid[sid & 0xFF] >= 0) {
                int tt; media_codec c = stream_type_codec((unsigned)stype_of_sid[sid & 0xFF], &tt);
                t->t.type = tt; t->t.codec = c;
            } else {
                t->t.type = pes_is_video(sid) ? MEDIA_TRACK_VIDEO : MEDIA_TRACK_AUDIO;
                t->t.codec = MEDIA_CODEC_UNKNOWN;
            }
            smap[nsmap].sid = (int)sid; smap[nsmap].track = t->t.index; ti = nsmap; nsmap++;
        }
        mtrack *t = &m->tr[smap[ti].track];
        int ai = smap[ti].track;

        if (t->t.type == MEDIA_TRACK_VIDEO && acc[ai].active && !hp && !hd) {
            /* continuation of the AU already being accumulated */
            if (esn > 0) { memcpy(scratch + wpos, espay, (size_t)esn); wpos += esn; acc[ai].size += esn; }
            continue;
        }

        if (acc[ai].active) {
            int rc2 = flush_acc(t, &acc[ai], scratch, &seen_key[ai]);
            if (rc2 != MEDIA_OK) { rc = rc2; goto fail; }
        }
        acc[ai].active = 1;
        acc[ai].start_off = wpos;
        acc[ai].size = 0;
        acc[ai].pts = pts; acc[ai].dts = dts;
        acc[ai].have_pts = hp; acc[ai].have_dts = hd;
        if (esn > 0) { memcpy(scratch + wpos, espay, (size_t)esn); wpos += esn; acc[ai].size += esn; }

        if (t->t.type != MEDIA_TRACK_VIDEO) {
            /* audio / private_stream_1: never split -- close it immediately */
            int rc2 = flush_acc(t, &acc[ai], scratch, &seen_key[ai]);
            if (rc2 != MEDIA_OK) { rc = rc2; goto fail; }
        }
    }

    if (rc == MEDIA_OK) {
        for (int i = 0; i < m->ntracks; i++) {
            int rc2 = flush_acc(&m->tr[i], &acc[i], scratch, &seen_key[i]);
            if (rc2 != MEDIA_OK) { rc = rc2; break; }
        }
    }
    if (rc != MEDIA_OK) goto fail;
    if (m->ntracks == 0) { rc = MEDIA_ERR_CORRUPT; goto fail; }

    m->data = scratch;
    m->len = wpos;

    for (int i = 0; i < m->ntracks; i++) {
        mtrack *t = &m->tr[i];
        if (t->t.codec == MEDIA_CODEC_AAC && t->n > 0) {
            int rate = 0, ch = 0;
            if (pes_adts_probe(scratch + t->s[0].off, (long)t->s[0].size, &rate, &ch)) {
                t->t.rate = rate; t->t.channels = ch;
            }
        }
    }
    return MEDIA_OK;

fail:
    free(scratch);
    return rc;
}

mdemux *ps_open(const uint8_t *data, long len, int *err)
{
    if (err) *err = MEDIA_OK;
    if (!ps_sniff(data, len)) { if (err) *err = MEDIA_ERR_UNSUPPORTED; return 0; }
    mdemux *m = (mdemux *)calloc(1, sizeof *m);
    if (!m) { if (err) *err = MEDIA_ERR_OOM; return 0; }
    m->data = data; m->len = len; m->kind = MEDIA_CONT_PS;
    m->movie_timescale = 90000;
    m->movie_duration = -1;
    m->selected = -1;

    /* Same leak as ts_open() had, same fix: on failure m->data is still the
     * caller's buffer (ps_parse only reassigns it to the reassembled scratch
     * buffer on success, and frees scratch itself on its own fail path --
     * see ps.h), but every track ps_parse built before the error already
     * owns a heap-allocated msample array (demux.c's md_push), and a bare
     * free(m) never freed those. Found by container_fuzz.c's ASan leak
     * check on a fuzzed stream where ps_parse fails (e.g. a truncated pack)
     * after at least one track already has samples. */
    int e = ps_parse(m);
    if (e != MEDIA_OK) {
        for (int i = 0; i < m->ntracks; i++) free(m->tr[i].s);
        free(m); if (err) *err = e; return 0;
    }
    for (int i = 0; i < m->ntracks; i++) md_finish_track(&m->tr[i]);
    return m;
}

void ps_close(mdemux *m)
{
    if (!m) return;
    free((void *)m->data);
    for (int i = 0; i < m->ntracks; i++) free(m->tr[i].s);
    free(m);
}
