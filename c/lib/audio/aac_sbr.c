/* c/lib/audio/aac_sbr.c -- LogitOS adapter for FFmpeg's HE-AAC v1 SBR core.
 *
 * SPDX-FileCopyrightText: 2026 LogitOS contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The included implementation files retain FFmpeg's LGPL-2.1-or-later
 * copyright and license.  They are an intentionally narrow import from tag
 * n4.4 (commit dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1): SBR v1 only, scalar
 * DSP only, no AAC core and no Parametric Stereo.  See THIRD_PARTY.md.
 *
 * WHY INCLUDE THE UPSTREAM C IN ONE TRANSLATION UNIT.  Its public surface is
 * built around FFmpeg-private AACContext/FFTContext/VLC types.  Re-exporting
 * those would turn a stable four-function codec boundary into a permanent
 * dependency on libavcodec internals.  Keeping it behind this TU also means
 * the ordinary LogitOS AAC decoder remains independently testable.
 */

#include <stdlib.h>
#include <string.h>
#include "aac_sbr.h"
#include "afft.h"

/* The compatibility headers beside the imported files map FFmpeg's small
 * private surface to these routines. */
#include "../../../third_party/ffmpeg-aacsbr/aacsbr.c"
#include "../../../third_party/ffmpeg-aacsbr/sbrdsp.c"

struct aac_sbr_ctx {
    AACContext ac;
    AVCodecContext av;
    AVFloatDSPContext fdsp;
    SpectralBandReplication sbr;
    float planar[2][2048];
};

static void compat_vector_fmul_reverse(float *dst, const float *a,
                                       const float *b, int n)
{
    for (int i = 0; i < n; i++) dst[i] = a[i] * b[n - 1 - i];
}

static void compat_vector_fmul(float *dst, const float *a,
                               const float *b, int n)
{
    for (int i = 0; i < n; i++) dst[i] = a[i] * b[i];
}

static void compat_vector_fmul_add(float *dst, const float *a,
                                   const float *b, const float *c, int n)
{
    for (int i = 0; i < n; i++) dst[i] = a[i] * b[i] + c[i];
}

/* FFmpeg's legacy imdct_half is the negative middle half of the literal full
 * IMDCT.  That mapping was cross-checked against libavutil 60's AV_TX MDCT at
 * N=128: max absolute difference 1.68e-6 over a nontrivial 64-coefficient
 * vector, exactly the expected float rounding.  Reusing afft here avoids a
 * second transform implementation and its independent sign/index traps. */
static void compat_imdct_half(FFTContext *s, float *out, const float *in)
{
    double x[64], y[128];
    for (int i = 0; i < 64; i++) x[i] = in[i];
    amdct_imdct(s->plan, x, y);
    for (int i = 0; i < 64; i++) out[i] = (float)(-y[32 + i] * s->scale);
}

int ff_mdct_init(FFTContext *s, int bits, int inverse, double scale)
{
    if (!s || bits != 7 || !inverse) return -1;
    memset(s, 0, sizeof(*s));
    s->plan = amdct_new(128);
    if (!s->plan) return -1;
    s->mdct_bits = bits;
    s->scale = scale;
    s->imdct_half = compat_imdct_half;
    return 0;
}

void ff_mdct_end(FFTContext *s)
{
    if (!s) return;
    amdct_free(s->plan);
    memset(s, 0, sizeof(*s));
}

void ff_ps_init(void) {}
void ff_ps_ctx_init(PSContext *ps) { if (ps) memset(ps, 0, sizeof(*ps)); }

int ff_ps_read_data(AVCodecContext *avctx, GetBitContext *gb,
                    PSCommonContext *ps, int bits_left)
{
    (void)avctx; (void)ps;
    skip_bits_long(gb, bits_left);
    return bits_left;
}

int ff_ps_apply(AVCodecContext *avctx, PSContext *ps,
                float L[2][38][64], float R[2][38][64], int top)
{
    (void)avctx; (void)ps; (void)L; (void)R; (void)top;
    return -1;
}

aac_sbr_ctx *aac_sbr_open(int core_rate, int output_rate, int channels)
{
    static int global_init;
    if (core_rate <= 0 || output_rate != core_rate * 2 ||
        (channels != 1 && channels != 2)) return NULL;

    aac_sbr_ctx *s = (aac_sbr_ctx *)malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    if (!global_init) {
        ff_aac_sbr_init();
        global_init = 1;
        for (int i = 0; i < 10; i++) {
            if (!vlc_sbr[i].table) {
                global_init = -1;
                break;
            }
        }
    }
    if (global_init < 0) { free(s); return NULL; }
    s->fdsp.vector_fmul_reverse = compat_vector_fmul_reverse;
    s->fdsp.vector_fmul = compat_vector_fmul;
    s->fdsp.vector_fmul_add = compat_vector_fmul_add;
    s->ac.avctx = &s->av;
    s->ac.fdsp = &s->fdsp;
    s->ac.oc[1].m4ac.sample_rate = core_rate;
    s->ac.oc[1].m4ac.ext_sample_rate = output_rate;
    s->ac.oc[1].m4ac.ps = 0;
    ff_aac_sbr_ctx_init(&s->ac, &s->sbr, channels == 2 ? TYPE_CPE : TYPE_SCE);
    if (!s->sbr.mdct.plan || !s->sbr.mdct_ana.plan) {
        aac_sbr_close(s);
        return NULL;
    }
    return s;
}

void aac_sbr_close(aac_sbr_ctx *s)
{
    if (!s) return;
    ff_aac_sbr_ctx_close(&s->sbr);
    free(s);
}

int aac_sbr_parse(aac_sbr_ctx *s, const uint8_t *data, long nbits,
                  long bitpos, int count, int crc, int element_type)
{
    if (!s || !data || nbits <= 0 || nbits > 0x7fffffffL ||
        bitpos < 4 || bitpos > nbits || count <= 0 ||
        bitpos - 4 > nbits - (long)count * 8) return -1;
    if (element_type != TYPE_SCE && element_type != TYPE_CPE &&
        element_type != TYPE_LFE) return -1;

    GetBitContext gb;
    if (init_get_bits(&gb, data, (int)nbits) < 0) return -1;
    gb.index = (int)bitpos;
    ff_decode_sbr_extension(&s->ac, &s->sbr, &gb, !!crc, count,
                            element_type == TYPE_LFE ? TYPE_SCE : element_type);
    return gb.error ? -1 : 0;
}

int aac_sbr_synthesize(aac_sbr_ctx *s, const float *core, float *output,
                       int channels)
{
    if (!s || !core || !output || (channels != 1 && channels != 2)) return -1;
    for (int c = 0; c < channels; c++)
        for (int i = 0; i < 1024; i++)
            s->planar[c][i] = core[i * channels + c];

    ff_sbr_apply(&s->ac, &s->sbr, channels == 2 ? TYPE_CPE : TYPE_SCE,
                 s->planar[0], channels == 2 ? s->planar[1] : NULL);

    for (int i = 0; i < 2048; i++)
        for (int c = 0; c < channels; c++)
            output[i * channels + c] = s->planar[c][i];
    return 0;
}
