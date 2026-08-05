#ifndef AETHER_DHCP_H
#define AETHER_DHCP_H

#include <stdint.h>

/* Synchronously negotiate a lease (DISCOVER -> OFFER -> REQUEST -> ACK) and
 * fill net_cfg.{ip,mask,gw,dns}. Runs with the stack otherwise unconfigured
 * (net_cfg.ip == 0). Returns 0 on success, -1 on timeout -- the caller then
 * applies the static fallback config. */
int  dhcp_run(int timeout_ticks);

/* net_poll hook: drains the client socket, retransmits, and drives T1 lease
 * renewal. A no-op until dhcp_run() has succeeded once. */
void dhcp_poll(void);

#endif /* AETHER_DHCP_H */
