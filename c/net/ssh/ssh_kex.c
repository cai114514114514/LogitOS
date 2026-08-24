#include "ssh_kex.h"
#include "ssh_wire.h"
#include "crypto.h"

int ssh_kexinit_build(uint8_t *out, int outmax, void (*randbytes)(uint8_t *, int))
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_KEXINIT);
    if (off < 0) return -1;
    uint8_t cookie[16];
    randbytes(cookie, 16);
    off = ssh_w_bytes(out, off, outmax, cookie, 16);
    off = ssh_w_namelist(out, off, outmax, SSH_KEX_LIST);
    off = ssh_w_namelist(out, off, outmax, SSH_HOSTKEY_LIST);
    off = ssh_w_namelist(out, off, outmax, SSH_CIPHER_LIST);   /* enc c2s */
    off = ssh_w_namelist(out, off, outmax, SSH_CIPHER_LIST);   /* enc s2c */
    off = ssh_w_namelist(out, off, outmax, SSH_MAC_LIST);      /* mac c2s */
    off = ssh_w_namelist(out, off, outmax, SSH_MAC_LIST);      /* mac s2c */
    off = ssh_w_namelist(out, off, outmax, SSH_COMPRESS_LIST); /* comp c2s */
    off = ssh_w_namelist(out, off, outmax, SSH_COMPRESS_LIST); /* comp s2c */
    off = ssh_w_namelist(out, off, outmax, "");                /* languages c2s */
    off = ssh_w_namelist(out, off, outmax, "");                /* languages s2c */
    off = ssh_w_bool(out, off, outmax, 0);                     /* first_kex_packet_follows */
    off = ssh_w_u32(out, off, outmax, 0);                      /* reserved */
    return off;
}

int ssh_kexinit_negotiate(const uint8_t *client_payload, int client_len,
                          struct ssh_negotiated *out, char *why, int whymax)
{
    int off = 1 + 16; /* msg type + cookie */
    struct { char *dst; const char *server_csv; const char *label; } fields[6] = {
        { out->kex,        SSH_KEX_LIST,      "kex algorithm" },
        { out->hostkey,    SSH_HOSTKEY_LIST,  "host key algorithm" },
        { out->cipher_c2s, SSH_CIPHER_LIST,   "cipher (client->server)" },
        { out->cipher_s2c, SSH_CIPHER_LIST,   "cipher (server->client)" },
        { out->mac_c2s,    SSH_MAC_LIST,      "mac (client->server)" },
        { out->mac_s2c,    SSH_MAC_LIST,      "mac (server->client)" },
    };
    for (int i = 0; i < 6; i++) {
        const uint8_t *list; int listlen;
        off = ssh_r_string(client_payload, off, client_len, &list, &listlen);
        if (off < 0) { if (why) { const char *m = "truncated KEXINIT"; int k=0; while(m[k]&&k<whymax-1){why[k]=m[k];k++;} why[k]=0; } return -1; }
        if (ssh_negotiate(list, listlen, fields[i].server_csv, fields[i].dst, SSH_MAX_NAME) < 0) {
            if (why) {
                int k = 0;
                const char *m1 = "no common ";
                while (m1[k] && k < whymax - 1) { why[k] = m1[k]; k++; }
                const char *m2 = fields[i].label;
                for (int j = 0; m2[j] && k < whymax - 1; j++) why[k++] = m2[j];
                why[k] = 0;
            }
            return -1;
        }
    }
    /* Remaining fields (compression x2, languages x2, boolean, reserved) are
     * read by nobody here -- we always answer "none" compression and ignore
     * languages, so there is nothing to negotiate from them. */
    return 0;
}

/* --- exchange-hash helpers: hash a field exactly as it would be WRITTEN on
 * the wire, without materialising the whole packet in memory. Streaming
 * matters most for I_C/I_S (a full KEXINIT, up to ~1 KiB but no fixed cap)
 * and would matter for V_C/V_S too if a client ever sent the full 255-byte
 * RFC 4253 4.2 identification string. --- */
static void hash_string(struct sha256 *c, const uint8_t *data, int len)
{
    uint8_t lb[4];
    ssh_w_u32(lb, 0, 4, (uint32_t)len);
    sha256_update(c, lb, 4);
    sha256_update(c, data, (unsigned long)len);
}

static void hash_mpint(struct sha256 *c, const uint8_t *raw, int rawlen)
{
    while (rawlen > 0 && raw[0] == 0) { raw++; rawlen--; }
    int pad = (rawlen > 0 && (raw[0] & 0x80)) ? 1 : 0;
    uint8_t lb[4];
    ssh_w_u32(lb, 0, 4, (uint32_t)(rawlen + pad));
    sha256_update(c, lb, 4);
    if (pad) { uint8_t z = 0; sha256_update(c, &z, 1); }
    sha256_update(c, raw, (unsigned long)rawlen);
}

int ssh_kex_ecdh_reply_build(
    const uint8_t *V_C, int vclen, const uint8_t *V_S, int vslen,
    const uint8_t *I_C, int iclen, const uint8_t *I_S, int islen,
    const uint8_t host_pub[32], const uint8_t host_seed[32],
    const uint8_t client_pub[32],
    void (*randbytes)(uint8_t *, int),
    uint8_t *out, int outmax, int *outlen,
    uint8_t k_raw_out[32], uint8_t h_out[32])
{
    uint8_t server_priv[32], server_pub[32], shared[32];
    randbytes(server_priv, 32);
    x25519_base(server_pub, server_priv);
    x25519(shared, server_priv, client_pub);
    /* RFC 7748 6.1: an all-zero output means the peer sent a point in the
     * curve's small-order subgroup (a low-order-point attack) -- refuse
     * rather than derive session keys from a secret an active attacker
     * chose. Constant-time-ish is not the point here (the check result is
     * about to be very visible either way, as a dropped connection); just
     * correct. */
    uint8_t allz = 0;
    for (int i = 0; i < 32; i++) allz |= shared[i];
    if (allz == 0) { crypto_wipe(server_priv, 32); return -1; }

    uint8_t ks_blob[4 + 11 + 4 + 32];
    int ko = ssh_w_cstring(ks_blob, 0, (int)sizeof ks_blob, "ssh-ed25519");
    ko = ssh_w_string(ks_blob, ko, (int)sizeof ks_blob, host_pub, 32);

    struct sha256 c;
    sha256_init(&c);
    hash_string(&c, V_C, vclen);
    hash_string(&c, V_S, vslen);
    hash_string(&c, I_C, iclen);
    hash_string(&c, I_S, islen);
    hash_string(&c, ks_blob, ko);
    hash_string(&c, client_pub, 32);
    hash_string(&c, server_pub, 32);
    hash_mpint(&c, shared, 32);
    sha256_final(&c, h_out);

    uint8_t sig[64];
    ed25519_sign(sig, h_out, 32, host_seed, host_pub);
    uint8_t sig_blob[4 + 11 + 4 + 64];
    int so = ssh_w_cstring(sig_blob, 0, (int)sizeof sig_blob, "ssh-ed25519");
    so = ssh_w_string(sig_blob, so, (int)sizeof sig_blob, sig, 64);

    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_KEX_ECDH_REPLY);
    off = ssh_w_string(out, off, outmax, ks_blob, ko);
    off = ssh_w_string(out, off, outmax, server_pub, 32);
    off = ssh_w_string(out, off, outmax, sig_blob, so);
    if (off < 0) { crypto_wipe(server_priv, 32); return -1; }

    for (int i = 0; i < 32; i++) k_raw_out[i] = shared[i];
    crypto_wipe(server_priv, 32);
    crypto_wipe(shared, 32);
    *outlen = off;
    return 0;
}

void ssh_kex_derive_keys(const uint8_t k_raw[32], const uint8_t h[32],
                         const uint8_t session_id[32],
                         uint8_t iv_c2s[16], uint8_t iv_s2c[16],
                         uint8_t enckey_c2s[16], uint8_t enckey_s2c[16],
                         uint8_t mackey_c2s[32], uint8_t mackey_s2c[32])
{
    /* RFC 4253 7.2. HASH = SHA-256 here (curve25519-sha256), whose 32-byte
     * output already covers every key we need (16 for IV/enc, 32 for mac) in
     * one round -- the K1||K2||... extension for a hash shorter than the
     * needed length is not reached. */
    uint8_t full[6][32];
    const char letters[6] = { 'A', 'B', 'C', 'D', 'E', 'F' };
    for (int i = 0; i < 6; i++) {
        struct sha256 c;
        sha256_init(&c);
        hash_mpint(&c, k_raw, 32);
        sha256_update(&c, h, 32);
        sha256_update(&c, &letters[i], 1);
        sha256_update(&c, session_id, 32);
        sha256_final(&c, full[i]);
    }
    for (int i = 0; i < 16; i++) { iv_c2s[i] = full[0][i]; iv_s2c[i] = full[1][i]; }
    for (int i = 0; i < 16; i++) { enckey_c2s[i] = full[2][i]; enckey_s2c[i] = full[3][i]; }
    for (int i = 0; i < 32; i++) { mackey_c2s[i] = full[4][i]; mackey_s2c[i] = full[5][i]; }
}
