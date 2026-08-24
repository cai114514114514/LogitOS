#include "base64.h"

static const char ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_encode(const uint8_t *data, int len, char *out, int outmax, int pad)
{
    int oi = 0;
    int i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        if (oi + 4 > outmax) return -1;
        out[oi++] = ALPHA[(v >> 18) & 63];
        out[oi++] = ALPHA[(v >> 12) & 63];
        out[oi++] = ALPHA[(v >> 6) & 63];
        out[oi++] = ALPHA[v & 63];
        i += 3;
    }
    int rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (oi + (pad ? 4 : 2) > outmax) return -1;
        out[oi++] = ALPHA[(v >> 18) & 63];
        out[oi++] = ALPHA[(v >> 12) & 63];
        if (pad) { out[oi++] = '='; out[oi++] = '='; }
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        if (oi + (pad ? 4 : 3) > outmax) return -1;
        out[oi++] = ALPHA[(v >> 18) & 63];
        out[oi++] = ALPHA[(v >> 12) & 63];
        out[oi++] = ALPHA[(v >> 6) & 63];
        if (pad) out[oi++] = '=';
    }
    return oi;
}

static int dec_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, int inlen, uint8_t *out, int outmax)
{
    /* Trim trailing '=' padding, if any, up front -- the group logic below
     * then only has to know how many of the 0..3 leftover input chars there
     * were, not distinguish "padded to 4" from "not padded at all". */
    while (inlen > 0 && in[inlen - 1] == '=') inlen--;

    int oi = 0, i = 0;
    while (i < inlen) {
        int n = inlen - i;
        if (n > 4) n = 4;
        int v[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < n; k++) {
            int d = dec_char(in[i + k]);
            if (d < 0) return -1;
            v[k] = d;
        }
        if (n == 1) return -1; /* one leftover base64 char cannot decode to a byte */
        uint32_t acc = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                       ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        int outbytes = n - 1; /* 2->1, 3->2, 4->3 */
        if (oi + outbytes > outmax) return -1;
        if (outbytes >= 1) out[oi++] = (uint8_t)(acc >> 16);
        if (outbytes >= 2) out[oi++] = (uint8_t)(acc >> 8);
        if (outbytes >= 3) out[oi++] = (uint8_t)(acc);
        i += n;
    }
    return oi;
}
