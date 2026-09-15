#ifndef LOGIT_WIFI_SECURITY_INTERNAL_H
#define LOGIT_WIFI_SECURITY_INTERNAL_H
#include "aes_backend.h"
#include "crypto.h"
#include <string.h>
enum wifi_crypto_parameters {
    WIFI_SHA1_DIGEST_BYTES = 20,
    WIFI_SHA1_BLOCK_BYTES = 64,
    WIFI_HMAC_INNER_PAD = 0x36,
    WIFI_HMAC_OUTER_PAD = 0x5c,
    WIFI_PBKDF2_ITERATIONS = 4096,
    WIFI_AES_BLOCK_BYTES = 16,
    WIFI_AES128_ROUNDS = 10,
    WIFI_AES128_SCHEDULE_BYTES = 176,
    WIFI_WRAP_REGISTER_BYTES = 8,
    WIFI_WRAP_INTEGRITY_BYTE = 0xa6,
    WIFI_CCM_NONCE_BYTES = 13,
    WIFI_CCM_TAG_BYTES = 8,
    WIFI_CCM_MAX_AAD_BYTES = 30,
    WIFI_CCM_FLAG_AAD = 0x40,
    WIFI_CCM_FLAG_TAG8 = 0x18,
    WIFI_CCM_FLAG_LENGTH2 = 1,
};
/* Volatile writes preserve erasure of caller-independent secret scratch. */
static inline void erase_secret(void *memory, size_t bytes)
{
    volatile uint8_t *cursor = memory;
    while (bytes--) {
        *cursor++ = 0;
    }
}
#endif
