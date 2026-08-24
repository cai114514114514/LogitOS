#ifndef LOGIT_SSH_KEX_H
#define LOGIT_SSH_KEX_H
#include <stdint.h>
#include "ssh.h"

/* Our fixed algorithm lists -- see the long comment at the top of ssh.h for
 * why each one, argued against a captured real client. Exposed as strings so
 * ssh_kexinit_build and a host test can both quote them without drifting. */
#define SSH_KEX_LIST      "curve25519-sha256,curve25519-sha256@libssh.org"
#define SSH_HOSTKEY_LIST  "ssh-ed25519"
#define SSH_CIPHER_LIST   "aes128-ctr"
#define SSH_MAC_LIST      "hmac-sha2-256"
#define SSH_COMPRESS_LIST "none"

/* Build our SSH_MSG_KEXINIT payload (msg type + 16-byte random cookie + our
 * six algorithm categories x2 directions + empty languages + a FALSE
 * first_kex_packet_follows + a zero reserved uint32). Returns the length, or
 * -1 if it would not fit `outmax`. */
int ssh_kexinit_build(uint8_t *out, int outmax, void (*randbytes)(uint8_t *, int));

struct ssh_negotiated {
    char kex[SSH_MAX_NAME];
    char hostkey[SSH_MAX_NAME];
    char cipher_c2s[SSH_MAX_NAME];
    char cipher_s2c[SSH_MAX_NAME];
    char mac_c2s[SSH_MAX_NAME];
    char mac_s2c[SSH_MAX_NAME];
};

/* Parse a client's SSH_MSG_KEXINIT payload and negotiate all six categories
 * against our fixed lists (RFC 4253 7.1: first name on the CLIENT's list that
 * is also on ours wins). Returns 0, or -1 with a NUL-terminated reason in
 * `why`/`whymax` (e.g. "no common cipher") if any category has no match. */
int ssh_kexinit_negotiate(const uint8_t *client_payload, int client_len,
                          struct ssh_negotiated *out, char *why, int whymax);

/* Build SSH_MSG_KEX_ECDH_REPLY (RFC 8731) for a curve25519-sha256 exchange:
 * generates our ephemeral X25519 keypair, computes the shared secret against
 * the client's Q_C, computes the exchange hash H = SHA-256(V_C||V_S||I_C||
 * I_S||K_S||Q_C||Q_S||mpint(K)) and signs H with the Ed25519 host key.
 * `I_C`/`I_S` are the FULL SSH_MSG_KEXINIT payloads (msg-type byte included,
 * packet framing excluded) exchanged earlier, per RFC 4253 8.
 * Writes the reply message into `out` and the raw 32-byte shared secret into
 * `k_raw_out` (needed, still in raw form, by ssh_kex_derive_keys below,
 * which re-encodes it as an mpint itself -- kept raw here rather than
 * pre-encoded so the caller cannot accidentally feed the KDF a DIFFERENT
 * mpint encoding than the one baked into H). Returns 0, or -1. */
int ssh_kex_ecdh_reply_build(
    const uint8_t *V_C, int vclen, const uint8_t *V_S, int vslen,
    const uint8_t *I_C, int iclen, const uint8_t *I_S, int islen,
    const uint8_t host_pub[32], const uint8_t host_seed[32],
    const uint8_t client_pub[32],
    void (*randbytes)(uint8_t *, int),
    uint8_t *out, int outmax, int *outlen,
    uint8_t k_raw_out[32], uint8_t h_out[32]);

/* RFC 4253 7.2 key derivation, HASH = SHA-256 (curve25519-sha256). `k_raw`
 * is the 32-byte raw shared secret from ssh_kex_ecdh_reply_build; it is
 * re-encoded here as the SAME mpint ssh_kex_ecdh_reply_build folded into H --
 * both call the identical ssh_w_mpint, so this cannot silently diverge.
 * `session_id` is H from the FIRST kex of the connection (unchanged by any
 * later rekey, which this server never performs). */
void ssh_kex_derive_keys(const uint8_t k_raw[32], const uint8_t h[32],
                         const uint8_t session_id[32],
                         uint8_t iv_c2s[16], uint8_t iv_s2c[16],
                         uint8_t enckey_c2s[16], uint8_t enckey_s2c[16],
                         uint8_t mackey_c2s[32], uint8_t mackey_s2c[32]);

#endif /* LOGIT_SSH_KEX_H */
