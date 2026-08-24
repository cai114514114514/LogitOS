#ifndef LOGIT_SSH_HOSTKEY_H
#define LOGIT_SSH_HOSTKEY_H
#include <stdint.h>

/* The host key file. This tree does NOT reuse OpenSSH's on-disk private-key
 * format (a bcrypt-wrapped, ASN.1-flavoured PEM) -- nothing here reads it
 * except this server, so a minimal self-describing record is the honest
 * choice, not a shortcut. What DOES have to match OpenSSH byte-for-byte is
 * the WIRE format (the "ssh-ed25519" key blob, see ssh_kex.c), and it does.
 *
 * Record layout, all fixed-size, no padding ambiguity because every field is
 * written and read byte-by-byte (never through a struct cast onto the disk
 * image):
 *   bytes 0..4   magic "LKH1"
 *   bytes 4..36  seed[32]   (the Ed25519 private seed -- ed25519_pubkey
 *                            re-derives the public half from this alone, so
 *                            there is no separate "did the pubkey get saved
 *                            correctly" failure mode)
 *   bytes 36..68 pub[32]    (redundant with seed, but kept alongside it so a
 *                            reader -- the fingerprint printer at boot --
 *                            never needs the private half in hand) */
#define SSH_HOSTKEY_RECORD_LEN 68

int ssh_hostkey_encode(const uint8_t seed[32], const uint8_t pub[32], uint8_t out[SSH_HOSTKEY_RECORD_LEN]);
/* Returns 0, or -1 if `len` is wrong or the magic does not match -- the two
 * ways a truncated or foreign file must not be quietly accepted as a key. */
int ssh_hostkey_decode(const uint8_t *buf, int len, uint8_t seed[32], uint8_t pub[32]);

/* The wire-format public key blob: string "ssh-ed25519" + string pub(32).
 * The SAME bytes as K_S in the exchange hash (ssh_kex.c) and as the second
 * field of an authorized_keys line once base64-decoded -- one encoding, three
 * consumers, so it lives here rather than being rebuilt three times. Returns
 * the length written, or -1. */
int ssh_hostkey_blob(const uint8_t pub[32], uint8_t out[4 + 11 + 4 + 32]);

/* "SHA256:<43 unpadded base64 chars>" -- ssh-keygen -l's default format
 * since OpenSSH 6.8. `out` must hold at least 7+43+1 = 51 bytes. Returns the
 * string length, or -1. */
int ssh_hostkey_fingerprint(const uint8_t pub[32], char *out, int outmax);

#endif /* LOGIT_SSH_HOSTKEY_H */
