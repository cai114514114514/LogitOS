#include "../internal.h"

int wifi_station_join(struct wifi_station *station, unsigned index, const uint8_t *password,
                      size_t password_bytes)
{
    uint8_t frame[32];
    int result = -1;
    if (!wifi_station_try_lock(station)) {
        return -1;
    }
    if ((station->state != WIFI_IDLE && station->state != WIFI_SCANNING) ||
        index >= station->bss_count) {
        goto done;
    }
    /* Explicit join starts a new security association; replay counters from a
     * previous AP must neither authenticate this session nor block its M1. */
    wifi_station_fail(station);
    station->replay = 0;
    station->pending_replay = 0;
    station->message_three_replay = 0;
    station->disconnect_reason = WIFI_DISCONNECT_NONE;
    station->ap = station->bss[index];
    if (wifi_psk(password, password_bytes, station->ap.ssid, station->ap.ssid_bytes,
                 station->pmk)) {
        goto done;
    }
    if (station->ops.set_channel(station->ops.opaque, station->ap.channel) ||
        wifi_station_set_deadline(station, WIFI_ASSOCIATION_TIMEOUT_MS)) {
        wifi_station_fail(station);
        goto done;
    }
    wifi_build_header(station, frame, WIFI_FC_AUTHENTICATION, station->ap.bssid);
    wifi_write_le16(frame + WIFI_MGMT_BODY_OFFSET, WIFI_AUTH_OPEN_SYSTEM);
    wifi_write_le16(frame + WIFI_MGMT_SECOND_FIELD_OFFSET, WIFI_AUTH_REQUEST_SEQUENCE);
    wifi_write_le16(frame + WIFI_MGMT_THIRD_FIELD_OFFSET, WIFI_STATUS_SUCCESS);
    station->state = WIFI_AUTHENTICATING;
    if (wifi_send_frame(station, frame, WIFI_MGMT_RESPONSE_BYTES)) {
        wifi_station_fail(station);
        goto done;
    }
    result = 0;
done:
    wifi_station_unlock(station);
    return result;
}

int wifi_send_association_request(struct wifi_station *station)
{
    uint8_t frame[128];
    wifi_build_header(station, frame, WIFI_FC_ASSOCIATION_REQUEST, station->ap.bssid);
    wifi_write_le16(frame + WIFI_MGMT_BODY_OFFSET,
                    station->ap.capability & WIFI_ASSOC_CAPABILITY_MASK);
    wifi_write_le16(frame + WIFI_MGMT_SECOND_FIELD_OFFSET, WIFI_ASSOC_LISTEN_INTERVAL);
    size_t frame_bytes = 28;
    frame[frame_bytes++] = WIFI_IE_SSID;
    frame[frame_bytes++] = station->ap.ssid_bytes;
    memcpy(frame + frame_bytes, station->ap.ssid, station->ap.ssid_bytes);
    frame_bytes += station->ap.ssid_bytes;
    frame[frame_bytes++] = WIFI_IE_RATES;
    frame[frame_bytes++] = station->ap.rate_count;
    memcpy(frame + frame_bytes, station->ap.rates, station->ap.rate_count);
    frame_bytes += station->ap.rate_count;
    memcpy(frame + frame_bytes, wifi_rsn_information_element, sizeof(wifi_rsn_information_element));
    frame_bytes += sizeof(wifi_rsn_information_element);
    station->state = WIFI_ASSOCIATING;
    return wifi_station_set_deadline(station, WIFI_ASSOCIATION_TIMEOUT_MS) ||
                   wifi_send_frame(station, frame, frame_bytes)
               ? -1
               : 0;
}
