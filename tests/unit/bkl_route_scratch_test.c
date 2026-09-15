/* SPDX-License-Identifier: MIT */
/* Same-thread interrupt preemption is synchronous here; protocol/route bodies
 * are production code, only register/user-task leaves are replaced. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "net.h"
#include "eth.h"
#include "arp.h"
#include "reasm.h"
#include "route.h"
static char syscall_scratch[256];
static unsigned scratch_calls, printed, checks, failures;
char *sched_name_scratch(void) { scratch_calls++; return syscall_scratch; }
struct net_config net_cfg = { .ip=0x0a00020f, .mask=0xffffff00, .gw=0x0a000202 };
const uint8_t eth_broadcast[ETH_ALEN] = {255,255,255,255,255,255};
int arp_output(uint32_t ip,uint16_t type,const void *p,uint16_t n)
{ (void)ip;(void)type;(void)p;(void)n;return 0; }
int eth_send(const uint8_t dst[6],uint16_t type,const void *p,uint16_t n)
{ (void)dst;(void)type;(void)p;(void)n;return 0; }
void kprintf(const char *fmt, ...) { (void)fmt;printed++; }
void net_poll(void) {}
void net_idle(void) {}
int reasm_input(uint32_t src,uint32_t dst,uint8_t proto,uint16_t id,
 const uint8_t *iph,uint8_t ihl,uint16_t off,int more,const uint8_t *data,
 uint16_t dlen,struct reasm_dgram *out)
{ (void)src;(void)dst;(void)proto;(void)id;(void)iph;(void)ihl;(void)off;
 (void)more;(void)data;(void)dlen;(void)out;return 0; }
void reasm_release(struct reasm_dgram *g) { (void)g; }
#include "route.c"
#include "ip.c"
static void check(int ok,const char *message)
{ checks++;if(!ok){failures++;printf("FAIL: %s\n",message);} }
int main(void)
{
    char saved[256];
    for(unsigned i=0;i<sizeof(saved);i++)saved[i]=(char)(i^0x5a);
    memcpy(syscall_scratch,saved,sizeof(saved));
    route_flush();
    unsigned gen=route_generation();
    /* Exactly the actual path reached by an RX interrupt's ip_send reply. */
    route_sync();
    check(route_generation()!=gen && printed>=9,"actual route_sync publishes and prints routes");
    check(memcmp(syscall_scratch,saved,sizeof(saved))==0,
          "IRQ route_sync preserves occupied syscall scratch");
    check(scratch_calls==0,"IRQ path never borrows the interrupted task scratch");
    unsigned before_print=printed;
    net_cfg.gw=0x0a000203;
    route_sync();
    check(printed>before_print,"route refresh also exercises the printing path");
    check(memcmp(syscall_scratch,saved,sizeof(saved))==0,"repeated IRQ refresh preserves syscall scratch");
    struct route_entry row;
    memset(&row,0x65,sizeof(row));struct route_entry before=row;
    check(!route_at_copy(-1,&row) && !route_at_copy(RT_NROUTE,&row) && !route_at_copy(0,0),
          "copy rejects invalid slot and null storage");
    check(memcmp(&row,&before,sizeof(row))==0,"failed copy leaves output unchanged");
    check(route_at_copy(0,&row)==1,"copy retrieves existing route");
    const struct route_entry *legacy=route_at(0);
    check(legacy && memcmp(legacy,&row,sizeof(row))==0,"legacy task wrapper remains compatible");
    printf("bkl route scratch: %u checks, %u failures\n",checks,failures);
    return failures!=0;
}
