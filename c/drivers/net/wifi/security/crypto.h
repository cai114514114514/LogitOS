#ifndef LOGIT_WIFI_CRYPTO_H
#define LOGIT_WIFI_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#define WIFI_MSDU_MAX 2304u
/* WPA2's mandated HMAC-SHA1 PRF is protocol compatibility, not a signature
 * hash choice. Existing SHA1 block implementation is reused; no SHA1 mode is
 * added to TLS or certificate validation. AES uses the shared backend. */
int wifi_hmac_sha1(const uint8_t *key, size_t key_bytes, const uint8_t *message, size_t bytes,
                   uint8_t digest[20]);
int wifi_psk(const uint8_t *pass, size_t pass_bytes, const uint8_t *ssid, size_t ssid_bytes,
             uint8_t pmk[32]);
int wifi_ptk(const uint8_t pmk[32], const uint8_t sta[6], const uint8_t ap[6],
             const uint8_t snonce[32], const uint8_t anonce[32], uint8_t ptk[48]);
int wifi_key_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t bytes, uint8_t *output,
                    size_t capacity);
/* RFC3610 CCM with 13-byte nonce and 8-byte tag, as used by CCMP-128.
 * open verifies into private scratch before publishing plaintext. All outputs
 * may overlap input but must not overlap key/nonce/AAD; caller owns buffers. */
int wifi_ccm_seal(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                  size_t aad_bytes, const uint8_t *plain, size_t bytes, uint8_t *out,
                  size_t capacity);
int wifi_ccm_open(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                  size_t aad_bytes, const uint8_t *cipher, size_t bytes, uint8_t *out,
                  size_t capacity);
#endif
