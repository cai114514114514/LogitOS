/* Exercise the H.264 bounds through the real NAL parser and slice dispatcher.
 * The slice specimen starts with the committed sample's decoded first picture:
 * merely calling a helper with a fabricated `cur` would miss the condition that
 * made the old first_mb_in_slice check exploitable. The next NAL keeps that
 * picture's valid header and macroblock data but replaces ue(0) with ue(-2 as
 * uint32), so an old signed >= check reaches mb[-2]. SPS crop specimens use the
 * same bitstream reader, including its 31-leading-zero Exp-Golomb edge. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"
#include "bs.h"

uint8_t *h264_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len);

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t bits;
} writer;

static int put_bit(writer *w, unsigned bit)
{
    if (w->bits / 8 >= w->cap) return 0;
    if (bit) w->data[w->bits / 8] |= (uint8_t)(0x80u >> (w->bits & 7));
    w->bits++;
    return 1;
}

static int put_bits(writer *w, uint64_t value, int count)
{
    for (int i = count - 1; i >= 0; i--)
        if (!put_bit(w, (unsigned)((value >> i) & 1))) return 0;
    return 1;
}

static int put_ue(writer *w, uint32_t value)
{
    uint64_t code = (uint64_t)value + 1;
    int width = 0;
    for (uint64_t n = code; n; n >>= 1) width++;
    for (int i = 1; i < width; i++)
        if (!put_bit(w, 0)) return 0;
    return put_bits(w, code, width);
}

static int start_code(const uint8_t *data, size_t len, size_t from)
{
    for (size_t i = from; i + 3 < len; i++)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return (int)i;
    return -1;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fclose(f); return NULL; }
    *len = (size_t)ftell(f);
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *data = malloc(*len ? *len : 1);
    if (!data || fread(data, 1, *len, f) != *len) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    return data;
}

static int slice_test(const char *path)
{
    size_t len = 0;
    uint8_t *stream = read_file(path, &len);
    h264dec *d = h264_open();
    int ok = 0;
    if (!stream || !d) goto done;

    /* Include SPS, PPS, SEI, and the first complete IDR slice, ending just
     * before the next NAL. This builds `cur` and `mb` through the public API. */
    int nal = -1, end = -1;
    for (int sc = start_code(stream, len, 0); sc >= 0;) {
        int at = sc + 3;
        int next = start_code(stream, len, (size_t)at + 1);
        if ((stream[at] & 31) == 5) { nal = at; end = next; break; }
        sc = next;
    }
    if (nal < 0 || end < 0) goto done;
    h264frame frame;
    int got = 0;
    int used = h264_decode(d, stream, end, &frame, &got);
    if (used != end || got || !d->cur || !d->mb) goto done;

    int rbsp_len = 0;
    uint8_t *rbsp = h264_nal_to_rbsp(stream + nal, end - nal, &rbsp_len);
    if (!rbsp) goto done;
    bs_t bs;
    bs_init(&bs, rbsp, rbsp_len);
    if (bs_ue(&bs) != 0 || bs_error(&bs)) { free(rbsp); goto done; }

    size_t cap = (size_t)rbsp_len + 16;
    uint8_t *changed = calloc(cap, 1);
    if (!changed) { free(rbsp); goto done; }
    writer w = {changed, cap, 0};
    /* ue(0xfffffffe) is the largest value bs_ue accepts: 31 zero bits
     * followed by 32 ones. Its cast to int is -2. ue(0xffffffff) is not
     * reachable here because bs_ue refuses 32 leading zeros. */
    int wrote = put_ue(&w, UINT32_MAX - 1);
    for (int bit = bs.bitpos; wrote && bit < rbsp_len * 8; bit++)
        wrote = put_bit(&w, (rbsp[bit / 8] >> (7 - (bit & 7))) & 1);
    free(rbsp);
    if (!wrote) { free(changed); goto done; }

    /* Reinsert emulation-prevention bytes after shifting the RBSP. Otherwise
     * the Annex B scanner could mistake a payload 00 00 01 for a new NAL. */
    size_t bytes = (w.bits + 7) / 8;
    uint8_t *packet = malloc(bytes * 2 + 4);
    if (!packet) { free(changed); goto done; }
    size_t n = 0;
    packet[n++] = 0; packet[n++] = 0; packet[n++] = 1;
    packet[n++] = stream[nal];
    int zeros = 0;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t b = changed[i];
        if (zeros == 2 && b <= 3) { packet[n++] = 3; zeros = 0; }
        packet[n++] = b;
        zeros = b == 0 ? zeros + 1 : 0;
    }
    free(changed);

    int check_len = 0;
    uint8_t *check = h264_nal_to_rbsp(packet + 3, (int)n - 3, &check_len);
    slice_t sl;
    bs_init(&bs, check, check_len);
    int header = check ? h264_parse_slice_header(d, &bs,
                     (stream[nal] >> 5) & 3, stream[nal] & 31, &sl) : -1;
    free(check);
    if (header != H264_OK || sl.first_mb_in_slice != -2) {
        fprintf(stderr, "H264-BOUNDS-SETUP-FAIL: crafted slice header rc=%d first=%d\n",
                header, header == H264_OK ? sl.first_mb_in_slice : 0);
        free(packet); goto done;
    }
    puts("H264-BOUNDS-SETUP: valid first picture and negative slice header parsed");
    fflush(stdout);
    used = h264_decode(d, packet, (int)n, &frame, &got);
    free(packet);
    if (used != H264_ERR_CORRUPT || got) {
        fprintf(stderr, "H264-BOUNDS-FAIL slice: got %d want %d\n",
                used, H264_ERR_CORRUPT);
        goto done;
    }
    puts("H264-BOUNDS-OK slice: negative first_mb_in_slice refused");
    ok = 1;
done:
    if (!ok) fprintf(stderr, "H264-BOUNDS-FAIL slice setup or rejection\n");
    h264_close(d);
    free(stream);
    return ok;
}

static int crop_case(const char *name, const uint32_t crop[4], int want)
{
    uint8_t data[96] = {0};
    writer w = {data, sizeof data, 0};
    int built = put_bits(&w, 66, 8) && put_bits(&w, 0, 8) &&
                put_bits(&w, 30, 8) && put_ue(&w, 0) &&
                put_ue(&w, 0) && put_ue(&w, 2) && put_ue(&w, 1) &&
                put_bit(&w, 0) && put_ue(&w, 0) && put_ue(&w, 0) &&
                put_bit(&w, 1) && put_bit(&w, 1) && put_bit(&w, 1);
    for (int i = 0; i < 4 && built; i++) built = put_ue(&w, crop[i]);
    built = built && put_bit(&w, 0) && put_bit(&w, 1);
    if (!built) return 0;
    h264dec *d = h264_open();
    if (!d) return 0;
    bs_t bs;
    bs_init(&bs, data, (int)((w.bits + 7) / 8));
    int got = h264_parse_sps(d, &bs);
    int width = 0, height = 0;
    int info = h264_stream_info(d, &width, &height, NULL);
    int ok = got == want && !bs_error(&bs) &&
             (want != H264_OK ? info == H264_ERR_CORRUPT :
              info == H264_OK && width == 12 && height == 12);
    if (!ok)
        fprintf(stderr, "H264-BOUNDS-FAIL crop %s: parse=%d info=%d size=%dx%d want=%d\n",
                name, got, info, width, height, want);
    h264_close(d);
    return ok;
}

static int crop_test(void)
{
    const uint32_t valid[4] = {1, 1, 1, 1};
    const uint32_t negative[4] = {UINT32_MAX - 1, 0, 0, 0};
    const uint32_t full_width[4] = {8, 0, 0, 0};
    const uint32_t over_height[4] = {0, 0, 9, 0};
    int ok = crop_case("valid", valid, H264_OK) &&
             crop_case("negative-left", negative, H264_ERR_CORRUPT) &&
             crop_case("full-width", full_width, H264_ERR_CORRUPT) &&
             crop_case("over-height", over_height, H264_ERR_CORRUPT);
    if (ok) puts("H264-BOUNDS-OK crop: legal window accepted; negative and empty windows refused");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "crop") == 0) return crop_test() ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "slice") == 0) return slice_test(argv[2]) ? 0 : 1;
    fprintf(stderr, "usage: %s crop | slice sample.h264\n", argv[0]);
    return 2;
}
