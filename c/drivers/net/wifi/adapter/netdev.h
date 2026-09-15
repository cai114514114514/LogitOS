#ifndef LOGIT_WIFI_ADAPTER_NETDEV_H
#define LOGIT_WIFI_ADAPTER_NETDEV_H
#include "../station.h"

/* One polled physical radio: the legacy netdev vtable has no context argument.
 * Each callback is bounded, synchronous and non-reentrant. RX returns 1 with a
 * FCS-checked complete MPDU, 0 when empty, -1 on transport loss. The transport
 * owns MAC ACK/retry/rate control and supplies regulatory-approved channels.
 * All buffers are borrowed until callback return; no DMA may retain them. */
struct wifi_radio_transport {
    void *opaque;
    int (*set_channel)(void *opaque, uint8_t channel);
    int (*transmit)(void *opaque, const uint8_t *frame, size_t bytes);
    int (*receive)(void *opaque, uint8_t *frame, size_t capacity, size_t *bytes,
                   int16_t *signal_dbm);
    int (*random)(void *opaque, uint8_t *output, size_t bytes);
    uint64_t (*now_ms)(void *opaque);
};

/* Call after netdev_init. Returns the permanent interface index, or a negative
 * error. Reattach may reuse the same name/MAC after successful detach. The
 * stable netdev and interface slot are never freed; transport storage may be
 * freed only after detach returns 0. -2 means busy, no ownership was removed.
 * Lock order: recursive net_lock, adapter gate, station gate. Callbacks may
 * not reenter; Ethernet delivery occurs after both inner gates are released. */
int wifi_adapter_attach(const char *name, const struct wifi_radio_transport *transport,
                        const uint8_t mac[6]);
int wifi_adapter_detach(void);
int wifi_adapter_scan(const uint8_t *channels, size_t count);
int wifi_adapter_join(unsigned bss_index, const uint8_t *password, size_t bytes);
int wifi_adapter_status(enum wifi_state *state, enum wifi_disconnect_reason *reason);
int wifi_adapter_scan_results(struct wifi_bss *output, size_t capacity);
/* Poll via netdev_rx_poll once the stack is running. Before net_init has a NIC,
 * the radio owner calls this to complete association; afterward it may invoke
 * net_init outside polling to configure the first NIC's IP settings/DHCP. */
int wifi_adapter_poll(void);
#endif
