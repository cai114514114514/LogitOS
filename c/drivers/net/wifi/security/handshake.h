#ifndef LOGIT_WIFI_HANDSHAKE_H
#define LOGIT_WIFI_HANDSHAKE_H
#include "../internal.h"
/* Helpers run under the station lock. Parsed key data remains caller-owned
 * secret scratch and must be erased on every exit. */
struct received_group_key {
    uint8_t key[WIFI_CIPHER_KEY_BYTES];
    uint8_t id;
    unsigned present;
};
int wifi_send_key_reply(struct wifi_station *station, uint16_t key_info, uint64_t replay,
                        const uint8_t *ptk);
int wifi_verify_key_mic(const uint8_t *ptk, const uint8_t *packet, size_t bytes);
int wifi_parse_key_data(const uint8_t *data, size_t bytes, struct received_group_key *key,
                        const struct wifi_bss *expected_bss);
void wifi_install_group_key(struct wifi_station *station, const uint8_t *packet,
                            const struct received_group_key *key);
int wifi_receive_group_rekey(struct wifi_station *station, const uint8_t *packet, size_t bytes,
                             uint64_t replay);
#endif
