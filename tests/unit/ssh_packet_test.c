/* Binary packet protocol (c/net/ssh/ssh_packet.c): round-trip framing at a
 * spread of payload sizes, both before and after NEWKEYS (block size 8 vs
 * 16, mac absent vs present), plus the negative controls a wire parser
 * needs: a corrupted MAC byte, a corrupted ciphertext byte, a truncated
 * read, and an oversized claimed length -- each must be REFUSED with the
 * SPECIFIC error it is, not merely "something failed" (see ssh_kex_test.c's
 * own note on the same trap: a compound corruption can fail for the wrong
 * reason and still look like a passing test). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ssh.h"
#include "ssh_packet.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static uint8_t g_wire[SSH_MAX_PACKET + 64];
static int g_len, g_off;

static int wr(void *ctx, uint8_t *buf, int len)
{
    (void)ctx;
    if (g_len + len > (int)sizeof g_wire) return -1;
    memcpy(g_wire + g_len, buf, (size_t)len);
    g_len += len;
    return len;
}
static int rd(void *ctx, uint8_t *buf, int len)
{
    (void)ctx;
    if (g_off + len > g_len) return -1;
    memcpy(buf, g_wire + g_off, (size_t)len);
    g_off += len;
    return len;
}
static void rnd(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(0xA5 ^ i); }

static void round_trip(struct ssh_dir_state *send_st, struct ssh_dir_state *recv_st,
                       int paylen, const char *label)
{
    static uint8_t payload[SSH_MAX_PAYLOAD];
    for (int i = 0; i < paylen; i++) payload[i] = (uint8_t)(i * 31 + 7);

    g_len = 0; g_off = 0;
    int src = ssh_pkt_send(send_st, wr, 0, payload, paylen, rnd);
    char msg[128];
    snprintf(msg, sizeof msg, "%s: send(len=%d) succeeds", label, paylen);
    ok(src == 0, msg);

    /* Every packet's wire length is a multiple of the block size, and at
     * least 16 bytes (or the block size) -- assert the FRAMING invariant
     * directly, not just that decode happens to work, since a decoder that
     * tolerated misaligned frames would hide an encoder bug here. */
    int block = send_st->cipher_on ? 16 : 8;
    int wire_before_mac = g_len - (send_st->mac_on ? 32 : 0);
    snprintf(msg, sizeof msg, "%s: wire length before mac (%d) is a multiple of block size %d",
             label, wire_before_mac, block);
    ok(wire_before_mac % block == 0, msg);
    int floor = block > 16 ? block : 16;
    snprintf(msg, sizeof msg, "%s: wire length before mac (%d) is >= %d", label, wire_before_mac, floor);
    ok(wire_before_mac >= floor, msg);

    static uint8_t out[SSH_MAX_PAYLOAD];
    int rc = ssh_pkt_recv(recv_st, rd, 0, out, sizeof out);
    snprintf(msg, sizeof msg, "%s: recv(len=%d) returns the same length", label, paylen);
    ok(rc == paylen, msg);
    snprintf(msg, sizeof msg, "%s: payload bytes survive the round trip", label);
    ok(rc == paylen && memcmp(out, payload, (size_t)paylen) == 0, msg);
}

int main(void)
{
    /* --- plaintext phase (pre-NEWKEYS): block size 8, no mac --- */
    struct ssh_dir_state ps = {0}, pr = {0};
    int sizes[] = { 0, 1, 5, 8, 16, 100, 255, 511, 1024, 4096, SSH_MAX_PAYLOAD };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char label[32]; snprintf(label, sizeof label, "plaintext");
        round_trip(&ps, &pr, sizes[i], label);
    }
    ok(ps.seq == sizeof sizes / sizeof sizes[0], "plaintext: sender seq advanced once per packet");
    ok(pr.seq == ps.seq, "plaintext: receiver seq tracks the sender's");

    /* --- encrypted phase (post-NEWKEYS): aes128-ctr + hmac-sha2-256 --- */
    uint8_t enc_key[16], iv[16], mac_key[32];
    for (int i = 0; i < 16; i++) { enc_key[i] = (uint8_t)(i * 3 + 1); iv[i] = (uint8_t)(i * 5 + 2); }
    for (int i = 0; i < 32; i++) mac_key[i] = (uint8_t)(i * 7 + 3);
    struct ssh_dir_state cs = {0}, cr = {0};
    ssh_dir_activate(&cs, enc_key, iv, mac_key);
    ssh_dir_activate(&cr, enc_key, iv, mac_key);
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char label[32]; snprintf(label, sizeof label, "encrypted");
        round_trip(&cs, &cr, sizes[i], label);
    }

    /* The counter really is a RUNNING one across packets (RFC 4344), not
     * reset each time -- assert the two directions' independent 128-bit
     * counters actually advanced past their starting IV. */
    ok(memcmp(cs.ctr, iv, 16) != 0, "encrypted: the CTR counter advanced past its starting IV");

    /* ==================== negative controls ==================== */
    struct ssh_dir_state ns = {0}, nr = {0};
    ssh_dir_activate(&ns, enc_key, iv, mac_key);
    ssh_dir_activate(&nr, enc_key, iv, mac_key);
    uint8_t payload[10] = { 1,2,3,4,5,6,7,8,9,10 };

    g_len = 0; g_off = 0;
    ssh_pkt_send(&ns, wr, 0, payload, sizeof payload, rnd);
    g_wire[g_len - 1] ^= 0x01; /* last byte of the wire is inside the MAC */
    {
        uint8_t out[64];
        int rc = ssh_pkt_recv(&nr, rd, 0, out, sizeof out);
        ok(rc == SSH_PKT_E_MAC, "NEGCTL: one flipped MAC byte -> SSH_PKT_E_MAC");
    }

    struct ssh_dir_state ns2 = {0}, nr2 = {0};
    ssh_dir_activate(&ns2, enc_key, iv, mac_key);
    ssh_dir_activate(&nr2, enc_key, iv, mac_key);
    g_len = 0; g_off = 0;
    ssh_pkt_send(&ns2, wr, 0, payload, sizeof payload, rnd);
    g_wire[0] ^= 0x01; /* first byte of ciphertext: corrupts the decrypted packet_length */
    {
        uint8_t out[64];
        int rc = ssh_pkt_recv(&nr2, rd, 0, out, sizeof out);
        ok(rc == SSH_PKT_E_MAC || rc == SSH_PKT_E_FORMAT || rc == SSH_PKT_E_TOOBIG,
           "NEGCTL: one flipped ciphertext byte -> refused (garbled length or bad mac), never accepted");
        ok(rc < 0, "NEGCTL: ... and specifically never returns a non-negative \"success\" length");
    }

    struct ssh_dir_state ns3 = {0}, nr3 = {0};
    ssh_dir_activate(&ns3, enc_key, iv, mac_key);
    ssh_dir_activate(&nr3, enc_key, iv, mac_key);
    g_len = 0; g_off = 0;
    ssh_pkt_send(&ns3, wr, 0, payload, sizeof payload, rnd);
    g_len -= 1; /* truncate the last byte -- read_fn will now short-fail */
    {
        uint8_t out[64];
        int rc = ssh_pkt_recv(&nr3, rd, 0, out, sizeof out);
        ok(rc == SSH_PKT_E_IO, "NEGCTL: a truncated wire -> SSH_PKT_E_IO");
    }

    /* Oversized claimed packet_length: a receiver that trusted the field
     * without a ceiling would read/allocate SSH_MAX_PACKET's worth on a
     * four-byte lie. Plaintext phase (block=8) so the header is easy to
     * hand-craft. */
    struct ssh_dir_state pr2 = {0};
    g_len = 0; g_off = 0;
    g_wire[g_len++] = 0x7F; g_wire[g_len++] = 0xFF; g_wire[g_len++] = 0xFF; g_wire[g_len++] = 0xFF; /* packet_length ~ 2^31 */
    g_wire[g_len++] = 4; /* padding_length */
    for (int i = 0; i < 11; i++) g_wire[g_len++] = 0; /* pad to a block boundary; recv reads 8 first anyway */
    {
        uint8_t out[64];
        int rc = ssh_pkt_recv(&pr2, rd, 0, out, sizeof out);
        ok(rc == SSH_PKT_E_FORMAT, "NEGCTL: a packet_length far beyond SSH_MAX_PACKET is refused, not trusted");
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
