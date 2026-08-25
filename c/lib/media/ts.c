/* c/lib/media/ts.c -- MPEG-2 Transport Stream demuxer.
 *
 * See ts.h for the memory-ownership story (samples point into a REASSEMBLED
 * scratch buffer, not the caller's file -- pair ts_open() with ts_close(),
 * not media_close()).
 *
 * PAT (PID 0) -> first program's PMT PID -> PMT's stream loop -> one track
 * per recognised elementary PID (stream_type_codec, pes.h, shared with
 * ps.c's program_stream_map). A PES payload is reassembled across every TS
 * packet for its PID between one payload_unit_start_indicator=1 and the
 * next; there is no need to trust PES_packet_length (video legally sets it
 * to 0, "unbounded") because PUSI is the reliable boundary regardless.
 *
 * TIMESTAMPS come straight out of each PES header's PTS/DTS, 90 kHz, with no
 * conversion -- ffprobe reports the same raw values for MPEG-TS, which is
 * what tools/gencontainers.sh's differential checks byte for byte (well,
 * integer for integer).
 *
 * KEYFRAME comes from the adaptation field's random_access_indicator on the
 * TS packet that starts the PES -- the spec-correct signal, and one TS has
 * that PS does not (see ps.c's NAL-scan fallback).
 *
 * WHAT THIS DOES NOT DO: 33-bit PTS/DTS wraparound (~26.5 h) is not
 * unwrapped -- every value is taken as ffprobe reports it (also unwrapped
 * within one 26.5 h window), fine for anything this test corpus produces and
 * named here rather than discovered later. Width/height are left at 0 for
 * every track: PAT/PMT genuinely carry neither (unlike AVI's strf or MP4's
 * stsd) -- a real player gets them by parsing the video SPS itself, which is
 * a decoder's job, not this demuxer's. AAC rate/channels ARE filled in, by
 * peeking at the first reassembled ADTS frame's own header (pes.h), because
 * TS carries no out-of-band AudioSpecificConfig either and that is the only
 * place the information exists.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"
#include "pes.h"
#include "ts.h"

#define TS_SYNC 0x47

/* 3-in-a-row at a candidate stride, from `at`: the sniff bar this file's
 * header promises. Returns 1/0. */
static int syncs3(const uint8_t *d, long n, long at, long stride)
{
    return at + stride * 2 < n &&
           d[at] == TS_SYNC && d[at + stride] == TS_SYNC && d[at + stride * 2] == TS_SYNC;
}

/* 188 (plain TS) or 192 (M2TS: 4-byte header + 188), or 0. Plain TS is tried
 * first: an M2TS file's bytes at offset 0 are its 4-byte header, essentially
 * never 0x47, so there is no real ambiguity between the two in practice, but
 * trying 188 first is also simply the more common format. */
static int detect_packet_size(const uint8_t *d, long n)
{
    if (syncs3(d, n, 0, 188)) return 188;
    if (syncs3(d, n, 4, 192)) return 192;
    return 0;
}

int ts_sniff(const uint8_t *d, long n)
{
    if (!d || n < 4) return 0;
    return detect_packet_size(d, n) != 0;
}

#define TS_MAX_PIDS 64

typedef struct { int pid, track; } pidtrack;
typedef struct { int pid, has_cc, last_cc; } ccent;

typedef struct {
    int       active;
    long      start_off;
    long      size;
    long long pts, dts;
    int       have_pts, have_dts;
    int       key;
} acc_t;

/* One PSI section's program/stream loop, shared shape between PAT and PMT:
 * both are "table header, then a loop of fixed-size entries up to
 * section_length". Kept inline per-caller below since PAT's and PMT's
 * headers differ (PAT has none of PMT's program_info_length dance) and the
 * loops parse different fields -- factoring it would cost more than it
 * saves for two call sites. */

int ts_parse(mdemux *m)
{
    long file_len = m->len;
    int psize = detect_packet_size(m->data, file_len);
    if (!psize) return MEDIA_ERR_CORRUPT;
    long pre = (psize == 192) ? 4 : 0;

    uint8_t *scratch = (uint8_t *)malloc((size_t)(file_len > 0 ? file_len : 1));
    if (!scratch) return MEDIA_ERR_OOM;
    long wpos = 0;

    pidtrack pidmap[TS_MAX_PIDS]; int npid = 0;
    ccent    cc[TS_MAX_PIDS];     int ncc = 0;
    int pmt_pid = -1;
    acc_t acc[MEDIA_MAX_TRACKS];
    memset(acc, 0, sizeof acc);

    int rc = MEDIA_OK;
    long off = 0;
    while (off + pre + 188 <= file_len) {
        const uint8_t *pkt = m->data + off + pre;
        off += psize;

        if (pkt[0] != TS_SYNC) { rc = MEDIA_ERR_CORRUPT; break; }
        int tei  = (pkt[1] >> 7) & 1;
        int pusi = (pkt[1] >> 6) & 1;
        int pid  = ((pkt[1] & 0x1F) << 8) | pkt[2];
        int afc  = (pkt[3] >> 4) & 0x3;
        int pcc  = pkt[3] & 0xF;

        if (pid == 0x1FFF) continue;              /* null/padding packet: not examined at all */
        if (tei)  { rc = MEDIA_ERR_CORRUPT; break; }
        if (afc == 0) { rc = MEDIA_ERR_CORRUPT; break; }   /* reserved value */

        const uint8_t *p = pkt + 4;
        long plen = 184;
        int discontinuity = 0, random_access = 0;
        if (afc == 2 || afc == 3) {
            int adlen = p[0];
            if (adlen > plen - 1) { rc = MEDIA_ERR_CORRUPT; break; }
            if (adlen > 0) {
                uint8_t fl = p[1];
                discontinuity  = (fl >> 7) & 1;
                random_access  = (fl >> 6) & 1;
            }
            p += 1 + adlen;
            plen -= 1 + adlen;
        }
        int has_payload = (afc == 1 || afc == 3);

        /* Continuity counter: one PID at a time, incrementing mod 16 on every
         * payload-bearing packet, reset only by its own discontinuity_indicator.
         * A gap here IS the "dropped TS packet" negative control, and failing
         * the parse is the only way this API has to report it (see ts.h). */
        if (has_payload) {
            int idx = -1;
            for (int k = 0; k < ncc; k++) if (cc[k].pid == pid) { idx = k; break; }
            if (idx < 0 && ncc < TS_MAX_PIDS) { idx = ncc++; cc[idx].pid = pid; cc[idx].has_cc = 0; }
            if (idx >= 0) {
                if (discontinuity) cc[idx].has_cc = 0;
                if (cc[idx].has_cc) {
                    if (pcc == cc[idx].last_cc) { continue; }   /* legal duplicate: discard */
                    int expect = (cc[idx].last_cc + 1) & 0xF;
                    if (pcc != expect) { rc = MEDIA_ERR_CORRUPT; break; }
                }
                cc[idx].last_cc = pcc;
                cc[idx].has_cc = 1;
            }
        }

        if (pid == 0x0000) {                       /* PAT */
            if (!has_payload || !pusi || plen < 1) continue;
            long pofs = p[0] + 1;                   /* pointer_field */
            if (pofs > plen) { rc = MEDIA_ERR_CORRUPT; break; }
            const uint8_t *sec = p + pofs;
            long seclen = plen - pofs;
            if (seclen < 8) continue;               /* PAT split across packets: not chased */
            unsigned seclength = ((unsigned)(sec[1] & 0x0F) << 8) | sec[2];
            long avail = seclen - 3;
            long uselen = (long)seclength < avail ? (long)seclength : avail;
            long po = 8;                            /* table_id..last_section_number = 8 bytes */
            while (po + 4 <= 3 + uselen && po + 4 <= seclen) {
                unsigned prog = ((unsigned)sec[po] << 8) | sec[po + 1];
                unsigned ppid = ((unsigned)(sec[po + 2] & 0x1F) << 8) | sec[po + 3];
                if (prog != 0 && pmt_pid < 0) pmt_pid = (int)ppid;
                po += 4;
            }
            continue;
        }

        if (pmt_pid >= 0 && pid == pmt_pid) {       /* PMT */
            if (!has_payload || !pusi || plen < 1) continue;
            long pofs = p[0] + 1;
            if (pofs > plen) { rc = MEDIA_ERR_CORRUPT; break; }
            const uint8_t *sec = p + pofs;
            long seclen = plen - pofs;
            if (seclen < 12) continue;
            unsigned seclength = ((unsigned)(sec[1] & 0x0F) << 8) | sec[2];
            long avail = seclen - 3;
            long uselen = (long)seclength < avail ? (long)seclength : avail;
            unsigned proginfolen = ((unsigned)(sec[10] & 0x0F) << 8) | sec[11];
            long so = 12 + proginfolen;
            while (so + 5 <= 3 + uselen && so + 5 <= seclen) {
                unsigned stype = sec[so];
                unsigned epid  = ((unsigned)(sec[so + 1] & 0x1F) << 8) | sec[so + 2];
                unsigned esil  = ((unsigned)(sec[so + 3] & 0x0F) << 8) | sec[so + 4];
                so += 5 + (long)esil;
                if (so > seclen) break;
                int already = -1;
                for (int k = 0; k < npid; k++) if (pidmap[k].pid == (int)epid) { already = k; break; }
                if (already >= 0) continue;
                if (npid >= TS_MAX_PIDS || m->ntracks >= MEDIA_MAX_TRACKS) continue;
                int ttype; media_codec codec = stream_type_codec(stype, &ttype);
                mtrack *t = md_add_track(m);
                if (!t) { rc = MEDIA_ERR_RANGE; goto fail; }
                t->t.id = (int)epid;
                t->t.type = ttype;
                t->t.codec = codec;
                t->t.timescale = 90000;
                pidmap[npid].pid = (int)epid; pidmap[npid].track = t->t.index; npid++;
            }
            continue;
        }

        int ti = -1;
        for (int k = 0; k < npid; k++) if (pidmap[k].pid == pid) { ti = pidmap[k].track; break; }
        if (ti < 0 || !has_payload) continue;

        mtrack *t = &m->tr[ti];
        if (pusi) {
            if (acc[ti].active) {
                long long dts = acc[ti].have_dts ? acc[ti].dts : acc[ti].pts;
                long long pts = acc[ti].have_pts ? acc[ti].pts : dts;
                int rc2 = md_push(t, dts, pts, acc[ti].start_off, acc[ti].size, acc[ti].key);
                if (rc2 != MEDIA_OK) { rc = rc2; goto fail; }
            }
            br pb; br_init(&pb, p, plen, 0);
            uint32_t startcode = br_u24(&pb);
            uint32_t stream_id = br_u8(&pb);
            br_u16(&pb);                            /* PES_packet_length: unused, see header comment */
            if (!br_ok(&pb) || startcode != 0x000001) { rc = MEDIA_ERR_CORRUPT; break; }
            long long pts = 0, dts = 0; int hp = 0, hd = 0;
            if (pes_has_std_header(stream_id)) {
                if (pes_opt_header_mpeg2(&pb, &pts, &dts, &hp, &hd) != 0) {
                    rc = MEDIA_ERR_CORRUPT; break;
                }
            }
            if (!br_ok(&pb)) { rc = MEDIA_ERR_CORRUPT; break; }
            long esn = plen - pb.pos;
            acc[ti].active = 1;
            acc[ti].start_off = wpos;
            acc[ti].size = 0;
            acc[ti].pts = pts; acc[ti].dts = dts;
            acc[ti].have_pts = hp; acc[ti].have_dts = hd;
            acc[ti].key = (t->t.type == MEDIA_TRACK_VIDEO) ? random_access : 1;
            if (esn > 0) {
                memcpy(scratch + wpos, p + pb.pos, (size_t)esn);
                wpos += esn;
                acc[ti].size += esn;
            }
        } else if (acc[ti].active && plen > 0) {
            memcpy(scratch + wpos, p, (size_t)plen);
            wpos += plen;
            acc[ti].size += plen;
        }
    }

    if (rc == MEDIA_OK) {
        for (int i = 0; i < m->ntracks; i++) {
            if (!acc[i].active) continue;
            long long dts = acc[i].have_dts ? acc[i].dts : acc[i].pts;
            long long pts = acc[i].have_pts ? acc[i].pts : dts;
            int rc2 = md_push(&m->tr[i], dts, pts, acc[i].start_off, acc[i].size, acc[i].key);
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

mdemux *ts_open(const uint8_t *data, long len, int *err)
{
    if (err) *err = MEDIA_OK;
    if (!ts_sniff(data, len)) { if (err) *err = MEDIA_ERR_UNSUPPORTED; return 0; }
    mdemux *m = (mdemux *)calloc(1, sizeof *m);
    if (!m) { if (err) *err = MEDIA_ERR_OOM; return 0; }
    m->data = data; m->len = len; m->kind = MEDIA_CONT_TS;
    m->movie_timescale = 90000;
    m->movie_duration = -1;
    m->selected = -1;

    /* ts_parse itself refuses a zero-track result (and frees its scratch
     * buffer before doing so) -- unlike avi_open's/mp4's caller-side check,
     * repeating that check here would risk freeing m->data twice (once
     * inside ts_parse's own fail path, once here), since a SUCCESSFUL
     * ts_parse has already reassigned m->data to the scratch buffer it owns.
     * See the memory-ownership note in ts.h.
     *
     * ON FAILURE, m->data is STILL THE CALLER'S BUFFER (the reassignment to
     * scratch happens only at the very end of a successful ts_parse), so it
     * must not be touched here -- but every track ts_parse built before
     * hitting the error already owns a heap-allocated msample array via
     * md_push's realloc (demux.c), exactly as a successful parse's tracks
     * do. media_close()/ts_close() both free those; a bare free(m) on this
     * path does not, and did not until this fix -- found by
     * container_fuzz.c's ASan leak check, which ts_parse's own error
     * injection (a continuity-counter gap, a corrupt PAT/PMT) reaches on a
     * real fuzzed stream after one or more tracks already have samples. */
    int e = ts_parse(m);
    if (e != MEDIA_OK) {
        for (int i = 0; i < m->ntracks; i++) free(m->tr[i].s);
        free(m); if (err) *err = e; return 0;
    }
    for (int i = 0; i < m->ntracks; i++) md_finish_track(&m->tr[i]);
    return m;
}

void ts_close(mdemux *m)
{
    if (!m) return;
    free((void *)m->data);                          /* the reassembled scratch buffer -- see ts.h */
    for (int i = 0; i < m->ntracks; i++) free(m->tr[i].s);
    free(m);
}
