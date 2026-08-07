#ifndef LOGIT_NET_H
#define LOGIT_NET_H

#include <stdint.h>

/* Host byte order helpers (x86 is little-endian; network is big-endian). */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x)
{
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Build an IPv4 address (a.b.c.d) in host order. */
#define IPV4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(c) << 8) | (uint32_t)(d))

struct net_config {
    uint8_t  mac[6];
    uint32_t ip;        /* host order */
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;       /* resolver (DHCP option 6, or static fallback) */
};

extern struct net_config net_cfg;

/* Bring up the NIC + stack (called from kmain). Returns 0 on success. */
int  net_init(void);
int  net_up(void);                  /* 1 if a NIC initialised successfully */

/* Pump the receive path (called from the WM main loop). */
void net_poll(void);

/* Called by a NIC ISR once it has acked its device: the drain is handed to
 * SOFTIRQ_NET rather than run inside the interrupt. Safe from interrupt
 * context; see the receive-path comment in net.c for the whole story. */
void net_rx_schedule(void);
void net_idle(void);                /* sti;hlt -- yield the host CPU while a blocking fetch waits */
extern volatile int g_net_busy;     /* 1 while a blocking fetch owns the network */

/* Mutual exclusion between the RX interrupt (which runs eth/ip/tcp_input) and
 * mainline net code (tcp_recv/send/poll, udp). Everything net runs on the BSP,
 * and the NIC IRQ is routed to the BSP, so masking interrupts is enough. Save
 * and restore IF so this is safe both in the IRQ (IF already 0) and mainline.
 * Host-side unit tests compile with -DLOGIT_NET_HOST: cli/sti are ring-0
 * only, so the lock degenerates to a no-op there. */
#ifdef LOGIT_NET_HOST
static inline uint64_t net_lock(void) { return 0; }
static inline void net_unlock(uint64_t f) { (void)f; }
#else
static inline uint64_t net_lock(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}
static inline void net_unlock(uint64_t f)
{
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}
#endif

#endif /* LOGIT_NET_H */
