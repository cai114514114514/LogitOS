#ifndef LOGIT_ARP_H
#define LOGIT_ARP_H

#include <stdint.h>
#include "eth.h"

/* Handle an incoming ARP frame (reply to requests for our IP; learn replies). */
void arp_input(const uint8_t *frame, uint16_t len);

/* Resolve `ip` (host order) to a MAC. On a cache hit, copies it into `mac` and
 * returns 0. On a miss, sends an ARP request and returns -1 (caller retries). */
int  arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN]);

/* Block (pumping net_poll) until `ip` is ARP-resolved or `timeout` ticks pass.
 * Warms the cache before a cold fetch's first send. Fetch context only. */
int  arp_warm(uint32_t ip, int timeout);

#endif /* LOGIT_ARP_H */
