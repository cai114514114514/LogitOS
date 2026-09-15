/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_EHCI_HW_H
#define LOGIT_EHCI_HW_H
#include <stdint.h>
#include <stddef.h>
/* EHCI 1.0 sections 3.5/3.6. Pad each object to a 32-byte multiple; the high
 * address fields remain zero because this backend deliberately uses DMA32.
 * CPU pointers may live above 4 GiB and are never stored in these fields. */
struct ehci_qtd {
    volatile uint32_t next, alt, token, page[5], high[5];
    uint32_t reserved[3];
} __attribute__((aligned(32)));
struct ehci_qh {
    volatile uint32_t link, ep, caps, current;
    volatile uint32_t next, alt, token, page[5], high[5];
    uint32_t reserved[7];
} __attribute__((aligned(32)));
_Static_assert(sizeof(struct ehci_qtd)==64, "qTD geometry");
_Static_assert(sizeof(struct ehci_qh)==96, "QH geometry");
#define EHCI_END 1u
#define EHCI_QH_LINK 2u
#define EHCI_ACTIVE (1u<<7)
#define EHCI_HALTED (1u<<6)
#define EHCI_ERRORS ((1u<<6)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2))
#define EHCI_IOC (1u<<15)
#define EHCI_TOGGLE (1u<<31)
#define EHCI_REMAIN(t) (((t)>>16)&0x7fffu)
/* Shared USB speed encoding is NOT EHCI's EPS encoding. */
static inline int ehci_eps(unsigned speed)
{ return speed==1 ? 0 : speed==2 ? 1 : speed==3 ? 2 : -1; }
static inline int ehci_qh_init(struct ehci_qh *q, unsigned addr, unsigned ep,
                             unsigned speed, unsigned packet, int control,
                             unsigned hub, unsigned port, unsigned smask,
                             unsigned cmask)
{
    int eps=ehci_eps(speed);
    if (!q || eps<0 || addr>127 || ep>15 || !packet || packet>1024 ||
        (speed!=3 && (!hub || hub>127 || !port || port>127)) ||
        (speed==2 && packet>8) || (speed==1 && packet>64)) return -1;
    uint32_t *p=(uint32_t *)q;
    for (unsigned i=0;i<sizeof(*q)/4;i++) p[i]=0;
    q->link=EHCI_END; q->next=q->alt=EHCI_END;
    q->ep=addr|(ep<<8)|((unsigned)eps<<12)|(packet<<16);
    if (control) q->ep|=1u<<14; /* qTD supplies SETUP/DATA1/status toggles */
    if (control && speed!=3) q->ep|=1u<<27;
    if (!smask) q->ep|=4u<<28; /* async NAK reload; periodic must use zero */
    q->caps=(1u<<30)|(smask&255u)|((cmask&255u)<<8);
#ifndef EHCI_NEGCTL_NO_TT
    if (speed!=3) q->caps|=(hub<<16)|(port<<23);
#endif
    return 0;
}
/* A qTD spans at most five pages. All nonfirst pointers are page-aligned;
 * the first keeps its offset. Refuse wrap/above-mask before truncation. */
static inline int ehci_qtd_init(struct ehci_qtd *q, uint64_t dma, unsigned len,
                              unsigned pid, unsigned toggle)
{
    if (!q || pid>2 || toggle>1 || len>0x7fff ||
        len>5u*4096u-(dma&4095u) || dma>UINT32_MAX ||
        (len && len-1>UINT32_MAX-dma)) return -1;
    uint32_t *p=(uint32_t *)q;
    for (unsigned i=0;i<sizeof(*q)/4;i++) p[i]=0;
    q->next=q->alt=EHCI_END;
    q->token=EHCI_ACTIVE|(3u<<10)|(pid<<8)|(len<<16)|(toggle<<31);
    q->page[0]=(uint32_t)dma;
    for (unsigned i=1;i<5;i++) {
        uint64_t a=(dma&~UINT64_C(4095))+4096u*i;
        if (a<=UINT32_MAX) q->page[i]=(uint32_t)a;
    }
    return 0;
}
static inline int ehci_actual(unsigned requested, uint32_t token)
{
    unsigned left=EHCI_REMAIN(token);
    if ((token&(EHCI_ACTIVE|EHCI_ERRORS)) || left>requested) return -1;
#ifdef EHCI_NEGCTL_FULL_SHORT
    return (int)requested;
#else
    return (int)(requested-left);
#endif
}
/* Conservatively reserve complete frames, not guessed TT bandwidth. Distinct
 * endpoints never share a frame, even across different TTs. This supports
 * common 8/10ms HID devices and refuses overcommit explicitly. */
static inline int ehci_period(unsigned speed, unsigned interval, unsigned *mask)
{
    if (!interval || !mask) return -1;
    if (speed==3) {
        if (interval>16) return -1;
        *mask=interval==1?255u:interval==2?85u:interval==3?17u:1u;
        return interval<=4?1:1<<(interval-4);
    }
    if (speed!=1 && speed!=2) return -1;
    unsigned period=1;
    while (period*2<=interval && period<128) period*=2;
    *mask=1;
    return (int)period;
}
#endif
