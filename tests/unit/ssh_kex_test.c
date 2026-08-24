/* curve25519-sha256 kex + RFC 4253 7.2 KDF against an INDEPENDENT oracle
 * (tests/unit/ssh_kex_gen.py, built from the RFC text with Python's
 * `cryptography` library -- no code shared with ssh_kex.c).
 *
 * THE NEGATIVE CONTROL THE TASK BRIEF ASKED FOR: flip one byte of the
 * session id into the KDF and every derived key must differ, and the MAC
 * check on a real packet framed with the correct key must then fail. Run
 * both directions -- "the vectors agree" and "a wrong session id visibly
 * breaks everything downstream" -- because a KDF that happened to ignore
 * session_id entirely would still pass the first half. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_kex.h"
#include "ssh_packet.h"
#include "ssh_hostkey.h"
#include "crypto.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex2bin(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

#define FIELD_MAX 4096
static char g_fields[32][2 * FIELD_MAX];
static const char *g_names[32];
static int g_nfields;

static const char *field(const char *name)
{
    for (int i = 0; i < g_nfields; i++) if (strcmp(g_names[i], name) == 0) return g_fields[i];
    fprintf(stderr, "missing vector field %s\n", name);
    exit(1);
}

static void load_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run ssh_kex_gen.py first)\n", path); exit(1); }
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char name[64], val[8192];
        if (sscanf(line, "%63s %8191s", name, val) != 2) continue;
        static char names_store[32][64];
        strcpy(names_store[g_nfields], name);
        g_names[g_nfields] = names_store[g_nfields];
        strcpy(g_fields[g_nfields], val);
        g_nfields++;
    }
    fclose(f);
}

static uint8_t g_server_priv[32];
static void fixed_server_priv(uint8_t *p, int n) { memcpy(p, g_server_priv, (size_t)n); }

/* File-scope (not nested -- this test must build under clang, which does not
 * accept GCC's nested-function extension) wire buffer for the packet-level
 * negative control below. */
static uint8_t g_wire[SSH_MAX_PACKET + 64];
static int g_wire_len, g_wire_off;

static int test_write_fn(void *ctx, uint8_t *buf, int len)
{
    (void)ctx;
    if (g_wire_len + len > (int)sizeof g_wire) return -1;
    memcpy(g_wire + g_wire_len, buf, (size_t)len);
    g_wire_len += len;
    return len;
}
static int test_read_fn(void *ctx, uint8_t *buf, int len)
{
    (void)ctx;
    if (g_wire_off + len > g_wire_len) return -1;
    memcpy(buf, g_wire + g_wire_off, (size_t)len);
    g_wire_off += len;
    return len;
}
static void test_rnd(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)i; }

int main(int argc, char **argv)
{
    const char *vecpath = argc > 1 ? argv[1] : "build/ssh_kex_vectors.txt";
    load_vectors(vecpath);

    uint8_t V_C[64], V_S[64], I_C[256], I_S[256];
    uint8_t host_seed[32], host_pub[32], client_pub[32], server_priv_v[32], server_pub_v[32];
    uint8_t K_v[32], H_v[32], sig_v[64];
    uint8_t iv_c2s_v[16], iv_s2c_v[16], enc_c2s_v[16], enc_s2c_v[16], mac_c2s_v[32], mac_s2c_v[32];

    int vclen = hex2bin(field("VC"), V_C, sizeof V_C);
    int vslen = hex2bin(field("VS"), V_S, sizeof V_S);
    int iclen = hex2bin(field("IC"), I_C, sizeof I_C);
    int islen = hex2bin(field("IS"), I_S, sizeof I_S);
    hex2bin(field("HOST_SEED"), host_seed, 32);
    hex2bin(field("HOST_PUB"), host_pub, 32);
    hex2bin(field("CLIENT_PUB"), client_pub, 32);
    hex2bin(field("SERVER_PRIV"), server_priv_v, 32);
    hex2bin(field("SERVER_PUB"), server_pub_v, 32);
    hex2bin(field("K"), K_v, 32);
    hex2bin(field("H"), H_v, 32);
    hex2bin(field("SIG"), sig_v, 64);
    hex2bin(field("IV_C2S"), iv_c2s_v, 16);
    hex2bin(field("IV_S2C"), iv_s2c_v, 16);
    hex2bin(field("ENC_C2S"), enc_c2s_v, 16);
    hex2bin(field("ENC_S2C"), enc_s2c_v, 16);
    hex2bin(field("MAC_C2S"), mac_c2s_v, 32);
    hex2bin(field("MAC_S2C"), mac_s2c_v, 32);
    memcpy(g_server_priv, server_priv_v, 32);

    /* Sanity: the vector's own host pubkey must be the one ed25519_pubkey
     * derives from its seed -- if this fails, the vectors are broken, not
     * ssh_kex.c (checked so a later failure is never misread). */
    uint8_t derived_pub[32];
    ed25519_pubkey(derived_pub, host_seed);
    ok(memcmp(derived_pub, host_pub, 32) == 0, "vector sanity: HOST_PUB derives from HOST_SEED");

    uint8_t reply[256]; int replylen;
    uint8_t k_raw[32], h[32];
    int rc = ssh_kex_ecdh_reply_build(V_C, vclen, V_S, vslen, I_C, iclen, I_S, islen,
                                      host_pub, host_seed, client_pub, fixed_server_priv,
                                      reply, sizeof reply, &replylen, k_raw, h);
    ok(rc == 0, "ssh_kex_ecdh_reply_build succeeds");
    ok(memcmp(k_raw, K_v, 32) == 0, "shared secret K matches the independent X25519 oracle");
    ok(memcmp(h, H_v, 32) == 0, "exchange hash H matches the independent SHA-256 oracle");

    /* Q_S and the signature are inside `reply` (RFC 8731's KEX_ECDH_REPLY:
     * byte type + string K_S + string Q_S + string sig-blob) -- parse it
     * back out with the SAME wire reader every real client uses, so this
     * checks the message ssh_kex.c actually SENDS, not a side channel. */
    {
        const uint8_t *ks, *qs, *sigblob;
        int kslen, qslen, sigbloblen;
        int off = 1;
        off = ssh_r_string(reply, off, replylen, &ks, &kslen);
        off = ssh_r_string(reply, off, replylen, &qs, &qslen);
        off = ssh_r_string(reply, off, replylen, &sigblob, &sigbloblen);
        ok(off == replylen, "KEX_ECDH_REPLY parses to exactly its own length");
        ok(qslen == 32 && memcmp(qs, server_pub_v, 32) == 0, "Q_S matches x25519_base(SERVER_PRIV)");
        const uint8_t *algn, *rawsig; int algnlen, rawsiglen;
        int so = ssh_r_string(sigblob, 0, sigbloblen, &algn, &algnlen);
        so = ssh_r_string(sigblob, so, sigbloblen, &rawsig, &rawsiglen);
        ok(so == sigbloblen && rawsiglen == 64, "signature blob parses (ssh-ed25519, 64-byte sig)");
        ok(rawsiglen == 64 && memcmp(rawsig, sig_v, 64) == 0,
           "Ed25519 signature over H is BYTE-IDENTICAL to Python cryptography's (EdDSA is deterministic)");
        ok(ed25519_verify(rawsig, h, 32, host_pub) == 1, "and it verifies");
    }

    uint8_t iv_c2s[16], iv_s2c[16], enc_c2s[16], enc_s2c[16], mac_c2s[32], mac_s2c[32];
    ssh_kex_derive_keys(k_raw, h, h /* session_id = H on first kex */,
                        iv_c2s, iv_s2c, enc_c2s, enc_s2c, mac_c2s, mac_s2c);
    ok(memcmp(iv_c2s, iv_c2s_v, 16) == 0, "IV_C2S matches oracle");
    ok(memcmp(iv_s2c, iv_s2c_v, 16) == 0, "IV_S2C matches oracle");
    ok(memcmp(enc_c2s, enc_c2s_v, 16) == 0, "ENC_C2S matches oracle");
    ok(memcmp(enc_s2c, enc_s2c_v, 16) == 0, "ENC_S2C matches oracle");
    ok(memcmp(mac_c2s, mac_c2s_v, 32) == 0, "MAC_C2S matches oracle");
    ok(memcmp(mac_s2c, mac_s2c_v, 32) == 0, "MAC_S2C matches oracle");

    /* ==================== THE NEGATIVE CONTROL ==================== */
    uint8_t bad_session_id[32];
    memcpy(bad_session_id, h, 32);
    bad_session_id[7] ^= 0x01; /* one bit, one byte */

    uint8_t b_iv_c2s[16], b_iv_s2c[16], b_enc_c2s[16], b_enc_s2c[16], b_mac_c2s[32], b_mac_s2c[32];
    ssh_kex_derive_keys(k_raw, h, bad_session_id,
                        b_iv_c2s, b_iv_s2c, b_enc_c2s, b_enc_s2c, b_mac_c2s, b_mac_s2c);
    ok(memcmp(iv_c2s, b_iv_c2s, 16) != 0, "NEGCTL: one flipped session_id byte changes IV_C2S");
    ok(memcmp(iv_s2c, b_iv_s2c, 16) != 0, "NEGCTL: ... changes IV_S2C");
    ok(memcmp(enc_c2s, b_enc_c2s, 16) != 0, "NEGCTL: ... changes ENC_C2S");
    ok(memcmp(enc_s2c, b_enc_s2c, 16) != 0, "NEGCTL: ... changes ENC_S2C");
    ok(memcmp(mac_c2s, b_mac_c2s, 32) != 0, "NEGCTL: ... changes MAC_C2S");
    ok(memcmp(mac_s2c, b_mac_s2c, 32) != 0, "NEGCTL: ... changes MAC_S2C");

    /* And the consequence that actually matters on the wire: a packet MACed
     * with the CORRECT key must be REJECTED by a receiver holding the key
     * derived from the wrong session id -- "every derived key must differ
     * AND the MAC check must fail", both halves, watched. */
    {
        struct ssh_dir_state good_send = {0}, bad_recv = {0};
        ssh_dir_activate(&good_send, enc_s2c, iv_s2c, mac_s2c);
        /* Wrong MAC key ONLY (correct enc/iv), so the packet DECRYPTS and
         * FRAMES cleanly -- packet_length parses fine, padding is
         * plausible -- and the one thing that can fail is the MAC compare.
         * Using the fully-wrong key set here would make a wrong ENC key the
         * more likely cause of the rejection (a garbled packet_length fails
         * FORMAT validation before the MAC is ever computed), which would
         * not be testing what this control claims to test -- exactly the
         * "the thing reporting failure is not looking at what you think"
         * trap CLAUDE.md names. */
        ssh_dir_activate(&bad_recv, enc_s2c, iv_s2c, b_mac_s2c);

        uint8_t payload[5] = { SSH_MSG_CHANNEL_DATA, 1, 2, 3, 4 };
        g_wire_len = 0; g_wire_off = 0;
        int src = ssh_pkt_send(&good_send, test_write_fn, 0, payload, sizeof payload, test_rnd);
        ok(src == 0, "a packet framed under the CORRECT keys sends");

        uint8_t out[64];
        int rc2 = ssh_pkt_recv(&bad_recv, test_read_fn, 0, out, sizeof out);
        ok(rc2 == SSH_PKT_E_MAC, "NEGCTL: the SAME packet, read back under the WRONG session-id-derived "
                                 "keys, fails with SSH_PKT_E_MAC (not a length/format error -- the MAC itself)");

        /* And the control on the control: the CORRECT receiver must accept
         * the same bytes, or the negative result above would be meaningless
         * (a receiver that rejects everything "fails" too). */
        struct ssh_dir_state good_recv = {0};
        ssh_dir_activate(&good_recv, enc_s2c, iv_s2c, mac_s2c);
        g_wire_off = 0; /* replay the same wire bytes from the start */
        uint8_t out2[64];
        int rc3 = ssh_pkt_recv(&good_recv, test_read_fn, 0, out2, sizeof out2);
        ok(rc3 == (int)sizeof payload && memcmp(out2, payload, sizeof payload) == 0,
           "control: the CORRECT receiver decodes the same bytes cleanly");
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
