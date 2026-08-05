#ifndef LOGIT_NET_H
#define LOGIT_NET_H
#include <stdint.h>
static inline uint16_t htons(uint16_t x){ return (uint16_t)((x<<8)|(x>>8)); }
static inline uint16_t ntohs(uint16_t x){ return htons(x); }
static inline uint32_t htonl(uint32_t x){ return ((x&0xFF)<<24)|((x&0xFF00)<<8)|((x>>8)&0xFF00)|((x>>24)&0xFF); }
static inline uint32_t ntohl(uint32_t x){ return htonl(x); }
struct net_config { uint32_t ip; };
extern struct net_config net_cfg;
void net_poll(void);
void net_idle(void);
static inline uint64_t net_lock(void){ return 0; }      /* host: no IRQ to mask */
static inline void net_unlock(uint64_t f){ (void)f; }
#endif
