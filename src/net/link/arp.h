#ifndef AQUA_ARP_H
#define AQUA_ARP_H

#include <stdint.h>
#include "eth.h"

/* Handle an incoming ARP frame (reply to requests for our IP; learn replies). */
void arp_input(const uint8_t *frame, uint16_t len);

/* Resolve `ip` (host order) to a MAC. On a cache hit, copies it into `mac` and
 * returns 0. On a miss, sends an ARP request and returns -1 (caller retries). */
int  arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN]);

#endif /* AQUA_ARP_H */
