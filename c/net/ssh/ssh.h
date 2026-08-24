#ifndef LOGIT_SSH_H
#define LOGIT_SSH_H
#include <stdint.h>

/* SSH-2 (RFC 4253/4252/4254), the smallest interoperable set.
 *
 * EVERYTHING IN c/net/ssh IS PLAIN C WITH NO RING ASSUMPTIONS -- no syscalls,
 * no sys_read/sys_write, no fork. Byte buffers in, byte buffers out, exactly
 * the shape c/crypto already uses (ed25519_keypair takes a randbytes callback
 * rather than calling the DRBG itself, for the same reason: it is what lets
 * the kernel, ring 3 AND a host unit test link the identical object file).
 * The socket, the thread, the fork+execve and the account-store file reads
 * are c/apps/coreutils/sshd.c's job, not this directory's.
 *
 * Algorithm set, and why, argued against a REAL OpenSSH_10.2p1 client's
 * default KEXINIT (captured off the wire, not guessed from `-Q`; see
 * ssh_algorithm_notes.txt alongside the host tests for the raw capture):
 *
 *   kex       curve25519-sha256 (+ the @libssh.org alias). The client offers
 *             two post-quantum hybrids FIRST (mlkem768x25519-sha256,
 *             sntrup761x25519-sha512[@openssh.com]) and both nist-P groups,
 *             but curve25519-sha256 is the third preference and the
 *             strongest thing this tree's crypto already has finished,
 *             verified Ed25519/X25519 primitives -- no new curve, no new
 *             finite-field code.
 *   host key  ssh-ed25519. It is in the client's server_host_key_algorithms
 *             list (position 9 of 16, after the *-cert-v01 variants this
 *             server has no CA for) and it is the only signature scheme in
 *             c/crypto that signs rather than only verifies (RSA and ECDSA
 *             here are verify-only until a TLS SERVER needs to sign, and
 *             p-256 ecdsa_sign landed only yesterday -- untested at this
 *             layer, so ed25519 is both the right default and the safe one).
 *   cipher    aes128-ctr. The client's C2S list is
 *             chacha20-poly1305@openssh.com,aes128-gcm@openssh.com,
 *             aes256-gcm@openssh.com,aes128-ctr,aes192-ctr,aes256-ctr --
 *             chacha wins if offered, but OpenSSH's chacha20 variant is NOT
 *             RFC 8439 as-is (two keys; the packet LENGTH is a separate
 *             ChaCha20 block keyed and nonced differently, encrypted before
 *             the length is known rather than after) and getting that
 *             encoding even one bit wrong is silent on the wire and "Bad
 *             packet length" on the client -- the task brief's own warning.
 *             The GCM variants need an AEAD-shaped packet layer (no separate
 *             MAC message) this transport does not have. aes128-ctr is a
 *             plain block cipher this tree already has (aes128_ctr in
 *             crypto.h) plus a detached MAC, which is exactly RFC 4253's
 *             shape.
 *   mac       hmac-sha2-256. The client offers six *-etm@openssh.com/
 *             umac-*@openssh.com names before it, all requiring either the
 *             ETM ordering (MAC over CIPHERTEXT, packet length sent in the
 *             clear) or UMAC (a different primitive this tree has never
 *             implemented). hmac-sha2-256 is the client's 8th offer and the
 *             plain MAC-then-encrypt RFC 4253 6.4 already specifies.
 *   compress  none. The client offers "none,zlib@openssh.com"; there is no
 *             DEFLATE anywhere in this tree, so "none" is not a downgrade,
 *             it is the only option that exists.
 *
 * NOT ADVERTISED ON PURPOSE, both because the client sends them as
 * pseudo-algorithms inside kex_algorithms and a server that names its own
 * corresponding pseudo-algorithm activates a real protocol extension:
 *   kex-strict-s-v00@openssh.com  -- the Terrapin countermeasure. Turning it
 *      on means refusing SSH_MSG_IGNORE/DEBUG before NEWKEYS and resetting
 *      sequence numbers exactly at NEWKEYS with no tolerance; half-implementing
 *      it is worse than not offering it, because the client believes the
 *      stricter contract is in force. Not offered -- the session runs in
 *      ordinary (non-strict) mode, which every OpenSSH client still accepts.
 *   ext-info-s -- RFC 8308 extension-info. Optional; not offered, so the
 *      client sends none and expects none.
 */

/* ---- message numbers (RFC 4253 12, RFC 4252 6, RFC 4254 5/6/9) ---- */
#define SSH_MSG_DISCONNECT                 1
#define SSH_MSG_IGNORE                     2
#define SSH_MSG_UNIMPLEMENTED              3
#define SSH_MSG_DEBUG                      4
#define SSH_MSG_SERVICE_REQUEST            5
#define SSH_MSG_SERVICE_ACCEPT             6
#define SSH_MSG_KEXINIT                    20
#define SSH_MSG_NEWKEYS                    21
#define SSH_MSG_KEX_ECDH_INIT              30
#define SSH_MSG_KEX_ECDH_REPLY             31
#define SSH_MSG_USERAUTH_REQUEST           50
#define SSH_MSG_USERAUTH_FAILURE           51
#define SSH_MSG_USERAUTH_SUCCESS           52
#define SSH_MSG_USERAUTH_BANNER            53
#define SSH_MSG_USERAUTH_PK_OK             60
#define SSH_MSG_GLOBAL_REQUEST             80
#define SSH_MSG_REQUEST_SUCCESS            81
#define SSH_MSG_REQUEST_FAILURE            82
#define SSH_MSG_CHANNEL_OPEN               90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION  91
#define SSH_MSG_CHANNEL_OPEN_FAILURE       92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST      93
#define SSH_MSG_CHANNEL_DATA               94
#define SSH_MSG_CHANNEL_EXTENDED_DATA      95
#define SSH_MSG_CHANNEL_EOF                96
#define SSH_MSG_CHANNEL_CLOSE              97
#define SSH_MSG_CHANNEL_REQUEST            98
#define SSH_MSG_CHANNEL_SUCCESS            99
#define SSH_MSG_CHANNEL_FAILURE            100

#define SSH_DISCONNECT_PROTOCOL_ERROR          2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED     3
#define SSH_DISCONNECT_MAC_ERROR               5
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS     7
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER  13
#define SSH_DISCONNECT_BY_APPLICATION          11
#define SSH_DISCONNECT_ILLEGAL_USER_NAME       15

#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED   1
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE          3

/* ---- limits ---- */
#define SSH_MAX_PACKET       35000   /* RFC 4253 6.1's implementation floor:
                                       * total packet incl. length+pad+mac */
#define SSH_MAX_PAYLOAD      32768   /* RFC 4253 6.1: implementations MUST be
                                       * able to process this uncompressed
                                       * payload size or less */
#define SSH_MAX_IDENT        255     /* RFC 4253 4.2 */
#define SSH_MAX_NAME         64      /* one algorithm name */
#define SSH_MAX_NAMELIST     1024    /* one comma list as we send it */

/* One direction's live symmetric state. Two of these per session (we->peer,
 * peer->we); see ssh_packet.h. */
struct ssh_dir_state {
    int      cipher_on;        /* 0 until this direction's NEWKEYS */
    uint8_t  enc_key[16];      /* aes128-ctr */
    uint8_t  ctr[16];          /* running counter block, RFC 4344 -- NOT reset
                                 * per packet; advanced by whole 16-byte blocks
                                 * across the entire direction's lifetime */
    int      mac_on;
    uint8_t  mac_key[32];      /* hmac-sha2-256 */
    uint32_t seq;               /* wraps; RFC 4253 6.4 */
};

#endif /* LOGIT_SSH_H */
