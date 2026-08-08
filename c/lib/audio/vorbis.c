/* c/lib/audio/vorbis.c -- from-scratch Ogg Vorbis I decoder. See vorbis.h for
 * what is implemented and, more importantly, for why the bar here is a
 * differential rather than a conformance criterion: Vorbis I defines no
 * numeric bound on decoder output and ships no conformance suite.
 *
 * THE BIT ORDER IS THE OPPOSITE OF EVERY OTHER CODEC IN THIS LIBRARY.
 * MP3, FLAC and AAC pack most-significant-bit first, so they all share
 * abits.h. Vorbis packs LEAST-significant-bit first: a value written with n
 * bits comes out low bit first, and a Huffman codeword is transmitted starting
 * from the LOW bit of the codeword rather than the high one. That single fact
 * decides the shape of the reader and of the codebook tree below, and getting
 * it backwards produces a decoder that reads a plausible number of bits and
 * then desynchronises somewhere in the middle of a stream -- so it has its own
 * reader here instead of a flag on the shared one.
 */

#include <stdlib.h>
#include <string.h>
#include "vorbis.h"
#include "ogg.h"
#include "amath.h"
#include "afft.h"

/* --- LSB-first bit reader ------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    long len;
    long bitpos;
    int error;
} vbits;

static void vb_init(vbits *b, const uint8_t *d, long n)
{
    b->data = d; b->len = n; b->bitpos = 0; b->error = 0;
}

static long vb_left(const vbits *b) { return b->len * 8 - b->bitpos; }

static uint32_t vb_u(vbits *b, int n)
{
    if (n <= 0) return 0;
    if (n > 32) { b->error = 1; return 0; }
    if (b->error || vb_left(b) < n) { b->error = 1; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        long byte = (b->bitpos + i) >> 3;
        int bit = (int)((b->bitpos + i) & 7);
        v |= (uint32_t)((b->data[byte] >> bit) & 1) << i;
    }
    b->bitpos += n;
    return v;
}

static int vb_u1(vbits *b) { return (int)vb_u(b, 1); }

/* One bit, LSB-first, without setting the error flag past the end: the
 * codebook walk legitimately runs out of bits at the tail of a packet. */
static int vb_bit_soft(vbits *b, int *ok)
{
    if (vb_left(b) < 1) { *ok = 0; return 0; }
    long byte = b->bitpos >> 3;
    int bit = (int)(b->bitpos & 7);
    b->bitpos++;
    return (b->data[byte] >> bit) & 1;
}

/* ilog as the Vorbis specification defines it: the position of the highest set
 * bit, so ilog(0)=0, ilog(1)=1, ilog(7)=3. NOT floor(log2(x)). */
static int vilog(uint32_t x)
{
    int n = 0;
    while (x) { n++; x >>= 1; }
    return n;
}

static double float32_unpack(uint32_t x)
{
    double mantissa = (double)(x & 0x1FFFFFu);
    int sign = (x & 0x80000000u) != 0;
    int exponent = (int)((x & 0x7FE00000u) >> 21);
    if (sign) mantissa = -mantissa;
    return mantissa * a_exp2((double)(exponent - 788));
}

/* The greatest integer r with r^dim <= entries. */
static uint32_t lookup1_values(uint32_t entries, int dim)
{
    uint32_t r = 1;
    for (;;) {
        uint32_t next = r + 1;
        double p = 1.0;
        for (int i = 0; i < dim; i++) {
            p *= (double)next;
            if (p > (double)entries) break;
        }
        if (p > (double)entries) return r;
        r = next;
        if (r > 65535) return r;
    }
}

/* --- codebooks ----------------------------------------------------------- */

#define VB_MAX_CODEBOOKS  256
#define VB_MAX_ENTRIES    (1 << 20)

typedef struct {
    int dim;
    uint32_t entries;
    uint8_t *lengths;
    /* Binary tree over the transmitted bit order (LSB of the codeword first).
     * node 0 is the root; child[2*i] / child[2*i+1] are the branches, negative
     * values are -(entry+1) leaves, 0 means absent. */
    int32_t *tree;
    uint32_t nodes;
    int maxlen;

    int lookup;                 /* 0, 1 or 2 */
    int sequence_p;
    double *vq;                 /* entries*dim reconstructed vectors */
} vcodebook;

static void cb_free(vcodebook *c)
{
    free(c->lengths);
    free(c->tree);
    free(c->vq);
    memset(c, 0, sizeof(*c));
}

/* Assign codewords by the algorithm in the Vorbis I specification (the
 * "under-full binary tree" walk libvorbis calls _make_words), then insert them
 * into a tree keyed on the TRANSMITTED bit order, which is the codeword read
 * from its low bit upward. */
static int cb_build_tree(vcodebook *c)
{
    uint32_t marker[33];
    memset(marker, 0, sizeof(marker));

    uint32_t *codes = (uint32_t *)malloc((size_t)c->entries * sizeof(uint32_t));
    if (!codes) return AUDIO_ERR_OOM;

    int used = 0;
    for (uint32_t i = 0; i < c->entries; i++) {
        int len = c->lengths[i];
        codes[i] = 0;
        if (!len) continue;
        used++;
        if (len > 32) { free(codes); return AUDIO_ERR_CORRUPT; }
        uint32_t entry = marker[len];
        if (len < 32 && (entry >> len)) { free(codes); return AUDIO_ERR_CORRUPT; }
        codes[i] = entry;

        for (int j = len; j > 0; j--) {
            if (marker[j] & 1) {
                if (j == 1) marker[1]++;
                else marker[j] = marker[j - 1] << 1;
                break;
            }
            marker[j]++;
        }
        for (int j = len + 1; j < 33; j++) {
            if ((marker[j] >> 1) == entry) {
                entry = marker[j];
                marker[j] = marker[j - 1] << 1;
            } else {
                break;
            }
        }
    }

    if (used == 0) { free(codes); c->tree = NULL; c->nodes = 0; return AUDIO_OK; }

    /* Worst case one internal node per bit of every codeword. */
    uint32_t cap = 2;
    for (uint32_t i = 0; i < c->entries; i++) cap += 2u * (uint32_t)c->lengths[i];
    c->tree = (int32_t *)calloc((size_t)cap * 2, sizeof(int32_t));
    if (!c->tree) { free(codes); return AUDIO_ERR_OOM; }
    c->nodes = 1;

    c->maxlen = 0;
    for (uint32_t i = 0; i < c->entries; i++) {
        int len = c->lengths[i];
        if (!len) continue;
        if (len > c->maxlen) c->maxlen = len;
        uint32_t node = 0;
        for (int k = 0; k < len; k++) {
            /* THE ORDER HERE IS THE ONE THING IN VORBIS MOST WORTH GETTING
             * RIGHT FIRST TIME. Values in a Vorbis packet are packed
             * least-significant-bit first, so the natural guess is that a
             * codeword arrives low bit first too. It does not. The reference
             * encoder bit-reverses each codeword before handing it to the
             * LSb-first packer, precisely so that the codeword still travels
             * MOST significant bit first -- which is what makes a prefix code
             * decodable one bit at a time at all. Reading it the other way
             * builds a tree in which two codewords collide, which is how this
             * was caught: the very first codebook of the very first file
             * refused to build. */
            int bit = (int)((codes[i] >> (len - 1 - k)) & 1);
            int32_t *slot = &c->tree[node * 2 + bit];
            if (k == len - 1) {
                if (*slot != 0) { free(codes); return AUDIO_ERR_CORRUPT; }
                *slot = -(int32_t)(i + 1);
            } else {
                if (*slot < 0) { free(codes); return AUDIO_ERR_CORRUPT; }
                if (*slot == 0) {
                    if (c->nodes >= cap) { free(codes); return AUDIO_ERR_CORRUPT; }
                    *slot = (int32_t)c->nodes++;
                }
                node = (uint32_t)*slot;
            }
        }
    }
    free(codes);
    return AUDIO_OK;
}

/* Returns the entry index, or -1 at end of packet / on an unused branch. */
static int cb_decode(vbits *b, const vcodebook *c)
{
    if (!c->tree) return -1;
    uint32_t node = 0;
    for (int k = 0; k < 32; k++) {
        int ok = 1;
        int bit = vb_bit_soft(b, &ok);
        if (!ok) return -1;
        int32_t nx = c->tree[node * 2 + bit];
        if (nx < 0) return (int)(-nx - 1);
        if (nx == 0) return -1;
        node = (uint32_t)nx;
    }
    return -1;
}

static int cb_read(vbits *b, vcodebook *c)
{
    memset(c, 0, sizeof(*c));
    if (vb_u(b, 24) != 0x564342u) return AUDIO_ERR_CORRUPT;
    c->dim = (int)vb_u(b, 16);
    c->entries = vb_u(b, 24);
    if (c->dim <= 0 || c->dim > 256) return AUDIO_ERR_CORRUPT;
    if (c->entries == 0 || c->entries > VB_MAX_ENTRIES) return AUDIO_ERR_CORRUPT;

    c->lengths = (uint8_t *)calloc(c->entries, 1);
    if (!c->lengths) return AUDIO_ERR_OOM;

    int ordered = vb_u1(b);
    if (!ordered) {
        int sparse = vb_u1(b);
        for (uint32_t i = 0; i < c->entries; i++) {
            if (sparse && !vb_u1(b)) { c->lengths[i] = 0; continue; }
            c->lengths[i] = (uint8_t)(vb_u(b, 5) + 1);
        }
    } else {
        uint32_t cur = 0;
        int curlen = (int)vb_u(b, 5) + 1;
        while (cur < c->entries) {
            uint32_t num = vb_u(b, vilog(c->entries - cur));
            if (cur + num > c->entries) return AUDIO_ERR_CORRUPT;
            for (uint32_t i = 0; i < num; i++) c->lengths[cur + i] = (uint8_t)curlen;
            cur += num;
            curlen++;
            if (curlen > 32) return AUDIO_ERR_CORRUPT;
            if (b->error) return AUDIO_ERR_CORRUPT;
        }
    }
    if (b->error) return AUDIO_ERR_CORRUPT;

    int e = cb_build_tree(c);
    if (e != AUDIO_OK) return e;

    c->lookup = (int)vb_u(b, 4);
    if (c->lookup > 2) return AUDIO_ERR_CORRUPT;
    if (c->lookup) {
        double minv = float32_unpack(vb_u(b, 32));
        double delta = float32_unpack(vb_u(b, 32));
        int value_bits = (int)vb_u(b, 4) + 1;
        c->sequence_p = vb_u1(b);
        uint32_t lookup_values = (c->lookup == 1)
            ? lookup1_values(c->entries, c->dim)
            : c->entries * (uint32_t)c->dim;
        if (lookup_values == 0 || lookup_values > (1u << 24)) return AUDIO_ERR_CORRUPT;

        double *mult = (double *)malloc((size_t)lookup_values * sizeof(double));
        if (!mult) return AUDIO_ERR_OOM;
        for (uint32_t i = 0; i < lookup_values; i++)
            mult[i] = (double)vb_u(b, value_bits);
        if (b->error) { free(mult); return AUDIO_ERR_CORRUPT; }

        /* The vectors are expanded once here rather than per lookup: a residue
         * partition asks for the same entry thousands of times. */
        c->vq = (double *)malloc((size_t)c->entries * (size_t)c->dim * sizeof(double));
        if (!c->vq) { free(mult); return AUDIO_ERR_OOM; }

        for (uint32_t i = 0; i < c->entries; i++) {
            double last = 0.0;
            if (c->lookup == 1) {
                uint32_t indexdiv = 1;
                for (int j = 0; j < c->dim; j++) {
                    uint32_t off = (i / indexdiv) % lookup_values;
                    double v = mult[off] * delta + minv + last;
                    c->vq[(size_t)i * c->dim + j] = v;
                    if (c->sequence_p) last = v;
                    indexdiv *= lookup_values;
                }
            } else {
                uint32_t off = i * (uint32_t)c->dim;
                for (int j = 0; j < c->dim; j++) {
                    double v = mult[off + j] * delta + minv + last;
                    c->vq[(size_t)i * c->dim + j] = v;
                    if (c->sequence_p) last = v;
                }
            }
        }
        free(mult);
    }
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- floors -------------------------------------------------------------- */

#define VB_FLOOR1_MAX_X 65

typedef struct {
    int type;

    /* type 1 */
    int partitions;
    uint8_t partition_class[32];
    uint8_t class_dim[16];
    uint8_t class_subclasses[16];
    uint8_t class_masterbook[16];
    int16_t subclass_books[16][8];
    int multiplier;
    int rangebits;
    int nx;
    uint16_t xlist[VB_FLOOR1_MAX_X];
    int sorted[VB_FLOOR1_MAX_X];        /* indices of xlist in increasing x */

    /* type 0 */
    int order, rate, bark_map_size, amplitude_bits, amplitude_offset;
    int nbooks;
    uint8_t books[16];
} vfloor;

/* --- residues ------------------------------------------------------------ */

typedef struct {
    int type;
    uint32_t begin, end, partition_size;
    int classifications;
    int classbook;
    uint8_t cascade[64];
    int16_t books[64][8];
} vresidue;

/* --- mappings and modes -------------------------------------------------- */

typedef struct {
    int submaps;
    int coupling_steps;
    uint8_t mag[256], ang[256];
    uint8_t mux[VORBIS_MAX_CHANNELS];
    uint8_t submap_floor[16];
    uint8_t submap_residue[16];
} vmapping;

typedef struct {
    int blockflag;
    int mapping;
} vmode;

/* --- decoder ------------------------------------------------------------- */

struct vorbisdec {
    oggreader *ogg;
    const uint8_t *data;
    long len;

    int rate, channels;
    int bs0, bs1;                       /* block sizes */

    int ncodebooks;
    vcodebook *books;
    int nfloors;
    vfloor *floors;
    int nresidues;
    vresidue *residues;
    int nmappings;
    vmapping *mappings;
    int nmodes;
    vmode *modes;

    amdct *mdct0, *mdct1;
    double *win0, *win1;                /* the slope halves */

    double *coeff[VORBIS_MAX_CHANNELS];      /* bs1/2 spectral coefficients */
    double *pcmbuf[VORBIS_MAX_CHANNELS];     /* bs1 time samples */
    double *lap[VORBIS_MAX_CHANNELS];        /* the saved falling slope */
    double *floorcurve[VORBIS_MAX_CHANNELS]; /* bs1/2 floor values */
    int lap_n;                          /* valid samples in lap */
    int prev_n;                         /* previous block size */
    int have_prev;

    int *floor_y;                       /* VB_FLOOR1_MAX_X scratch */
    int *floor_step2;

    float *out;
    int64_t granule_end;
    int eos;
};

/* --- floor 1 ------------------------------------------------------------- */

static int floor1_read(vbits *b, vfloor *f)
{
    f->partitions = (int)vb_u(b, 5);
    int maxclass = -1;
    for (int i = 0; i < f->partitions; i++) {
        f->partition_class[i] = (uint8_t)vb_u(b, 4);
        if (f->partition_class[i] > 15) return AUDIO_ERR_CORRUPT;
        if ((int)f->partition_class[i] > maxclass) maxclass = f->partition_class[i];
    }
    for (int j = 0; j <= maxclass; j++) {
        f->class_dim[j] = (uint8_t)(vb_u(b, 3) + 1);
        f->class_subclasses[j] = (uint8_t)vb_u(b, 2);
        if (f->class_subclasses[j]) f->class_masterbook[j] = (uint8_t)vb_u(b, 8);
        int n = 1 << f->class_subclasses[j];
        for (int k = 0; k < n; k++)
            f->subclass_books[j][k] = (int16_t)((int)vb_u(b, 8) - 1);
    }
    f->multiplier = (int)vb_u(b, 2) + 1;
    f->rangebits = (int)vb_u(b, 4);

    f->xlist[0] = 0;
    f->xlist[1] = (uint16_t)(1u << f->rangebits);
    f->nx = 2;
    for (int i = 0; i < f->partitions; i++) {
        int cls = f->partition_class[i];
        for (int j = 0; j < f->class_dim[cls]; j++) {
            if (f->nx >= VB_FLOOR1_MAX_X) return AUDIO_ERR_CORRUPT;
            f->xlist[f->nx++] = (uint16_t)vb_u(b, f->rangebits);
        }
    }
    if (b->error) return AUDIO_ERR_CORRUPT;

    /* Insertion sort of the index list by x; the spec's neighbour search wants
     * the values in order and the list is at most 65 long. */
    for (int i = 0; i < f->nx; i++) f->sorted[i] = i;
    for (int i = 1; i < f->nx; i++) {
        int t = f->sorted[i], j = i - 1;
        while (j >= 0 && f->xlist[f->sorted[j]] > f->xlist[t]) {
            f->sorted[j + 1] = f->sorted[j];
            j--;
        }
        f->sorted[j + 1] = t;
    }
    /* Duplicate x values make the curve ambiguous, and the specification
     * forbids them. */
    for (int i = 1; i < f->nx; i++)
        if (f->xlist[f->sorted[i]] == f->xlist[f->sorted[i - 1]])
            return AUDIO_ERR_CORRUPT;
    return AUDIO_OK;
}

static const int FLOOR1_RANGE[4] = { 256, 128, 86, 64 };

/* The dB lookup of the Vorbis specification, floor1_inverse_dB_table.
 *
 * It is published as 256 float constants, and it is tempting to transcribe
 * them -- 256 chances to mistype. It is in fact a pure geometric progression:
 * a 140 dB range spread over 256 steps, ending at exactly 1.0, i.e.
 *
 *     table[i] = 10^( (i - 255) * (140/256) / 20 )
 *
 * which reproduces the published endpoints 1.0649863e-07 and 1.0 to every
 * digit they are printed with. Getting the step wrong is not loud: it is a
 * frequency-dependent gain error, so the output is still recognisably the
 * music and every sample is wrong by a different amount -- which is exactly
 * what the first run of this decoder produced, with a first guess of 0.5 dB
 * per step instead of 140/256. */
static double floor1_db(int x)
{
    static double tab[256];
    static int ready;
    if (!ready) {
#if AUDIO_SABOTAGE == 4
        /* THE NEGATIVE CONTROL. Half a dB per step instead of 140/256
         * is the wrong-but-plausible guess this decoder was actually
         * written with first. It leaves a decoder that still produces
         * the music with a tilted spectral envelope -- every sample
         * wrong by a different amount, nothing crashing, no structural
         * check disturbed. The gate must reject it. Only
         * -DAUDIO_SABOTAGE=4 builds it. */
        for (int i = 0; i < 256; i++)
            tab[i] = a_exp2(((double)i - 255.0) * 0.5 / 20.0
                            * 3.321928094887362);
#else
        for (int i = 0; i < 256; i++)
            tab[i] = a_exp2(((double)i - 255.0) * (140.0 / 256.0) / 20.0
                            * 3.321928094887362);
#endif
        ready = 1;
    }
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return tab[x];
}

static int low_neighbor(const uint16_t *v, int x)
{
    int best = -1;
    for (int i = 0; i < x; i++)
        if (v[i] < v[x] && (best < 0 || v[i] > v[best])) best = i;
    return best;
}

static int high_neighbor(const uint16_t *v, int x)
{
    int best = -1;
    for (int i = 0; i < x; i++)
        if (v[i] > v[x] && (best < 0 || v[i] < v[best])) best = i;
    return best;
}

static int render_point(int x0, int y0, int x1, int y1, int X)
{
    int dy = y1 - y0;
    int adx = x1 - x0;
    int ady = dy < 0 ? -dy : dy;
    int err = ady * (X - x0);
    int off = adx ? err / adx : 0;
    return dy < 0 ? y0 - off : y0 + off;
}

static void render_line(int x0, int y0, int x1, int y1, double *out, int n)
{
    int dy = y1 - y0;
    int adx = x1 - x0;
    int ady = dy < 0 ? -dy : dy;
    int base = adx ? dy / adx : 0;
    int sy = dy < 0 ? base - 1 : base + 1;
    int err = 0;
    int y = y0;
    ady = ady - (base < 0 ? -base : base) * adx;

    if (x0 < n) out[x0] = floor1_db(y);
    for (int x = x0 + 1; x < x1; x++) {
        if (x >= n) return;
        err += ady;
        if (err >= adx) { err -= adx; y += sy; }
        else y += base;
        out[x] = floor1_db(y);
    }
}

/* Returns 1 if the floor is non-zero (curve written to `out`), 0 if the packet
 * says this channel is silent, negative on error. */
static int floor1_decode(vorbisdec *v, vbits *b, vfloor *f, double *out, int n)
{
    if (!vb_u1(b)) return 0;

    int range = FLOOR1_RANGE[f->multiplier - 1];
    int *Y = v->floor_y;
    int *step2 = v->floor_step2;

    Y[0] = (int)vb_u(b, vilog((uint32_t)range - 1));
    Y[1] = (int)vb_u(b, vilog((uint32_t)range - 1));
    int offset = 2;

    for (int i = 0; i < f->partitions; i++) {
        int cls = f->partition_class[i];
        int cdim = f->class_dim[cls];
        int cbits = f->class_subclasses[cls];
        int csub = (1 << cbits) - 1;
        int cval = 0;
        if (cbits) {
            int mb = f->class_masterbook[cls];
            if (mb >= v->ncodebooks) return AUDIO_ERR_CORRUPT;
            cval = cb_decode(b, &v->books[mb]);
            if (cval < 0) return AUDIO_ERR_CORRUPT;
        }
        for (int j = 0; j < cdim; j++) {
            int bk = f->subclass_books[cls][cval & csub];
            cval >>= cbits;
            if (offset + j >= VB_FLOOR1_MAX_X) return AUDIO_ERR_CORRUPT;
            if (bk >= 0) {
                if (bk >= v->ncodebooks) return AUDIO_ERR_CORRUPT;
                int e = cb_decode(b, &v->books[bk]);
                if (e < 0) return AUDIO_ERR_CORRUPT;
                Y[offset + j] = e;
            } else {
                Y[offset + j] = 0;
            }
        }
        offset += cdim;
    }
    if (offset != f->nx) return AUDIO_ERR_CORRUPT;

    /* Amplitude reconstruction: each point is a delta from the line already
     * drawn between its neighbours, folded into a signed value by the
     * specification's zigzag. */
    step2[0] = 1;
    step2[1] = 1;
    int *final = Y;                       /* rewritten in place */
    for (int i = 2; i < f->nx; i++) {
        int lo = low_neighbor(f->xlist, i);
        int hi = high_neighbor(f->xlist, i);
        int predicted = render_point(f->xlist[lo], final[lo],
                                     f->xlist[hi], final[hi], f->xlist[i]);
        int val = Y[i];
        int highroom = range - predicted;
        int lowroom = predicted;
        int room = (highroom < lowroom ? highroom : lowroom) * 2;
        if (val) {
            step2[lo] = 1;
            step2[hi] = 1;
            step2[i] = 1;
            if (val >= room) {
                final[i] = (highroom > lowroom) ? val - lowroom + predicted
                                                : -val + highroom + predicted - 1;
            } else {
                final[i] = (val & 1) ? predicted - ((val + 1) >> 1)
                                     : predicted + (val >> 1);
            }
        } else {
            step2[i] = 0;
            final[i] = predicted;
        }
        if (final[i] < 0) final[i] = 0;
        if (final[i] >= range) final[i] = range - 1;
    }

    /* Curve synthesis over the sorted x values. */
    int hx = 0, hy = 0;
    int lx = 0, ly = final[f->sorted[0]] * f->multiplier;
    for (int i = 1; i < f->nx; i++) {
        int idx = f->sorted[i];
        if (!step2[idx]) continue;
        hx = f->xlist[idx];
        hy = final[idx] * f->multiplier;
        if (lx < hx) render_line(lx, ly, hx < n ? hx : n, hy, out, n);
        lx = hx;
        ly = hy;
    }
    if (lx < n) {
        /* The curve is flat from the last defined point to the end. */
        double vlast = floor1_db(ly);
        for (int i = lx; i < n; i++) out[i] = vlast;
    }
    return 1;
}

/* --- floor 0 (LSP) ------------------------------------------------------- */

static int floor0_read(vbits *b, vfloor *f)
{
    f->order = (int)vb_u(b, 8);
    f->rate = (int)vb_u(b, 16);
    f->bark_map_size = (int)vb_u(b, 16);
    f->amplitude_bits = (int)vb_u(b, 6);
    f->amplitude_offset = (int)vb_u(b, 8);
    f->nbooks = (int)vb_u(b, 4) + 1;
    if (f->order <= 0 || f->order > 255) return AUDIO_ERR_CORRUPT;
    if (f->bark_map_size <= 0) return AUDIO_ERR_CORRUPT;
    if (f->amplitude_bits > 32) return AUDIO_ERR_CORRUPT;
    for (int i = 0; i < f->nbooks; i++) f->books[i] = (uint8_t)vb_u(b, 8);
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* The Bark-scale map of the Vorbis specification. */
static double bark(double x)
{
    return 13.1 * a_atan(0.00074 * x)
         + 2.24 * a_atan(0.0000000185 * x * x)
         + 0.0001 * x;
}

/* FLOOR 0 IS IMPLEMENTED FROM THE SPECIFICATION AND IS NOT EXERCISED BY ANY
 * STREAM IN THE CORPUS, AND THAT IS WORTH SAYING PLAINLY. libvorbis has never
 * emitted floor 0 and neither does any encoder within reach, so the
 * differential in tests/unit/vorbis_test.c cannot reach this function. It is
 * here because floor 0 files exist in the wild from the format's early years
 * and refusing them outright would be a worse answer than decoding them with
 * code written straight from the formula -- but it has not been measured
 * against a reference, and nothing in this project's test output claims it
 * has. */
static int floor0_decode(vorbisdec *v, vbits *b, vfloor *f, double *out, int n)
{
    int amplitude = (int)vb_u(b, f->amplitude_bits);
    if (b->error) return AUDIO_ERR_CORRUPT;
    if (amplitude <= 0) return 0;

    int bookno = (int)vb_u(b, vilog((uint32_t)f->nbooks));
    if (bookno >= f->nbooks) return AUDIO_ERR_CORRUPT;
    int bk = f->books[bookno];
    if (bk >= v->ncodebooks) return AUDIO_ERR_CORRUPT;
    vcodebook *c = &v->books[bk];
    if (!c->vq || c->dim <= 0) return AUDIO_ERR_CORRUPT;

    double coefs[256];
    int ncoef = 0;
    double last = 0.0;
    while (ncoef < f->order) {
        int e = cb_decode(b, c);
        if (e < 0) return AUDIO_ERR_CORRUPT;
        for (int j = 0; j < c->dim && ncoef < f->order; j++)
            coefs[ncoef++] = c->vq[(size_t)e * c->dim + j] + last;
        last = coefs[ncoef - 1];
    }

    double barkmax = bark((double)f->rate * 0.5);
    if (barkmax <= 0.0) return AUDIO_ERR_CORRUPT;

    int i = 0;
    while (i < n) {
        double bx = bark((double)f->rate * 0.5 / (double)n * (double)i);
        int mapped = (int)(bx * (double)f->bark_map_size / barkmax);
        if (mapped > f->bark_map_size - 1) mapped = f->bark_map_size - 1;
        if (mapped < 0) mapped = 0;

        double w = A_PI * (double)mapped / (double)f->bark_map_size;
        double cw = a_cos(w);
        double p = 1.0, q = 1.0;

        if (f->order & 1) {
            for (int j = 0; j + 1 < f->order - 1; j += 2) {
                double t = a_cos(coefs[j + 1]) - cw;
                p *= 4.0 * t * t;
            }
            p *= (1.0 - cw * cw);
            for (int j = 0; j < f->order; j += 2) {
                double t = a_cos(coefs[j]) - cw;
                q *= 4.0 * t * t;
            }
            q *= 0.25;
        } else {
            for (int j = 0; j + 1 < f->order; j += 2) {
                double t = a_cos(coefs[j + 1]) - cw;
                p *= 4.0 * t * t;
            }
            p *= (1.0 - cw) * 0.5;
            for (int j = 0; j + 1 < f->order; j += 2) {
                double t = a_cos(coefs[j]) - cw;
                q *= 4.0 * t * t;
            }
            q *= (1.0 + cw) * 0.5;
        }

        double denom = a_sqrt(p + q);
        double scale = (double)((1u << f->amplitude_bits) - 1u);
        double linear = 0.0;
        if (denom > 0.0) {
            double arg = 0.11512925 * ((double)amplitude * (double)f->amplitude_offset
                                       / (scale * denom)
                                       - (double)f->amplitude_offset);
            linear = a_exp(arg);
        }

        int cond = mapped;
        while (i < n) {
            double bx2 = bark((double)f->rate * 0.5 / (double)n * (double)i);
            int m2 = (int)(bx2 * (double)f->bark_map_size / barkmax);
            if (m2 > f->bark_map_size - 1) m2 = f->bark_map_size - 1;
            if (m2 < 0) m2 = 0;
            if (m2 != cond) break;
            out[i++] = linear;
        }
    }
    return 1;
}

/* --- residues ------------------------------------------------------------ */

static int residue_read(vbits *b, vresidue *r)
{
    r->type = (int)vb_u(b, 16);
    if (r->type > 2) return AUDIO_ERR_CORRUPT;
    r->begin = vb_u(b, 24);
    r->end = vb_u(b, 24);
    r->partition_size = vb_u(b, 24) + 1;
    r->classifications = (int)vb_u(b, 6) + 1;
    r->classbook = (int)vb_u(b, 8);
    if (r->end < r->begin) return AUDIO_ERR_CORRUPT;
    if (r->partition_size == 0 || r->partition_size > (1u << 20)) return AUDIO_ERR_CORRUPT;
    if (r->classifications > 64) return AUDIO_ERR_CORRUPT;

    for (int i = 0; i < r->classifications; i++) {
        int high = 0;
        int low = (int)vb_u(b, 3);
        if (vb_u1(b)) high = (int)vb_u(b, 5);
        r->cascade[i] = (uint8_t)(low | (high << 3));
    }
    for (int i = 0; i < r->classifications; i++)
        for (int j = 0; j < 8; j++)
            r->books[i][j] = (r->cascade[i] & (1 << j))
                ? (int16_t)vb_u(b, 8) : (int16_t)-1;
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* Decode one residue vector set. `vecs` are the per-channel output vectors of
 * length n; `dnc` says which are actually decoded (a channel whose floor was
 * silent is skipped in types 0 and 1 and zeroed in type 2's deinterleave). */
static int residue_decode(vorbisdec *v, vbits *b, vresidue *r,
                          double **vecs, int nch, int n, const int *do_not_decode)
{
    if (r->classbook >= v->ncodebooks) return AUDIO_ERR_CORRUPT;
    vcodebook *cbook = &v->books[r->classbook];
    if (cbook->dim <= 0) return AUDIO_ERR_CORRUPT;

    uint32_t begin = r->begin, end = r->end;
    if (r->type == 2) {
        /* Type 2 treats the channels as one interleaved vector. */
        if ((uint32_t)(n * nch) < end) end = (uint32_t)(n * nch);
    } else {
        if ((uint32_t)n < end) end = (uint32_t)n;
    }
    if (begin >= end) return AUDIO_OK;

    uint32_t nres = end - begin;
    uint32_t partitions = nres / r->partition_size;
    if (partitions == 0) return AUDIO_OK;
    int pass_ch = (r->type == 2) ? 1 : nch;

    /* classifications[ch][partition] */
    uint8_t *cls = (uint8_t *)malloc((size_t)pass_ch * partitions);
    if (!cls) return AUDIO_ERR_OOM;

    int partitions_per_word = cbook->dim;
    for (int pass = 0; pass < 8; pass++) {
        uint32_t p = 0;
        while (p < partitions) {
            if (pass == 0) {
                for (int j = 0; j < pass_ch; j++) {
                    if (r->type != 2 && do_not_decode[j]) continue;
                    int e = cb_decode(b, cbook);
                    if (e < 0) { free(cls); return AUDIO_ERR_CORRUPT; }
                    /* The class word packs partitions_per_word classifications
                     * base-`classifications`, most recent LAST. */
                    for (int k = partitions_per_word - 1; k >= 0; k--) {
                        uint32_t idx = p + (uint32_t)k;
                        int cv = e % r->classifications;
                        e /= r->classifications;
                        if (idx < partitions) cls[(size_t)j * partitions + idx] = (uint8_t)cv;
                    }
                }
            }
            for (int k = 0; k < partitions_per_word && p < partitions; k++, p++) {
                for (int j = 0; j < pass_ch; j++) {
                    if (r->type != 2 && do_not_decode[j]) continue;
                    int cv = cls[(size_t)j * partitions + p];
                    int bk = r->books[cv][pass];
                    if (bk < 0) continue;
                    if (bk >= v->ncodebooks) { free(cls); return AUDIO_ERR_CORRUPT; }
                    vcodebook *c = &v->books[bk];
                    if (!c->vq) { free(cls); return AUDIO_ERR_CORRUPT; }

                    uint32_t base = begin + p * r->partition_size;
                    uint32_t psize = r->partition_size;

                    if (r->type == 0) {
                        /* Interleaved by the codebook dimension. */
                        uint32_t step = psize / (uint32_t)c->dim;
                        for (uint32_t s = 0; s < step; s++) {
                            int e = cb_decode(b, c);
                            if (e < 0) { free(cls); return AUDIO_ERR_CORRUPT; }
                            for (int q = 0; q < c->dim; q++) {
                                uint32_t off = base + s + (uint32_t)q * step;
                                if (off < (uint32_t)n) vecs[j][off] += c->vq[(size_t)e * c->dim + q];
                            }
                        }
                    } else if (r->type == 1) {
                        uint32_t s = 0;
                        while (s < psize) {
                            int e = cb_decode(b, c);
                            if (e < 0) { free(cls); return AUDIO_ERR_CORRUPT; }
                            for (int q = 0; q < c->dim && s < psize; q++, s++) {
                                uint32_t off = base + s;
                                if (off < (uint32_t)n) vecs[j][off] += c->vq[(size_t)e * c->dim + q];
                            }
                        }
                    } else {
                        /* Type 2: one interleaved vector across all channels. */
                        uint32_t s = 0;
                        while (s < psize) {
                            int e = cb_decode(b, c);
                            if (e < 0) { free(cls); return AUDIO_ERR_CORRUPT; }
                            for (int q = 0; q < c->dim && s < psize; q++, s++) {
                                uint32_t off = base + s;
                                uint32_t ch = off % (uint32_t)nch;
                                uint32_t idx = off / (uint32_t)nch;
                                if (idx < (uint32_t)n)
                                    vecs[ch][idx] += c->vq[(size_t)e * c->dim + q];
                            }
                        }
                    }
                }
            }
        }
    }
    free(cls);
    return AUDIO_OK;
}

/* --- headers ------------------------------------------------------------- */

static int check_header(const uint8_t *p, long len, int type)
{
    return len >= 7 && p[0] == (uint8_t)type && memcmp(p + 1, "vorbis", 6) == 0;
}

static int parse_id(vorbisdec *v, const uint8_t *p, long len)
{
    if (!check_header(p, len, 1) || len < 30) return AUDIO_ERR_CORRUPT;
    vbits b;
    vb_init(&b, p + 7, len - 7);
    if (vb_u(&b, 32) != 0) return AUDIO_ERR_UNSUPPORTED;      /* vorbis_version */
    v->channels = (int)vb_u(&b, 8);
    v->rate = (int)vb_u(&b, 32);
    vb_u(&b, 32); vb_u(&b, 32); vb_u(&b, 32);                 /* bitrates */
    int b0 = (int)vb_u(&b, 4);
    int b1 = (int)vb_u(&b, 4);
    if (!vb_u1(&b)) return AUDIO_ERR_CORRUPT;                 /* framing */
    if (v->channels < 1 || v->channels > VORBIS_MAX_CHANNELS) return AUDIO_ERR_UNSUPPORTED;
    if (v->rate < AUDIO_MIN_RATE || v->rate > AUDIO_MAX_RATE) return AUDIO_ERR_RANGE;
    if (b0 < 6 || b0 > 13 || b1 < 6 || b1 > 13 || b0 > b1) return AUDIO_ERR_CORRUPT;
    v->bs0 = 1 << b0;
    v->bs1 = 1 << b1;
    return AUDIO_OK;
}

static int parse_setup(vorbisdec *v, const uint8_t *p, long len)
{
    if (!check_header(p, len, 5)) return AUDIO_ERR_CORRUPT;
    vbits b;
    vb_init(&b, p + 7, len - 7);
    int e;

    v->ncodebooks = (int)vb_u(&b, 8) + 1;
    if (v->ncodebooks > VB_MAX_CODEBOOKS) return AUDIO_ERR_CORRUPT;
    v->books = (vcodebook *)calloc((size_t)v->ncodebooks, sizeof(vcodebook));
    if (!v->books) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->ncodebooks; i++) {
        e = cb_read(&b, &v->books[i]);
        if (e != AUDIO_OK) return e;
    }

    /* Time domain transforms: a count of placeholders that must all be zero. */
    int ntimes = (int)vb_u(&b, 6) + 1;
    for (int i = 0; i < ntimes; i++)
        if (vb_u(&b, 16) != 0) return AUDIO_ERR_CORRUPT;

    v->nfloors = (int)vb_u(&b, 6) + 1;
    v->floors = (vfloor *)calloc((size_t)v->nfloors, sizeof(vfloor));
    if (!v->floors) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->nfloors; i++) {
        v->floors[i].type = (int)vb_u(&b, 16);
        if (v->floors[i].type == 0) e = floor0_read(&b, &v->floors[i]);
        else if (v->floors[i].type == 1) e = floor1_read(&b, &v->floors[i]);
        else return AUDIO_ERR_CORRUPT;
        if (e != AUDIO_OK) return e;
    }

    v->nresidues = (int)vb_u(&b, 6) + 1;
    v->residues = (vresidue *)calloc((size_t)v->nresidues, sizeof(vresidue));
    if (!v->residues) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->nresidues; i++) {
        e = residue_read(&b, &v->residues[i]);
        if (e != AUDIO_OK) return e;
    }

    v->nmappings = (int)vb_u(&b, 6) + 1;
    v->mappings = (vmapping *)calloc((size_t)v->nmappings, sizeof(vmapping));
    if (!v->mappings) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->nmappings; i++) {
        vmapping *m = &v->mappings[i];
        if (vb_u(&b, 16) != 0) return AUDIO_ERR_CORRUPT;       /* mapping type */
        m->submaps = vb_u1(&b) ? (int)vb_u(&b, 4) + 1 : 1;
        if (m->submaps > 16) return AUDIO_ERR_CORRUPT;
        if (vb_u1(&b)) {
            m->coupling_steps = (int)vb_u(&b, 8) + 1;
            if (m->coupling_steps > 256) return AUDIO_ERR_CORRUPT;
            int bits = vilog((uint32_t)v->channels - 1);
            for (int k = 0; k < m->coupling_steps; k++) {
                m->mag[k] = (uint8_t)vb_u(&b, bits);
                m->ang[k] = (uint8_t)vb_u(&b, bits);
                if (m->mag[k] == m->ang[k] ||
                    m->mag[k] >= v->channels || m->ang[k] >= v->channels)
                    return AUDIO_ERR_CORRUPT;
            }
        }
        if (vb_u(&b, 2) != 0) return AUDIO_ERR_CORRUPT;        /* reserved */
        if (m->submaps > 1) {
            for (int c = 0; c < v->channels; c++) {
                m->mux[c] = (uint8_t)vb_u(&b, 4);
                if (m->mux[c] >= m->submaps) return AUDIO_ERR_CORRUPT;
            }
        }
        for (int s = 0; s < m->submaps; s++) {
            vb_u(&b, 8);                                       /* unused */
            m->submap_floor[s] = (uint8_t)vb_u(&b, 8);
            m->submap_residue[s] = (uint8_t)vb_u(&b, 8);
            if (m->submap_floor[s] >= v->nfloors ||
                m->submap_residue[s] >= v->nresidues)
                return AUDIO_ERR_CORRUPT;
        }
    }

    v->nmodes = (int)vb_u(&b, 6) + 1;
    v->modes = (vmode *)calloc((size_t)v->nmodes, sizeof(vmode));
    if (!v->modes) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->nmodes; i++) {
        v->modes[i].blockflag = vb_u1(&b);
        vb_u(&b, 16);                                          /* windowtype */
        vb_u(&b, 16);                                          /* transformtype */
        v->modes[i].mapping = (int)vb_u(&b, 8);
        if (v->modes[i].mapping >= v->nmappings) return AUDIO_ERR_CORRUPT;
    }
    if (!vb_u1(&b)) return AUDIO_ERR_CORRUPT;                  /* framing */
    return b.error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- allocation ---------------------------------------------------------- */

static int alloc_state(vorbisdec *v)
{
    v->mdct0 = amdct_new(v->bs0);
    v->mdct1 = amdct_new(v->bs1);
    if (!v->mdct0 || !v->mdct1) return AUDIO_ERR_OOM;

    /* The Vorbis window slope, from the specification:
     *     w(i) = sin( pi/2 * sin^2( (i+0.5)/n * pi ) )
     * where n is the block size and i runs over the n/2 samples of the slope. */
    v->win0 = (double *)malloc((size_t)(v->bs0 / 2) * sizeof(double));
    v->win1 = (double *)malloc((size_t)(v->bs1 / 2) * sizeof(double));
    if (!v->win0 || !v->win1) return AUDIO_ERR_OOM;
    for (int i = 0; i < v->bs0 / 2; i++) {
        double s = a_sin(A_PI / (double)v->bs0 * ((double)i + 0.5));
        v->win0[i] = a_sin(A_PI / 2.0 * s * s);
    }
    for (int i = 0; i < v->bs1 / 2; i++) {
        double s = a_sin(A_PI / (double)v->bs1 * ((double)i + 0.5));
        v->win1[i] = a_sin(A_PI / 2.0 * s * s);
    }

    for (int c = 0; c < v->channels; c++) {
        v->coeff[c] = (double *)calloc((size_t)v->bs1, sizeof(double));
        v->pcmbuf[c] = (double *)calloc((size_t)v->bs1, sizeof(double));
        v->lap[c] = (double *)calloc((size_t)v->bs1, sizeof(double));
        v->floorcurve[c] = (double *)calloc((size_t)v->bs1, sizeof(double));
        if (!v->coeff[c] || !v->pcmbuf[c] || !v->lap[c] || !v->floorcurve[c])
            return AUDIO_ERR_OOM;
    }
    v->floor_y = (int *)calloc(VB_FLOOR1_MAX_X, sizeof(int));
    v->floor_step2 = (int *)calloc(VB_FLOOR1_MAX_X, sizeof(int));
    v->out = (float *)calloc((size_t)v->bs1 * (size_t)v->channels, sizeof(float));
    if (!v->floor_y || !v->floor_step2 || !v->out) return AUDIO_ERR_OOM;
    return AUDIO_OK;
}

/* --- audio packet -------------------------------------------------------- */

/* THE LAPPING, WHICH IS THE PART OF VORBIS MOST WORTH WRITING DOWN.
 *
 * Every block carries a window with a rising slope, a flat middle, and a
 * falling slope. Consecutive blocks are placed so that one block's falling
 * slope lands exactly on the next block's rising slope, and the two slopes are
 * mirror images, so their sum is one. When the block sizes differ the SHORTER
 * slope wins: a long block next to a short one grows a flat shoulder and puts
 * its short slope in the middle of it, which is what the prev_window and
 * next_window flags in the packet are for.
 *
 * The four boundaries below are exactly the specification's, with n0 and n1
 * the two block sizes:
 *
 *   short block          left  [0, n0/2)            right [n0/2, n0)
 *   long, prev short     left  [(n1-n0)/4, (n1+n0)/4)
 *   long, prev long      left  [0, n1/2)
 *   long, next short     right [(3n1-n0)/4, (3n1+n0)/4)
 *   long, next long      right [n1/2, n1)
 *
 * The implementation then needs no windowing pass at all. The IMDCT output is
 * left unwindowed; the falling slope of the previous block is saved verbatim;
 * and when the next block arrives its rising region and that saved region are
 * combined in one step with w[j] and w[L-1-j]. Everything outside
 * [left_start, right_start) either belongs to a neighbour or is windowed to
 * zero, so it is never touched. The frame's output is precisely
 * [left_start, right_start), and the FIRST packet of a stream produces none at
 * all -- it only primes the overlap, which is why vorbis_decode can return a
 * frame with nsamples == 0 and that is not an error.
 */
static int decode_audio(vorbisdec *v, const uint8_t *pkt, long len, vorbisframe *out)
{
    out->rate = v->rate;
    out->channels = v->channels;
    out->nsamples = 0;
    out->pcm = v->out;

    vbits b;
    vb_init(&b, pkt, len);
    if (vb_u1(&b)) return AUDIO_ERR_CORRUPT;                  /* not an audio packet */

    int modeno = (int)vb_u(&b, vilog((uint32_t)v->nmodes - 1));
    if (modeno >= v->nmodes) return AUDIO_ERR_CORRUPT;
    vmode *mode = &v->modes[modeno];
    vmapping *map = &v->mappings[mode->mapping];

    int n0 = v->bs0, n1 = v->bs1;
    int n = mode->blockflag ? n1 : n0;
    int prev_win = 1, next_win = 1;
    if (mode->blockflag) {
        prev_win = vb_u1(&b);
        next_win = vb_u1(&b);
    }
    if (b.error) return AUDIO_ERR_CORRUPT;

    int left_start, left_end, right_start, right_end;
    if (!mode->blockflag) {
        left_start = 0;      left_end = n0 / 2;
        right_start = n0 / 2; right_end = n0;
    } else {
        if (prev_win) { left_start = 0;                 left_end = n1 / 2; }
        else          { left_start = (n1 - n0) / 4;     left_end = (n1 + n0) / 4; }
        if (next_win) { right_start = n1 / 2;           right_end = n1; }
        else          { right_start = (3 * n1 - n0) / 4; right_end = (3 * n1 + n0) / 4; }
    }

    int half = n / 2;

    /* --- floors ---------------------------------------------------------- */
    int no_residue[VORBIS_MAX_CHANNELS];
    for (int c = 0; c < v->channels; c++) {
        memset(v->coeff[c], 0, (size_t)half * sizeof(double));
        memset(v->floorcurve[c], 0, (size_t)half * sizeof(double));
        int sm = (map->submaps > 1) ? map->mux[c] : 0;
        vfloor *f = &v->floors[map->submap_floor[sm]];
        int r = (f->type == 1)
            ? floor1_decode(v, &b, f, v->floorcurve[c], half)
            : floor0_decode(v, &b, f, v->floorcurve[c], half);
        if (r < 0) return r;
        no_residue[c] = !r;
    }

    /* A coupled pair is silent only if BOTH its channels are: the angle
     * channel's residue still feeds the magnitude channel through the inverse
     * coupling below. */
    for (int k = 0; k < map->coupling_steps; k++) {
        if (!no_residue[map->mag[k]] || !no_residue[map->ang[k]]) {
            no_residue[map->mag[k]] = 0;
            no_residue[map->ang[k]] = 0;
        }
    }

    /* --- residues -------------------------------------------------------- */
    for (int s = 0; s < map->submaps; s++) {
        double *vecs[VORBIS_MAX_CHANNELS];
        int dnd[VORBIS_MAX_CHANNELS];
        int nch = 0;
        for (int c = 0; c < v->channels; c++) {
            int sm = (map->submaps > 1) ? map->mux[c] : 0;
            if (sm != s) continue;
            vecs[nch] = v->coeff[c];
            dnd[nch] = no_residue[c];
            nch++;
        }
        if (!nch) continue;
        int e = residue_decode(v, &b, &v->residues[map->submap_residue[s]],
                               vecs, nch, half, dnd);
        if (e != AUDIO_OK) return e;
    }

    /* --- inverse coupling ------------------------------------------------ */
    for (int k = map->coupling_steps - 1; k >= 0; k--) {
        double *M = v->coeff[map->mag[k]];
        double *A = v->coeff[map->ang[k]];
        for (int i = 0; i < half; i++) {
            double m = M[i], a = A[i];
            double nm, na;
            if (m > 0) {
                if (a > 0) { nm = m; na = m - a; }
                else       { na = m; nm = m + a; }
            } else {
                if (a > 0) { nm = m; na = m + a; }
                else       { na = m; nm = m - a; }
            }
            M[i] = nm;
            A[i] = na;
        }
    }

    /* --- floor multiply and inverse MDCT --------------------------------- */
    for (int c = 0; c < v->channels; c++) {
        double *X = v->coeff[c];
        if (no_residue[c]) {
            memset(v->pcmbuf[c], 0, (size_t)n * sizeof(double));
            continue;
        }
        for (int i = 0; i < half; i++) X[i] *= v->floorcurve[c][i];
        /* Vorbis's backward transform is EXACTLY the cosine sum that afft.h
         * defines: no scale factor and no sign flip. That is worth writing
         * down, because it is the one transform in this library that needs
         * neither, and the first guess here was -2/n by analogy with AAC. The
         * differential against a reference decode reported a ratio of 1/1024
         * flat across EVERY frequency bin -- which is 2/n at n = 2048, and a
         * constant ratio across the whole spectrum is exactly what a scale
         * error looks like. That is why the check which found it measured a
         * spectrum rather than a handful of samples: in the time domain the
         * same error looked like a decaying envelope and sent the search off
         * after the overlap-add. */
        amdct_imdct(mode->blockflag ? v->mdct1 : v->mdct0, X, v->pcmbuf[c]);
    }

    /* --- overlap-add ------------------------------------------------------ */
    int lslope = left_end - left_start;
    if (v->have_prev) {
        if (v->lap_n != lslope) {
            /* The previous block's falling slope and this one's rising slope
             * must be the same length; the block size flags decide both, so a
             * mismatch means the stream lied about one of them. */
            return AUDIO_ERR_CORRUPT;
        }
        const double *w = (lslope == n1 / 2) ? v->win1 : v->win0;
        for (int c = 0; c < v->channels; c++) {
            double *t = v->pcmbuf[c];
            const double *L = v->lap[c];
            for (int j = 0; j < lslope; j++)
                t[left_start + j] = t[left_start + j] * w[j] + L[j] * w[lslope - 1 - j];
        }
        int outn = right_start - left_start;
        for (int c = 0; c < v->channels; c++) {
            const double *t = v->pcmbuf[c];
            /* NOT clamped to +-1. A lossy reconstruction of a signal that
             * was already near full scale overshoots, routinely; every float
             * decoder hands the overshoot back and lets the output stage
             * decide. Clamping here cost nothing on fifteen of sixteen test
             * signals and put the full-scale chirp three orders of magnitude
             * out, because that is the one case that spends its life at the
             * rail. audio_f32_to_s16 saturates on the way to integers, which
             * is where saturation belongs. */
            for (int i = 0; i < outn; i++)
                v->out[i * v->channels + c] = (float)t[left_start + i];
        }
        out->nsamples = outn;
    } else {
        out->nsamples = 0;                 /* the first packet only primes */
    }

    /* Save the falling-slope region, unwindowed, for the next packet. */
    v->lap_n = right_end - right_start;
    for (int c = 0; c < v->channels; c++)
        memcpy(v->lap[c], v->pcmbuf[c] + right_start,
               (size_t)v->lap_n * sizeof(double));
    v->have_prev = 1;
    v->prev_n = n;
    return AUDIO_OK;
}

/* --- public -------------------------------------------------------------- */

int vorbis_info(const vorbisdec *v, int *rate, int *channels)
{
    if (!v) return AUDIO_ERR_RANGE;
    if (rate) *rate = v->rate;
    if (channels) *channels = v->channels;
    return AUDIO_OK;
}

int64_t vorbis_granule_end(const vorbisdec *v) { return v ? v->granule_end : -1; }

void vorbis_close(vorbisdec *v)
{
    if (!v) return;
    if (v->books) {
        for (int i = 0; i < v->ncodebooks; i++) cb_free(&v->books[i]);
        free(v->books);
    }
    free(v->floors);
    free(v->residues);
    free(v->mappings);
    free(v->modes);
    amdct_free(v->mdct0);
    amdct_free(v->mdct1);
    free(v->win0);
    free(v->win1);
    for (int c = 0; c < VORBIS_MAX_CHANNELS; c++) {
        free(v->coeff[c]);
        free(v->pcmbuf[c]);
        free(v->lap[c]);
        free(v->floorcurve[c]);
    }
    free(v->floor_y);
    free(v->floor_step2);
    free(v->out);
    ogg_close(v->ogg);
    free(v);
}

static vorbisdec *vorbis_alloc(void)
{
    vorbisdec *v = (vorbisdec *)malloc(sizeof(*v));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->granule_end = -1;
    return v;
}

vorbisdec *vorbis_open_headers(const uint8_t *id, long idlen,
                               const uint8_t *comment, long clen,
                               const uint8_t *setup, long slen, int *err)
{
    int e = AUDIO_ERR_CORRUPT;
    vorbisdec *v = vorbis_alloc();
    if (!v) { if (err) *err = AUDIO_ERR_OOM; return NULL; }

    e = parse_id(v, id, idlen);
    if (e != AUDIO_OK) goto fail;
    if (!check_header(comment, clen, 3)) { e = AUDIO_ERR_CORRUPT; goto fail; }
    e = parse_setup(v, setup, slen);
    if (e != AUDIO_OK) goto fail;
    e = alloc_state(v);
    if (e != AUDIO_OK) goto fail;

    if (err) *err = AUDIO_OK;
    return v;
fail:
    vorbis_close(v);
    if (err) *err = e;
    return NULL;
}

vorbisdec *vorbis_open(const uint8_t *data, long len, int *err)
{
    int e = AUDIO_ERR_CORRUPT;
    if (!data || len <= 0) { if (err) *err = AUDIO_ERR_RANGE; return NULL; }

    vorbisdec *v = vorbis_alloc();
    if (!v) { if (err) *err = AUDIO_ERR_OOM; return NULL; }
    v->data = data;
    v->len = len;
    v->ogg = ogg_open(data, len, &e);
    if (!v->ogg) goto fail;

    const uint8_t *p;
    long l;
    /* The three header packets, in order and each in its own right. */
    if (ogg_packet(v->ogg, &p, &l) != 1) { e = AUDIO_ERR_CORRUPT; goto fail; }
    e = parse_id(v, p, l);
    if (e != AUDIO_OK) goto fail;

    if (ogg_packet(v->ogg, &p, &l) != 1) { e = AUDIO_ERR_CORRUPT; goto fail; }
    if (!check_header(p, l, 3)) { e = AUDIO_ERR_CORRUPT; goto fail; }

    if (ogg_packet(v->ogg, &p, &l) != 1) { e = AUDIO_ERR_CORRUPT; goto fail; }
    e = parse_setup(v, p, l);
    if (e != AUDIO_OK) goto fail;

    e = alloc_state(v);
    if (e != AUDIO_OK) goto fail;

    if (err) *err = AUDIO_OK;
    return v;
fail:
    vorbis_close(v);
    if (err) *err = e;
    return NULL;
}

int vorbis_packet(vorbisdec *v, const uint8_t *pkt, long len, vorbisframe *out)
{
    if (!v || !pkt || !out || len <= 0) return AUDIO_ERR_RANGE;
    int e = decode_audio(v, pkt, len, out);
    return e == AUDIO_OK ? 1 : e;
}

int vorbis_decode(vorbisdec *v, vorbisframe *out)
{
    if (!v || !out) return AUDIO_ERR_RANGE;
    if (!v->ogg) return AUDIO_ERR_UNSUPPORTED;
    for (;;) {
        const uint8_t *p;
        long l;
        int r = ogg_packet(v->ogg, &p, &l);
        if (r <= 0) return r;
        int64_t g = ogg_granulepos(v->ogg);
        if (g >= 0) v->granule_end = g;
        if (l == 0) continue;
        int e = decode_audio(v, p, l, out);
        if (e != AUDIO_OK) {
            /* A damaged packet loses its block, not the rest of the file. */
            v->have_prev = 0;
            v->lap_n = 0;
            continue;
        }
        return 1;
    }
}
