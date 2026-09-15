#include "../internal.h"

int wifi_station_scan(struct wifi_station *station, const uint8_t *channels, size_t channel_count)
{
    int result = -1;
    if (!channels || !channel_count || channel_count > 64 || !wifi_station_try_lock(station)) {
        return -1;
    }
    if (station->state != WIFI_IDLE && station->state != WIFI_FAILED) {
        goto done;
    }
    for (size_t index = 0; index < channel_count; ++index) {
        if (!channels[index]) {
            goto done;
        }
    }
    station->bss_count = 0;
    station->channel_count = (uint8_t)channel_count;
    station->channel_index = 0;
    memcpy(station->channels, channels, channel_count);
    if (station->ops.set_channel(station->ops.opaque, channels[0]) ||
        wifi_station_set_deadline(station, WIFI_SCAN_DWELL_MS)) {
        wifi_station_fail(station);
        goto done;
    }
    station->state = WIFI_SCANNING;
    result = 0;
done:
    wifi_station_unlock(station);
    return result;
}

static int parse_beacon_element(struct wifi_bss *candidate, unsigned type, const uint8_t *payload,
                                size_t element_bytes)
{
    if (type == WIFI_IE_SSID) {
        if (element_bytes > 32 || candidate->ssid_bytes) {
            return -1;
        }
        memcpy(candidate->ssid, payload, element_bytes);
        candidate->ssid_bytes = (uint8_t)element_bytes;
    }
    if (type == WIFI_IE_RATES) {
        if (!element_bytes || element_bytes > 8 || candidate->rate_count) {
            return -1;
        }
        memcpy(candidate->rates, payload, element_bytes);
        candidate->rate_count = (uint8_t)element_bytes;
    }
    if (type == WIFI_IE_CHANNEL && (element_bytes != 1 || payload[0] != candidate->channel)) {
        return -1;
    }
    if (type == WIFI_IE_RSN) {
        if (element_bytes > 64 || candidate->rsn_bytes ||
            !wifi_rsn_supported(payload, element_bytes)) {
            return -1;
        }
        memcpy(candidate->rsn, payload, element_bytes);
        candidate->rsn_bytes = (uint8_t)element_bytes;
    }
    return 0;
}

int wifi_scan_receive_beacon(struct wifi_station *station, const uint8_t *frame, size_t frame_bytes,
                             int16_t signal_dbm)
{
    struct wifi_bss candidate = {0};
    if (frame_bytes < WIFI_BEACON_FIXED_BYTES || (frame[10] & 1) ||
        !wifi_bytes_equal(frame + WIFI_ADDR2_OFFSET, frame + WIFI_ADDR3_OFFSET, 6)) {
        return -1;
    }
    memcpy(candidate.bssid, frame + WIFI_ADDR3_OFFSET, 6);
    candidate.capability = wifi_read_le16(frame + WIFI_BEACON_CAPABILITY_OFFSET);
    candidate.signal_dbm = signal_dbm;
    candidate.channel = station->channels[station->channel_index];
    for (size_t offset = WIFI_BEACON_FIXED_BYTES; offset < frame_bytes;) {
        if (frame_bytes - offset < 2 || frame[offset + 1] > frame_bytes - offset - 2) {
            return -1;
        }
        unsigned type = frame[offset], element_bytes = frame[offset + 1];
        const uint8_t *payload = frame + offset + 2;
        if (parse_beacon_element(&candidate, type, payload, element_bytes)) {
            return -1;
        }
        offset += 2 + element_bytes;
    }
    if (!candidate.ssid_bytes || !candidate.rate_count || !candidate.rsn_bytes ||
        (candidate.capability & WIFI_BSS_ESS_PRIVACY_MASK) != WIFI_BSS_ESS_PRIVACY) {
        return -1;
    }
    unsigned index;
    for (index = 0; index < station->bss_count; ++index) {
        if (wifi_bytes_equal(station->bss[index].bssid, candidate.bssid, 6)) {
            break;
        }
    }
    if (index == WIFI_SCAN_MAX) {
        return -1;
    }
    station->bss[index] = candidate;
    if (index == station->bss_count) {
        ++station->bss_count;
    }
    return 0;
}
