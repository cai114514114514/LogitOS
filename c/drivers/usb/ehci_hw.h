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
/* EHCI 3.6 endpoint characteristics bit 7. Deliberately not used by unlink:
 * Linux v6.12 ehci-sched.c qh_unlink_periodic skips I-bit handshaking and
 * instead waits EHCI_HRTIMER_UNLINK_INTR before modifying the old QH. */
#define EHCI_QH_INACTIVATE (1u << 7)
#ifndef EHCI_HW_STORE
#define EHCI_HW_STORE(ptr, value) (*(ptr) = (value))
#endif
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
/* The old policy reserved whole frames for each endpoint. Correction: period
 * now describes hardware recurrence; ehci_sched.h accounts bandwidth and
 * shares tails. Linux v6.12 ehci-q.c qh_make clamps hardware recurrence to
 * periodic_size=1024 frames; intervals 15/16 must not be refused as >1024. */
static inline int ehci_period(unsigned speed, unsigned interval, unsigned *mask)
{
    if (!interval || !mask) return -1;
    if (speed==3) {
        if (interval>16) return -1;
        *mask=interval==1?255u:interval==2?85u:interval==3?17u:1u;
        unsigned period = interval <= 4 ? 1u : 1u << (interval - 4);
#ifndef EHCI_NEGCTL_HS_NO_CLAMP
        if (period > 1024) {
            period = 1024;
        }
#endif
        return (int)period;
    }
    if (speed!=1 && speed!=2) return -1;
    unsigned period=1;
    while (period*2<=interval && period<128) period*=2;
    *mask=1;
    return (int)period;
}

/* Linux v6.12 ehci-mem.c ehci_qtd_init: an inactive HALTED sentinel prevents
 * Advance Queue (EHCI 4.10.2) from loading an unfinished descriptor. This
 * token is a software sentinel, not a STALL returned by the device. */
static inline void ehci_qtd_dummy(struct ehci_qtd *q)
{
    volatile uint32_t *words = (volatile uint32_t *)q;
    for (unsigned i = 0; i < sizeof(*q) / sizeof(*words); i++) {
        EHCI_HW_STORE(&words[i], 0);
    }
    EHCI_HW_STORE(&q->next, EHCI_END);
    EHCI_HW_STORE(&q->alt, EHCI_END);
    EHCI_HW_STORE(&q->token, EHCI_HALTED);
}

/* Fill an old dummy while it is still inactive. Unlike ehci_qtd_init(), never
 * expose Active before next/pages are complete: Linux ehci-q.c qh_append_tds
 * and EHCI 4.10.2 require the caller's final dma_wmb + token publication.
 * Both next and alt lead to the next dummy so a short HID packet also advances
 * without changing an HC-owned overlay (EHCI 4.10.2 short-packet traversal). */
static inline int ehci_qtd_fill_in(struct ehci_qtd *q, uint32_t next_dma,
        uint64_t buf, unsigned len, uint32_t *token_out)
{
    if (!q || !token_out || (next_dma & 31) || len > 0x7fff ||
        len > 5u * 4096u - (buf & 4095u) || buf > UINT32_MAX ||
        (len && len - 1 > UINT32_MAX - buf)) {
        return -1;
    }
    EHCI_HW_STORE(&q->token, EHCI_HALTED);
    EHCI_HW_STORE(&q->next, next_dma);
    EHCI_HW_STORE(&q->alt, next_dma);
    for (unsigned i = 0; i < 5; i++) {
        uint64_t page = i ? (buf & ~UINT64_C(4095)) + 4096u * i : buf;
        EHCI_HW_STORE(&q->page[i], page <= UINT32_MAX ? (uint32_t)page : 0);
        EHCI_HW_STORE(&q->high[i], 0);
    }
    for (unsigned i = 0; i < 3; i++) {
        EHCI_HW_STORE(&q->reserved[i], 0);
    }
    /* EHCI 3.5.3: PID=IN (1), CERR=3, IOC, length, Active last. DTC=0
     * periodic QHs obtain the toggle from their overlay, never this token. */
    *token_out = EHCI_ACTIVE | (1u << 8) | (3u << 10) | EHCI_IOC | (len << 16);
    return 0;
}

/* Decode EHCI 3.5.3 status without printf/heap dependencies, for the frozen
 * diagnostic snapshot rather than a later read from possibly retired DMA. */
static inline void ehci_ov_decode(uint32_t token, char *buf, unsigned n)
{
    static const char *const names[] = {
        "PING", "SPLITX", "MMF", "XACT", "BABBLE", "DBE", "HALT", "ACTIVE"
    };
    unsigned used = 0;
    if (!n) {
        return;
    }
    for (int bit = 7; bit >= 0; bit--) {
        if (!(token & (1u << bit))) {
            continue;
        }
        if (used && used + 1 < n) {
            buf[used++] = '|';
        }
        for (const char *letter = names[bit]; *letter && used + 1 < n; letter++) {
            buf[used++] = *letter;
        }
    }
    buf[used] = 0;
}
#endif
