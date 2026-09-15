#include "../internal.h"

int wifi_rsn_supported(const uint8_t *payload, size_t frame_bytes)
{
    if (frame_bytes < 18 || wifi_read_le16(payload) != 1 ||
        !wifi_bytes_equal(payload + 2, wifi_rsn_information_element + 4, 4)) {
        return 0;
    }
    size_t offset = 6;
    unsigned count = wifi_read_le16(payload + offset);
    offset += 2;
    int has_ccmp_cipher = 0, has_psk_authentication = 0;
    if (!count || count > (frame_bytes - offset) / 4) {
        return 0;
    }
    for (unsigned index = 0; index < count; ++index) {
        if (wifi_bytes_equal(payload + offset, wifi_rsn_information_element + 10, 4)) {
            has_ccmp_cipher = 1;
        }
        offset += 4;
    }
    if (frame_bytes - offset < 2) {
        return 0;
    }
    count = wifi_read_le16(payload + offset);
    offset += 2;
    if (!count || count > (frame_bytes - offset) / 4) {
        return 0;
    }
    for (unsigned index = 0; index < count; ++index) {
        if (wifi_bytes_equal(payload + offset, wifi_rsn_information_element + 16, 4)) {
            has_psk_authentication = 1;
        }
        offset += 4;
    }
    /* PMF-required needs BIP/IGTK and robust-management support, not a flag. */
    if (frame_bytes - offset >= 2 && (wifi_read_le16(payload + offset) & WIFI_PMF_REQUIRED)) {
        return 0;
    }
    return has_ccmp_cipher && has_psk_authentication;
}
