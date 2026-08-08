/* c/lib/audio/aac.c -- from-scratch MPEG-2/4 AAC Low Complexity decoder.
 * See aac.h for what is and is not implemented, and for why the bar for this
 * format is a conformance tolerance rather than a bit pattern.
 *
 * The pipeline, in the order the standard's block diagram gives:
 *
 *   ADTS/ASC -> raw_data_block -> per element:
 *     ics_info + section_data + scale_factor_data + pulse + tns
 *     -> spectral Huffman decode into quantised integers, in GROUPED order
 *     -> inverse quantisation (|q|^(4/3) * 2^((sf-100)/4)) and de-interleaving
 *        into per-window order
 *     -> PNS, then M/S, then intensity stereo   (that order matters; see below)
 *     -> TNS all-pole filtering along frequency
 *     -> IMDCT, window, overlap-add
 *
 * PNS BEFORE M/S is not an arbitrary choice. A band that is noise-substituted
 * in both channels of a pair AND flagged ms_used must use the SAME noise in
 * both channels -- that is how the encoder asks for a correlated noise image
 * -- so the substitution has to happen while the pair is still being
 * considered jointly, and the M/S butterfly must then skip those bands rather
 * than mixing two independent noises.
 *
 * EVERY INPUT BYTE IS UNTRUSTED. Audio arrives over the network. Every count,
 * length and index read from the bitstream is bounded before it is used as an
 * array subscript or a loop limit, and the bit reader itself (abits.h) returns
 * zeros and a sticky error rather than reading outside the caller's buffer.
 */

#include <stdlib.h>
#include <string.h>
#include "aac.h"
#include "abits.h"
#include "amath.h"
#include "afft.h"
#include "aac_tables.h"

/* --- constants ----------------------------------------------------------- */

#define ZERO_HCB        0
#define ESC_HCB         11
#define NOISE_HCB       13
#define INTENSITY_HCB2  14
#define INTENSITY_HCB   15

#define ONLY_LONG       0
#define LONG_START      1
#define EIGHT_SHORT     2
#define LONG_STOP       3

#define ID_SCE 0
#define ID_CPE 1
#define ID_CCE 2
#define ID_LFE 3
#define ID_DSE 4
#define ID_PCE 5
#define ID_FIL 6
#define ID_END 7

#define MAX_GROUPS      8
#define MAX_SFB         52
#define MAX_SECTIONS    64
#define TNS_MAX_ORDER   20
#define TNS_MAX_FILTERS 8

#define SF_OFFSET       100

/* The inverse quantiser (|q|^(4/3) * 2^((sf-100)/4)) reconstructs the spectrum
 * at 16-bit integer full scale, so the IMDCT output lands in +-32768 rather
 * than +-1. Every other decoder in this library hands back floats normalised
 * to +-1, and audio_f32_to_s16 multiplies by 32768 on the way out, so the
 * conversion belongs here -- once, in the output stage -- rather than as a
 * surprise in every consumer. The factor is exact, and the differential
 * against ffmpeg measured it as exactly 32768 to seven digits before it was
 * applied, which is the kind of confirmation worth writing down. */
#define AAC_FULL_SCALE  32768.0

/* --- per-channel bitstream state ----------------------------------------- */

typedef struct {
    int n_filt;
    int coef_res;                       /* 0 or 1; resolution bits = 3 + this */
    int length[TNS_MAX_FILTERS];
    int order[TNS_MAX_FILTERS];
    int direction[TNS_MAX_FILTERS];
    int coef_compress[TNS_MAX_FILTERS];
    uint8_t coef[TNS_MAX_FILTERS][TNS_MAX_ORDER];
} tnswin;

typedef struct {
    int window_sequence;
    int window_shape;
    int max_sfb;
    int num_swb;
    int num_windows;                    /* 1 or 8 */
    int num_groups;
    int group_len[MAX_GROUPS];
    const uint16_t *swb;                /* num_swb+1 offsets */
    int global_gain;

    int nsect[MAX_GROUPS];
    uint8_t sect_cb[MAX_GROUPS][MAX_SECTIONS];
    uint8_t sect_start[MAX_GROUPS][MAX_SECTIONS];
    uint8_t sect_end[MAX_GROUPS][MAX_SECTIONS];
    uint8_t sfb_cb[MAX_GROUPS][MAX_SFB];
    int16_t sf[MAX_GROUPS][MAX_SFB];

    /* Grouped band offsets: within a group, band sfb occupies
     * width*group_len consecutive coefficients. */
    uint16_t sect_off[MAX_GROUPS][MAX_SFB + 1];

    int pulse_present;
    int pulse_start_sfb;
    int number_pulse;
    int pulse_offset[4];
    int pulse_amp[4];

    int tns_present;
    tnswin tns[8];
} icsinfo;

typedef struct {
    double overlap[AAC_FRAME_LEN];
    int prev_shape;
    int inited;
} chanstate;

struct aacdec {
    int have_config;
    int sfi;                            /* sampling frequency index */
    int rate;
    int channels;                       /* channels of the last decoded frame */
    int chancfg;
    int had_sbr;
    int had_pns;

    uint32_t rng;                       /* PNS */

    amdct *mdct_long;
    amdct *mdct_short;

    chanstate ch[AAC_MAX_CHANNELS];
    icsinfo ics[AAC_MAX_CHANNELS];
    double spec[AAC_MAX_CHANNELS][AAC_FRAME_LEN];
    int32_t quant[AAC_FRAME_LEN];

    float pcm[AAC_MAX_CHANNELS * AAC_FRAME_LEN];

    /* ms_used for the channel pair currently being decoded */
    uint8_t ms_used[MAX_GROUPS][MAX_SFB];
    int ms_mask_present;

    double tbuf[2048];                  /* IMDCT scratch */
    double wbuf[2048];                  /* windowed/overlap scratch */

    /* The element ids of the block just parsed, in OUTPUT order, so the
     * channel order can be worked out. See channel_permutation(). */
    uint8_t elem_seq[AAC_MAX_CHANNELS];
    int nelem;

    /* The program_config_element's declared element list, when the stream has
     * one (channel_configuration 0). See find_pce_slot(). */
    int pce_valid;
    int pce_n;
    uint8_t pce_type[AAC_MAX_CHANNELS];   /* ID_SCE / ID_CPE / ID_LFE */
    uint8_t pce_tag[AAC_MAX_CHANNELS];
    uint8_t pce_base[AAC_MAX_CHANNELS];   /* first output channel of entry i */
    int pce_nch;

    uint8_t slot_type[AAC_MAX_CHANNELS];
};

/* WHICH OUTPUT CHANNEL AN ELEMENT FEEDS.
 *
 * With a channel_configuration in the header the answer is "the next one":
 * elements arrive in the order Table 1.19 fixes. With channel_configuration 0
 * the program_config_element declares the layout instead, as a list of
 * (is_cpe, element_instance_tag) entries, and an element belongs wherever its
 * TAG says it does -- the bitstream order carries no meaning at all.
 *
 * This is not a corner case dressed up as one. ISO conformance stream al17 is
 * two single-channel elements described by a PCE, and it emits them in
 * whichever order it likes from frame to frame: taking them positionally
 * swaps left and right in about half the frames, which is inaudible on the
 * mono-ish content the test uses and put the whole file 600000% over the
 * conformance bound. Every other test in this project passed while that was
 * broken, because ffmpeg's encoder never writes a PCE unless asked.
 *
 * Returns the first output channel index for (type, tag), or -1 if the PCE
 * does not describe it (in which case the caller falls back to order). */
static int find_pce_slot(const aacdec *d, int type, int tag)
{
    if (!d->pce_valid) return -1;
    for (int i = 0; i < d->pce_n; i++)
        if (d->pce_type[i] == type && d->pce_tag[i] == tag)
            return d->pce_base[i];
    return -1;
}

/* AAC's default element order is not the interleave order anything else uses.
 * ISO/IEC 14496-3 Table 1.19 lays a 5.1 stream out as
 *
 *     SCE(centre) CPE(front L,R) CPE(surround L,R) LFE
 *
 * so a decoder that simply appends channels in bitstream order hands back
 * C L R Ls Rs LFE, while every WAV file, every sound card and ffmpeg itself
 * expect L R C LFE Ls Rs. Getting this wrong does not sound broken -- it
 * sounds like a badly mixed room -- and it made the 5.1 case miss the
 * conformance bound by six thousand times while every stereo case passed.
 *
 * The permutation is applied only when the elements actually came in the
 * default order for that channel count, matched below on the recorded element
 * sequence rather than on the channel_configuration field, because a PCE can
 * define an arbitrary layout that no fixed table describes. Anything
 * unrecognised is left in bitstream order.
 *
 * Returns 1 and fills perm[decoded_index] = output_index, or 0 for identity. */
static int channel_permutation(const aacdec *d, int nch, int *perm)
{
    static const uint8_t SEQ3[] = { ID_SCE, ID_CPE };
    static const uint8_t SEQ4[] = { ID_SCE, ID_CPE, ID_SCE };
    static const uint8_t SEQ5[] = { ID_SCE, ID_CPE, ID_CPE };
    static const uint8_t SEQ6[] = { ID_SCE, ID_CPE, ID_CPE, ID_LFE };
    static const uint8_t SEQ8[] = { ID_SCE, ID_CPE, ID_CPE, ID_CPE, ID_LFE };
    /* perm[i] = where decoded channel i belongs in the output. */
    static const int P3[] = { 2, 0, 1 };
    static const int P4[] = { 2, 0, 1, 3 };
    static const int P5[] = { 2, 0, 1, 3, 4 };
    static const int P6[] = { 2, 0, 1, 4, 5, 3 };
    static const int P8[] = { 2, 0, 1, 6, 7, 4, 5, 3 };

    const uint8_t *seq;
    const int *p;
    int nseq;
    switch (nch) {
    case 3: seq = SEQ3; nseq = 2; p = P3; break;
    case 4: seq = SEQ4; nseq = 3; p = P4; break;
    case 5: seq = SEQ5; nseq = 3; p = P5; break;
    case 6: seq = SEQ6; nseq = 4; p = P6; break;
    case 8: seq = SEQ8; nseq = 5; p = P8; break;
    default: return 0;
    }
    if (d->nelem != nseq) return 0;
    for (int i = 0; i < nseq; i++)
        if (d->elem_seq[i] != seq[i]) return 0;
    for (int i = 0; i < nch; i++) perm[i] = p[i];
    return 1;
}

/* --- Huffman ------------------------------------------------------------- */

typedef struct {
    const uint8_t *count;
    const uint16_t *sym;
    int maxlen;
    int quad;        /* 4 values per codeword, else 2 */
    int unsign;      /* values are magnitudes and sign bits follow */
    int mod;         /* radix for the pair books */
} hcbdesc;

static const hcbdesc HCB[12] = {
    { 0, 0, 0, 0, 0, 0 },                                            /* unused */
    { aac_hcb_count_1,  aac_hcb_sym_1,  AAC_HCB1_MAXLEN,  1, 0, 3 },
    { aac_hcb_count_2,  aac_hcb_sym_2,  AAC_HCB2_MAXLEN,  1, 0, 3 },
    { aac_hcb_count_3,  aac_hcb_sym_3,  AAC_HCB3_MAXLEN,  1, 1, 3 },
    { aac_hcb_count_4,  aac_hcb_sym_4,  AAC_HCB4_MAXLEN,  1, 1, 3 },
    { aac_hcb_count_5,  aac_hcb_sym_5,  AAC_HCB5_MAXLEN,  0, 0, 9 },
    { aac_hcb_count_6,  aac_hcb_sym_6,  AAC_HCB6_MAXLEN,  0, 0, 9 },
    { aac_hcb_count_7,  aac_hcb_sym_7,  AAC_HCB7_MAXLEN,  0, 1, 8 },
    { aac_hcb_count_8,  aac_hcb_sym_8,  AAC_HCB8_MAXLEN,  0, 1, 8 },
    { aac_hcb_count_9,  aac_hcb_sym_9,  AAC_HCB9_MAXLEN,  0, 1, 13 },
    { aac_hcb_count_10, aac_hcb_sym_10, AAC_HCB10_MAXLEN, 0, 1, 13 },
    { aac_hcb_count_11, aac_hcb_sym_11, AAC_HCB11_MAXLEN, 0, 1, 17 },
};

/* Canonical decode: the generator proved every codebook is a complete,
 * prefix-free code whose codewords are the canonical assignment starting at
 * zero, which is exactly the precondition this walk needs. */
static int hcb_get(abits *b, const uint8_t *count, const uint16_t *sym, int maxlen)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= maxlen; len++) {
        code |= (int)ab_u1(b);
        if (b->error) return -1;
        int c = count[len - 1];
        if (code - first < c) return (int)sym[index + code - first];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int hcb_scalefactor(abits *b)
{
    int s = hcb_get(b, aac_sf_count, aac_sf_sym, AAC_SF_MAXLEN);
    if (s < 0) return -1000;
    return s - 60;
}

/* ISO/IEC 14496-3: after the codeword and the sign bits, a magnitude of 16 in
 * codebook 11 is followed by an escape: ones until a zero, starting the count
 * at four, then that many bits. */
static int32_t hcb_escape(abits *b, int32_t v)
{
    int neg = v < 0;
    int32_t mag = neg ? -v : v;
    if (mag != 16) return v;
    int i = 4;
    while (i < 24) {
        if (b->error) return 0;
        if (!ab_u1(b)) break;
        i++;
    }
    if (i >= 24) { b->error = 1; return 0; }
    int32_t off = (int32_t)ab_u(b, i);
    int32_t j = off | ((int32_t)1 << i);
    return neg ? -j : j;
}

/* Decode one codeword of codebook cb into `n` (2 or 4) quantised values. */
static int hcb_tuple(abits *b, int cb, int32_t *v, int *n)
{
    const hcbdesc *h = &HCB[cb];
    int idx = hcb_get(b, h->count, h->sym, h->maxlen);
    if (idx < 0) return AUDIO_ERR_CORRUPT;

    if (h->quad) {
        int m = h->mod;                          /* 3 */
        v[0] = idx / (m * m * m);
        v[1] = (idx / (m * m)) % m;
        v[2] = (idx / m) % m;
        v[3] = idx % m;
        *n = 4;
        if (!h->unsign) for (int i = 0; i < 4; i++) v[i] -= 1;
    } else {
        int m = h->mod;
        v[0] = idx / m;
        v[1] = idx % m;
        *n = 2;
        if (!h->unsign) for (int i = 0; i < 2; i++) v[i] -= (m - 1) / 2;
    }

    if (h->unsign) {
        for (int i = 0; i < *n; i++)
            if (v[i]) { if (ab_u1(b)) v[i] = -v[i]; }
    }
    if (cb == ESC_HCB)
        for (int i = 0; i < *n; i++) v[i] = hcb_escape(b, v[i]);

    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* Coefficients per window: 1024 for a long transform, 128 for each of the
 * eight short ones. Getting this wrong is invisible on long blocks (where the
 * group length is always 1) and catastrophic on short ones. */
static int win_len(const icsinfo *ic) { return ic->num_windows == 8 ? 128 : 1024; }

/* --- ics_info ------------------------------------------------------------ */

static int ics_info(aacdec *d, abits *b, icsinfo *ic)
{
    ab_u1(b);                                    /* ics_reserved_bit */
    ic->window_sequence = (int)ab_u(b, 2);
    ic->window_shape = (int)ab_u1(b);

    if (ic->window_sequence == EIGHT_SHORT) {
        ic->max_sfb = (int)ab_u(b, 4);
        int grouping = (int)ab_u(b, 7);
        ic->num_windows = 8;
        ic->num_groups = 1;
        ic->group_len[0] = 1;
        for (int i = 0; i < 7; i++) {
            if (grouping & (1 << (6 - i))) {
                ic->group_len[ic->num_groups - 1]++;
            } else {
                ic->num_groups++;
                ic->group_len[ic->num_groups - 1] = 1;
            }
        }
        ic->num_swb = aac_num_swb_128[d->sfi];
        ic->swb = aac_swb_128[d->sfi];
    } else {
        ic->max_sfb = (int)ab_u(b, 6);
        if (ab_u1(b)) {
            /* predictor_data_present. Main-profile prediction and LTP both
             * live here and neither is LC; refusing is the honest answer,
             * because guessing would silently drop a prediction contribution
             * and produce plausible noise. */
            return AUDIO_ERR_UNSUPPORTED;
        }
        ic->num_windows = 1;
        ic->num_groups = 1;
        ic->group_len[0] = 1;
        ic->num_swb = aac_num_swb_1024[d->sfi];
        ic->swb = aac_swb_1024[d->sfi];
    }

    if (ic->max_sfb > ic->num_swb) return AUDIO_ERR_CORRUPT;
    if (ic->num_groups > MAX_GROUPS) return AUDIO_ERR_CORRUPT;

    /* Grouped band offsets. */
    for (int g = 0; g < ic->num_groups; g++) {
        int off = 0;
        for (int sfb = 0; sfb < ic->num_swb; sfb++) {
            ic->sect_off[g][sfb] = (uint16_t)off;
            off += (ic->swb[sfb + 1] - ic->swb[sfb]) * ic->group_len[g];
        }
        ic->sect_off[g][ic->num_swb] = (uint16_t)off;
    }

    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- section_data -------------------------------------------------------- */

static int section_data(abits *b, icsinfo *ic)
{
    int bits = (ic->window_sequence == EIGHT_SHORT) ? 3 : 5;
    int esc = (1 << bits) - 1;

    memset(ic->sfb_cb, 0, sizeof(ic->sfb_cb));

    for (int g = 0; g < ic->num_groups; g++) {
        int k = 0, i = 0;
        while (k < ic->max_sfb) {
            if (i >= MAX_SECTIONS) return AUDIO_ERR_CORRUPT;
            int cb = (int)ab_u(b, 4);
            int len = 0, incr;
            /* The escape run is bounded by max_sfb: a corrupt stream must not
             * be able to spin here for the length of the buffer. */
            int guard = 0;
            while ((incr = (int)ab_u(b, bits)) == esc) {
                len += esc;
                if (b->error || ++guard > MAX_SFB) return AUDIO_ERR_CORRUPT;
            }
            len += incr;
            if (b->error) return AUDIO_ERR_CORRUPT;
            if (len <= 0 || k + len > ic->max_sfb) return AUDIO_ERR_CORRUPT;
            if (cb == 12) return AUDIO_ERR_CORRUPT;      /* reserved */

            ic->sect_cb[g][i] = (uint8_t)cb;
            ic->sect_start[g][i] = (uint8_t)k;
            ic->sect_end[g][i] = (uint8_t)(k + len);
            for (int sfb = k; sfb < k + len; sfb++) ic->sfb_cb[g][sfb] = (uint8_t)cb;
            k += len;
            i++;
        }
        ic->nsect[g] = i;
    }
    return AUDIO_OK;
}

/* --- scale_factor_data --------------------------------------------------- */

static int scale_factor_data(abits *b, icsinfo *ic)
{
    int sf = ic->global_gain;
    int is_pos = 0;
    int noise = ic->global_gain - 90;
    int noise_pcm = 1;

    for (int g = 0; g < ic->num_groups; g++) {
        for (int sfb = 0; sfb < ic->max_sfb; sfb++) {
            int cb = ic->sfb_cb[g][sfb];
            if (cb == ZERO_HCB) { ic->sf[g][sfb] = 0; continue; }

            if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) {
                int t = hcb_scalefactor(b);
                if (t == -1000) return AUDIO_ERR_CORRUPT;
                is_pos += t;
                ic->sf[g][sfb] = (int16_t)is_pos;
            } else if (cb == NOISE_HCB) {
                int t;
                if (noise_pcm) { noise_pcm = 0; t = (int)ab_u(b, 9) - 256; }
                else {
                    t = hcb_scalefactor(b);
                    if (t == -1000) return AUDIO_ERR_CORRUPT;
                }
                noise += t;
                ic->sf[g][sfb] = (int16_t)noise;
            } else {
                int t = hcb_scalefactor(b);
                if (t == -1000) return AUDIO_ERR_CORRUPT;
                sf += t;
                /* A scalefactor outside [0,255] means the stream is not
                 * describing an amplitude any more. */
                if (sf < 0 || sf > 255) return AUDIO_ERR_CORRUPT;
                ic->sf[g][sfb] = (int16_t)sf;
            }
        }
    }
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- pulse_data / tns_data ----------------------------------------------- */

static int pulse_data(abits *b, icsinfo *ic)
{
    ic->number_pulse = (int)ab_u(b, 2) + 1;
    ic->pulse_start_sfb = (int)ab_u(b, 6);
    if (ic->pulse_start_sfb >= ic->num_swb) return AUDIO_ERR_CORRUPT;
    for (int i = 0; i < ic->number_pulse; i++) {
        ic->pulse_offset[i] = (int)ab_u(b, 5);
        ic->pulse_amp[i] = (int)ab_u(b, 4);
    }
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

static int tns_data(abits *b, icsinfo *ic)
{
    int shrt = (ic->window_sequence == EIGHT_SHORT);
    int nfbits = shrt ? 1 : 2;
    int lbits = shrt ? 4 : 6;
    int obits = shrt ? 3 : 5;

    for (int w = 0; w < ic->num_windows; w++) {
        tnswin *t = &ic->tns[w];
        t->n_filt = (int)ab_u(b, nfbits);
        if (t->n_filt > TNS_MAX_FILTERS) return AUDIO_ERR_CORRUPT;
        if (t->n_filt) t->coef_res = (int)ab_u1(b);
        for (int f = 0; f < t->n_filt; f++) {
            t->length[f] = (int)ab_u(b, lbits);
            t->order[f] = (int)ab_u(b, obits);
            if (t->order[f] > TNS_MAX_ORDER) return AUDIO_ERR_CORRUPT;
            if (t->order[f]) {
                t->direction[f] = (int)ab_u1(b);
                t->coef_compress[f] = (int)ab_u1(b);
                int clen = t->coef_res + 3 - t->coef_compress[f];
                for (int i = 0; i < t->order[f]; i++)
                    t->coef[f][i] = (uint8_t)ab_u(b, clen);
            }
        }
    }
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- spectral data ------------------------------------------------------- */

static int spectral_data(aacdec *d, abits *b, icsinfo *ic)
{
    memset(d->quant, 0, sizeof(d->quant));

    int wl = win_len(ic);
    int gbase = 0;
    for (int g = 0; g < ic->num_groups; g++) {
        int gsize = ic->group_len[g] * wl;
        for (int s = 0; s < ic->nsect[g]; s++) {
            int cb = ic->sect_cb[g][s];
            if (cb == ZERO_HCB || cb == NOISE_HCB ||
                cb == INTENSITY_HCB || cb == INTENSITY_HCB2)
                continue;
            if (cb < 1 || cb > 11) return AUDIO_ERR_CORRUPT;

            int from = ic->sect_off[g][ic->sect_start[g][s]];
            int to = ic->sect_off[g][ic->sect_end[g][s]];
            if (to > gsize || from > to) return AUDIO_ERR_CORRUPT;
            if (gbase + to > AAC_FRAME_LEN) return AUDIO_ERR_CORRUPT;

            int step = HCB[cb].quad ? 4 : 2;
            for (int k = from; k < to; k += step) {
                int32_t v[4];
                int n = 0;
                int e = hcb_tuple(b, cb, v, &n);
                if (e != AUDIO_OK) return e;
                if (k + n > to) return AUDIO_ERR_CORRUPT;
                for (int i = 0; i < n; i++) d->quant[gbase + k + i] = v[i];
            }
        }
        gbase += gsize;
    }
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* Pulses are added to the quantised values, before inverse quantisation, and
 * only ever on long windows. */
static void apply_pulses(aacdec *d, icsinfo *ic)
{
    if (!ic->pulse_present || ic->window_sequence == EIGHT_SHORT) return;
    int k = ic->swb[ic->pulse_start_sfb];
    for (int i = 0; i < ic->number_pulse; i++) {
        k += ic->pulse_offset[i];
        if (k >= AAC_FRAME_LEN) return;
        if (d->quant[k] > 0) d->quant[k] += ic->pulse_amp[i];
        else d->quant[k] -= ic->pulse_amp[i];
    }
}

/* --- inverse quantisation and de-interleaving ---------------------------- */

/* |q|^(4/3), the AAC quantiser's companding law. Small magnitudes dominate
 * real streams, so the first 256 are cached; the rest go through a_pow. */
static double pow43(int32_t q)
{
    static double cache[256];
    static int cached;
    if (!cached) {
        for (int i = 0; i < 256; i++) cache[i] = a_pow((double)i, 4.0 / 3.0);
        cached = 1;
    }
    int32_t m = q < 0 ? -q : q;
    double v = (m < 256) ? cache[m] : a_pow((double)m, 4.0 / 3.0);
    return q < 0 ? -v : v;
}

static void dequant_deinterleave(aacdec *d, icsinfo *ic, double *spec)
{
    memset(spec, 0, AAC_FRAME_LEN * sizeof(double));

    int wl = win_len(ic);
    int gbase = 0;
    for (int g = 0; g < ic->num_groups; g++) {
        int src = gbase;
        for (int sfb = 0; sfb < ic->num_swb; sfb++) {
            int width = ic->swb[sfb + 1] - ic->swb[sfb];
            int cb = (sfb < ic->max_sfb) ? ic->sfb_cb[g][sfb] : ZERO_HCB;
            double gain = 0.0;
            int live = (cb != ZERO_HCB && cb != NOISE_HCB &&
                        cb != INTENSITY_HCB && cb != INTENSITY_HCB2);
            if (live) gain = a_exp2(0.25 * (double)(ic->sf[g][sfb] - SF_OFFSET));

            for (int w = 0; w < ic->group_len[g]; w++) {
                int dst = gbase + w * wl + ic->swb[sfb];
                for (int i = 0; i < width; i++) {
                    if (live) {
                        int32_t q = d->quant[src + i];
                        if (q) spec[dst + i] = pow43(q) * gain;
                    }
                }
                src += width;
            }
        }
        gbase += ic->group_len[g] * wl;
    }
}

/* --- PNS, M/S, intensity ------------------------------------------------- */

/* A decoder-defined PRNG. Perceptual noise substitution replaces a band with
 * noise of the transmitted energy, so its output is by construction NOT
 * comparable sample-for-sample with any other decoder -- which is why the
 * conformance corpus is generated without PNS and the fact is reported rather
 * than hidden. It must still be deterministic, so that the host and the guest
 * produce the same samples: that comparison is the whole content of
 * test-audio-codec-os. */
static uint32_t pns_rand(aacdec *d)
{
    uint32_t x = d->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    d->rng = x ? x : 0x9E3779B9u;
    return d->rng;
}

static void pns_band(aacdec *d, double *spec, int off, int width, int sf)
{
    double energy = 0.0;
    for (int i = 0; i < width; i++) {
        double v = (double)(int32_t)pns_rand(d) * (1.0 / 2147483648.0);
        spec[off + i] = v;
        energy += v * v;
    }
    if (energy <= 0.0) return;
    double scale = a_exp2(0.25 * (double)sf) / a_sqrt(energy);
    for (int i = 0; i < width; i++) spec[off + i] *= scale;
}

static int is_noise(const icsinfo *ic, int g, int sfb)
{
    return sfb < ic->max_sfb && ic->sfb_cb[g][sfb] == NOISE_HCB;
}

static void pns_decode(aacdec *d, icsinfo *ic0, icsinfo *ic1,
                       double *s0, double *s1, int pair)
{
    int wl = win_len(ic0);
    for (int g = 0; g < ic0->num_groups; g++) {
        for (int sfb = 0; sfb < ic0->max_sfb; sfb++) {
            int n0 = is_noise(ic0, g, sfb);
            int n1 = ic1 ? is_noise(ic1, g, sfb) : 0;
            if (!n0 && !n1) continue;
            d->had_pns = 1;

            int width = ic0->swb[sfb + 1] - ic0->swb[sfb];
            int gbase = 0;
            for (int gg = 0; gg < g; gg++) gbase += ic0->group_len[gg] * wl;

            for (int w = 0; w < ic0->group_len[g]; w++) {
                int off = gbase + w * wl + ic0->swb[sfb];
                int correlated = pair && n0 && n1 && d->ms_mask_present &&
                                 d->ms_used[g][sfb];
                if (n0) pns_band(d, s0, off, width, ic0->sf[g][sfb]);
                if (n1) {
                    if (correlated) {
                        /* The same noise in both channels, scaled to the
                         * second channel's transmitted energy. */
                        double e = 0.0;
                        for (int i = 0; i < width; i++) e += s0[off + i] * s0[off + i];
                        double sc = (e > 0.0)
                            ? a_exp2(0.25 * (double)ic1->sf[g][sfb]) / a_sqrt(e)
                            : 0.0;
                        for (int i = 0; i < width; i++) s1[off + i] = s0[off + i] * sc;
                    } else {
                        pns_band(d, s1, off, width, ic1->sf[g][sfb]);
                    }
                }
            }
        }
    }
}

static void ms_decode(aacdec *d, icsinfo *ic0, icsinfo *ic1, double *s0, double *s1)
{
    if (!d->ms_mask_present) return;
    int wl = win_len(ic0);
    for (int g = 0; g < ic0->num_groups; g++) {
        int gbase = 0;
        for (int gg = 0; gg < g; gg++) gbase += ic0->group_len[gg] * wl;
        for (int sfb = 0; sfb < ic0->max_sfb; sfb++) {
            if (d->ms_mask_present == 2) {
                /* mask 2 means "all bands", except the ones that carry no
                 * spectrum of their own. */
            } else if (!d->ms_used[g][sfb]) {
                continue;
            }
            int cb = ic0->sfb_cb[g][sfb];
            if (cb == NOISE_HCB || cb == INTENSITY_HCB || cb == INTENSITY_HCB2)
                continue;
            if (ic1->sfb_cb[g][sfb] == INTENSITY_HCB ||
                ic1->sfb_cb[g][sfb] == INTENSITY_HCB2)
                continue;
            int width = ic0->swb[sfb + 1] - ic0->swb[sfb];
            for (int w = 0; w < ic0->group_len[g]; w++) {
                int off = gbase + w * wl + ic0->swb[sfb];
                for (int i = 0; i < width; i++) {
                    double m = s0[off + i], s = s1[off + i];
                    s0[off + i] = m + s;
                    s1[off + i] = m - s;
                }
            }
        }
    }
}

static void is_decode(aacdec *d, icsinfo *ic0, icsinfo *ic1, double *s0, double *s1)
{
    int wl = win_len(ic1);
    for (int g = 0; g < ic1->num_groups; g++) {
        int gbase = 0;
        for (int gg = 0; gg < g; gg++) gbase += ic1->group_len[gg] * wl;
        for (int sfb = 0; sfb < ic1->max_sfb; sfb++) {
            int cb = ic1->sfb_cb[g][sfb];
            if (cb != INTENSITY_HCB && cb != INTENSITY_HCB2) continue;

            /* Codebook 15 is a positive image, 14 a negative one, and an
             * ms_used flag on the band inverts it again. */
            double sign = (cb == INTENSITY_HCB) ? 1.0 : -1.0;
            if (d->ms_mask_present && d->ms_used[g][sfb]) sign = -sign;
            double scale = sign * a_exp2(-0.25 * (double)ic1->sf[g][sfb]);

            int width = ic1->swb[sfb + 1] - ic1->swb[sfb];
            for (int w = 0; w < ic1->group_len[g]; w++) {
                int off = gbase + w * wl + ic1->swb[sfb];
                for (int i = 0; i < width; i++) s1[off + i] = s0[off + i] * scale;
            }
        }
    }
    (void)ic0;
}

/* --- TNS ----------------------------------------------------------------- */

static void tns_lpc(int order, int coef_res_bits, int compress,
                    const uint8_t *coef, double *a)
{
    double tmp[TNS_MAX_ORDER];
    double b[TNS_MAX_ORDER + 1];
    const double *tab = aac_tns_coef[compress * 2 + (coef_res_bits - 3)];

    for (int i = 0; i < order; i++) tmp[i] = tab[coef[i]];

    a[0] = 1.0;
    for (int m = 1; m <= order; m++) {
        for (int i = 1; i < m; i++) b[i] = a[i] + tmp[m - 1] * a[m - i];
        for (int i = 1; i < m; i++) a[i] = b[i];
        a[m] = tmp[m - 1];
    }
}

static void tns_filter(double *spec, int size, int inc, const double *a, int order)
{
    double state[TNS_MAX_ORDER];
    for (int i = 0; i < order; i++) state[i] = 0.0;
    for (int i = 0; i < size; i++) {
        double y = *spec;
        for (int j = 0; j < order; j++) y -= state[j] * a[j + 1];
        for (int j = order - 1; j > 0; j--) state[j] = state[j - 1];
        if (order) state[0] = y;
        *spec = y;
        spec += inc;
    }
}

static void tns_decode(aacdec *d, icsinfo *ic, double *spec)
{
#if AUDIO_SABOTAGE == 3
    /* THE NEGATIVE CONTROL. Temporal noise shaping is an all-pole filter run
     * ALONG FREQUENCY; dropping it leaves a decoder that still produces music,
     * still passes every structural check, and smears quantisation noise
     * across each transient as pre-echo -- the exact failure TNS exists to
     * prevent, and one that no "did it decode" test can see. The conformance
     * gate must reject this. Only -DAUDIO_SABOTAGE=3 builds it, and no
     * shipping build defines that. */
    (void)d; (void)ic; (void)spec;
    return;
#endif
    if (!ic->tns_present) return;
    int shrt = (ic->window_sequence == EIGHT_SHORT);
    int nshort = shrt ? 128 : 1024;
    int maxbands = shrt ? aac_tns_max_bands_128[d->sfi] : aac_tns_max_bands_1024[d->sfi];

    for (int w = 0; w < ic->num_windows; w++) {
        tnswin *t = &ic->tns[w];
        int bottom = ic->num_swb;
        for (int f = 0; f < t->n_filt; f++) {
            int top = bottom;
            bottom = top - t->length[f];
            if (bottom < 0) bottom = 0;
            int order = t->order[f];
            if (!order) continue;

            double a[TNS_MAX_ORDER + 1];
            tns_lpc(order, t->coef_res + 3, t->coef_compress[f], t->coef[f], a);

            int lo = bottom < maxbands ? bottom : maxbands;
            if (lo > ic->max_sfb) lo = ic->max_sfb;
            int hi = top < maxbands ? top : maxbands;
            if (hi > ic->max_sfb) hi = ic->max_sfb;
            int start = ic->swb[lo];
            int end = ic->swb[hi];
            int size = end - start;
            if (size <= 0) continue;

            int base = w * nshort;
            if (base + end > AAC_FRAME_LEN) continue;
            if (t->direction[f]) tns_filter(spec + base + end - 1, size, -1, a, order);
            else tns_filter(spec + base + start, size, 1, a, order);
        }
    }
}

/* --- filterbank ---------------------------------------------------------- */

static const double *win_long(int shape) { return shape ? aac_win_kbd_1024 : aac_win_sine_1024; }
static const double *win_short(int shape) { return shape ? aac_win_kbd_128 : aac_win_sine_128; }

static void filterbank(aacdec *d, icsinfo *ic, chanstate *cs, double *spec, float *out,
                       int nch, int chan_index)
{
    double *z = d->tbuf;                 /* 2048 IMDCT output */
    double *acc = d->wbuf;               /* 2048 windowed accumulator */

    /* The left half of a window is shaped by the PREVIOUS frame's
     * window_shape, and on the very first frame there is no previous frame.
     * The standard does not say what to assume, because the samples that half
     * produces are the encoder's priming and are not signal. It still has to
     * be SOMETHING, and it has to be the same something every other decoder
     * assumes or the first 1024 samples disagree: the reference implementations
     * zero-initialise the field, i.e. the sine shape, and using the current
     * frame's shape instead is what made the impulse case miss the peak bound
     * by 34x in frame 0 and nowhere else. */
    int prev = cs->inited ? cs->prev_shape : 0;

    if (ic->window_sequence == EIGHT_SHORT) {
        memset(acc, 0, 2048 * sizeof(double));
        const double *wl_first = win_short(prev);
        const double *wl = win_short(ic->window_shape);
        const double *wr = win_short(ic->window_shape);
        double sc = 2.0 / 256.0 / AAC_FULL_SCALE;
        for (int j = 0; j < 8; j++) {
            amdct_imdct(d->mdct_short, spec + j * 128, z);
            const double *lw = (j == 0) ? wl_first : wl;
            int base = 448 + j * 128;
            for (int n = 0; n < 128; n++)
                acc[base + n] += z[n] * sc * lw[n];
            for (int n = 128; n < 256; n++)
                acc[base + n] += z[n] * sc * wr[255 - n];
        }
    } else {
        amdct_imdct(d->mdct_long, spec, z);
        double sc = 2.0 / 2048.0 / AAC_FULL_SCALE;
        const double *wlL = win_long(prev);
        const double *wrL = win_long(ic->window_shape);
        const double *wlS = win_short(prev);
        const double *wrS = win_short(ic->window_shape);

        if (ic->window_sequence == ONLY_LONG) {
            for (int n = 0; n < 1024; n++) acc[n] = z[n] * sc * wlL[n];
            for (int n = 1024; n < 2048; n++) acc[n] = z[n] * sc * wrL[2047 - n];
        } else if (ic->window_sequence == LONG_START) {
            for (int n = 0; n < 1024; n++) acc[n] = z[n] * sc * wlL[n];
            for (int n = 1024; n < 1472; n++) acc[n] = z[n] * sc;
            for (int n = 1472; n < 1600; n++) acc[n] = z[n] * sc * wrS[1599 - n];
            for (int n = 1600; n < 2048; n++) acc[n] = 0.0;
        } else {                                    /* LONG_STOP */
            for (int n = 0; n < 448; n++) acc[n] = 0.0;
            for (int n = 448; n < 576; n++) acc[n] = z[n] * sc * wlS[n - 448];
            for (int n = 576; n < 1024; n++) acc[n] = z[n] * sc;
            for (int n = 1024; n < 2048; n++) acc[n] = z[n] * sc * wrL[2047 - n];
        }
    }

    for (int n = 0; n < 1024; n++) {
        double v = acc[n] + cs->overlap[n];
        out[n * nch + chan_index] = (float)v;
        cs->overlap[n] = acc[1024 + n];
    }
    cs->prev_shape = ic->window_shape;
    cs->inited = 1;
}

/* --- one individual_channel_stream --------------------------------------- */

static int decode_ics(aacdec *d, abits *b, icsinfo *ic, int common_window, int scale_flag)
{
    ic->global_gain = (int)ab_u(b, 8);
    if (!common_window && !scale_flag) {
        int e = ics_info(d, b, ic);
        if (e != AUDIO_OK) return e;
    }

    int e = section_data(b, ic);
    if (e != AUDIO_OK) return e;
    e = scale_factor_data(b, ic);
    if (e != AUDIO_OK) return e;

    ic->pulse_present = 0;
    ic->tns_present = 0;
    if (!scale_flag) {
        ic->pulse_present = (int)ab_u1(b);
        if (ic->pulse_present) {
            if (ic->window_sequence == EIGHT_SHORT) return AUDIO_ERR_CORRUPT;
            e = pulse_data(b, ic);
            if (e != AUDIO_OK) return e;
        }
        ic->tns_present = (int)ab_u1(b);
        if (ic->tns_present) {
            memset(ic->tns, 0, sizeof(ic->tns));
            e = tns_data(b, ic);
            if (e != AUDIO_OK) return e;
        }
        if (ab_u1(b)) return AUDIO_ERR_UNSUPPORTED;    /* gain_control (SSR) */
    }

    e = spectral_data(d, b, ic);
    if (e != AUDIO_OK) return e;
    apply_pulses(d, ic);
    return AUDIO_OK;
}

/* --- elements ------------------------------------------------------------ */

static int skip_dse(abits *b)
{
    ab_u(b, 4);                                   /* element_instance_tag */
    int align = (int)ab_u1(b);
    int count = (int)ab_u(b, 8);
    if (count == 255) count += (int)ab_u(b, 8);
    if (align) ab_align(b);
    ab_skip(b, (long)count * 8);
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

static int skip_fil(aacdec *d, abits *b)
{
    int count = (int)ab_u(b, 4);
    if (count == 15) {
        int esc = (int)ab_u(b, 8);
        count += esc - 1;
    }
    if (count <= 0) return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;

    long end = ab_pos(b) + (long)count * 8;
    if (end > b->len * 8) { b->error = 1; return AUDIO_ERR_CORRUPT; }

    int type = (int)ab_u(b, 4);
    /* EXT_SBR_DATA = 13, EXT_SBR_DATA_CRC = 14. Skipping the payload decodes
     * the core, which is the right samples at half the intended rate -- so it
     * is recorded and reported rather than passed off as a full decode. */
    if (type == 13 || type == 14) d->had_sbr = 1;

    ab_seek(b, end);
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* program_config_element. Parsed for three things: the sampling frequency, the
 * channel count, and -- the part that matters -- the ordered list of elements
 * with their instance tags, which is the only thing that says where each
 * element's channels belong. See find_pce_slot(). */
static int parse_pce(aacdec *d, abits *b, int *sfi, int *nch)
{
    ab_u(b, 4);                                   /* element_instance_tag */
    ab_u(b, 2);                                   /* object_type */
    int s = (int)ab_u(b, 4);
    int nfront = (int)ab_u(b, 4);
    int nside = (int)ab_u(b, 4);
    int nback = (int)ab_u(b, 4);
    int nlfe = (int)ab_u(b, 2);
    int nassoc = (int)ab_u(b, 3);
    int ncc = (int)ab_u(b, 4);
    if (ab_u1(b)) ab_u(b, 4);                     /* mono_mixdown */
    if (ab_u1(b)) ab_u(b, 4);                     /* stereo_mixdown */
    if (ab_u1(b)) ab_u(b, 3);                     /* matrix_mixdown */

    int total = 0;
    int n = 0;
    int ok = 1;

    /* front, then side, then back, then LFE: that IS the channel order the
     * PCE declares, and the tags inside it are what each element answers to. */
    int counts[4] = { nfront, nside, nback, nlfe };
    for (int grp = 0; grp < 4; grp++) {
        for (int i = 0; i < counts[grp]; i++) {
            int is_cpe = (grp == 3) ? 0 : (int)ab_u1(b);
            int tag = (int)ab_u(b, 4);
            int type = (grp == 3) ? ID_LFE : (is_cpe ? ID_CPE : ID_SCE);
            if (n < AAC_MAX_CHANNELS && total + (is_cpe ? 2 : 1) <= AAC_MAX_CHANNELS) {
                d->pce_type[n] = (uint8_t)type;
                d->pce_tag[n] = (uint8_t)tag;
                d->pce_base[n] = (uint8_t)total;
                n++;
            } else {
                ok = 0;
            }
            total += is_cpe ? 2 : 1;
        }
    }
    for (int i = 0; i < nassoc; i++) ab_u(b, 4);
    for (int i = 0; i < ncc; i++)    { ab_u1(b); ab_u(b, 4); }
    ab_align(b);
    int cmt = (int)ab_u(b, 8);
    ab_skip(b, (long)cmt * 8);

    if (b->error) return AUDIO_ERR_CORRUPT;

    /* A layout that does not fit is not mapped rather than half-mapped: a
     * partial map would put some channels in the right place and some in the
     * wrong one, which is worse than falling back to bitstream order. */
    d->pce_valid = ok && n > 0;
    d->pce_n = ok ? n : 0;
    d->pce_nch = ok ? total : 0;

    if (sfi) *sfi = s;
    if (nch) *nch = total;
    return AUDIO_OK;
}

/* --- raw_data_block ------------------------------------------------------ */

static int decode_block(aacdec *d, abits *b, int *nch_out)
{
    int nch = 0;
    d->nelem = 0;
    memset(d->slot_type, 0, sizeof(d->slot_type));

    for (;;) {
        if (ab_left(b) < 3) break;
        int id = (int)ab_u(b, 3);
        if (id == ID_END) break;

        if (id == ID_SCE || id == ID_LFE) {
            if (nch >= AAC_MAX_CHANNELS) return AUDIO_ERR_UNSUPPORTED;
            int tag = (int)ab_u(b, 4);             /* element_instance_tag */
            int slot = find_pce_slot(d, id, tag);
            if (slot < 0 || slot >= AAC_MAX_CHANNELS) slot = nch;
            icsinfo *ic = &d->ics[slot];
            d->ms_mask_present = 0;
            int e = decode_ics(d, b, ic, 0, 0);
            if (e != AUDIO_OK) return e;
            dequant_deinterleave(d, ic, d->spec[slot]);
            pns_decode(d, ic, NULL, d->spec[slot], NULL, 0);
            tns_decode(d, ic, d->spec[slot]);
            d->slot_type[slot] = (uint8_t)id;
            if (slot + 1 > nch) nch = slot + 1;
        } else if (id == ID_CPE) {
            if (nch + 2 > AAC_MAX_CHANNELS) return AUDIO_ERR_UNSUPPORTED;
            int cpetag = (int)ab_u(b, 4);
            int cslot = find_pce_slot(d, ID_CPE, cpetag);
            if (cslot < 0 || cslot + 2 > AAC_MAX_CHANNELS) cslot = nch;
            icsinfo *ic0 = &d->ics[cslot];
            icsinfo *ic1 = &d->ics[cslot + 1];
            int common = (int)ab_u1(b);
            d->ms_mask_present = 0;
            memset(d->ms_used, 0, sizeof(d->ms_used));

            if (common) {
                int e = ics_info(d, b, ic0);
                if (e != AUDIO_OK) return e;
                d->ms_mask_present = (int)ab_u(b, 2);
                if (d->ms_mask_present == 3) return AUDIO_ERR_CORRUPT;
                if (d->ms_mask_present == 1) {
                    for (int g = 0; g < ic0->num_groups; g++)
                        for (int sfb = 0; sfb < ic0->max_sfb; sfb++)
                            d->ms_used[g][sfb] = (uint8_t)ab_u1(b);
                } else if (d->ms_mask_present == 2) {
                    for (int g = 0; g < MAX_GROUPS; g++)
                        for (int sfb = 0; sfb < MAX_SFB; sfb++)
                            d->ms_used[g][sfb] = 1;
                }
                /* Both channels share the window layout. */
                const uint16_t *swb = ic0->swb;
                int ws = ic0->window_sequence, sh = ic0->window_shape;
                int msfb = ic0->max_sfb, nswb = ic0->num_swb;
                int nwin = ic0->num_windows, ngrp = ic0->num_groups;
                memcpy(ic1, ic0, sizeof(*ic1));
                ic1->swb = swb; ic1->window_sequence = ws; ic1->window_shape = sh;
                ic1->max_sfb = msfb; ic1->num_swb = nswb;
                ic1->num_windows = nwin; ic1->num_groups = ngrp;
            }

            int e = decode_ics(d, b, ic0, common, 0);
            if (e != AUDIO_OK) return e;
            dequant_deinterleave(d, ic0, d->spec[cslot]);

            e = decode_ics(d, b, ic1, common, 0);
            if (e != AUDIO_OK) return e;
            dequant_deinterleave(d, ic1, d->spec[cslot + 1]);

            if (!common) d->ms_mask_present = 0;

            pns_decode(d, ic0, ic1, d->spec[cslot], d->spec[cslot + 1], 1);
            if (common) ms_decode(d, ic0, ic1, d->spec[cslot], d->spec[cslot + 1]);
            is_decode(d, ic0, ic1, d->spec[cslot], d->spec[cslot + 1]);
            tns_decode(d, ic0, d->spec[cslot]);
            tns_decode(d, ic1, d->spec[cslot + 1]);
            d->slot_type[cslot] = ID_CPE;
            d->slot_type[cslot + 1] = 0xFF;    /* second half of a pair */
            if (cslot + 2 > nch) nch = cslot + 2;
        } else if (id == ID_DSE) {
            int e = skip_dse(b);
            if (e != AUDIO_OK) return e;
        } else if (id == ID_PCE) {
            int s = -1, n = 0;
            int e = parse_pce(d, b, &s, &n);
            if (e != AUDIO_OK) return e;
            if (s >= 0 && s < 13 && !d->have_config) {
                d->sfi = s;
                d->rate = aac_sample_rates[s];
                d->have_config = 1;
            }
        } else if (id == ID_FIL) {
            int e = skip_fil(d, b);
            if (e != AUDIO_OK) return e;
        } else {
            /* CCE: coupling changes the output of other elements, so silently
             * ignoring it would produce a quietly wrong mix. */
            return AUDIO_ERR_UNSUPPORTED;
        }
    }

    if (nch == 0) return AUDIO_ERR_CORRUPT;

    /* elem_seq in OUTPUT order, with the second channel of each pair dropped,
     * which is the form channel_permutation() matches on. */
    d->nelem = 0;
    for (int i = 0; i < nch; i++)
        if (d->slot_type[i] != 0xFF && d->nelem < AAC_MAX_CHANNELS)
            d->elem_seq[d->nelem++] = d->slot_type[i];

    *nch_out = nch;
    return AUDIO_OK;
}

/* --- public -------------------------------------------------------------- */

static int alloc_transforms(aacdec *d)
{
    if (d->mdct_long) return AUDIO_OK;
    d->mdct_long = amdct_new(2048);
    d->mdct_short = amdct_new(256);
    if (!d->mdct_long || !d->mdct_short) return AUDIO_ERR_OOM;
    return AUDIO_OK;
}

static aacdec *aac_alloc(void)
{
    aacdec *d = (aacdec *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->rng = 0x1234567u;
    d->sfi = -1;
    if (alloc_transforms(d) != AUDIO_OK) { aac_close(d); return NULL; }
    return d;
}

aacdec *aac_open(void) { return aac_alloc(); }

aacdec *aac_open_asc(const uint8_t *asc, long len, int *err)
{
    int e = AUDIO_ERR_CORRUPT;
    if (!asc || len < 2) { if (err) *err = AUDIO_ERR_RANGE; return NULL; }

    abits b;
    ab_init(&b, asc, len);
    int aot = (int)ab_u(&b, 5);
    if (aot == 31) aot = 32 + (int)ab_u(&b, 6);
    int sfi = (int)ab_u(&b, 4);
    if (sfi == 15) {
        ab_u(&b, 24);
        sfi = -1;
    }
    int chancfg = (int)ab_u(&b, 4);

    /* Explicit SBR/PS signalling. Decoding the core and calling it a decode
     * would be the right samples at half the rate, which sounds nearly right
     * and is wrong; refusing says so. */
    if (aot == 5 || aot == 29) { if (err) *err = AUDIO_ERR_UNSUPPORTED; return NULL; }
    if (aot != 2) { if (err) *err = AUDIO_ERR_UNSUPPORTED; return NULL; }
    if (sfi < 0 || sfi >= 13) { if (err) *err = AUDIO_ERR_UNSUPPORTED; return NULL; }

    /* GASpecificConfig: frameLengthFlag must be 0 (1024 samples). */
    int framelen_flag = (int)ab_u1(&b);
    if (framelen_flag) { if (err) *err = AUDIO_ERR_UNSUPPORTED; return NULL; }
    if (ab_u1(&b)) ab_u(&b, 14);                   /* dependsOnCoreCoder */
    if (ab_u1(&b)) { if (err) *err = AUDIO_ERR_UNSUPPORTED; return NULL; }  /* extensionFlag */

    aacdec *d = aac_alloc();
    if (!d) { if (err) *err = AUDIO_ERR_OOM; return NULL; }
    d->sfi = sfi;
    d->rate = aac_sample_rates[sfi];
    d->chancfg = chancfg;
    d->channels = (chancfg == 7) ? 8 : chancfg;
    d->have_config = 1;
    if (err) *err = AUDIO_OK;
    (void)e;
    return d;
}

void aac_close(aacdec *d)
{
    if (!d) return;
    amdct_free(d->mdct_long);
    amdct_free(d->mdct_short);
    free(d);
}

int aac_info(const aacdec *d, int *rate, int *channels)
{
    if (!d) return AUDIO_ERR_RANGE;
    if (rate) *rate = d->rate;
    if (channels) *channels = d->channels;
    return AUDIO_OK;
}

int aac_had_sbr(const aacdec *d) { return d ? d->had_sbr : 0; }
int aac_had_pns(const aacdec *d) { return d ? d->had_pns : 0; }

long aac_adts_frame_len(const uint8_t *data, long len)
{
    if (!data || len < 7) return 0;
    if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return 0;   /* sync + layer 0 */
    int sfi = (data[2] >> 2) & 0x0F;
    if (sfi >= 13) return 0;
    long fl = ((long)(data[3] & 0x03) << 11) | ((long)data[4] << 3) |
              ((long)(data[5] >> 5) & 0x07);
    int prot = data[1] & 1;
    long hdr = prot ? 7 : 9;
    if (fl < hdr) return 0;
    return fl;
}

static int finish_frame(aacdec *d, int nch, aacframe *out)
{
    int perm[AAC_MAX_CHANNELS];
    if (!channel_permutation(d, nch, perm))
        for (int c = 0; c < nch; c++) perm[c] = c;

    for (int c = 0; c < nch; c++)
        filterbank(d, &d->ics[c], &d->ch[c], d->spec[c], d->pcm, nch, perm[c]);

    d->channels = nch;
    out->rate = d->rate;
    out->channels = nch;
    out->nsamples = AAC_FRAME_LEN;
    out->pcm = d->pcm;
    return AUDIO_OK;
}

int aac_decode_raw(aacdec *d, const uint8_t *data, long len, aacframe *out, int *got)
{
    if (got) *got = 0;
    if (!d || !data || !out || len <= 0) return AUDIO_ERR_RANGE;
    if (!d->have_config) return AUDIO_ERR_CORRUPT;

    abits b;
    ab_init(&b, data, len);
    int nch = 0;
    int e = decode_block(d, &b, &nch);
    if (e != AUDIO_OK) return e;
    e = finish_frame(d, nch, out);
    if (e != AUDIO_OK) return e;
    if (got) *got = 1;
    return (int)len;
}

int aac_decode(aacdec *d, const uint8_t *data, long len, aacframe *out, int *got)
{
    if (got) *got = 0;
    if (!d || !data || !out || len < 0) return AUDIO_ERR_RANGE;

    /* Find the next syncword. A stream can begin mid-frame after a seek. */
    long off = 0;
    while (off + 7 <= len && aac_adts_frame_len(data + off, len - off) == 0) off++;
    if (off + 7 > len) return 0;

    const uint8_t *p = data + off;
    long avail = len - off;
    long flen = aac_adts_frame_len(p, avail);
    if (flen == 0) return 0;
    if (flen > avail) return 0;                    /* need more bytes */

    int sfi = (p[2] >> 2) & 0x0F;
    int profile = (p[2] >> 6) & 3;
    int chancfg = (int)(((p[2] & 1) << 2) | ((p[3] >> 6) & 3));
    int prot = p[1] & 1;
    long hdr = prot ? 7 : 9;
    int nblocks = (p[6] & 3) + 1;

    if (profile != 1) return AUDIO_ERR_UNSUPPORTED;    /* 1 == LC */
    if (nblocks != 1) return AUDIO_ERR_UNSUPPORTED;

    if (!d->have_config || d->sfi != sfi) {
        d->sfi = sfi;
        d->rate = aac_sample_rates[sfi];
        d->have_config = 1;
        /* A geometry change invalidates the overlap tails. */
        for (int c = 0; c < AAC_MAX_CHANNELS; c++) {
            memset(d->ch[c].overlap, 0, sizeof(d->ch[c].overlap));
            d->ch[c].inited = 0;
        }
    }
    d->chancfg = chancfg;

    abits b;
    ab_init(&b, p + hdr, flen - hdr);
    int nch = 0;
    int e = decode_block(d, &b, &nch);
    if (e != AUDIO_OK) return e;
    e = finish_frame(d, nch, out);
    if (e != AUDIO_OK) return e;
    if (got) *got = 1;
    return (int)(off + flen);
}
