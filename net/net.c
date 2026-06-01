#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "e1000.h"
#include "eth.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

/* Static config matching QEMU SLIRP (user networking) defaults. A net_config
 * struct rather than constants so DHCP can fill it in later. */
struct net_config net_cfg = {
    .ip   = IPV4(10, 0, 2, 15),
    .mask = IPV4(255, 255, 255, 0),
    .gw   = IPV4(10, 0, 2, 2),
};

static int up;

int net_up(void) { return up; }

int net_init(void)
{
    if (e1000_init() != 0)
        return -1;
    memcpy(net_cfg.mac, e1000_mac(), 6);
    up = 1;
    kprintf("[net] ip 10.0.2.15/24 gw 10.0.2.2\n");
    return 0;
}

void net_poll(void)
{
    if (up)
        e1000_rx_poll(eth_input);
}
