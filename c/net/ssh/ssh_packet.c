#include "ssh_packet.h"
#include "ssh_wire.h"
#include "crypto.h"

#define BLOCK   16
#define MAC_LEN 32

/* RFC 4344 3.1: the counter is the WHOLE 128-bit IV, incremented as one
 * running value across every block this direction ever encrypts -- not reset
 * per packet (that is GCM's inc32, and it wraps only the low 4 bytes; see the
 * note in crypto.h). Adds `nblocks` (fits in 32 bits: even the max packet
 * here is ~35000/16 =~ 2188 blocks) with carry propagating left through the
 * full 16 bytes. */
static void ctr_advance(uint8_t ctr[16], uint32_t nblocks)
{
    uint32_t carry = nblocks;
    for (int i = 15; i >= 0 && carry; i--) {
        uint32_t sum = (uint32_t)ctr[i] + (carry & 0xFF);
        ctr[i] = (uint8_t)sum;
        carry = (carry >> 8) + (sum >> 8);
    }
}

void ssh_dir_activate(struct ssh_dir_state *st,
                      const uint8_t enc_key[16], const uint8_t iv[16],
                      const uint8_t mac_key[32])
{
    for (int i = 0; i < 16; i++) { st->enc_key[i] = enc_key[i]; st->ctr[i] = iv[i]; }
    for (int i = 0; i < 32; i++) st->mac_key[i] = mac_key[i];
    st->cipher_on = 1;
    st->mac_on = 1;
    /* st->seq is deliberately UNTOUCHED: RFC 4253 6.4 initialises it to zero
     * for the direction's first packet ever (the plaintext KEXINIT included)
     * and never resets it at NEWKEYS. */
}

int ssh_pkt_send(struct ssh_dir_state *st, ssh_io_fn write_fn, void *ctx,
                 const uint8_t *payload, int paylen,
                 void (*randbytes)(uint8_t *, int))
{
    if (paylen < 0 || paylen > SSH_MAX_PAYLOAD) return -1;
    int block = st->cipher_on ? BLOCK : 8;

    /* RFC 4253 6: (packet_length-field + padding_length-field + payload +
     * padding) must be a multiple of `block`; padding_length in [4,255];
     * total (still excluding the mac) at least 16 bytes. */
    int pad = block - ((5 + paylen) % block);
    if (pad < 4) pad += block;
    int total = 4 + 1 + paylen + pad;
    while (total < 16) { pad += block; total += block; }
    if (pad > 255) return -1; /* unreachable given SSH_MAX_PAYLOAD, kept honest */

    /* An ORDINARY stack array, not `static`: sshd.c is thread-per-connection
     * (see its header), so two connections can call this at the same instant
     * on two different `st`s in two different threads. A function-static
     * buffer here would be ONE shared instance across every thread in the
     * process -- silent cross-connection corruption the first time two
     * clients overlapped -- which is exactly the class of bug this file's
     * house style calls out (see CLAUDE.md's "measured, not feared"). ~33 KB
     * on the stack is why sshd.c gives each connection thread a stack sized
     * for it plus headroom, rather than the default. */
    uint8_t mbuf[4 + 1 + SSH_MAX_PAYLOAD + 255];
    const int mbuf_max = (int)sizeof mbuf;

    int off = ssh_w_u32(mbuf, 0, mbuf_max, (uint32_t)(1 + paylen + pad));
    off = ssh_w_u8(mbuf, off, mbuf_max, (uint8_t)pad);
    off = ssh_w_bytes(mbuf, off, mbuf_max, payload, paylen);
    if (off < 0) return -1;
    randbytes(mbuf + off, pad);
    off += pad;
    if (off != total) return -1; /* internal consistency; never trips */

    uint8_t mac[MAC_LEN];
    if (st->mac_on) {
        uint8_t macmsg[4 + sizeof mbuf];
        int mo = ssh_w_u32(macmsg, 0, (int)sizeof macmsg, st->seq);
        mo = ssh_w_bytes(macmsg, mo, (int)sizeof macmsg, mbuf, total);
        hmac(32, st->mac_key, 32, macmsg, mo, mac);
    }

    if (st->cipher_on) {
        aes128_ctr(st->enc_key, st->ctr, mbuf, total, mbuf); /* in place: aliasing is documented-ok */
        ctr_advance(st->ctr, (uint32_t)((total + BLOCK - 1) / BLOCK));
    }

    if (write_fn(ctx, mbuf, total) != total) return -1;
    if (st->mac_on && write_fn(ctx, mac, MAC_LEN) != MAC_LEN) return -1;
    st->seq++;
    return 0;
}

int ssh_pkt_recv(struct ssh_dir_state *st, ssh_io_fn read_fn, void *ctx,
                 uint8_t *out, int outmax)
{
    int block = st->cipher_on ? BLOCK : 8;

    /* plain[0..4) = the sequence number (filled in only once we know `total`
     * and need it for the MAC input); plain[4..) = the decrypted packet
     * (packet_length | padding_length | payload | padding). One buffer,
     * decrypted in place, for the same reason ssh_pkt_send uses one. */
    uint8_t plain[4 + SSH_MAX_PACKET];   /* ordinary stack array -- see the
                                           * thread-safety note in ssh_pkt_send */
    const int plain_max = (int)sizeof plain;

    if (read_fn(ctx, plain + 4, block) != block) return SSH_PKT_E_IO;
    if (st->cipher_on) aes128_ctr(st->enc_key, st->ctr, plain + 4, block, plain + 4);

    uint32_t packet_length;
    if (ssh_r_u32(plain, 4, plain_max, &packet_length) < 0) return SSH_PKT_E_FORMAT;
    if (packet_length < 1 || (int)packet_length > SSH_MAX_PACKET - 4) return SSH_PKT_E_FORMAT;

    int total = 4 + (int)packet_length; /* excludes the mac */
    int floor = block > 16 ? block : 16;
    if (total < floor) return SSH_PKT_E_FORMAT;
    if (total % block != 0) return SSH_PKT_E_FORMAT;
    if (4 + total > plain_max) return SSH_PKT_E_TOOBIG;

    int got = block;
    if (got < total) {
        int need = total - got;
        if (read_fn(ctx, plain + 4 + got, need) != need) return SSH_PKT_E_IO;
        if (st->cipher_on) {
            uint8_t ctr2[16];
            for (int i = 0; i < 16; i++) ctr2[i] = st->ctr[i];
            ctr_advance(ctr2, 1); /* phase 1 above consumed exactly one block */
            aes128_ctr(st->enc_key, ctr2, plain + 4 + got, need, plain + 4 + got);
        }
    }

    if (st->mac_on) {
        ssh_w_u32(plain, 0, 4, st->seq);
        uint8_t mac_calc[MAC_LEN], mac_wire[MAC_LEN];
        hmac(32, st->mac_key, 32, plain, 4 + total, mac_calc);
        if (read_fn(ctx, mac_wire, MAC_LEN) != MAC_LEN) return SSH_PKT_E_IO;
        uint8_t diff = 0;
        for (int i = 0; i < MAC_LEN; i++) diff |= (uint8_t)(mac_calc[i] ^ mac_wire[i]);
        if (diff) return SSH_PKT_E_MAC;
    }

    if (st->cipher_on) ctr_advance(st->ctr, (uint32_t)((total + BLOCK - 1) / BLOCK));

    uint8_t padding_length = plain[4 + 4];
    if (padding_length < 4) return SSH_PKT_E_FORMAT;
    int paylen = (int)packet_length - 1 - (int)padding_length;
    if (paylen < 0) return SSH_PKT_E_FORMAT;
    if (paylen > outmax) return SSH_PKT_E_TOOBIG;
    for (int i = 0; i < paylen; i++) out[i] = plain[4 + 5 + i];

    st->seq++;
    return paylen;
}
