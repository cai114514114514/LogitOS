/* c/lib/audio/ogg.c -- see ogg.h. */

#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "ogg.h"

#define OGG_MAX_PACKET (1 << 22)      /* 4 MiB: no real Vorbis/Opus packet is
                                       * within two orders of magnitude of this,
                                       * and it bounds what a lying segment
                                       * table can make us allocate */

struct oggreader {
    const uint8_t *data;
    long len;
    long pos;                          /* next page to parse */

    uint32_t serial;
    int have_serial;
    int nstreams;
    int64_t granule;

    /* Current page. */
    const uint8_t *body;
    long body_len;
    const uint8_t *segs;
    int nsegs;
    int seg;                           /* next segment index */
    long body_off;                     /* byte offset within body */
    int64_t page_granule;
    int continued;

    /* Assembly buffer for a packet that spans pages. */
    uint8_t *acc;
    long acc_len, acc_cap;

    int eos;
};

/* The Ogg CRC: a 32-bit CRC with polynomial 0x04c11db7, no reflection and no
 * final inversion, which is not the same as the CRC32 in zlib or in
 * /bin/audiocheck. Getting it wrong would reject every page, so the table is
 * built from the polynomial rather than copied. */
static uint32_t ogg_crc_table[256];
static int ogg_crc_ready;

static void crc_init(void)
{
    if (ogg_crc_ready) return;
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i << 24;
        for (int k = 0; k < 8; k++)
            r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
        ogg_crc_table[i] = r;
    }
    ogg_crc_ready = 1;
}

static uint32_t crc_run(uint32_t crc, const uint8_t *p, long n)
{
    while (n--) crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ *p++];
    return crc;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ogg_sniff(const uint8_t *data, long len)
{
    return data && len >= 27 && memcmp(data, "OggS", 4) == 0 && data[4] == 0;
}

oggreader *ogg_open(const uint8_t *data, long len, int *err)
{
    if (!data || len < 27) { if (err) *err = AUDIO_ERR_RANGE; return NULL; }
    if (!ogg_sniff(data, len)) { if (err) *err = AUDIO_ERR_CORRUPT; return NULL; }
    crc_init();

    oggreader *o = (oggreader *)malloc(sizeof(*o));
    if (!o) { if (err) *err = AUDIO_ERR_OOM; return NULL; }
    memset(o, 0, sizeof(*o));
    o->data = data;
    o->len = len;
    o->granule = -1;
    o->page_granule = -1;
    if (err) *err = AUDIO_OK;
    return o;
}

void ogg_close(oggreader *o)
{
    if (!o) return;
    free(o->acc);
    free(o);
}

int64_t ogg_granulepos(const oggreader *o) { return o ? o->granule : -1; }
uint32_t ogg_serial(const oggreader *o) { return o ? o->serial : 0; }
int ogg_nstreams(const oggreader *o) { return o ? o->nstreams : 0; }

/* Parse the page starting at o->pos into the current-page fields. Skips pages
 * belonging to other logical streams. Returns 1 on success, 0 at end of data,
 * negative on a malformed page. */
static int next_page(oggreader *o)
{
    for (;;) {
        /* Resynchronise on the capture pattern: a stream that has been cut and
         * spliced should cost pages, not the whole file. */
        while (o->pos + 27 <= o->len &&
               memcmp(o->data + o->pos, "OggS", 4) != 0)
            o->pos++;
        if (o->pos + 27 > o->len) return 0;

        const uint8_t *h = o->data + o->pos;
        if (h[4] != 0) { o->pos++; continue; }          /* unknown version */

        int nsegs = h[26];
        long hdrlen = 27 + nsegs;
        if (o->pos + hdrlen > o->len) return 0;

        long blen = 0;
        for (int i = 0; i < nsegs; i++) blen += h[27 + i];
        if (o->pos + hdrlen + blen > o->len) return 0;

        /* CRC over the page with the CRC field zeroed. */
        uint32_t want = rd32le(h + 22);
        uint32_t crc = crc_run(0, h, 22);
        static const uint8_t zero4[4] = { 0, 0, 0, 0 };
        crc = crc_run(crc, zero4, 4);
        crc = crc_run(crc, h + 26, 1 + nsegs);
        crc = crc_run(crc, h + hdrlen, blen);
        if (crc != want) {
            /* A page that fails its own checksum is not a page. Step past the
             * capture pattern and look for the next one rather than trusting a
             * length field from a corrupt header. */
            o->pos++;
            continue;
        }

        uint32_t serial = rd32le(h + 14);
        if (!o->have_serial) {
            o->serial = serial;
            o->have_serial = 1;
            o->nstreams = 1;
        } else if (serial != o->serial) {
            /* Another logical stream: counted and skipped. */
            int seen = 0;
            (void)seen;
            o->nstreams = 2;
            o->pos += hdrlen + blen;
            continue;
        }

        int64_t g = 0;
        for (int i = 7; i >= 0; i--) g = (g << 8) | h[6 + i];

        o->segs = h + 27;
        o->nsegs = nsegs;
        o->body = h + hdrlen;
        o->body_len = blen;
        o->seg = 0;
        o->body_off = 0;
        o->page_granule = g;
        o->continued = (h[5] & 0x01) ? 1 : 0;
        o->eos = (h[5] & 0x04) ? 1 : 0;
        o->pos += hdrlen + blen;
        return 1;
    }
}

int ogg_packet(oggreader *o, const uint8_t **pkt, long *len)
{
    if (!o || !pkt || !len) return AUDIO_ERR_RANGE;

    o->acc_len = 0;
    int spanning = 0;

    for (;;) {
        if (o->seg >= o->nsegs) {
            int r = next_page(o);
            if (r <= 0) {
                /* A packet left half-assembled at end of data is a truncated
                 * file: report end of stream rather than a partial packet. */
                return r;
            }
            /* If we are mid-packet, this page must be marked continued. */
            if (spanning && !o->continued) {
                /* A lost page: drop what we had and start fresh here. */
                o->acc_len = 0;
                spanning = 0;
            }
        }

        /* Walk segments until one is shorter than 255, which terminates the
         * packet. */
        long start = o->body_off;
        int term = 0;
        while (o->seg < o->nsegs) {
            int s = o->segs[o->seg++];
            o->body_off += s;
            if (s < 255) { term = 1; break; }
        }
        long chunk = o->body_off - start;
        if (start + chunk > o->body_len) return AUDIO_ERR_CORRUPT;

        if (!spanning && term) {
            /* The common case: the whole packet is contiguous inside the page,
             * so it is handed back in place with no copy. */
            *pkt = o->body + start;
            *len = chunk;
            o->granule = (o->seg >= o->nsegs) ? o->page_granule : -1;
            return 1;
        }

        if (o->acc_len + chunk > OGG_MAX_PACKET) return AUDIO_ERR_RANGE;
        if (o->acc_len + chunk > o->acc_cap) {
            long nc = o->acc_cap ? o->acc_cap * 2 : 65536;
            while (nc < o->acc_len + chunk) nc *= 2;
            uint8_t *nb = (uint8_t *)realloc(o->acc, (size_t)nc);
            if (!nb) return AUDIO_ERR_OOM;
            o->acc = nb;
            o->acc_cap = nc;
        }
        memcpy(o->acc + o->acc_len, o->body + start, (size_t)chunk);
        o->acc_len += chunk;
        spanning = 1;

        if (term) {
            *pkt = o->acc;
            *len = o->acc_len;
            o->granule = (o->seg >= o->nsegs) ? o->page_granule : -1;
            return 1;
        }
        /* Not terminated: fall round to pull the next page. */
    }
}
