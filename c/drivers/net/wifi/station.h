#ifndef LOGIT_WIFI_STATION_H
#define LOGIT_WIFI_STATION_H
#include <stddef.h>
#include <stdint.h>
#define WIFI_SCAN_MAX 16u
#define WIFI_FRAME_MAX 2400u
enum wifi_state {
    WIFI_IDLE,
    WIFI_SCANNING,
    WIFI_AUTHENTICATING,
    WIFI_ASSOCIATING,
    WIFI_KEY_EXCHANGE,
    WIFI_WAIT_M3,
    WIFI_CONNECTED,
    WIFI_REKEYING,
    WIFI_FAILED
};
struct wifi_bss {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_bytes;
    uint8_t channel;
    uint8_t rates[8];
    uint8_t rate_count;
    uint8_t rsn[64];
    uint8_t rsn_bytes;
    uint16_t capability;
    int16_t signal_dbm;
};
struct wifi_station_ops {
    void *opaque;
    int (*set_channel)(void *opaque, uint8_t channel);
    int (*transmit)(void *opaque, const uint8_t *frame, size_t bytes);
    int (*random)(void *opaque, uint8_t *output,
                  size_t bytes); /* CSPRNG, not time/sequence based. */
    uint64_t (*now_ms)(void *opaque);
    void (*ethernet_rx)(void *opaque, const uint8_t *frame, size_t bytes);
};
enum wifi_disconnect_reason {
    WIFI_DISCONNECT_NONE,
    WIFI_DISCONNECT_REQUESTED,
    WIFI_DISCONNECT_PROTOCOL,
    WIFI_DISCONNECT_HANDSHAKE_TIMEOUT,
    WIFI_DISCONNECT_BEACON_LOSS,
    WIFI_DISCONNECT_TRANSPORT,
};
struct wifi_group_key_slot {
    uint8_t key[16];
    uint64_t replay[17];
    unsigned installed;
};
struct wifi_station {
    unsigned lock;
    enum wifi_state state;
    enum wifi_disconnect_reason disconnect_reason;
    unsigned link_active; /* Release-published; query through wifi_station_link_up(). */
    uint64_t last_beacon_ms;
    uint64_t pending_replay;
    uint64_t message_three_replay;
    uint64_t group_replay;
    uint8_t group_message_mic[16];
    unsigned group_reply_valid;
    uint8_t pending_ptk[48];
    struct wifi_group_key_slot group_keys[4];
    struct wifi_station_ops ops;
    uint8_t mac[6];
    uint8_t channels[64];
    uint8_t channel_count;
    uint8_t channel_index;
    uint8_t bss_count;
    struct wifi_bss bss[WIFI_SCAN_MAX];
    struct wifi_bss ap;
    uint64_t deadline;
    uint64_t last_time;
    uint64_t replay;
    uint64_t tx_pn;
    /* Independent replay windows: QoS priorities 0..15, then non-QoS. */
    uint64_t rx_pn[17];
    /* WPA2 vocabulary: PMK = master key; PTK = MIC key | wrapping key | traffic key. */
    uint8_t pmk[32];
    uint8_t ptk[48];
    /* SNonce is the client nonce; ANonce comes from the access point. */
    uint8_t snonce[32];
    uint8_t anonce[32];
    uint8_t m3_mic[16];
    uint8_t message_three_fingerprint[20];
    uint16_t sequence;
    uint16_t aid;
    uint64_t tx_frames;
    uint64_t rx_frames;
};
/* A single physical station, infrastructure WPA2-PSK/CCMP-128. No AP mode,
 * monitor/injection API, TKIP, WEP, SAE, FT or PMF downgrade. The transport
 * supplies complete MPDUs with CRC/FCS already checked and FCS removed.
 * Host crypto handles CCMP: hardware must preserve IV/MIC and not decrypt.
 * Channels are supplied by the radio's regulatory policy, never guessed here.
 * Initialize only an unowned instance; reinitialization requires a stopped transport.
 * Nonblocking callbacks may not reenter this instance. Caller serializes the
 * transport and keeps buffers/ops alive. No CONNECTED without validated M3,
 * installed PTK/GTK and successful M4 transmit. Ethernet delivery is borrowed.
 */
int wifi_station_init(struct wifi_station *station, const struct wifi_station_ops *ops,
                      const uint8_t mac[6]);
int wifi_station_scan(struct wifi_station *station, const uint8_t *channels, size_t count);
int wifi_station_join(struct wifi_station *station, unsigned bss, const uint8_t *password,
                      size_t bytes);
int wifi_station_tick(struct wifi_station *station);
int wifi_station_receive(struct wifi_station *station, const uint8_t *mpdu, size_t bytes,
                         int16_t dbm);
int wifi_station_transmit(struct wifi_station *station, const uint8_t *ethernet, size_t bytes);
int wifi_station_link_up(const struct wifi_station *station);
void wifi_station_transport_lost(struct wifi_station *station);
void wifi_station_disconnect(struct wifi_station *station);
#endif
