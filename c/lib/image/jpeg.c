#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memset(void *, int, unsigned long);

/* A from-scratch JPEG decoder: BASELINE (SOF0) sequential-DCT and PROGRESSIVE
 * (SOF2). Output is straight RGBA8. Supports 8-bit precision, 1-component
 * (grayscale) and 3-component (YCbCr) frames with arbitrary HxV sampling
 * factors (4:4:4 / 4:2:2 / 4:2:0 and the general case), 1-2 quantisation
 * tables (8- and 16-bit), DC+AC Huffman tables, and DRI/RSTn restart
 * intervals. Arithmetic coding, extended sequential (SOF1), 12-bit, lossless,
 * CMYK / 4-component and anything else are rejected gracefully (return -1) --
 * never a crash.
 *
 * WHY PROGRESSIVE IS A SECOND PATH AND NOT A GENERALISATION. Baseline decodes
 * one block at a time: entropy -> dequantise -> IDCT -> 8x8 of samples, and
 * the coefficients are never all live at once. Progressive cannot do that --
 * a coefficient is refined by later scans, so the WHOLE image's coefficients
 * must be held until EOI. That is the difference between a 64-long scratch
 * block and ~3 bytes per pixel of buffer, and it is the reason the baseline
 * path is left exactly as it was rather than rewritten to share code with
 * this one: the common case should not pay progressive's memory, and the
 * proven path should not be disturbed to save a hundred lines.
 *
 * SECURITY: every input byte is UNTRUSTED. Like png.c, all marker/segment lengths
 * are bounds-checked in subtraction form (no signed overflow), every table index,
 * sampling factor, MCU/block count and entropy bit read is validated, dimensions
 * are capped, every kmalloc is OOM-checked, and a single `fail:` path frees all.
 * The decode runs single-threaded under the kernel BKL, so the file-scope tables
 * below are safe; they are fully reset at the top of jpeg_decode. */

/* Zig-zag -> natural (row-major) coefficient order. */
static const unsigned char zz[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

struct huff {
    unsigned char bits[17];   /* bits[1..16] = #codes of each length */
    unsigned char vals[256];  /* symbols in canonical order */
    int mincode[17], maxcode[18], valptr[17];
    int defined;
};
struct comp { int id, h, v, tq, td, ta; long dcpred; };

/* One SOS header. Baseline has exactly one of these and constrains it hard
 * (all components, Ss=0, Se=63, Ah=Al=0); progressive has many, and the four
 * spectral/approximation fields are the whole of what distinguishes them. */
struct scan { int ns, ci[3], ss, se, ah, al; };

/* --- file-scope decode state (reset per call; single-threaded under BKL) --- */
static unsigned short qt[4][64];     /* quant tables, de-zig-zagged to natural order */
static struct huff hdc[4], hac[4];   /* DC / AC Huffman tables */
static struct comp jcomp[3];
static int jncomp, jmaxh, jmaxv, jrestart;

/* --- progressive state ---
 * pcoef[c] holds every coefficient of component c until EOI, in ZIG-ZAG order
 * within each block (scans address coefficients by zig-zag index, so storing
 * them that way keeps Ss..Se a plain range and moves the de-zigzag to the one
 * place that needs natural order, the IDCT). The grid is the PADDED MCU grid,
 * pbw[c] x pbh[c] blocks, so an interleaved scan's block address is a
 * multiplication and a non-interleaved scan's is a different one over the same
 * memory -- see the comment on the non-interleaved loop, which is where every
 * progressive decoder goes wrong.
 *
 * Coefficients are `short`, as in libjpeg. 8-bit JPEG coefficients fit, but a
 * CRAFTED file can name a DC category of 11 and a point transform of 13, whose
 * product does not -- so the shift is range-checked and refuses rather than
 * wrapping. eobrun spans blocks within one scan, never across scans. */
static short *pcoef[3];
static int pbw[3], pbh[3];
static int jprog, eobrun;

/* Progressive holds the whole coefficient plane, so it needs a bound the
 * baseline path does not: ~3 bytes/pixel at 4:2:0, 6 at 4:4:4. 64 MiB is
 * roughly a 20 Mpix photo -- past any image a page serves, and small enough
 * that a hostile 8192x8192 header cannot ask this kernel for 400 MB. */
#define JPEG_PROG_MAX_COEF (64u * 1024 * 1024)

static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }

static int jpeg_detect(const unsigned char *p, int n)
{
    return n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;  /* SOI */
}

/* Build the canonical Huffman decode tables (JPEG Annex F.2.2.3). */
static int build_huff(struct huff *h)
{
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len]  = k;
        h->mincode[len] = code;
        code += h->bits[len];
        h->maxcode[len] = code - 1;          /* -1 if no codes of this length */
        code <<= 1;
        k += h->bits[len];
        if (k > 256) return -1;              /* corrupt: too many symbols */
    }
    h->maxcode[17] = 0x7fffffff;             /* sentinel */
    h->defined = 1;
    return 0;
}

/* DQT: one or more quant tables packed into the segment. */
static int parse_dqt(const unsigned char *d, int len)
{
    int o = 0;
    while (o < len) {
        int pq = d[o] >> 4, tq = d[o] & 15; o++;
        if (tq > 3 || pq > 1) return -1;     /* 8-bit (pq=0) or 16-bit (pq=1) only */
        int ebytes = pq ? 2 : 1;
        if (64 * ebytes > len - o) return -1;  /* subtraction form, no overflow */
        for (int k = 0; k < 64; k++) {
            int v = pq ? be16(d + o) : d[o];
            o += ebytes;
            qt[tq][zz[k]] = (unsigned short)v;  /* store in natural order */
        }
    }
    return 0;
}

/* DHT: one or more Huffman tables packed into the segment. */
static int parse_dht(const unsigned char *d, int len)
{
    int o = 0;
    while (o < len) {
        if (17 > len - o) return -1;          /* class/id byte + 16 length counts */
        int tc = d[o] >> 4, th = d[o] & 15; o++;
        if (tc > 1 || th > 3) return -1;
        struct huff *h = tc ? &hac[th] : &hdc[th];
        memset(h->bits, 0, sizeof h->bits);
        int tot = 0;
        for (int i = 1; i <= 16; i++) { h->bits[i] = d[o + i - 1]; tot += h->bits[i]; if (tot > 256) return -1; }
        o += 16;
        if (tot > len - o) return -1;          /* subtraction form */
        for (int i = 0; i < tot; i++) h->vals[i] = d[o + i];
        o += tot;
        if (build_huff(h)) return -1;
    }
    return 0;
}

/* SOF0 / SOF2 frame header -- identical layout; only the marker differs. */
static int parse_sof(const unsigned char *d, int len, int *W, int *H)
{
    if (len < 6) return -1;
    if (d[0] != 8) return -1;                 /* 8-bit precision only */
    *H = be16(d + 1);
    *W = be16(d + 3);
    jncomp = d[5];
    if (jncomp != 1 && jncomp != 3) return -1; /* grayscale or YCbCr; reject CMYK/4-comp */
    if (*W <= 0 || *H <= 0) return -1;
    if (*W > 8192 || *H > 8192 || (long)*W * *H > 8192L * 8192) return -1;
    if (6 + jncomp * 3 > len) return -1;
    jmaxh = jmaxv = 0;
    for (int i = 0; i < jncomp; i++) {
        const unsigned char *c = d + 6 + i * 3;
        jcomp[i].id = c[0];
        jcomp[i].h  = c[1] >> 4;
        jcomp[i].v  = c[1] & 15;
        jcomp[i].tq = c[2];
        jcomp[i].td = jcomp[i].ta = jcomp[i].dcpred = 0;
        if (jcomp[i].h < 1 || jcomp[i].h > 4 || jcomp[i].v < 1 || jcomp[i].v > 4) return -1;
        if (jcomp[i].tq > 3) return -1;
        if (jcomp[i].h > jmaxh) jmaxh = jcomp[i].h;
        if (jcomp[i].v > jmaxv) jmaxv = jcomp[i].v;
    }
    return 0;
}

/* SOS scan header. Fills `sc` and binds each scan component's table selectors.
 *
 * The table-existence check is per ROLE, not per scan: a progressive DC scan
 * names an AC selector byte it never uses, and real encoders emit a DC-only
 * first scan BEFORE any DHT for an AC table exists. Demanding both tables
 * there rejects a large fraction of the progressive JPEGs on the web -- for a
 * table the scan is not going to read. */
static int parse_sos(const unsigned char *d, int len, struct scan *sc)
{
    if (len < 1) return -1;
    int ns = d[0];
    if (ns < 1 || ns > jncomp) return -1;
    if (1 + ns * 2 + 3 > len) return -1;
    const unsigned char *sp = d + 1 + ns * 2;
    sc->ns = ns;
    sc->ss = sp[0];
    sc->se = sp[1];
    sc->ah = sp[2] >> 4;
    sc->al = sp[2] & 15;
    if (sc->ss > 63 || sc->se > 63 || sc->ss > sc->se) return -1;
    if (sc->ah > 13 || sc->al > 13) return -1;

    int wants_dc = (sc->ss == 0), wants_ac = (sc->se > 0);
    for (int i = 0; i < ns; i++) {
        int cid = d[1 + i * 2], sel = d[2 + i * 2];
        int td = sel >> 4, ta = sel & 15;
        if (td > 3 || ta > 3) return -1;
        int j;
        for (j = 0; j < jncomp; j++) if (jcomp[j].id == cid) break;
        if (j == jncomp) return -1;           /* scan component not in frame */
        for (int q = 0; q < i; q++) if (sc->ci[q] == j) return -1;  /* named twice */
        sc->ci[i] = j;
        jcomp[j].td = td;
        jcomp[j].ta = ta;
        if (wants_dc && !hdc[td].defined) return -1;
        if (wants_ac && !hac[ta].defined) return -1;
    }

    if (!jprog) {
        /* Baseline: one interleaved scan over the whole spectrum, no
         * successive approximation. Unchanged from before progressive. */
        if (ns != jncomp) return -1;
        if (sc->ss != 0 || sc->se != 63 || sc->ah != 0 || sc->al != 0) return -1;
        return 0;
    }
    /* Progressive (G.1.1.1.1): a DC scan is Ss=0 AND Se=0 and may interleave;
     * an AC scan is Ss>0 and must name exactly one component, because AC
     * coefficients of different components have no common MCU ordering. */
    if (sc->ss == 0) { if (sc->se != 0) return -1; }
    else             { if (ns != 1) return -1; }
    return 0;
}

/* --- entropy-coded bit reader (MSB first, FF00 destuffing, marker latch) --- */
struct br { const unsigned char *d; int n, pos; unsigned buf; int cnt; int marker; };

static void br_init(struct br *b, const unsigned char *d, int n, int pos)
{ b->d = d; b->n = n; b->pos = pos; b->buf = 0; b->cnt = 0; b->marker = 0; }

static int br_bit(struct br *b)
{
    if (b->cnt == 0) {
        if (b->marker || b->pos >= b->n) return -1;   /* hit a marker / EOF */
        int c = b->d[b->pos++];
        if (c == 0xFF) {
            int m = (b->pos < b->n) ? b->d[b->pos] : 0xD9;
            if (m == 0x00) b->pos++;                   /* stuffed FF */
            else { b->marker = m; return -1; }         /* real marker: stop */
        }
        b->buf = (unsigned)c;
        b->cnt = 8;
    }
    b->cnt--;
    return (b->buf >> b->cnt) & 1;
}

static int br_recv(struct br *b, int s)   /* read s raw bits, s in 0..16 */
{
    int v = 0;
    for (int i = 0; i < s; i++) { int t = br_bit(b); if (t < 0) return -1; v = (v << 1) | t; }
    return v;
}

static int br_ext(int v, int s)           /* JPEG sign-extend an s-bit magnitude */
{ return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

/* Resync at a restart marker: align to a byte, skip to FF Dn, reset reader. */
static int br_restart(struct br *b)
{
    b->cnt = 0; b->buf = 0;
    /* If the bit reader already latched a marker, consume it if it is RSTn. */
    if (b->marker >= 0xD0 && b->marker <= 0xD7) {
        b->marker = 0;
        /* b->pos points at the marker byte (after the 0xFF). Step past it. */
        if (b->pos < b->n && b->d[b->pos] >= 0xD0 && b->d[b->pos] <= 0xD7) b->pos++;
        return 0;
    }
    b->marker = 0;
    /* Otherwise scan forward for FF D0..D7 within a bounded window. */
    int scanned = 0;
    while (b->pos + 1 < b->n && scanned < 8) {
        if (b->d[b->pos] == 0xFF) {
            int m = b->d[b->pos + 1];
            if (m >= 0xD0 && m <= 0xD7) { b->pos += 2; return 0; }
            if (m == 0xD9) return -1;          /* EOI before restart */
        }
        b->pos++; scanned++;
    }
    return -1;
}

/* Decode one Huffman symbol (Annex F.2.2.3). Returns -1 on invalid code. */
static int huff_decode(struct br *b, struct huff *h)
{
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int t = br_bit(b); if (t < 0) return -1;
        code = (code << 1) | t;
        if (code <= h->maxcode[len]) {
            int idx = h->valptr[len] + code - h->mincode[len];
            if (idx < 0 || idx >= 256) return -1;
            return h->vals[idx];
        }
    }
    return -1;
}

/* Decode + dequantise one 8x8 block into `blk` (natural order).
 * `blk` and `dcpred` are `long`: a coefficient magnitude can reach 2^15-1 and a
 * 16-bit DQT entry 65535, so coeff*quant nears 2^31; the running DC predictor can
 * sum that over every block in the image. 64-bit keeps both products and the
 * accumulator well clear of overflow (UB / wrong-pixel) on attacker-crafted input. */
static int decode_block(struct br *b, struct comp *c, long blk[64])
{
    memset(blk, 0, 64 * sizeof(long));
    int t = huff_decode(b, &hdc[c->td]);
    if (t < 0 || t > 11) return -1;             /* baseline: DC categories 12+ are reserved (12-bit precision) */
    int rv = t ? br_recv(b, t) : 0;
    if (rv < 0) return -1;                      /* entropy data ended mid-block */
    int diff = t ? br_ext(rv, t) : 0;
    c->dcpred += diff;
    blk[0] = c->dcpred * (long)qt[c->tq][0];
    int k = 1;
    while (k < 64) {
        int rs = huff_decode(b, &hac[c->ta]);
        if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }   /* ZRL: skip 16 zeros */
            break;                                /* EOB */
        }
        k += r;
        if (k > 63) return -1;                    /* run overrun: corrupt */
        int av = br_recv(b, s);
        if (av < 0) return -1;                    /* entropy data ended mid-block */
        int v = br_ext(av, s);
        blk[zz[k]] = (long)v * (long)qt[c->tq][zz[k]];   /* de-zigzag + dequant */
        k++;
    }
    return 0;
}

/* --- progressive (SOF2) block decoders (ITU T.81 Annex G) ---------------
 * Four of them, because a progressive coefficient is written by two kinds of
 * scan: a FIRST scan that establishes the high bits of a band, and REFINEMENT
 * scans that each append one lower bit. DC and AC differ again because AC has
 * the end-of-band run (one code standing for "the rest of this band is zero,
 * in this block and the next N"), which DC has no equivalent of.
 *
 * `blk` is 64 coefficients in ZIG-ZAG order, so Ss..Se is a plain index range.
 * Every shift that can be applied to a negative value is written as a
 * multiply, and every result is range-checked into `short` -- a crafted DC
 * category of 11 with Al=13 is representable in the file and not in the
 * buffer, and wrapping there is how a decoder produces confident garbage. */

static int prog_dc_first(struct br *b, struct comp *cp, short *blk, int al)
{
    int t = huff_decode(b, &hdc[cp->td]);
    if (t < 0 || t > 15) return -1;
    int diff = 0;
    if (t) { int rv = br_recv(b, t); if (rv < 0) return -1; diff = br_ext(rv, t); }
    cp->dcpred += diff;
    long v = cp->dcpred * (1L << al);
    if (v < -32768 || v > 32767) return -1;
    blk[0] = (short)v;
    return 0;
}

static int prog_dc_refine(struct br *b, short *blk, int al)
{
    int t = br_bit(b);
    if (t < 0) return -1;
    if (t) blk[0] = (short)(blk[0] | (1 << al));
    return 0;
}

static int prog_ac_first(struct br *b, struct comp *cp, short *blk,
                         int ss, int se, int al)
{
    if (eobrun > 0) { eobrun--; return 0; }
    int k = ss;
    while (k <= se) {
        int rs = huff_decode(b, &hac[cp->ta]);
        if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > se) return -1;               /* run walks past the band */
            int av = br_recv(b, s);
            if (av < 0) return -1;
            long v = (long)br_ext(av, s) * (1L << al);
            if (v < -32768 || v > 32767) return -1;
            blk[k] = (short)v;
            k++;
        } else if (r != 15) {
            /* EOB run: this block's band ends here, and so do the next
             * eobrun blocks'. The -1 is this block, consumed now. */
            eobrun = (1 << r) - 1;
            if (r) { int e = br_recv(b, r); if (e < 0) return -1; eobrun += e; }
            break;
        } else {
            k += 16;                             /* ZRL */
        }
    }
    return 0;
}

/* Refinement (G.1.2.3), the one everybody gets wrong. Each already-nonzero
 * coefficient in the band takes one correction bit -- IN BAND ORDER, whether
 * or not this scan has anything else to say about it -- and the run lengths
 * count only the coefficients that are still ZERO. Reading the correction
 * bits in any other order desynchronises the whole rest of the scan, which
 * shows up as a plausible-looking image with the bottom two thirds wrong. */
static int prog_ac_refine(struct br *b, struct comp *cp, short *blk,
                          int ss, int se, int al)
{
    int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;

    if (eobrun == 0) {
        for (; k <= se; k++) {
            int rs = huff_decode(b, &hac[cp->ta]);
            if (rs < 0) return -1;
            int r = rs >> 4, s = rs & 15;
            int newval = 0;
            if (s) {
                if (s != 1) return -1;           /* refinement magnitude is 1 bit */
                int t = br_bit(b);
                if (t < 0) return -1;
                newval = t ? p1 : m1;
            } else if (r != 15) {
                eobrun = 1 << r;
                if (r) { int e = br_recv(b, r); if (e < 0) return -1; eobrun += e; }
                break;
            }
            /* Walk forward over already-nonzero coefficients, giving each its
             * correction bit, until r still-zero ones have been passed. */
            while (k <= se) {
                short *cc = &blk[k];
                if (*cc) {
                    int t = br_bit(b);
                    if (t < 0) return -1;
                    if (t && (*cc & p1) == 0) {
                        int nv = *cc + ((*cc >= 0) ? p1 : m1);
                        if (nv < -32768 || nv > 32767) return -1;
                        *cc = (short)nv;
                    }
                } else if (--r < 0) {
                    break;
                }
                k++;
            }
            if (newval && k <= se) blk[k] = (short)newval;
        }
    }

    if (eobrun > 0) {
        /* Inside an EOB run this block contributes no new coefficients, but
         * its existing ones still each take a correction bit. */
#ifdef JPEG_NO_EOBRUN_CORRECTION
        /* NEGATIVE CONTROL: skip them. The bits are still in the stream, so
         * every later block in the scan reads someone else's -- which is why
         * this is the failure that looks like a working decoder. */
        k = se + 1;
#endif
        for (; k <= se; k++) {
            short *cc = &blk[k];
            if (*cc) {
                int t = br_bit(b);
                if (t < 0) return -1;
                if (t && (*cc & p1) == 0) {
                    int nv = *cc + ((*cc >= 0) ? p1 : m1);
                    if (nv < -32768 || nv > 32767) return -1;
                    *cc = (short)nv;
                }
            }
        }
        eobrun--;
    }
    return 0;
}

static short *pblk(int c, int bx, int by)
{ return pcoef[c] + ((long)by * pbw[c] + bx) * 64; }

static int prog_block(struct br *b, const struct scan *sc, int c, short *blk)
{
    if (sc->ss == 0)
        return sc->ah ? prog_dc_refine(b, blk, sc->al)
                      : prog_dc_first(b, &jcomp[c], blk, sc->al);
    return sc->ah ? prog_ac_refine(b, &jcomp[c], blk, sc->ss, sc->se, sc->al)
                  : prog_ac_first(b, &jcomp[c], blk, sc->ss, sc->se, sc->al);
}

/* Drive one progressive scan over its blocks.
 *
 * THE TRAP, and it is the reason this function does not just reuse the
 * baseline MCU loop: a scan naming ONE component is NON-INTERLEAVED, and its
 * unit is a single block over that component's OWN block grid --
 * ceil(ceil(W*h/hmax)/8) wide -- NOT the padded MCU grid. Those two differ
 * whenever the image is not a whole number of MCUs, which is most images: a
 * 4:2:0 chroma plane of a 100x100 picture is 7x7 blocks of real content in an
 * 8x8-block MCU grid. Walking the MCU grid instead decodes 15 blocks too many
 * and shifts every subsequent block by one, so the picture arrives sheared.
 * The buffer is still strided by pbw[c] -- only the ITERATION is different. */
static int prog_scan(struct br *b, const struct scan *sc, int W, int H,
                     int mx, int my)
{
    eobrun = 0;
    for (int i = 0; i < sc->ns; i++) jcomp[sc->ci[i]].dcpred = 0;

    int single = (sc->ns == 1);
    int c0 = sc->ci[0], cbw = 0, cbh = 0;
    long nunits;
    if (single) {
#ifdef JPEG_PROG_MCU_GRID
        /* NEGATIVE CONTROL: walk the padded MCU grid, the mistake described
         * above. Whole-MCU images are unaffected, which is the point -- a
         * control that broke everything would not show that the odd-size
         * cases are the ones carrying the property. */
        cbw = pbw[c0]; cbh = pbh[c0];
#else
        int compw = (W * jcomp[c0].h + jmaxh - 1) / jmaxh;
        int comph = (H * jcomp[c0].v + jmaxv - 1) / jmaxv;
        cbw = (compw + 7) / 8;
        cbh = (comph + 7) / 8;
#endif
        if (cbw <= 0 || cbh <= 0 || cbw > pbw[c0] || cbh > pbh[c0]) return -1;
        nunits = (long)cbw * cbh;
    } else {
        nunits = (long)mx * my;
    }

    for (long unit = 0; unit < nunits; unit++) {
        if (jrestart && unit && unit % jrestart == 0) {
            if (br_restart(b)) return -1;
            eobrun = 0;
            for (int i = 0; i < sc->ns; i++) jcomp[sc->ci[i]].dcpred = 0;
        }
        if (single) {
            if (prog_block(b, sc, c0, pblk(c0, (int)(unit % cbw), (int)(unit / cbw))))
                return -1;
        } else {
            int mxi = (int)(unit % mx), myi = (int)(unit / mx);
            for (int i = 0; i < sc->ns; i++) {
                int c = sc->ci[i];
                for (int by = 0; by < jcomp[c].v; by++)
                    for (int bx = 0; bx < jcomp[c].h; bx++)
                        if (prog_block(b, sc, c,
                                       pblk(c, mxi * jcomp[c].h + bx,
                                               myi * jcomp[c].v + by)))
                            return -1;
            }
        }
    }
    return 0;
}

/* First marker at or after `pos` that ends a scan's entropy data: not a
 * stuffed FF00, not a fill FF, not an RSTn (those belong to the scan). */
static int next_marker(const unsigned char *p, int n, int pos)
{
    if (pos < 0) pos = 0;
    while (pos + 1 < n) {
        if (p[pos] == 0xFF) {
            int m = p[pos + 1];
            if (m != 0x00 && m != 0xFF && !(m >= 0xD0 && m <= 0xD7)) return pos;
        }
        pos++;
    }
    return -1;
}

static int prog_alloc(int mx, int my)
{
    unsigned long total = 0;
    for (int c = 0; c < jncomp; c++) {
        pbw[c] = mx * jcomp[c].h;
        pbh[c] = my * jcomp[c].v;
        unsigned long sz = (unsigned long)pbw[c] * pbh[c] * 64 * sizeof(short);
        total += sz;
        if (total > JPEG_PROG_MAX_COEF) return -1;   /* refused out loud, not clamped */
    }
    for (int c = 0; c < jncomp; c++) {
        unsigned long sz = (unsigned long)pbw[c] * pbh[c] * 64 * sizeof(short);
        pcoef[c] = kmalloc(sz);
        if (!pcoef[c]) return -1;
        memset(pcoef[c], 0, sz);
    }
    return 0;
}

/* --- integer IDCT: IJG jpeg_idct_islow (LL&M), exact fixed-point, in natural
 * order, with the +128 level shift and 0..255 range-limit. Constants << 13. --- */
#define FIX_0_298631336  2446
#define FIX_0_390180644  3196
#define FIX_0_541196100  4433
#define FIX_0_765366865  6270
#define FIX_0_899976223  7373
#define FIX_1_175875602  9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172
#define CBITS 13
#define PASS1_BITS 2
#define DESCALE(x, nbits) (((x) + (1L << ((nbits) - 1))) >> (nbits))
#define CLAMP8(x) ((x) < 0 ? 0 : (x) > 255 ? 255 : (x))
/* Left-shift a possibly-negative signed value WITHOUT UB: C11 6.5.7p4 makes
 * `negative << n` undefined, and at -O2 (no -fwrapv) the optimiser may miscompile
 * around it. Shift through unsigned long (well-defined wraparound) and cast back.
 * Reachable on essentially every real JPEG (negative DCT coefficients). */
#define LSHIFT(x, n) ((long)((unsigned long)(long)(x) << (n)))

static void idct8x8(const long *in, unsigned char *out)
{
    long ws[64];
    /* Pass 1: columns. Output scaled up by PASS1_BITS (= descale by CBITS-PASS1_BITS). */
    for (int col = 0; col < 8; col++) {
        const long *ip = in + col;
        long *wp = ws + col;
        if (ip[8] == 0 && ip[16] == 0 && ip[24] == 0 && ip[32] == 0 &&
            ip[40] == 0 && ip[48] == 0 && ip[56] == 0) {
            /* DC-only column. ip[0] = coeff*quant can reach ~2^31, so shift in
             * `long` (the old `int dc = ip[0] << PASS1_BITS` overflowed/was UB). */
            long dc = LSHIFT(ip[0], PASS1_BITS);
            for (int r = 0; r < 8; r++) wp[r * 8] = dc;
            continue;
        }
        long z2 = ip[16], z3 = ip[48];
        long z1 = (z2 + z3) * FIX_0_541196100;
        long tmp2 = z1 + z3 * (-FIX_1_847759065);
        long tmp3 = z1 + z2 * FIX_0_765366865;
        z2 = ip[0]; z3 = ip[32];
        long tmp0 = LSHIFT(z2 + z3, CBITS), tmp1 = LSHIFT(z2 - z3, CBITS);
        long t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
        long o0 = ip[56], o1 = ip[40], o2 = ip[24], o3 = ip[8];
        z1 = o0 + o3; z2 = o1 + o2; z3 = o0 + o2; long z4 = o1 + o3;
        long z5 = (z3 + z4) * FIX_1_175875602;
        o0 *= FIX_0_298631336; o1 *= FIX_2_053119869; o2 *= FIX_3_072711026; o3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447; z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        o0 += z1 + z3; o1 += z2 + z4; o2 += z2 + z3; o3 += z1 + z4;
        wp[0]  = DESCALE(t10 + o3, CBITS - PASS1_BITS); wp[56] = DESCALE(t10 - o3, CBITS - PASS1_BITS);
        wp[8]  = DESCALE(t11 + o2, CBITS - PASS1_BITS); wp[48] = DESCALE(t11 - o2, CBITS - PASS1_BITS);
        wp[16] = DESCALE(t12 + o1, CBITS - PASS1_BITS); wp[40] = DESCALE(t12 - o1, CBITS - PASS1_BITS);
        wp[24] = DESCALE(t13 + o0, CBITS - PASS1_BITS); wp[32] = DESCALE(t13 - o0, CBITS - PASS1_BITS);
    }
    /* Pass 2: rows; +128 level shift; range-limit. Descale by CBITS+PASS1_BITS+3. */
    for (int r = 0; r < 8; r++) {
        const long *wp = ws + r * 8;
        unsigned char *op = out + r * 8;
        long z2 = wp[2], z3 = wp[6];
        long z1 = (z2 + z3) * FIX_0_541196100;
        long tmp2 = z1 + z3 * (-FIX_1_847759065);
        long tmp3 = z1 + z2 * FIX_0_765366865;
        long tmp0 = LSHIFT(wp[0] + wp[4], CBITS), tmp1 = LSHIFT(wp[0] - wp[4], CBITS);
        long t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
        long o0 = wp[7], o1 = wp[5], o2 = wp[3], o3 = wp[1];
        z1 = o0 + o3; z2 = o1 + o2; z3 = o0 + o2; long z4 = o1 + o3;
        long z5 = (z3 + z4) * FIX_1_175875602;
        o0 *= FIX_0_298631336; o1 *= FIX_2_053119869; o2 *= FIX_3_072711026; o3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447; z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        o0 += z1 + z3; o1 += z2 + z4; o2 += z2 + z3; o3 += z1 + z4;
        int v;
        v = (int)DESCALE(t10 + o3, CBITS + PASS1_BITS + 3) + 128; op[0] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t10 - o3, CBITS + PASS1_BITS + 3) + 128; op[7] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t11 + o2, CBITS + PASS1_BITS + 3) + 128; op[1] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t11 - o2, CBITS + PASS1_BITS + 3) + 128; op[6] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t12 + o1, CBITS + PASS1_BITS + 3) + 128; op[2] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t12 - o1, CBITS + PASS1_BITS + 3) + 128; op[5] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t13 + o0, CBITS + PASS1_BITS + 3) + 128; op[3] = (unsigned char)CLAMP8(v);
        v = (int)DESCALE(t13 - o0, CBITS + PASS1_BITS + 3) + 128; op[4] = (unsigned char)CLAMP8(v);
    }
}

/* YCbCr -> RGB, libjpeg jdcolor.c fixed-point coefficients, rounded + clamped. */
static void ycbcr(int y, int cb, int cr, unsigned char *o)
{
    cb -= 128; cr -= 128;
    int r = y + ((91881 * cr + (1 << 15)) >> 16);
    int g = y - ((22554 * cb + 46802 * cr + (1 << 15)) >> 16);
    int b = y + ((116130 * cb + (1 << 15)) >> 16);
    o[0] = (unsigned char)CLAMP8(r);
    o[1] = (unsigned char)CLAMP8(g);
    o[2] = (unsigned char)CLAMP8(b);
    o[3] = 255;
}

static int jpeg_decode(const unsigned char *p, int n, struct image *out)
{
    if (n < 3 || p[0] != 0xFF || p[1] != 0xD8) return -1;

    /* reset all decode state */
    memset(qt, 0, sizeof qt);
    memset(hdc, 0, sizeof hdc);
    memset(hac, 0, sizeof hac);
    memset(jcomp, 0, sizeof jcomp);
    jncomp = jmaxh = jmaxv = jrestart = 0;
    jprog = eobrun = 0;
    for (int c = 0; c < 3; c++) { pcoef[c] = 0; pbw[c] = pbh[c] = 0; }

    int W = 0, H = 0, sof_seen = 0;
    unsigned char *rgba = 0;
    unsigned char *plane[3] = { 0, 0, 0 };
    int cw[3] = { 0, 0, 0 }, ch[3] = { 0, 0, 0 };
    int mx = 0, my = 0;
    struct scan sc;

    int i = 2;
    int entry = -1;                 /* byte offset where entropy data begins */
    while (i + 4 <= n) {
        if (p[i] != 0xFF) goto fail;        /* every marker starts with 0xFF */
        int m = p[i + 1];
        if (m == 0xD9) goto fail;           /* EOI before SOS -> no image */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }  /* TEM/RSTn standalone */
        int len = be16(p + i + 2);
        if (len < 2 || len > n - i - 2) goto fail;  /* subtraction form; 16-bit length tops out at 65535 */
        const unsigned char *seg = p + i + 4;
        int slen = len - 2;
        if (m == 0xDB) { if (parse_dqt(seg, slen)) goto fail; }
        else if (m == 0xC4) { if (parse_dht(seg, slen)) goto fail; }
        else if (m == 0xC0 || m == 0xC2) {  /* baseline / progressive frame */
            if (sof_seen) goto fail;        /* two frames in one stream: not ours */
            if (parse_sof(seg, slen, &W, &H)) goto fail;
            jprog = (m == 0xC2);
            sof_seen = 1;
            /* MCU geometry, needed here rather than after the loop because a
             * progressive scan is decoded the moment its header is read. */
            int mcuw = jmaxh * 8, mcuh = jmaxv * 8;
            mx = (W + mcuw - 1) / mcuw;
            my = (H + mcuh - 1) / mcuh;
            if (mx <= 0 || my <= 0 || (long)mx * my > (8192L * 8192) / 64 + 8) goto fail;
            if (jprog && prog_alloc(mx, my)) goto fail;
        }
        else if (m == 0xC1 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8))
            goto fail;                      /* extended/lossless/arith SOF: reject */
        else if (m == 0xDD) { if (slen != 2) goto fail; jrestart = be16(seg); }
        else if (m == 0xDA) {               /* SOS: entropy data follows the header */
            if (!sof_seen) goto fail;
            if (parse_sos(seg, slen, &sc)) goto fail;
            if (!jprog) { entry = i + 4 + slen; break; }   /* baseline: one scan */
            /* Progressive: decode the scan now and keep walking markers. A
             * progressive file interleaves DHT segments between scans, so the
             * tables in force are whatever the last DHT set -- which is why
             * this cannot be hoisted out into a "collect all scans first" pass. */
            struct br pb;
            br_init(&pb, p, n, i + 4 + slen);
            if (prog_scan(&pb, &sc, W, H, mx, my)) goto fail;
            int nx = next_marker(p, n, pb.marker ? pb.pos - 1 : pb.pos);
            if (nx < 0) break;              /* stream ends with this scan */
            if (p[nx + 1] == 0xD9) break;   /* EOI */
            i = nx;
            continue;
        }
        /* APPn (E0..EF), COM (FE), DNL (DC), DAC (CC) and others: skip by length.
         * A marker is FF + code (2 bytes); the 2-byte length field includes itself,
         * so the next marker is at i + 2 + len. */
        i += 2 + len;
    }
    if (!sof_seen) goto fail;
    if (!jprog && entry < 0) goto fail;         /* baseline with no scan */
    if (jprog && !pcoef[0]) goto fail;          /* SOF2 whose scans never ran */

    rgba = kmalloc((unsigned long)W * H * 4);
    if (!rgba) goto fail;

    /* Per-component sample planes sized to the FULL MCU grid, so block writes and
     * chroma-upsample reads are always in-bounds. */
    for (int c = 0; c < jncomp; c++) {
        cw[c] = mx * jcomp[c].h * 8;
        ch[c] = my * jcomp[c].v * 8;
        plane[c] = kmalloc((unsigned long)cw[c] * ch[c]);
        if (!plane[c]) goto fail;
    }

    if (jprog) {
        /* Every scan is in; dequantise and transform the coefficient plane.
         * The whole padded MCU grid is transformed, not just the visible
         * blocks: the chroma upsample below reads the pad, exactly as the
         * baseline path's does. */
        for (int c = 0; c < jncomp; c++) {
            for (int by = 0; by < pbh[c]; by++) {
                for (int bx = 0; bx < pbw[c]; bx++) {
                    const short *src = pcoef[c] + ((long)by * pbw[c] + bx) * 64;
                    long blk[64];
                    unsigned char pix[64];
                    for (int k = 0; k < 64; k++)
                        blk[zz[k]] = (long)src[k] * (long)qt[jcomp[c].tq][zz[k]];
                    idct8x8(blk, pix);
                    for (int yy = 0; yy < 8; yy++)
                        for (int xx = 0; xx < 8; xx++)
                            plane[c][(by * 8 + yy) * cw[c] + bx * 8 + xx] = pix[yy * 8 + xx];
                }
            }
        }
        goto colour;
    }

    /* Entropy loop: interleaved MCU order (the only baseline ordering). */
    struct br b;
    br_init(&b, p, n, entry);
    int mcu = 0;
    for (int myi = 0; myi < my; myi++) {
        for (int mxi = 0; mxi < mx; mxi++) {
            if (jrestart && mcu && mcu % jrestart == 0) {
                if (br_restart(&b)) goto fail;
                for (int c = 0; c < jncomp; c++) jcomp[c].dcpred = 0;
            }
            for (int c = 0; c < jncomp; c++) {
                for (int by = 0; by < jcomp[c].v; by++) {
                    for (int bx = 0; bx < jcomp[c].h; bx++) {
                        long blk[64];
                        unsigned char pix[64];
                        if (decode_block(&b, &jcomp[c], blk)) goto fail;
                        idct8x8(blk, pix);
                        int ox = (mxi * jcomp[c].h + bx) * 8;
                        int oy = (myi * jcomp[c].v + by) * 8;
                        for (int yy = 0; yy < 8; yy++)
                            for (int xx = 0; xx < 8; xx++)
                                plane[c][(oy + yy) * cw[c] + ox + xx] = pix[yy * 8 + xx];
                    }
                }
            }
            mcu++;
        }
    }

colour:
    /* Color-convert + nearest-neighbour chroma upsample into rgba. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char *o = rgba + ((long)y * W + x) * 4;
            if (jncomp == 1) {
                int g = plane[0][y * cw[0] + x];   /* h=v=1 so cw[0] >= W */
                o[0] = o[1] = o[2] = (unsigned char)g; o[3] = 255;
            } else {
                int yv = plane[0][(y * jcomp[0].v / jmaxv) * cw[0] + (x * jcomp[0].h / jmaxh)];
                int cb = plane[1][(y * jcomp[1].v / jmaxv) * cw[1] + (x * jcomp[1].h / jmaxh)];
                int cr = plane[2][(y * jcomp[2].v / jmaxv) * cw[2] + (x * jcomp[2].h / jmaxh)];
                ycbcr(yv, cb, cr, o);
            }
        }
    }

    for (int c = 0; c < 3; c++) {
        if (plane[c]) kfree(plane[c]);
        if (pcoef[c]) { kfree(pcoef[c]); pcoef[c] = 0; }
    }
    out->w = W; out->h = H; out->rgba = rgba;
    return 0;

fail:
    for (int c = 0; c < 3; c++) {
        if (plane[c]) kfree(plane[c]);
        if (pcoef[c]) { kfree(pcoef[c]); pcoef[c] = 0; }
    }
    if (rgba) kfree(rgba);
    return -1;
}

void jpeg_register(void) { img_register(jpeg_detect, jpeg_decode); }
