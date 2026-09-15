#include "handshake.h"

void wifi_install_group_key(struct wifi_station *station, const uint8_t *packet,
                            const struct received_group_key *key)
{
    struct wifi_group_key_slot *slot = &station->group_keys[key->id];
    /* Group rotations commonly resend the same key with a newer EAPOL replay
     * counter. Keep its data replay windows; accepting the message is not
     * permission to reset CCMP packet numbers (hostap install_gtk rule). */
#ifndef WIFI_NEGCTL_REINSTALL
    if (slot->installed && wifi_bytes_equal(slot->key, key->key, sizeof(slot->key))) {
        return;
    }
#endif
    uint64_t receive_sequence = 0;
    for (unsigned index = 0; index < WIFI_MAC_BYTES; ++index) {
        receive_sequence |= (uint64_t)packet[WIFI_KEY_RSC_OFFSET + index] << (8 * index);
    }
    uint64_t replay_floor[WIFI_REPLAY_SLOT_COUNT];
    for (unsigned index = 0; index < WIFI_REPLAY_SLOT_COUNT; ++index) {
        replay_floor[index] = receive_sequence;
#ifndef WIFI_NEGCTL_REINSTALL
        /* Key ID is not part of the CCMP nonce. Moving identical key material
         * to another slot must preserve the original replay floor as well. */
        for (unsigned prior = 0; prior < 4; ++prior) {
            const struct wifi_group_key_slot *installed = &station->group_keys[prior];
            if (installed->installed && wifi_bytes_equal(installed->key, key->key, 16) &&
                installed->replay[index] > replay_floor[index]) {
                replay_floor[index] = installed->replay[index];
            }
        }
#endif
    }
    memcpy(slot->key, key->key, sizeof(slot->key));
    memcpy(slot->replay, replay_floor, sizeof(slot->replay));
    slot->installed = 1;
}

int wifi_receive_group_rekey(struct wifi_station *station, const uint8_t *packet, size_t bytes,
                             uint64_t replay)
{
    if (wifi_verify_key_mic(station->ptk, packet, bytes)) {
        return -1;
    }
    if (station->group_reply_valid && replay == station->group_replay &&
        wifi_bytes_equal(packet + WIFI_KEY_MIC_OFFSET, station->group_message_mic,
                         WIFI_KEY_MIC_BYTES)) {
        return wifi_send_key_reply(station, WIFI_KEY_INFO_GROUP_TWO, replay, station->ptk);
    }
    if (replay <= station->replay) {
        return -1;
    }
    uint8_t key_data[WIFI_KEY_DATA_MAX_BYTES];
    struct received_group_key key = {0};
    int result = -1;
    int key_bytes =
        wifi_key_unwrap(station->ptk + WIFI_PTK_KEK_OFFSET, packet + WIFI_KEY_FIXED_BYTES,
                        bytes - WIFI_KEY_FIXED_BYTES, key_data, sizeof(key_data));
    if (key_bytes >= 0 && !wifi_parse_key_data(key_data, (size_t)key_bytes, &key, NULL) &&
        !wifi_send_key_reply(station, WIFI_KEY_INFO_GROUP_TWO, replay, station->ptk)) {
        wifi_install_group_key(station, packet, &key);
        station->replay = replay;
        station->group_replay = replay;
        memcpy(station->group_message_mic, packet + WIFI_KEY_MIC_OFFSET, WIFI_KEY_MIC_BYTES);
        station->group_reply_valid = 1;
        result = 0;
    }
    wifi_erase_secret(key_data, sizeof(key_data));
    wifi_erase_secret(&key, sizeof(key));
    return result;
}
