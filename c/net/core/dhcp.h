#ifndef LOGIT_DHCP_H
#define LOGIT_DHCP_H

#include <stdint.h>

/* Synchronously negotiate a lease (DISCOVER -> OFFER -> REQUEST -> ACK) and
 * fill net_cfg.{ip,mask,gw,dns}. Runs with the stack otherwise unconfigured
 * (net_cfg.ip == 0). The boot attempt first waits up to 6 s for the NIC's
 * link, then spends `timeout_ticks` on the exchange. Returns 0 on success, -1
 * on timeout. -1 does NOT end the client: it carries on from dhcp_poll() with
 * the RFC 2131 4.1 backoff and restarts on every link-up, applying a late
 * lease exactly as the boot path does. The caller decides only what the
 * address is in the meantime (net_cfg_acquire(): the QEMU SLIRP static
 * configuration under a hypervisor, unconfigured on real hardware). */
int  dhcp_run(int timeout_ticks);

/* net_poll hook: drains the client socket, retransmits, drives T1 lease
 * renewal, and -- once dhcp_run() has returned, success or not -- follows the
 * link and keeps acquiring. A no-op until dhcp_run() has been called. */
void dhcp_poll(void);

/* A snapshot for the diagnostic HUD (c/kernel/diag/hwhud.c). `state` is a
 * static string. `next_seconds` is the time to the next transmission, or to
 * the T1 renewal when bound; 0 when nothing is scheduled. */
struct dhcp_status {
    const char *state;
    unsigned    retries;
    unsigned    next_seconds;
    int         link_up;
};
void dhcp_status(struct dhcp_status *out);

#endif /* LOGIT_DHCP_H */
