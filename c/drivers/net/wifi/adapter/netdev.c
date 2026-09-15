#include "netdev.h"
#include "../../netdev.h"
#include "net.h"
#include <string.h>

enum {
    WIFI_ADAPTER_RX_SLOTS = 8,
    WIFI_ADAPTER_POLL_BUDGET = 16,
    WIFI_ADAPTER_ETHERNET_BYTES = 1514
};
struct ethernet_slot {
    uint16_t bytes;
    uint8_t data[WIFI_ADAPTER_ETHERNET_BYTES];
};
static struct {
    unsigned gate;
    unsigned attached;
    int interface_index;
    struct wifi_station station;
    struct wifi_radio_transport transport;
    struct netdev device;
    struct ethernet_slot received[WIFI_ADAPTER_RX_SLOTS];
    unsigned head;
    unsigned count;
} adapter;

static int enter_adapter(void)
{
    return !__atomic_exchange_n(&adapter.gate, 1, __ATOMIC_ACQUIRE);
}
static void leave_adapter(void)
{
    __atomic_store_n(&adapter.gate, 0, __ATOMIC_RELEASE);
}
static int set_channel(void *unused, uint8_t channel)
{
    (void)unused;
    return adapter.transport.set_channel(adapter.transport.opaque, channel);
}
static int transmit_radio(void *unused, const uint8_t *frame, size_t bytes)
{
    (void)unused;
    return adapter.transport.transmit(adapter.transport.opaque, frame, bytes);
}
static int random_bytes(void *unused, uint8_t *output, size_t bytes)
{
    (void)unused;
    return adapter.transport.random(adapter.transport.opaque, output, bytes);
}
static uint64_t clock_ms(void *unused)
{
    (void)unused;
    return adapter.transport.now_ms(adapter.transport.opaque);
}
static void queue_ethernet(void *unused, const uint8_t *frame, size_t bytes)
{
    (void)unused;
    if (bytes > WIFI_ADAPTER_ETHERNET_BYTES || adapter.count == WIFI_ADAPTER_RX_SLOTS) {
        return;
    }
    unsigned tail = (adapter.head + adapter.count) % WIFI_ADAPTER_RX_SLOTS;
    memcpy(adapter.received[tail].data, frame, bytes);
    adapter.received[tail].bytes = (uint16_t)bytes;
    ++adapter.count;
}

static void update_link(void)
{
    int linked = adapter.attached && wifi_station_link_up(&adapter.station);
#ifdef WIFI_ADAPTER_NEGCTL_LINK
    linked = 1;
#endif
    netdev_set_link(&adapter.device, linked);
    if (!linked) {
        /* Previously authenticated frames may have been queued before link
         * loss. A later attachment must never receive that session's data. */
        memset(adapter.received, 0, sizeof(adapter.received));
        adapter.head = 0;
        adapter.count = 0;
    }
}

static int transmit_ethernet(const void *frame, uint16_t bytes)
{
    NET_GUARD;
    if (!enter_adapter()) {
        return -2;
    }
    int result = adapter.attached ? wifi_station_transmit(&adapter.station, frame, bytes) : -1;
    update_link();
    leave_adapter();
    return result;
}

static int poll_transport(void)
{
    uint8_t frame[WIFI_FRAME_MAX];
    for (unsigned remaining = WIFI_ADAPTER_POLL_BUDGET; remaining; --remaining) {
        size_t bytes = 0;
        int16_t signal_dbm = 0;
        int result = adapter.transport.receive(adapter.transport.opaque, frame, sizeof(frame),
                                               &bytes, &signal_dbm);
        if (!result) {
            break;
        }
        if (result != 1 || bytes > sizeof(frame)) {
            wifi_station_transport_lost(&adapter.station);
            return -1;
        }
        wifi_station_receive(&adapter.station, frame, bytes, signal_dbm);
    }
    return wifi_station_tick(&adapter.station);
}

int wifi_adapter_poll(void)
{
    NET_GUARD;
    if (!enter_adapter()) {
        return -2;
    }
    int result = adapter.attached ? poll_transport() : -1;
    update_link();
    leave_adapter();
    return result;
}

static int receive_ethernet(net_rx_cb callback)
{
    NET_GUARD;
    if (!callback || wifi_adapter_poll() == -2) {
        return 0;
    }
    int delivered = 0;
    for (unsigned remaining = WIFI_ADAPTER_RX_SLOTS; remaining; --remaining) {
        if (!enter_adapter()) {
            break;
        }
        struct netif *interface = netif_by_index(adapter.interface_index);
        int up = interface && (interface->flags & NETIF_F_UP);
        if (!up || !adapter.attached || !wifi_station_link_up(&adapter.station) || !adapter.count) {
            if (!up) {
                adapter.count = 0;
            }
            leave_adapter();
            break;
        }
        struct ethernet_slot frame = adapter.received[adapter.head];
        memset(&adapter.received[adapter.head], 0, sizeof(frame));
        adapter.head = (adapter.head + 1) % WIFI_ADAPTER_RX_SLOTS;
        --adapter.count;
        leave_adapter();
        /* eth_input may synchronously transmit a reply; never hold the
         * station/adapter gate while entering the network stack. */
        callback(frame.data, frame.bytes);
        ++delivered;
    }
    return delivered;
}

int wifi_adapter_attach(const char *name, const struct wifi_radio_transport *transport,
                        const uint8_t mac[6])
{
    NET_GUARD;
    if (!name || !transport || !mac || !transport->receive || !transport->set_channel ||
        !transport->transmit || !transport->random || !transport->now_ms) {
        return -1;
    }
    if (!enter_adapter()) {
        return -2;
    }
    int result = -1;
    if (adapter.attached) {
        goto done;
    }
    if (adapter.interface_index) {
        struct netif *interface = netif_by_index(adapter.interface_index);
        if (!interface || netif_by_name(name) != interface || memcmp(mac, adapter.device.mac, 6)) {
            goto done;
        }
    }
    adapter.transport = *transport;
    struct wifi_station_ops operations = {
        .set_channel = set_channel,
        .transmit = transmit_radio,
        .random = random_bytes,
        .now_ms = clock_ms,
        .ethernet_rx = queue_ethernet,
    };
    if (wifi_station_init(&adapter.station, &operations, mac)) {
        goto clear_transport;
    }
    if (!adapter.interface_index) {
        adapter.device = (struct netdev){
            .name = "wifi-station",
            .irq_line = -1,
            .tx = transmit_ethernet,
            .rx_poll = receive_ethernet,
        };
        memcpy(adapter.device.mac, mac, 6);
        int index = netdev_attach_transport(name, &adapter.device);
        if (index < 0) {
            goto clear_transport;
        }
        adapter.interface_index = index;
    }
    adapter.attached = 1;
    update_link();
    result = adapter.interface_index;
    goto done;
clear_transport:
    memset(&adapter.transport, 0, sizeof(adapter.transport));
done:
    leave_adapter();
    return result;
}

int wifi_adapter_detach(void)
{
    NET_GUARD;
    if (!enter_adapter()) {
        return -2;
    }
    wifi_station_disconnect(&adapter.station);
    adapter.attached = 0;
    update_link();
    memset(&adapter.transport, 0, sizeof(adapter.transport));
    leave_adapter();
    return 0;
}

int wifi_adapter_scan(const uint8_t *channels, size_t count)
{
    NET_GUARD;
    if (!enter_adapter()) {
        return -2;
    }
    int result = adapter.attached ? wifi_station_scan(&adapter.station, channels, count) : -1;
    update_link();
    leave_adapter();
    return result;
}

int wifi_adapter_join(unsigned bss_index, const uint8_t *password, size_t bytes)
{
    NET_GUARD;
    if (!enter_adapter()) {
        return -2;
    }
    int result =
        adapter.attached ? wifi_station_join(&adapter.station, bss_index, password, bytes) : -1;
    update_link();
    leave_adapter();
    return result;
}

int wifi_adapter_status(enum wifi_state *state, enum wifi_disconnect_reason *reason)
{
    NET_GUARD;
    if (!state || !reason || !enter_adapter()) {
        return -1;
    }
    *state = adapter.station.state;
    *reason = adapter.station.disconnect_reason;
    int linked = adapter.attached && wifi_station_link_up(&adapter.station);
    leave_adapter();
    return linked;
}

int wifi_adapter_scan_results(struct wifi_bss *output, size_t capacity)
{
    NET_GUARD;
    if (!output || !enter_adapter()) {
        return -1;
    }
    size_t count = adapter.station.bss_count;
    if (!adapter.attached || capacity < count) {
        leave_adapter();
        return -1;
    }
    memcpy(output, adapter.station.bss, count * sizeof(*output));
    leave_adapter();
    return (int)count;
}
