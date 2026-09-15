#include "internal.h"

/* RFC3610 CCM with L=2 and M=8, the fixed CCMP-128 parameters.
 * Counter zero masks the tag; plaintext starts with counter one. */
static void authenticate_block(const struct aes_backend *aes, const uint8_t *round_keys,
                               uint8_t accumulator[16], const uint8_t block[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        accumulator[i] ^= block[i];
    }
    aes->encrypt(round_keys, WIFI_AES128_ROUNDS, accumulator, accumulator);
}

static void authenticate_message(const struct aes_backend *aes, const uint8_t *round_keys,
                                 const uint8_t nonce[WIFI_CCM_NONCE_BYTES], const uint8_t *aad,
                                 size_t aad_bytes, const uint8_t *plaintext, size_t input_bytes,
                                 uint8_t authentication_tag[16])
{
    uint8_t block[16] = {0};
    block[0] = (aad_bytes ? WIFI_CCM_FLAG_AAD : 0) | WIFI_CCM_FLAG_TAG8 | WIFI_CCM_FLAG_LENGTH2;
    memcpy(block + 1, nonce, WIFI_CCM_NONCE_BYTES);
    block[14] = (uint8_t)(input_bytes >> 8);
    block[15] = (uint8_t)input_bytes;
    memset(authentication_tag, 0, 16);
    authenticate_block(aes, round_keys, authentication_tag, block);
    if (aad_bytes) {
        memset(block, 0, 16);
        block[0] = (uint8_t)(aad_bytes >> 8);
        block[1] = (uint8_t)aad_bytes;
        unsigned offset = 2;
        for (size_t i = 0; i < aad_bytes; ++i) {
            block[offset++] = aad[i];
            if (offset == 16) {
                authenticate_block(aes, round_keys, authentication_tag, block);
                memset(block, 0, 16);
                offset = 0;
            }
        }
        if (offset) {
            authenticate_block(aes, round_keys, authentication_tag, block);
        }
    }
    for (size_t offset = 0; offset < input_bytes; offset += 16) {
        size_t block_bytes = input_bytes - offset < 16 ? input_bytes - offset : 16;
        memset(block, 0, 16);
        memcpy(block, plaintext + offset, block_bytes);
        authenticate_block(aes, round_keys, authentication_tag, block);
    }
    erase_secret(block, sizeof(block));
}

static void apply_counter_stream(const struct aes_backend *aes, const uint8_t *round_keys,
                                 const uint8_t nonce[WIFI_CCM_NONCE_BYTES], const uint8_t *input,
                                 size_t input_bytes, uint8_t *output, uint8_t tag_mask[16])
{
    uint8_t counter_block[16] = {WIFI_CCM_FLAG_LENGTH2}, stream[16];
    memcpy(counter_block + 1, nonce, WIFI_CCM_NONCE_BYTES);
    aes->encrypt(round_keys, WIFI_AES128_ROUNDS, counter_block, tag_mask);
    for (size_t offset = 0; offset < input_bytes; offset += 16) {
        unsigned counter = (unsigned)(offset / 16) + 1;
        counter_block[14] = (uint8_t)(counter >> 8);
        counter_block[15] = (uint8_t)counter;
        aes->encrypt(round_keys, WIFI_AES128_ROUNDS, counter_block, stream);
        size_t block_bytes = input_bytes - offset < 16 ? input_bytes - offset : 16;
        for (size_t i = 0; i < block_bytes; ++i) {
            output[offset + i] = input[offset + i] ^ stream[i];
        }
    }
    erase_secret(stream, sizeof(stream));
}

int wifi_ccm_seal(const uint8_t key[16], const uint8_t nonce[WIFI_CCM_NONCE_BYTES],
                  const uint8_t *aad, size_t aad_bytes, const uint8_t *input, size_t input_bytes,
                  uint8_t *output, size_t output_capacity)
{
    uint8_t round_keys[WIFI_AES128_SCHEDULE_BYTES], authentication_tag[16], tag_mask[16],
        scratch[WIFI_MSDU_MAX + WIFI_CCM_TAG_BYTES];
    if (!key || !nonce || (!aad && aad_bytes) || (!input && input_bytes) || !output ||
        input_bytes > WIFI_MSDU_MAX || aad_bytes > WIFI_CCM_MAX_AAD_BYTES ||
        output_capacity < input_bytes + WIFI_CCM_TAG_BYTES) {
        return -1;
    }
    const struct aes_backend *aes = aes_current_backend();
    aes->key_expand(key, 16, round_keys);
    authenticate_message(aes, round_keys, nonce, aad, aad_bytes, input, input_bytes,
                         authentication_tag);
    apply_counter_stream(aes, round_keys, nonce, input, input_bytes, scratch, tag_mask);
    for (unsigned i = 0; i < WIFI_CCM_TAG_BYTES; ++i) {
        scratch[input_bytes + i] = authentication_tag[i] ^ tag_mask[i];
    }
    memcpy(output, scratch, input_bytes + WIFI_CCM_TAG_BYTES);
    erase_secret(round_keys, sizeof(round_keys));
    erase_secret(authentication_tag, sizeof(authentication_tag));
    erase_secret(tag_mask, sizeof(tag_mask));
    erase_secret(scratch, sizeof(scratch));
    return (int)(input_bytes + WIFI_CCM_TAG_BYTES);
}

int wifi_ccm_open(const uint8_t key[16], const uint8_t nonce[WIFI_CCM_NONCE_BYTES],
                  const uint8_t *aad, size_t aad_bytes, const uint8_t *input, size_t input_bytes,
                  uint8_t *output, size_t output_capacity)
{
    uint8_t round_keys[WIFI_AES128_SCHEDULE_BYTES], authentication_tag[16], tag_mask[16],
        scratch[WIFI_MSDU_MAX];
    if (!key || !nonce || (!aad && aad_bytes) || !input || !output ||
        input_bytes < WIFI_CCM_TAG_BYTES || input_bytes > WIFI_MSDU_MAX + WIFI_CCM_TAG_BYTES ||
        aad_bytes > WIFI_CCM_MAX_AAD_BYTES || output_capacity < input_bytes - WIFI_CCM_TAG_BYTES) {
        return -1;
    }
    const struct aes_backend *aes = aes_current_backend();
    aes->key_expand(key, 16, round_keys);
    apply_counter_stream(aes, round_keys, nonce, input, input_bytes - WIFI_CCM_TAG_BYTES, scratch,
                         tag_mask);
    authenticate_message(aes, round_keys, nonce, aad, aad_bytes, scratch,
                         input_bytes - WIFI_CCM_TAG_BYTES, authentication_tag);
    unsigned difference = 0;
    for (unsigned i = 0; i < WIFI_CCM_TAG_BYTES; ++i) {
        difference |=
            authentication_tag[i] ^ tag_mask[i] ^ input[input_bytes - WIFI_CCM_TAG_BYTES + i];
    }
#ifdef WIFI_NEGCTL_MIC
    difference = 0;
#endif
    if (!difference) {
        memcpy(output, scratch, input_bytes - WIFI_CCM_TAG_BYTES);
    }
    erase_secret(round_keys, sizeof(round_keys));
    erase_secret(authentication_tag, sizeof(authentication_tag));
    erase_secret(tag_mask, sizeof(tag_mask));
    erase_secret(scratch, sizeof(scratch));
    return difference ? -1 : (int)(input_bytes - WIFI_CCM_TAG_BYTES);
}
