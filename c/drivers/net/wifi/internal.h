#ifndef LOGIT_WIFI_INTERNAL_H
#define LOGIT_WIFI_INTERNAL_H
#include "protocol.h"
#include "security/crypto.h"
#include "station.h"
#include <string.h>
/* Internal entry points require the caller to hold station->lock. Transport
 * callbacks borrow frame storage only for the duration of the callback. */
static inline uint16_t wifi_read_le16(const uint8_t *payload)
{
    return payload[0] | (uint16_t)payload[1] << 8;
}

static inline uint16_t wifi_read_be16(const uint8_t *payload)
{
    return (uint16_t)payload[0] << 8 | payload[1];
}

static inline void wifi_write_le16(uint8_t *payload, uint16_t value)
{
    payload[0] = (uint8_t)value;
    payload[1] = (uint8_t)(value >> 8);
}

static inline void wifi_write_be16(uint8_t *payload, uint16_t value)
{
    payload[0] = (uint8_t)(value >> 8);
    payload[1] = (uint8_t)value;
}

static inline uint64_t wifi_read_be64(const uint8_t *payload)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value = value << 8 | payload[index];
    }
    return value;
}

static inline void wifi_write_be64(uint8_t *payload, uint64_t value)
{
    for (unsigned index = 0; index < 8; ++index) {
        payload[7 - index] = (uint8_t)(value >> (8 * index));
    }
}

static inline void wifi_erase_secret(void *payload, size_t frame_bytes)
{
    volatile uint8_t *cursor = payload;
    while (frame_bytes--) {
        *cursor++ = 0;
    }
}

static inline int wifi_bytes_equal(const uint8_t *left, const uint8_t *right, size_t frame_bytes)
{
    unsigned difference = 0;
    for (size_t index = 0; index < frame_bytes; ++index) {
        difference |= left[index] ^ right[index];
    }
    return !difference;
}

static inline int wifi_station_try_lock(struct wifi_station *station)
{
    return station && !__atomic_exchange_n(&station->lock, 1, __ATOMIC_ACQUIRE);
}

static inline void wifi_station_unlock(struct wifi_station *station)
{
    __atomic_store_n(&station->lock, 0, __ATOMIC_RELEASE);
}

int wifi_rsn_supported(const uint8_t *payload, size_t frame_bytes);
void wifi_build_header(struct wifi_station *station, uint8_t *frame, uint16_t frame_control,
                       const uint8_t *destination);
int wifi_send_frame(struct wifi_station *station, uint8_t *frame, size_t frame_bytes);
int wifi_scan_receive_beacon(struct wifi_station *station, const uint8_t *frame, size_t frame_bytes,
                             int16_t signal_dbm);
int wifi_send_association_request(struct wifi_station *station);
int wifi_receive_key_exchange(struct wifi_station *station, const uint8_t *key_frame,
                              size_t frame_bytes);
int wifi_receive_data(struct wifi_station *station, const uint8_t *frame, size_t frame_bytes);
void wifi_station_fail(struct wifi_station *station);
int wifi_station_set_deadline(struct wifi_station *station, unsigned delay);
int wifi_transmit_ethernet_locked(struct wifi_station *station, const uint8_t *ethernet,
                                  size_t bytes);
#endif
