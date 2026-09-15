#include "../internal.h"

static int receive_authentication_response(struct wifi_station *station, const uint8_t *frame)
{
    if (wifi_read_le16(frame + WIFI_MGMT_BODY_OFFSET) != WIFI_AUTH_OPEN_SYSTEM ||
        wifi_read_le16(frame + WIFI_MGMT_SECOND_FIELD_OFFSET) != WIFI_AUTH_RESPONSE_SEQUENCE ||
        wifi_read_le16(frame + WIFI_MGMT_THIRD_FIELD_OFFSET) != WIFI_STATUS_SUCCESS) {
        wifi_station_fail(station);
        return -1;
    }
    int result = wifi_send_association_request(station);
    if (result) {
        wifi_station_fail(station);
    }
    return result;
}

static int receive_association_response(struct wifi_station *station, const uint8_t *frame)
{
    station->aid = wifi_read_le16(frame + WIFI_MGMT_THIRD_FIELD_OFFSET) & WIFI_ASSOC_AID_MASK;
    if (wifi_read_le16(frame + WIFI_MGMT_SECOND_FIELD_OFFSET) != WIFI_STATUS_SUCCESS ||
        !station->aid || station->aid > WIFI_ASSOC_AID_MAX) {
        wifi_station_fail(station);
        return -1;
    }
    /* Association establishes a MAC relationship only. The controlled port
     * stays closed until the four-way handshake authenticates the peer. */
    station->state = WIFI_KEY_EXCHANGE;
    return wifi_station_set_deadline(station, WIFI_KEY_EXCHANGE_TIMEOUT_MS);
}

int wifi_station_receive(struct wifi_station *station, const uint8_t *frame, size_t frame_bytes,
                         int16_t signal_dbm)
{
    int result = -1;
    if (!frame || frame_bytes < WIFI_MAC_HEADER_BYTES || frame_bytes > WIFI_FRAME_MAX ||
        !wifi_station_try_lock(station)) {
        return -1;
    }
    uint16_t frame_control = wifi_read_le16(frame);
    if ((frame_control & WIFI_FC_VERSION_MASK) || (frame_control & WIFI_FC_MORE_FRAGMENTS) ||
        (wifi_read_le16(frame + WIFI_SEQUENCE_OFFSET) & WIFI_SEQUENCE_FRAGMENT_MASK)) {
        goto done;
    }
    if (wifi_station_link_up(station) && frame_bytes >= WIFI_BEACON_FIXED_BYTES &&
        (frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_BEACON &&
        !(frame_control & WIFI_FC_DS_MASK) &&
        wifi_bytes_equal(frame + WIFI_ADDR2_OFFSET, station->ap.bssid, WIFI_MAC_BYTES) &&
        wifi_bytes_equal(frame + WIFI_ADDR3_OFFSET, station->ap.bssid, WIFI_MAC_BYTES)) {
        station->last_beacon_ms = station->ops.now_ms(station->ops.opaque);
        result = 0;
        goto done;
    }
    if (station->state == WIFI_SCANNING &&
        ((frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_BEACON ||
         (frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_PROBE_RESPONSE)) {
        result = wifi_scan_receive_beacon(station, frame, frame_bytes, signal_dbm);
        goto done;
    }
    if ((frame_control & WIFI_FC_TYPE_MASK) == WIFI_FC_DATA) {
        result = wifi_receive_data(station, frame, frame_bytes);
        goto done;
    }
    if ((frame_control & (WIFI_FC_DS_MASK | WIFI_FC_TYPE_MASK)) ||
        !wifi_bytes_equal(frame + WIFI_ADDR1_OFFSET, station->mac, 6) ||
        !wifi_bytes_equal(frame + WIFI_ADDR2_OFFSET, station->ap.bssid, 6) ||
        !wifi_bytes_equal(frame + WIFI_ADDR3_OFFSET, station->ap.bssid, 6)) {
        goto done;
    }
    if ((frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_DEAUTHENTICATION ||
        (frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_DISASSOCIATION) {
        wifi_station_fail(station);
        goto done;
    }
    if (station->state == WIFI_AUTHENTICATING &&
        (frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_AUTHENTICATION &&
        frame_bytes >= WIFI_MGMT_RESPONSE_BYTES) {
        result = receive_authentication_response(station, frame);
        goto done;
    }
    if (station->state == WIFI_ASSOCIATING &&
        (frame_control & WIFI_FC_SUBTYPE_MASK) == WIFI_FC_ASSOCIATION_RESPONSE &&
        frame_bytes >= WIFI_MGMT_RESPONSE_BYTES) {
        result = receive_association_response(station, frame);
    }
done:
    wifi_station_unlock(station);
    return result;
}
