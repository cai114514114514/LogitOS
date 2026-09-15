#include "../internal.h"

static size_t wifi_ccmp_build_aad_nonce(const uint8_t *frame, size_t header_bytes,
                                        uint64_t packet_number,
                                        uint8_t aad[WIFI_CCMP_AAD_MAX_BYTES],
                                        uint8_t nonce[WIFI_CCMP_NONCE_BYTES])
{
    /* CCMP authenticates immutable header fields. Mask retry/power bits that
     * the MAC may change during retransmission (Linux mac80211 wpa.c). */
    aad[0] = frame[0] & WIFI_CCMP_AAD_FC0_MASK;
    aad[1] = frame[1] & WIFI_CCMP_AAD_FC1_MASK;
    memcpy(aad + 2, frame + WIFI_ADDR1_OFFSET, 18);
    aad[20] = frame[WIFI_SEQUENCE_OFFSET] & WIFI_SEQUENCE_FRAGMENT_MASK;
    aad[21] = 0;
    nonce[0] = header_bytes == WIFI_QOS_HEADER_BYTES
                   ? (frame[WIFI_QOS_OFFSET] & WIFI_CCMP_QOS_PRIORITY_MASK)
                   : 0;
    memcpy(nonce + 1, frame + WIFI_ADDR2_OFFSET, 6);
    for (unsigned index = 0; index < 6; ++index) {
        nonce[12 - index] = (uint8_t)(packet_number >> (8 * index));
    }
    if (header_bytes == WIFI_QOS_HEADER_BYTES) {
        aad[22] = frame[WIFI_QOS_OFFSET] & WIFI_CCMP_QOS_PRIORITY_MASK;
        aad[23] = 0;
        return WIFI_CCMP_QOS_AAD_BYTES;
    }
    return WIFI_CCMP_AAD_BYTES;
}

static int decrypt_received_payload(struct wifi_station *station, const uint8_t *frame,
                                    size_t header_bytes, const uint8_t *payload, size_t bytes,
                                    uint8_t plain[WIFI_MSDU_MAX])
{
    if (!wifi_station_link_up(station) || bytes < WIFI_CCMP_HEADER_BYTES + WIFI_CCMP_TAG_BYTES ||
        payload[2] || (payload[3] & WIFI_CCMP_FLAG_MASK) != WIFI_CCMP_EXTENDED_IV) {
        return -1;
    }
    unsigned key_id = payload[3] >> WIFI_CCMP_KEY_ID_SHIFT,
             replay_index = header_bytes == WIFI_QOS_HEADER_BYTES
                                ? (frame[WIFI_QOS_OFFSET] & WIFI_CCMP_QOS_PRIORITY_MASK)
                                : WIFI_NON_QOS_REPLAY_INDEX;
    const uint8_t *key = station->ptk + WIFI_PTK_TK_OFFSET;
    uint64_t *last_packet_number = &station->rx_pn[replay_index];
    if (frame[WIFI_ADDR1_OFFSET] & 1) {
        struct wifi_group_key_slot *slot = &station->group_keys[key_id];
        if (!slot->installed) {
            return -1;
        }
        key = slot->key;
        last_packet_number = &slot->replay[replay_index];
    } else if (key_id) {
        return -1;
    }
    uint64_t packet_number = payload[0] | (uint64_t)payload[1] << 8;
    for (unsigned index = 2; index < 6; ++index) {
        packet_number |= (uint64_t)payload[index + 2] << (8 * index);
    }
    if (!packet_number || packet_number <= *last_packet_number) {
        return -1;
    }
    uint8_t aad[WIFI_CCMP_AAD_MAX_BYTES], nonce[WIFI_CCMP_NONCE_BYTES];
    size_t aad_bytes = wifi_ccmp_build_aad_nonce(frame, header_bytes, packet_number, aad, nonce);
    int size = wifi_ccm_open(key, nonce, aad, aad_bytes, payload + WIFI_CCMP_HEADER_BYTES,
                             bytes - WIFI_CCMP_HEADER_BYTES, plain, WIFI_MSDU_MAX);
    if (size < 0) {
        return -1;
    }
    /* Authentication owns the replay update. A forged higher PN cannot
     * discard later valid traffic merely by reaching the parser first. */
    *last_packet_number = packet_number;
    return size;
}

int wifi_receive_data(struct wifi_station *station, const uint8_t *frame, size_t frame_bytes)
{
    uint16_t frame_control = wifi_read_le16(frame);
    size_t header_bytes =
        (frame_control & WIFI_FC_QOS_SUBTYPE) ? WIFI_QOS_HEADER_BYTES : WIFI_MAC_HEADER_BYTES;
    uint8_t plain[WIFI_MSDU_MAX], ethernet[WIFI_MSDU_MAX];
    if (frame_bytes < header_bytes || (frame_control & WIFI_FC_DS_MASK) != WIFI_FC_FROM_DS ||
        (frame_control & (WIFI_FC_ORDER | WIFI_FC_MORE_FRAGMENTS)) ||
        !wifi_bytes_equal(frame + WIFI_ADDR2_OFFSET, station->ap.bssid, 6) ||
        (!(frame[WIFI_ADDR1_OFFSET] & 1) &&
         !wifi_bytes_equal(frame + WIFI_ADDR1_OFFSET, station->mac, 6)) ||
        (wifi_read_le16(frame + WIFI_SEQUENCE_OFFSET) & WIFI_SEQUENCE_FRAGMENT_MASK) ||
        (header_bytes == WIFI_QOS_HEADER_BYTES && (frame[WIFI_QOS_OFFSET] & WIFI_QOS_AMSDU))) {
        return -1;
    }
    const uint8_t *payload = frame + header_bytes;
    size_t bytes = frame_bytes - header_bytes;
    if (frame_control & WIFI_FC_PROTECTED) {
        int plaintext_bytes =
            decrypt_received_payload(station, frame, header_bytes, payload, bytes, plain);
        if (plaintext_bytes < 0) {
            return -1;
        }
        payload = plain;
        bytes = (size_t)plaintext_bytes;
    }
    if (bytes < 8 || !wifi_bytes_equal(payload, wifi_snap_header, 6)) {
        return -1;
    }
    if (wifi_read_be16(payload + WIFI_SNAP_BYTES) == WIFI_EAPOL_ETHERTYPE) {
        return wifi_receive_key_exchange(station, payload + 8, bytes - 8);
    }
    if (!wifi_station_link_up(station) || !(frame_control & WIFI_FC_PROTECTED) || bytes > 1510) {
        return -1;
    }
    memcpy(ethernet, frame + WIFI_ADDR1_OFFSET, 6);
    memcpy(ethernet + 6, frame + WIFI_ADDR3_OFFSET, 6);
    memcpy(ethernet + 12, payload + 6, bytes - 6);
    ++station->rx_frames;
    station->ops.ethernet_rx(station->ops.opaque, ethernet, bytes + 6);
    return 0;
}

int wifi_transmit_ethernet_locked(struct wifi_station *station, const uint8_t *ethernet,
                                  size_t frame_bytes)
{
    int result = -1;
    uint8_t frame[WIFI_FRAME_MAX], plain[1510], aad[WIFI_CCMP_AAD_MAX_BYTES],
        nonce[WIFI_CCMP_NONCE_BYTES];
    if (!wifi_station_link_up(station) || !wifi_bytes_equal(ethernet + 6, station->mac, 6) ||
        wifi_read_be16(ethernet + 12) < WIFI_ETHERNET_MIN_TYPE ||
        station->tx_pn >= WIFI_CCMP_MAX_PACKET_NUMBER) {
        return -1;
    }
    wifi_build_header(station, frame, (WIFI_FC_DATA | WIFI_FC_TO_DS | WIFI_FC_PROTECTED), ethernet);
    /* Consume the nonce before submission: a failed callback may have already
     * handed the frame to hardware, so retry must use a fresh packet number. */
    uint64_t packet_number = ++station->tx_pn;
    uint8_t *ccmp_header = frame + WIFI_MAC_HEADER_BYTES;
    ccmp_header[0] = (uint8_t)packet_number;
    ccmp_header[1] = (uint8_t)(packet_number >> 8);
    ccmp_header[2] = 0;
    ccmp_header[3] = WIFI_CCMP_EXTENDED_IV;
    for (unsigned index = 2; index < 6; ++index) {
        ccmp_header[index + 2] = (uint8_t)(packet_number >> (8 * index));
    }
    memcpy(plain, wifi_snap_header, 6);
    memcpy(plain + 6, ethernet + 12, frame_bytes - 12);
    size_t aad_bytes =
        wifi_ccmp_build_aad_nonce(frame, WIFI_MAC_HEADER_BYTES, packet_number, aad, nonce);
    int bytes = wifi_ccm_seal(station->ptk + WIFI_PTK_TK_OFFSET, nonce, aad, aad_bytes, plain,
                              frame_bytes - 6, frame + 32, sizeof(frame) - 32);
    if (bytes < 0) {
        return -1;
    }
    result = wifi_send_frame(station, frame, 32 + (size_t)bytes);
    if (!result) {
        ++station->tx_frames;
    }
    return result;
}

int wifi_station_transmit(struct wifi_station *station, const uint8_t *ethernet, size_t bytes)
{
    if (!ethernet || bytes < WIFI_ETHERNET_HEADER_BYTES || bytes > WIFI_ETHERNET_MAX_BYTES ||
        !wifi_station_try_lock(station)) {
        return -1;
    }
    int result = wifi_transmit_ethernet_locked(station, ethernet, bytes);
    wifi_station_unlock(station);
    return result;
}
