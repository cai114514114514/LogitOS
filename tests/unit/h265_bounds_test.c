/* Direct boundary tests for the HEVC parser and reference-picture keeper.
 *
 * Include the orchestrator so this test can call apply_rps with a legal
 * parsed-count combination. A whole-stream fixture would need many decoded
 * reference pictures before reaching that one dangerous append; checking
 * only a malformed header would miss it. The controls compile the very same
 * functions with one historical guard disabled and must fail by name (or
 * ASan's stack-buffer-overflow), rather than pass because a fixture is absent.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../c/lib/video/h265.c"
#include "../../c/lib/video/h265_nal.c"

typedef struct {
    uint8_t bytes[128];
    int bits;
} bits_t;

static void put_bit(bits_t *b, unsigned v)
{
    if (b->bits >= (int)sizeof b->bytes * 8) abort();
    b->bytes[b->bits / 8] |= (uint8_t)((v & 1u) << (7 - b->bits % 8));
    b->bits++;
}

static void put_bits(bits_t *b, uint32_t v, int count)
{
    for (int i = count - 1; i >= 0; i--) put_bit(b, v >> i);
}

static void put_ue(bits_t *b, uint32_t value)
{
    uint64_t code = (uint64_t)value + 1;
    int width = 0;
    for (uint64_t v = code; v; v >>= 1) width++;
    for (int i = 1; i < width; i++) put_bit(b, 0);
    for (int i = width - 1; i >= 0; i--) put_bit(b, (unsigned)(code >> i));
}

/* Minimal 64x64, 8-bit, 4:2:0 SPS. The crop fields are the only variable;
 * this tests the actual h265_parse_sps entry, not a duplicated crop formula. */
static int parse_crop(h265dec *d, const uint32_t crop[4])
{
    bits_t b = {0};
    put_bits(&b, 0, 4);               /* sps_video_parameter_set_id */
    put_bits(&b, 0, 3);               /* one temporal sub-layer */
    put_bit(&b, 1);                   /* temporal nesting */
    put_bits(&b, 0, 32);              /* profile_tier_level */
    put_bits(&b, 0, 32);
    put_bits(&b, 0, 24);
    put_bits(&b, 120, 8);
    put_ue(&b, 0);                    /* sps_id */
    put_ue(&b, 1);                    /* chroma_format_idc */
    put_ue(&b, 64); put_ue(&b, 64);  /* coded size */
    put_bit(&b, 1);                   /* conformance_window_flag */
    for (int i = 0; i < 4; i++) put_ue(&b, crop[i]);
    put_ue(&b, 0); put_ue(&b, 0);    /* 8-bit luma and chroma */
    put_ue(&b, 0);                    /* log2_max_poc_lsb_minus4 */
    put_bit(&b, 1);                   /* sub_layer_ordering_info_present */
    put_ue(&b, 0); put_ue(&b, 0); put_ue(&b, 0);
    put_ue(&b, 0); put_ue(&b, 0);    /* min block 8, CTB 8 */
    put_ue(&b, 0); put_ue(&b, 0);    /* min/max transform 4 */
    put_ue(&b, 0); put_ue(&b, 0);    /* transform hierarchy depths */
    put_bit(&b, 0); put_bit(&b, 0);  /* scaling list, AMP */
    put_bit(&b, 0); put_bit(&b, 0);  /* SAO, PCM */
    put_ue(&b, 0);                    /* no short-term RPS */
    put_bit(&b, 0); put_bit(&b, 0);  /* long-term refs, temporal MVP */
    put_bit(&b, 0); put_bit(&b, 0);  /* smoothing, VUI */

    bs_t bs;
    bs_init(&bs, b.bytes, (b.bits + 7) / 8);
    return h265_parse_sps(d, &bs);
}

static int test_crop(void)
{
    h265dec *d = calloc(1, sizeof *d);
    if (!d) return 2;
    const uint32_t valid[4] = {1, 1, 1, 1};
    const uint32_t negative[4] = {0x80000000u, 0, 0, 0};
    const uint32_t zero_width[4] = {16, 16, 0, 0};
    const uint32_t beyond[4] = {0, 33, 0, 0};
    int good = parse_crop(d, valid) == H265_OK &&
               d->sps[0].conf_win[0] == 1 &&
               parse_crop(d, negative) == H265_ERR_CORRUPT &&
               parse_crop(d, zero_width) == H265_ERR_CORRUPT &&
               parse_crop(d, beyond) == H265_ERR_CORRUPT;
    free(d);
    if (!good) {
        puts("FAIL crop: malformed SPS accepted or valid crop refused");
        return 1;
    }
    puts("PASS crop: valid accepted; negative, zero-size and oversized rejected");
    return 0;
}

static int test_rps(void)
{
    h265dec *d = calloc(1, sizeof *d);
    if (!d) return 2;
    d->poc = 32;
    d->max_poc_lsb = 256;
    d->cur = &d->pics[H265_MAX_DPB - 1];
    for (int i = 0; i < H265_MAX_REFS; i++) {
        d->pics[i].used = 1;
        d->pics[i].reference = 1;
        d->pics[i].poc = 31 - i;
    }
    slice_t sl = {0};
    sl.type = SLICE_I;
    sl.rps.num_negative = H265_MAX_REFS;
    for (int i = 0; i < H265_MAX_REFS; i++) sl.rps.delta_poc[i] = -1 - i;
    sl.num_long_term = 19;
    for (int i = 0; i < sl.num_long_term; i++) sl.lt_poc[i] = 31;
    int rc = apply_rps(d, &sl, NAL_TRAIL_R);
    free(d);
    if (rc != H265_ERR_CORRUPT) {
        printf("FAIL rps: 16 short + 19 duplicate long-term refs returned %d\n", rc);
        return 1;
    }
    puts("PASS rps: 35th retained entry rejected before keep[34]");
    return 0;
}

static int test_strps(void)
{
    strps_t sets[2] = {0};
    sets[0].num_negative = H265_MAX_REFS;
    for (int i = 0; i < H265_MAX_REFS; i++)
        sets[0].delta_poc[i] = -2 - i;

    /* Inter-RPS predicts -1 plus 16 shifted negatives. Every flag keeps
     * its delta, so the derived set has 17 entries and must be refused
     * before writing delta_poc[16] or used[16]. */
    bits_t b = {0};
    put_bit(&b, 1);       /* inter_ref_pic_set_prediction_flag */
    put_bit(&b, 1);       /* delta_rps_sign: negative */
    put_ue(&b, 0);       /* abs_delta_rps_minus1 */
    for (int i = 0; i <= H265_MAX_REFS; i++) put_bit(&b, 1);
    bs_t bs;
    bs_init(&bs, b.bytes, (b.bits + 7) / 8);
    strps_t out = {0};
    int rc = parse_strps(&bs, &out, sets, 1, 2);
    if (rc != H265_ERR_CORRUPT) {
        printf("FAIL strps: 17 derived deltas returned %d\n", rc);
        return 1;
    }
    puts("PASS strps: 17th derived delta rejected before array write");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "crop") == 0) return test_crop();
    if (strcmp(argv[1], "rps") == 0) return test_rps();
    if (strcmp(argv[1], "strps") == 0) return test_strps();
    return 2;
}
