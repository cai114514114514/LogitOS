#include "internal.h"

/* Failure closes the controlled port and erases session secrets together.
 * No radio or management error is allowed to retain usable traffic keys. */
void wifi_station_fail(struct wifi_station *station)
{
    __atomic_store_n(&station->link_active, 0, __ATOMIC_RELEASE);
    station->state = WIFI_FAILED;
    station->disconnect_reason = WIFI_DISCONNECT_PROTOCOL;
    wifi_erase_secret(station->pmk, 32);
    wifi_erase_secret(station->ptk, 48);
    wifi_erase_secret(station->pending_ptk, sizeof(station->pending_ptk));
    wifi_erase_secret(station->group_keys, sizeof(station->group_keys));
    station->group_reply_valid = 0;
    wifi_erase_secret(station->snonce, 32);
    wifi_erase_secret(station->anonce, 32);
}

int wifi_station_set_deadline(struct wifi_station *station, unsigned delay)
{
    uint64_t now = station->ops.now_ms(station->ops.opaque);
    if (now < station->last_time || now > UINT64_MAX - delay) {
        wifi_station_fail(station);
        return -1;
    }
    station->last_time = now;
    station->deadline = now + delay;
    return 0;
}

void wifi_build_header(struct wifi_station *station, uint8_t *frame, uint16_t frame_control,
                       const uint8_t *dest)
{
    memset(frame, 0, WIFI_MAC_HEADER_BYTES);
    wifi_write_le16(frame, frame_control);
    memcpy(frame + WIFI_ADDR1_OFFSET, station->ap.bssid, 6);
    memcpy(frame + WIFI_ADDR2_OFFSET, station->mac, 6);
    memcpy(frame + WIFI_ADDR3_OFFSET, dest, 6);
    wifi_write_le16(frame + WIFI_SEQUENCE_OFFSET,
                    (uint16_t)((station->sequence++ & WIFI_SEQUENCE_NUMBER_MASK)
                               << WIFI_SEQUENCE_NUMBER_SHIFT));
}

int wifi_send_frame(struct wifi_station *station, uint8_t *frame, size_t frame_bytes)
{
    return station->ops.transmit(station->ops.opaque, frame, frame_bytes) ? -1 : 0;
}

int wifi_station_init(struct wifi_station *station, const struct wifi_station_ops *ops,
                      const uint8_t mac[6])
{
    uint8_t nonzero = 0;
    if (!station || !ops || !mac || !ops->set_channel || !ops->transmit || !ops->random ||
        !ops->now_ms || !ops->ethernet_rx || (mac[0] & 1)) {
        return -1;
    }
    for (unsigned index = 0; index < 6; ++index) {
        nonzero |= mac[index];
    }
    if (!nonzero) {
        return -1;
    }
    memset(station, 0, sizeof(*station));
    station->ops = *ops;
    memcpy(station->mac, mac, 6);
    station->last_time = ops->now_ms(ops->opaque);
    return 0;
}

int wifi_station_tick(struct wifi_station *station)
{
    int result = 0;
    if (!wifi_station_try_lock(station)) {
        return -1;
    }
    uint64_t now = station->ops.now_ms(station->ops.opaque);
    if (now < station->last_time) {
        wifi_station_fail(station);
        result = -1;
        goto done;
    }
    station->last_time = now;
    if (wifi_station_link_up(station) && now - station->last_beacon_ms >= WIFI_BEACON_LOSS_MS) {
        wifi_station_fail(station);
        station->disconnect_reason = WIFI_DISCONNECT_BEACON_LOSS;
        result = -1;
        goto done;
    }
    if (station->state == WIFI_IDLE || station->state == WIFI_CONNECTED ||
        station->state == WIFI_FAILED || now < station->deadline) {
        goto done;
    }
    if (station->state == WIFI_SCANNING) {
        if (++station->channel_index == station->channel_count) {
            station->state = WIFI_IDLE;
            goto done;
        }
        if (station->ops.set_channel(station->ops.opaque,
                                     station->channels[station->channel_index]) ||
            wifi_station_set_deadline(station, WIFI_SCAN_DWELL_MS)) {
            wifi_station_fail(station);
            result = -1;
        }
    } else {
        wifi_station_fail(station);
        station->disconnect_reason = WIFI_DISCONNECT_HANDSHAKE_TIMEOUT;
        result = -1;
    }
done:
    wifi_station_unlock(station);
    return result;
}

void wifi_station_disconnect(struct wifi_station *station)
{
    if (!wifi_station_try_lock(station)) {
        return;
    }
    wifi_station_fail(station);
    station->state = WIFI_IDLE;
    station->disconnect_reason = WIFI_DISCONNECT_REQUESTED;
    wifi_station_unlock(station);
}

int wifi_station_link_up(const struct wifi_station *station)
{
    return station && __atomic_load_n(&station->link_active, __ATOMIC_ACQUIRE);
}

void wifi_station_transport_lost(struct wifi_station *station)
{
    if (!wifi_station_try_lock(station)) {
        return;
    }
    wifi_station_fail(station);
    station->disconnect_reason = WIFI_DISCONNECT_TRANSPORT;
    wifi_station_unlock(station);
}
