/* Reuse only the existing synthetic PCI/backend fixture. Its included netdev.c
 * is production code; the Wi-Fi station, crypto and adapter are linked intact. */
#define main unused_netif_fixture_main
#include "../../unit/netif_test.c"
#undef main
#undef CHECK
#include "adapter/netdev.h"
#include "oracle.h"

static unsigned wifi_checks, wifi_failures;
#define VERIFY(condition)                                                                          \
    do {                                                                                           \
        ++wifi_checks;                                                                             \
        if (!(condition)) {                                                                        \
            ++wifi_failures;                                                                       \
            printf("WIFI_FAIL %d: %s\n", __LINE__, #condition);                                    \
        }                                                                                          \
    } while (0)

static struct {
    uint64_t now;
    const uint8_t *next_frame;
    size_t next_bytes;
    uint8_t last_transmit[2400];
    size_t transmit_bytes;
    unsigned transmitted;
    unsigned callbacks;
    unsigned delivered;
    int lost;
    int reenter_detach;
    int reentrant_result;
} radio;

static int radio_channel(void *opaque, uint8_t channel)
{
    (void)opaque;
    ++radio.callbacks;
    return channel ? 0 : -1;
}
static int radio_transmit(void *opaque, const uint8_t *frame, size_t bytes)
{
    (void)opaque;
    ++radio.callbacks;
    if (bytes > sizeof(radio.last_transmit)) {
        return -1;
    }
    memcpy(radio.last_transmit, frame, bytes);
    radio.transmit_bytes = bytes;
    ++radio.transmitted;
    return 0;
}
static int radio_receive(void *opaque, uint8_t *frame, size_t capacity, size_t *bytes,
                         int16_t *signal)
{
    (void)opaque;
    ++radio.callbacks;
    if (radio.reenter_detach) {
        radio.reentrant_result = wifi_adapter_detach();
        radio.reenter_detach = 0;
    }
    if (radio.lost) {
        return -1;
    }
    if (!radio.next_frame) {
        return 0;
    }
    if (capacity < radio.next_bytes) {
        return -1;
    }
    memcpy(frame, radio.next_frame, radio.next_bytes);
    *bytes = radio.next_bytes;
    *signal = -40;
    radio.next_frame = NULL;
    return 1;
}
static int radio_random(void *opaque, uint8_t *output, size_t bytes)
{
    (void)opaque;
    ++radio.callbacks;
    for (size_t index = 0; index < bytes; ++index) {
        output[index] = (uint8_t)(index + 1);
    }
    return 0;
}
static uint64_t radio_time(void *opaque)
{
    (void)opaque;
    ++radio.callbacks;
    return radio.now;
}
static void input_frame(const uint8_t *frame, uint16_t bytes)
{
    ++radio.delivered;
    VERIFY(bytes == sizeof(oracle_ethernet) && !memcmp(frame, oracle_ethernet, bytes));
    uint8_t response[sizeof(oracle_ethernet)];
    memcpy(response, frame, bytes);
    memcpy(response + 6, netdev_mac(), 6);
    VERIFY(netdev_tx(response, bytes) == 0);
}
static void inject(const uint8_t *frame, size_t bytes)
{
    radio.next_frame = frame;
    radio.next_bytes = bytes;
    VERIFY(wifi_adapter_poll() == 0);
}
static void connect_adapter(int index)
{
    VERIFY(wifi_adapter_scan((uint8_t[]){1, 6}, 2) == 0);
    inject(oracle_beacon, sizeof(oracle_beacon));
    struct wifi_bss candidates[16];
    VERIFY(wifi_adapter_scan_results(candidates, 16) == 1);
    VERIFY(!memcmp(candidates[0].ssid, "IEEE", 4));
    VERIFY(wifi_adapter_join(0, (const uint8_t *)"password", 8) == 0);
    inject(oracle_auth, sizeof(oracle_auth));
    inject(oracle_assoc, sizeof(oracle_assoc));
    VERIFY(!(netif_by_index(index)->flags & NETIF_F_RUNNING));
    inject(oracle_m1, sizeof(oracle_m1));
    inject(oracle_m3, sizeof(oracle_m3));
    VERIFY((netif_by_index(index)->flags & NETIF_F_RUNNING) != 0);
}
int main(void)
{
    ndev = 0;
    VERIFY(netdev_init() == -1 && netif_count() == 1);
    struct wifi_radio_transport transport = {
        .set_channel = radio_channel,
        .transmit = radio_transmit,
        .receive = radio_receive,
        .random = radio_random,
        .now_ms = radio_time,
    };
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    VERIFY(wifi_adapter_attach("lo", &transport, mac) == -1);
    int index = wifi_adapter_attach("wlan0", &transport, mac);
    VERIFY(index == 2 && netdev_primary_ifindex() == index);
    VERIFY(netdev_init() == 0 && netif_count() == 2);
    VERIFY(wifi_adapter_attach("wlan0", &transport, mac) == -1);
    /* A bounded nonterminated name must be rejected without reading past it. */
    char unterminated_name[NETIF_NAMELEN];
    memset(unterminated_name, 'x', sizeof(unterminated_name));
    VERIFY(netdev_attach_transport(unterminated_name, &fake[0]) == -1);
    VERIFY(netdev_attach_transport("wlan0", &fake[0]) == -1);
    VERIFY(netdev_attach_transport("alias", netif_by_index(index)->dev) == -1);
    VERIFY(netdev_tx(oracle_ethernet, sizeof(oracle_ethernet)) == -1);
    connect_adapter(index);
    radio.next_frame = oracle_rx;
    radio.next_bytes = sizeof(oracle_rx);
    VERIFY(netdev_rx_poll(input_frame) == 1 && radio.delivered == 1);
    VERIFY(radio.last_transmit[1] == 0x41);
    unsigned before = radio.transmitted;
    netif_by_index(index)->flags &= ~NETIF_F_UP;
    VERIFY(netdev_set_link(netif_by_index(index)->dev, 1) == 0);
    VERIFY(!(netif_by_index(index)->flags & NETIF_F_UP));
    VERIFY(netdev_tx(oracle_ethernet, sizeof(oracle_ethernet)) == -1 &&
           radio.transmitted == before);
    netif_by_index(index)->flags |= NETIF_F_UP;
    radio.reenter_detach = 1;
    VERIFY(wifi_adapter_poll() == 0 && radio.reentrant_result == -2);
    radio.now = 5000;
    VERIFY(wifi_adapter_poll() == -1 && !(netif_by_index(index)->flags & NETIF_F_RUNNING));
    VERIFY(netdev_tx(oracle_ethernet, sizeof(oracle_ethernet)) == -1);
    VERIFY(wifi_adapter_detach() == 0);
    before = radio.callbacks;
    VERIFY(wifi_adapter_poll() == -1 && radio.callbacks == before);
    VERIFY(netdev_rx_poll(input_frame) == 0 && radio.callbacks == before);
    VERIFY(wifi_adapter_attach("different", &transport, mac) == -1);
    VERIFY(wifi_adapter_attach("wlan0", &transport, mac) == index && netif_count() == 2);
    connect_adapter(index);
    radio.next_frame = oracle_rx;
    radio.next_bytes = sizeof(oracle_rx);
    VERIFY(wifi_adapter_poll() == 0);
    VERIFY(wifi_adapter_detach() == 0);
    VERIFY(netdev_rx_poll(input_frame) == 0 && radio.delivered == 1);
    VERIFY(netdev_attach_transport("extra0", &fake[0]) == 3);
    VERIFY(netdev_primary_ifindex() == index);
    VERIFY(netdev_attach_transport("extra1", &fake[1]) == 4);
    VERIFY(netdev_attach_transport("full", &fake[2]) == -1);
    printf("WIFI_ADAPTER: %u checks, %u failures\n", wifi_checks, wifi_failures);
    return wifi_failures ? 1 : 0;
}
