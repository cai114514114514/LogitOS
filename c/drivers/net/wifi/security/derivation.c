#include "internal.h"

/* Reuse the existing SHA1 compression implementation for WPA2 HMAC. */
extern void ocsp_sha1(const void *, size_t, uint8_t[WIFI_SHA1_DIGEST_BYTES]);

int wifi_hmac_sha1(const uint8_t *key, size_t key_bytes, const uint8_t *message, size_t input_bytes,
                   uint8_t output[WIFI_SHA1_DIGEST_BYTES])
{
    uint8_t inner[WIFI_SHA1_BLOCK_BYTES + WIFI_MSDU_MAX],
        outer[(WIFI_SHA1_BLOCK_BYTES + WIFI_SHA1_DIGEST_BYTES)],
        padded_key[WIFI_SHA1_BLOCK_BYTES] = {0};
    if (!key || !output || (!message && input_bytes) || key_bytes > WIFI_SHA1_BLOCK_BYTES ||
        input_bytes > WIFI_MSDU_MAX) {
        return -1;
    }
    memcpy(padded_key, key, key_bytes);
    for (unsigned i = 0; i < WIFI_SHA1_BLOCK_BYTES; ++i) {
        inner[i] = padded_key[i] ^ WIFI_HMAC_INNER_PAD;
        outer[i] = padded_key[i] ^ WIFI_HMAC_OUTER_PAD;
    }
    if (input_bytes) {
        memcpy(inner + WIFI_SHA1_BLOCK_BYTES, message, input_bytes);
    }
    ocsp_sha1(inner, WIFI_SHA1_BLOCK_BYTES + input_bytes, outer + WIFI_SHA1_BLOCK_BYTES);
    ocsp_sha1(outer, (WIFI_SHA1_BLOCK_BYTES + WIFI_SHA1_DIGEST_BYTES), output);
    erase_secret(inner, sizeof(inner));
    erase_secret(outer, sizeof(outer));
    erase_secret(padded_key, sizeof(padded_key));
    return 0;
}

int wifi_psk(const uint8_t *passphrase, size_t passphrase_bytes, const uint8_t *ssid,
             size_t ssid_bytes, uint8_t output[32])
{
    uint8_t salt[36], iteration_digest[WIFI_SHA1_DIGEST_BYTES],
        block_digest[WIFI_SHA1_DIGEST_BYTES], result[2 * WIFI_SHA1_DIGEST_BYTES];
    if (!passphrase || !ssid || !output || passphrase_bytes < 8 || passphrase_bytes > 63 ||
        !ssid_bytes || ssid_bytes > 32) {
        return -1;
    }
    memcpy(salt, ssid, ssid_bytes);
    memset(salt + ssid_bytes, 0, 4);
    for (unsigned block = 1; block <= 2; ++block) {
        salt[ssid_bytes + 3] = (uint8_t)block;
        wifi_hmac_sha1(passphrase, passphrase_bytes, salt, ssid_bytes + 4, iteration_digest);
        memcpy(block_digest, iteration_digest, WIFI_SHA1_DIGEST_BYTES);
        for (unsigned i = 1; i < WIFI_PBKDF2_ITERATIONS; ++i) {
            wifi_hmac_sha1(passphrase, passphrase_bytes, iteration_digest, WIFI_SHA1_DIGEST_BYTES,
                           iteration_digest);
            for (unsigned j = 0; j < WIFI_SHA1_DIGEST_BYTES; ++j) {
                block_digest[j] ^= iteration_digest[j];
            }
        }
        memcpy(result + (block - 1) * WIFI_SHA1_DIGEST_BYTES, block_digest, WIFI_SHA1_DIGEST_BYTES);
    }
    memcpy(output, result, 32);
    erase_secret(iteration_digest, sizeof(iteration_digest));
    erase_secret(block_digest, sizeof(block_digest));
    erase_secret(result, sizeof(result));
    return 0;
}

int wifi_ptk(const uint8_t pmk[32], const uint8_t station_mac[6], const uint8_t ap_mac[6],
             const uint8_t supplicant_nonce[32], const uint8_t authenticator_nonce[32],
             uint8_t output[48])
{
    uint8_t input[100], digest[WIFI_SHA1_DIGEST_BYTES], result[3 * WIFI_SHA1_DIGEST_BYTES];
    const char label[] = "Pairwise key expansion";
    if (!pmk || !station_mac || !ap_mac || !supplicant_nonce || !authenticator_nonce || !output) {
        return -1;
    }
    /* WPA2 sorts both peers' MAC addresses and nonces, so each endpoint
     * derives identical keys regardless of which side is the supplicant.
     * The label includes its NUL terminator before the sorted seed. */
    memcpy(input, label, sizeof(label));
    size_t offset = sizeof(label);
    const uint8_t *lower = station_mac, *higher = ap_mac;
    if (memcmp(lower, higher, 6) > 0) {
        lower = ap_mac;
        higher = station_mac;
    }
    memcpy(input + offset, lower, 6);
    memcpy(input + offset + 6, higher, 6);
    offset += 12;
    lower = supplicant_nonce;
    higher = authenticator_nonce;
    if (memcmp(lower, higher, 32) > 0) {
        lower = authenticator_nonce;
        higher = supplicant_nonce;
    }
    memcpy(input + offset, lower, 32);
    memcpy(input + offset + 32, higher, 32);
    offset += 64;
    for (unsigned i = 0; i < 3; ++i) {
        input[offset] = (uint8_t)i;
        wifi_hmac_sha1(pmk, 32, input, offset + 1, digest);
        memcpy(result + WIFI_SHA1_DIGEST_BYTES * i, digest, WIFI_SHA1_DIGEST_BYTES);
    }
    memcpy(output, result, 48);
    erase_secret(digest, sizeof(digest));
    erase_secret(result, sizeof(result));
    erase_secret(input, sizeof(input));
    return 0;
}
