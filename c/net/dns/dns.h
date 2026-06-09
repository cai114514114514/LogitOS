#ifndef AETHER_DNS_H
#define AETHER_DNS_H

#include <stdint.h>

/* Resolve `name` to an IPv4 address (host order) via the SLIRP DNS server
 * (10.0.2.3) over UDP. Blocking-ish: pumps net_poll() with a timeout. Returns
 * the address, or 0 on failure/timeout. Must run with interrupts enabled. */
uint32_t dns_resolve(const char *name);

/* Non-blocking variant for apps (the WM loop pumps net_poll). dns_start() sends
 * the query; dns_result() returns the resolved IP, 0 while pending, or
 * 0xFFFFFFFF on timeout. */
void     dns_start(const char *name);
uint32_t dns_result(void);

#endif /* AETHER_DNS_H */
