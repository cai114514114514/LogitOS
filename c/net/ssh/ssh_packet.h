#ifndef LOGIT_SSH_PACKET_H
#define LOGIT_SSH_PACKET_H
#include <stdint.h>
#include "ssh.h"

/* RFC 4253 6: the binary packet protocol.
 *
 *   uint32 packet_length   (not counting itself or the MAC)
 *   byte   padding_length
 *   byte[] payload
 *   byte[] random padding
 *   byte[] mac              (mac_length bytes; only once this direction's
 *                             NEWKEYS has taken effect)
 *
 * ONE cipher (aes128-ctr) and ONE mac (hmac-sha2-256), MAC-then-encrypt
 * (RFC 4253 6.4, NOT the *-etm@openssh.com ordering): the mac covers
 * sequence_number || plaintext-packet, and encryption then covers
 * packet_length..padding inclusive -- the length field is NOT sent in the
 * clear, unlike ETM. aes128-ctr's counter (RFC 4344) is a single running
 * 128-bit value per direction that keeps incrementing across every packet of
 * that direction's whole lifetime; it is NOT reset per packet the way GCM's
 * inc32 is (see crypto.h's own note on the difference).
 *
 * `read_fn`/`write_fn` do the actual I/O (a real socket in sshd.c, an
 * in-memory buffer or a POSIX socket in a host test) and MUST transfer
 * exactly the requested length or report failure -- short transfers are not
 * a concept this layer has a way to recover from mid-packet. */

typedef int (*ssh_io_fn)(void *ctx, uint8_t *buf, int len);

/* Turn on this direction's cipher+mac (called once, right after this side
 * either SENDS or RECEIVES its own NEWKEYS, per RFC 4253 7.3 -- each
 * direction switches independently and on its OWN NEWKEYS, not the peer's). */
void ssh_dir_activate(struct ssh_dir_state *st,
                      const uint8_t enc_key[16], const uint8_t iv[16],
                      const uint8_t mac_key[32]);

/* Encode `payload`/`paylen` as one wire packet and write it via `write_fn`.
 * Advances st->seq and (if active) st->ctr by however many cipher blocks the
 * packet consumed. `randbytes` fills the padding. Returns 0, or -1 on a
 * write failure or an oversized payload. */
int ssh_pkt_send(struct ssh_dir_state *st, ssh_io_fn write_fn, void *ctx,
                 const uint8_t *payload, int paylen,
                 void (*randbytes)(uint8_t *, int));

/* Read one wire packet via `read_fn`, decrypt/verify it, and write the
 * payload into `out` (capacity `outmax`). Returns the payload length, or a
 * negative SSH_PKT_E_* on failure. Advances st->seq/st->ctr on success only
 * -- a caller that gets an error has a dead connection either way. */
#define SSH_PKT_E_IO      (-1)   /* read_fn failed / peer closed */
#define SSH_PKT_E_FORMAT  (-2)   /* length/padding out of the RFC's bounds */
#define SSH_PKT_E_TOOBIG  (-3)   /* payload would not fit `out` */
#define SSH_PKT_E_MAC     (-4)   /* MAC did not verify -- see the KDF note */
int ssh_pkt_recv(struct ssh_dir_state *st, ssh_io_fn read_fn, void *ctx,
                 uint8_t *out, int outmax);

#endif /* LOGIT_SSH_PACKET_H */
