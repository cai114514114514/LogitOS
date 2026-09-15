#include "handshake.h"

int wifi_parse_key_data(const uint8_t *key_data, size_t bytes, struct received_group_key *group_key,
                        const struct wifi_bss *expected_bss)
{
    const uint8_t gtk_selector[] = {0, 15, 172, 1};
    int found_group_key = 0;
    int found_rsn = 0;
    for (size_t offset = 0; offset < bytes;) {
        /* RFC3394 key data is padded with vendor IE 0xdd followed by zeros. */
        if (key_data[offset] == WIFI_IE_VENDOR &&
            (bytes - offset == 1 || key_data[offset + 1] == 0)) {
            break;
        }
        if (bytes - offset < 2 || key_data[offset + 1] > bytes - offset - 2) {
            return -1;
        }
        size_t element_bytes = key_data[offset + 1];
        const uint8_t *payload = key_data + offset + 2;
        if (key_data[offset] == WIFI_IE_RSN) {
            if (found_rsn || !wifi_rsn_supported(payload, element_bytes)) {
                return -1;
            }
            if (expected_bss && (element_bytes != expected_bss->rsn_bytes ||
                                 !wifi_bytes_equal(payload, expected_bss->rsn, element_bytes))) {
                return -1;
            }
            found_rsn = 1;
        }
        if (key_data[offset] == WIFI_IE_VENDOR && element_bytes >= sizeof(gtk_selector) &&
            wifi_bytes_equal(payload, gtk_selector, sizeof(gtk_selector))) {
            if (found_group_key || element_bytes != WIFI_GTK_KDE_BYTES || payload[5] ||
                (payload[4] & ~WIFI_GTK_KDE_FLAGS_MASK)) {
                return -1;
            }
            group_key->id = payload[4] & WIFI_GTK_KEY_ID_MASK;
            memcpy(group_key->key, payload + WIFI_GTK_KDE_KEY_OFFSET, sizeof(group_key->key));
            found_group_key = 1;
            group_key->present = 1;
        }
        offset += 2 + element_bytes;
    }
    return (expected_bss ? found_rsn : found_group_key) ? 0 : -1;
}
