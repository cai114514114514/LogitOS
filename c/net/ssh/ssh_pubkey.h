#ifndef LOGIT_SSH_PUBKEY_H
#define LOGIT_SSH_PUBKEY_H
#include <stdint.h>
/* Client authentication algorithms, independent of the Ed25519 host identity.
 * RSA keeps the ssh-rsa key encoding, but uses SHA-2 signatures (RFC 8332). */
#define SSH_AUTH_ALGORITHMS "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256"
#define SSH_AUTH_KEY_MAX 1024
/* Call once before starting authentication workers: ECDSA's shared curve
 * tables are initialized lazily and are read-only after initialization. */
void ssh_pubkey_init(void);
int ssh_pubkey_supported(const char *alg,const uint8_t *blob,int len);
int ssh_pubkey_verify(const char *alg,const uint8_t *blob,int len,
                     const uint8_t *signature,int siglen,const uint8_t *data,int datalen);
int ssh_pubkey_ext_info(uint8_t *out,int max);
#endif
