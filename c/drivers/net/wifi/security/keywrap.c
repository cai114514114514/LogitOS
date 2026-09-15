#include "internal.h"

/* RFC3394 section 2.2.2: reverse six wrapping rounds, then validate A.
 * Keep plaintext private until the 64-bit integrity register is verified. */
int wifi_key_unwrap(const uint8_t key[16], const uint8_t *input, size_t input_bytes,
                    uint8_t *output, size_t output_capacity)
{
    uint8_t scratch[512], integrity_register[8], aes_block[16],
        round_keys[WIFI_AES128_SCHEDULE_BYTES];
    const struct aes_backend *aes = aes_current_backend();
    if (!key || !input || !output || input_bytes < 24 || input_bytes > sizeof(scratch) ||
        input_bytes % 8 || output_capacity < input_bytes - 8) {
        return -1;
    }
    memcpy(integrity_register, input, 8);
    memcpy(scratch, input + 8, input_bytes - 8);
    aes->key_expand(key, 16, round_keys);
    size_t blocks = input_bytes / 8 - 1;
    for (int round = 5; round >= 0; --round) {
        for (size_t block_index = blocks; block_index > 0; --block_index) {
            uint64_t round_counter = blocks * (unsigned)round + block_index;
            memcpy(aes_block, integrity_register, 8);
            for (unsigned byte_index = 0; byte_index < 8; ++byte_index) {
                aes_block[7 - byte_index] ^= (uint8_t)(round_counter >> (8 * byte_index));
            }
            memcpy(aes_block + 8, scratch + (block_index - 1) * 8, 8);
            aes->decrypt(round_keys, WIFI_AES128_ROUNDS, aes_block, aes_block);
            memcpy(integrity_register, aes_block, 8);
            memcpy(scratch + (block_index - 1) * 8, aes_block + 8, 8);
        }
    }
    unsigned difference = 0;
    for (unsigned block_index = 0; block_index < 8; ++block_index) {
        difference |= integrity_register[block_index] ^ WIFI_WRAP_INTEGRITY_BYTE;
    }
    if (!difference) {
        memcpy(output, scratch, input_bytes - 8);
    }
    erase_secret(scratch, sizeof(scratch));
    erase_secret(round_keys, sizeof(round_keys));
    erase_secret(aes_block, sizeof(aes_block));
    erase_secret(integrity_register, sizeof(integrity_register));
    return difference ? -1 : (int)(input_bytes - 8);
}
