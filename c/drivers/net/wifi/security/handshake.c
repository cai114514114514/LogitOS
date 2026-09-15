#include "handshake.h"

/* EAPOL-Key fields are offsets within the EAPOL packet, after LLC/SNAP.
 * The caller has already checked the AP address. Only an authenticated M3
 * followed by successful M4 submission may open the controlled port. */

static int nonce_is_nonzero(const uint8_t nonce[WIFI_NONCE_BYTES])
{
    unsigned combined = 0;
    for (unsigned index = 0; index < WIFI_NONCE_BYTES; ++index) {
        combined |= nonce[index];
    }
    return combined != 0;
}

static int validate_key_envelope(const uint8_t *packet, size_t bytes)
{
    if (bytes < WIFI_KEY_FIXED_BYTES || bytes > WIFI_KEY_DATA_MAX_BYTES) {
        return -1;
    }
    if (packet[0] < 1 || packet[0] > WIFI_EAPOL_VERSION_RSN || packet[1] != WIFI_EAPOL_TYPE_KEY ||
        packet[4] != WIFI_KEY_DESCRIPTOR_RSN ||
        wifi_read_be16(packet + 2) != bytes - WIFI_EAPOL_HEADER_BYTES ||
        wifi_read_be16(packet + WIFI_KEY_LENGTH_OFFSET) != WIFI_CIPHER_KEY_BYTES ||
        wifi_read_be16(packet + WIFI_KEY_DATA_LENGTH_OFFSET) != bytes - WIFI_KEY_FIXED_BYTES) {
        return -1;
    }
    return 0;
}

int wifi_send_key_reply(struct wifi_station *station, uint16_t key_info, uint64_t replay,
                        const uint8_t *ptk)
{
    uint8_t frame[180] = {0};
    uint8_t digest[20];
    const size_t packet_offset = WIFI_MAC_HEADER_BYTES + WIFI_SNAP_BYTES + WIFI_ETHERTYPE_BYTES;
    uint8_t *packet = frame + packet_offset;
    size_t bytes = WIFI_KEY_FIXED_BYTES;
    if (key_info == WIFI_KEY_INFO_M2) {
        bytes += sizeof(wifi_rsn_information_element);
    }
    wifi_build_header(station, frame, WIFI_FC_DATA | WIFI_FC_TO_DS, station->ap.bssid);
    memcpy(frame + WIFI_MAC_HEADER_BYTES, wifi_snap_header, WIFI_SNAP_BYTES);
    wifi_write_be16(frame + WIFI_MAC_HEADER_BYTES + WIFI_SNAP_BYTES, WIFI_EAPOL_ETHERTYPE);
    packet[0] = WIFI_EAPOL_VERSION_RSN;
    packet[1] = WIFI_EAPOL_TYPE_KEY;
    wifi_write_be16(packet + 2, (uint16_t)(bytes - WIFI_EAPOL_HEADER_BYTES));
    packet[4] = WIFI_KEY_DESCRIPTOR_RSN;
    wifi_write_be16(packet + WIFI_KEY_INFO_OFFSET, key_info);
    /* RSN supplicant replies use zero Key Length (hostap wpa.c), unlike WPA1. */
    wifi_write_be16(packet + WIFI_KEY_LENGTH_OFFSET, 0);
    wifi_write_be64(packet + WIFI_KEY_REPLAY_OFFSET, replay);
    if (key_info == WIFI_KEY_INFO_M2) {
        memcpy(packet + WIFI_KEY_NONCE_OFFSET, station->snonce, WIFI_NONCE_BYTES);
        wifi_write_be16(packet + WIFI_KEY_DATA_LENGTH_OFFSET, sizeof(wifi_rsn_information_element));
        memcpy(packet + WIFI_KEY_FIXED_BYTES, wifi_rsn_information_element,
               sizeof(wifi_rsn_information_element));
    }
    wifi_hmac_sha1(ptk, WIFI_KEY_MIC_BYTES, packet, bytes, digest);
    memcpy(packet + WIFI_KEY_MIC_OFFSET, digest, WIFI_KEY_MIC_BYTES);
    wifi_erase_secret(digest, sizeof(digest));
    if (wifi_station_link_up(station)) {
        /* Rekey replies carry the new EAPOL MIC but retain the installed
         * traffic key until M4 has been submitted (hostap RSN ordering). */
        uint8_t ethernet[WIFI_ETHERNET_HEADER_BYTES + WIFI_KEY_DATA_MAX_BYTES];
        memcpy(ethernet, station->ap.bssid, WIFI_MAC_BYTES);
        memcpy(ethernet + WIFI_MAC_BYTES, station->mac, WIFI_MAC_BYTES);
        wifi_write_be16(ethernet + 2 * WIFI_MAC_BYTES, WIFI_EAPOL_ETHERTYPE);
        memcpy(ethernet + WIFI_ETHERNET_HEADER_BYTES, packet, bytes);
        return wifi_transmit_ethernet_locked(station, ethernet, WIFI_ETHERNET_HEADER_BYTES + bytes);
    }
    return wifi_send_frame(station, frame, packet_offset + bytes);
}

static int receive_message_one(struct wifi_station *station, const uint8_t *packet, uint64_t replay)
{
    const uint8_t *authenticator_nonce = packet + WIFI_KEY_NONCE_OFFSET;
    if (station->state == WIFI_WAIT_M3 || station->state == WIFI_REKEYING) {
        /* Reuse SNonce for the same M1 retransmission; generating a new one
         * here would invalidate the AP's already prepared M3. */
        if (replay < station->pending_replay ||
            !wifi_bytes_equal(authenticator_nonce, station->anonce, WIFI_NONCE_BYTES)) {
            return -1;
        }
        station->pending_replay = replay;
        return wifi_send_key_reply(station, WIFI_KEY_INFO_M2, replay, station->pending_ptk);
    }
    if ((wifi_station_link_up(station) && replay <= station->replay) ||
        !nonce_is_nonzero(authenticator_nonce) ||
        station->ops.random(station->ops.opaque, station->snonce, WIFI_NONCE_BYTES) ||
        !nonce_is_nonzero(station->snonce)) {
        return -1;
    }
    station->pending_replay = replay;
    memcpy(station->anonce, authenticator_nonce, WIFI_NONCE_BYTES);
    wifi_ptk(station->pmk, station->mac, station->ap.bssid, station->snonce, station->anonce,
             station->pending_ptk);
    station->state = wifi_station_link_up(station) ? WIFI_REKEYING : WIFI_WAIT_M3;
    if (wifi_station_set_deadline(station, WIFI_KEY_EXCHANGE_TIMEOUT_MS)) {
        return -1;
    }
    return wifi_send_key_reply(station, WIFI_KEY_INFO_M2, replay, station->pending_ptk);
}

int wifi_verify_key_mic(const uint8_t *ptk, const uint8_t *packet, size_t bytes)
{
    uint8_t authenticated_bytes[WIFI_KEY_DATA_MAX_BYTES];
    uint8_t digest[20];
    memcpy(authenticated_bytes, packet, bytes);
    memset(authenticated_bytes + WIFI_KEY_MIC_OFFSET, 0, WIFI_KEY_MIC_BYTES);
    wifi_hmac_sha1(ptk, WIFI_KEY_MIC_BYTES, authenticated_bytes, bytes, digest);
    int valid = wifi_bytes_equal(digest, packet + WIFI_KEY_MIC_OFFSET, WIFI_KEY_MIC_BYTES);
    wifi_erase_secret(authenticated_bytes, sizeof(authenticated_bytes));
    wifi_erase_secret(digest, sizeof(digest));
    return valid ? 0 : -1;
}

/* AP retries may use a newer EAPOL replay counter. Compare the authenticated
 * message body with counter/MIC excluded, rather than reinstalling its keys. */
static void message_three_fingerprint(const uint8_t *ptk, const uint8_t *packet, size_t bytes,
                                      uint8_t digest[20])
{
    uint8_t canonical[WIFI_KEY_DATA_MAX_BYTES];
    memcpy(canonical, packet, bytes);
    memset(canonical + WIFI_KEY_REPLAY_OFFSET, 0, 8);
    memset(canonical + WIFI_KEY_MIC_OFFSET, 0, WIFI_KEY_MIC_BYTES);
    wifi_hmac_sha1(ptk, WIFI_KEY_MIC_BYTES, canonical, bytes, digest);
    wifi_erase_secret(canonical, sizeof(canonical));
}

static int repeat_message_three(struct wifi_station *station, const uint8_t *packet, size_t bytes,
                                uint64_t replay)
{
    uint8_t fingerprint[20];
    message_three_fingerprint(station->ptk, packet, bytes, fingerprint);
    int same =
        wifi_bytes_equal(fingerprint, station->message_three_fingerprint, sizeof(fingerprint));
    wifi_erase_secret(fingerprint, sizeof(fingerprint));
    if (!same || replay < station->message_three_replay ||
        (replay != station->message_three_replay && replay <= station->replay)) {
        return -1;
    }
    int result = wifi_send_key_reply(station, WIFI_KEY_INFO_M4, replay, station->ptk);
    if (!result) {
        station->message_three_replay = replay;
        if (replay > station->replay) {
            station->replay = replay;
        }
    }
    return result;
}

static void install_session_keys(struct wifi_station *station, const uint8_t *packet,
                                 const struct received_group_key *group_key, uint64_t replay,
                                 size_t bytes)
{
    memcpy(station->ptk, station->pending_ptk, sizeof(station->ptk));
    wifi_erase_secret(station->pending_ptk, sizeof(station->pending_ptk));
    memcpy(station->m3_mic, packet + WIFI_KEY_MIC_OFFSET, sizeof(station->m3_mic));
    message_three_fingerprint(station->ptk, packet, bytes, station->message_three_fingerprint);
    station->message_three_replay = replay;
    station->replay = replay;
    station->tx_pn = 0;
    memset(station->rx_pn, 0, sizeof(station->rx_pn));
    if (group_key->present) {
        wifi_install_group_key(station, packet, group_key);
    }
    if (!wifi_station_link_up(station)) {
        station->last_beacon_ms = station->ops.now_ms(station->ops.opaque);
    }
    /* Retain the PMK for rekey. Failure/disconnect erases it with traffic keys. */
    station->state = WIFI_CONNECTED;
    __atomic_store_n(&station->link_active, 1, __ATOMIC_RELEASE);
}

static int receive_message_three(struct wifi_station *station, const uint8_t *packet, size_t bytes,
                                 uint64_t replay)
{
    int pending = station->state == WIFI_WAIT_M3 || station->state == WIFI_REKEYING;
    const uint8_t *ptk = pending ? station->pending_ptk : station->ptk;
    if (!wifi_bytes_equal(packet + WIFI_KEY_NONCE_OFFSET, station->anonce, WIFI_NONCE_BYTES) ||
        wifi_verify_key_mic(ptk, packet, bytes)) {
        return -1;
    }
    if (!pending) {
        return repeat_message_three(station, packet, bytes, replay);
    }
    if (replay <= station->pending_replay || replay <= station->replay) {
        return -1;
    }
    uint8_t key_data[WIFI_KEY_DATA_MAX_BYTES];
    struct received_group_key group_key = {0};
    int result = -1;
    int key_bytes = wifi_key_unwrap(ptk + WIFI_PTK_KEK_OFFSET, packet + WIFI_KEY_FIXED_BYTES,
                                    bytes - WIFI_KEY_FIXED_BYTES, key_data, sizeof(key_data));
    if (key_bytes >= 0 &&
        !wifi_parse_key_data(key_data, (size_t)key_bytes, &group_key, &station->ap) &&
        (group_key.present || wifi_station_link_up(station))) {
        /* Even a new replay counter cannot authorize reinstallation of the
         * same PTK. Reusing its key with reset packet numbers is unsafe. */
        if (wifi_station_link_up(station) && wifi_bytes_equal(ptk, station->ptk, 48)) {
            result = -1;
        } else if (!wifi_send_key_reply(station, WIFI_KEY_INFO_M4, replay, ptk)) {
            install_session_keys(station, packet, &group_key, replay, bytes);
            result = 0;
        }
    }
    wifi_erase_secret(key_data, sizeof(key_data));
    wifi_erase_secret(&group_key, sizeof(group_key));
    return result;
}

int wifi_receive_key_exchange(struct wifi_station *station, const uint8_t *packet, size_t bytes)
{
    if (validate_key_envelope(packet, bytes)) {
        return -1;
    }
    uint16_t key_info = wifi_read_be16(packet + WIFI_KEY_INFO_OFFSET);
    uint64_t replay = wifi_read_be64(packet + WIFI_KEY_REPLAY_OFFSET);
    if (key_info == WIFI_KEY_INFO_M1 &&
        (station->state == WIFI_KEY_EXCHANGE || station->state == WIFI_WAIT_M3 ||
         station->state == WIFI_CONNECTED || station->state == WIFI_REKEYING)) {
        return receive_message_one(station, packet, replay);
    }
    if (key_info == WIFI_KEY_INFO_M3 &&
        (station->state == WIFI_WAIT_M3 || station->state == WIFI_CONNECTED ||
         station->state == WIFI_REKEYING)) {
        return receive_message_three(station, packet, bytes, replay);
    }
    if (key_info == WIFI_KEY_INFO_GROUP_ONE && wifi_station_link_up(station)) {
        return wifi_receive_group_rekey(station, packet, bytes, replay);
    }
    return -1;
}
