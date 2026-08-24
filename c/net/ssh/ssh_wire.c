#include "ssh_wire.h"

int ssh_w_u8(uint8_t *buf, int off, int max, uint8_t v)
{
    if (off < 0 || off + 1 > max) return -1;
    buf[off] = v;
    return off + 1;
}

int ssh_w_u32(uint8_t *buf, int off, int max, uint32_t v)
{
    if (off < 0 || off + 4 > max) return -1;
    buf[off + 0] = (uint8_t)(v >> 24);
    buf[off + 1] = (uint8_t)(v >> 16);
    buf[off + 2] = (uint8_t)(v >> 8);
    buf[off + 3] = (uint8_t)(v);
    return off + 4;
}

int ssh_w_bool(uint8_t *buf, int off, int max, int v)
{
    return ssh_w_u8(buf, off, max, v ? 1 : 0);
}

int ssh_w_bytes(uint8_t *buf, int off, int max, const uint8_t *data, int len)
{
    if (off < 0 || len < 0 || off + len > max) return -1;
    for (int i = 0; i < len; i++) buf[off + i] = data[i];
    return off + len;
}

int ssh_w_string(uint8_t *buf, int off, int max, const uint8_t *data, int len)
{
    off = ssh_w_u32(buf, off, max, (uint32_t)len);
    if (off < 0) return -1;
    return ssh_w_bytes(buf, off, max, data, len);
}

int ssh_w_cstring(uint8_t *buf, int off, int max, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return ssh_w_string(buf, off, max, (const uint8_t *)s, n);
}

int ssh_w_mpint(uint8_t *buf, int off, int max, const uint8_t *raw, int rawlen)
{
    /* Strip leading zero bytes. */
    while (rawlen > 0 && raw[0] == 0) { raw++; rawlen--; }
    int need_pad = (rawlen > 0 && (raw[0] & 0x80)) ? 1 : 0;
    int total = rawlen + need_pad;

    off = ssh_w_u32(buf, off, max, (uint32_t)total);
    if (off < 0) return -1;
    if (need_pad) { off = ssh_w_u8(buf, off, max, 0); if (off < 0) return -1; }
    return ssh_w_bytes(buf, off, max, raw, rawlen);
}

static int csv_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

int ssh_w_namelist(uint8_t *buf, int off, int max, const char *csv)
{
    return ssh_w_string(buf, off, max, (const uint8_t *)csv, csv_len(csv));
}

int ssh_r_u8(const uint8_t *buf, int off, int len, uint8_t *out)
{
    if (off < 0 || off + 1 > len) return -1;
    *out = buf[off];
    return off + 1;
}

int ssh_r_u32(const uint8_t *buf, int off, int len, uint32_t *out)
{
    if (off < 0 || off + 4 > len) return -1;
    *out = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
           ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
    return off + 4;
}

int ssh_r_bool(const uint8_t *buf, int off, int len, int *out)
{
    uint8_t v;
    off = ssh_r_u8(buf, off, len, &v);
    if (off < 0) return -1;
    *out = v != 0;
    return off;
}

int ssh_r_string(const uint8_t *buf, int off, int len, const uint8_t **data, int *dlen)
{
    uint32_t n;
    off = ssh_r_u32(buf, off, len, &n);
    if (off < 0) return -1;
    /* n is attacker-controlled; refuse anything that would overrun before
     * ever indexing with it. SSH_MAX_PAYLOAD-scale strings are the largest
     * anything here legitimately sends. */
    if ((int)n < 0 || off + (int)n > len) return -1;
    *data = buf + off;
    *dlen = (int)n;
    return off + (int)n;
}

int ssh_r_string_cpy(const uint8_t *buf, int off, int len, char *out, int outmax, int *dlen)
{
    const uint8_t *p;
    int n;
    off = ssh_r_string(buf, off, len, &p, &n);
    if (off < 0) return -1;
    int copy = n;
    if (copy > outmax - 1) copy = outmax - 1;
    for (int i = 0; i < copy; i++) out[i] = (char)p[i];
    out[copy] = 0;
    if (dlen) *dlen = n;
    return off;
}

/* 1 if `name` occurs as a whole token (comma- or edge-delimited) in list. */
int ssh_namelist_has(const uint8_t *list, int listlen, const char *name)
{
    int nlen = csv_len(name);
    int i = 0;
    while (i < listlen) {
        int start = i;
        while (i < listlen && list[i] != ',') i++;
        int tok = i - start;
        if (tok == nlen) {
            int eq = 1;
            for (int k = 0; k < tok; k++) if (list[start + k] != (uint8_t)name[k]) { eq = 0; break; }
            if (eq) return 1;
        }
        i++; /* skip comma */
    }
    return 0;
}

int ssh_negotiate(const uint8_t *client_list, int client_len,
                   const char *server_csv, char *out, int outmax)
{
    int i = 0;
    while (i < client_len) {
        int start = i;
        while (i < client_len && client_list[i] != ',') i++;
        int tok = i - start;
        if (tok > 0 && tok < outmax) {
            char cand[96];
            int cn = tok < (int)sizeof(cand) - 1 ? tok : (int)sizeof(cand) - 1;
            for (int k = 0; k < cn; k++) cand[k] = (char)client_list[start + k];
            cand[cn] = 0;
            if (ssh_namelist_has((const uint8_t *)server_csv, csv_len(server_csv), cand)) {
                for (int k = 0; k < tok; k++) out[k] = (char)client_list[start + k];
                out[tok] = 0;
                return tok;
            }
        }
        i++;
    }
    return -1;
}
