/* The on-disk shape of a LogitOS page-reference trace.
 *
 * One definition site, included by the QEMU plugin that WRITES the file
 * (tools/mmtrace/mmtrace.c) and by the simulator that READS it
 * (tools/mmtrace/mmsim.c). If these two ever disagree the numbers are silently
 * wrong rather than loudly broken, which is why they are not allowed to hold
 * separate copies of the layout. */
#ifndef LOGIT_MMTRACE_FMT_H
#define LOGIT_MMTRACE_FMT_H

#include <stdint.h>

#define MMT_MAGIC   "MMTRACE1"
#define MMT_VERSION 1u

/* A reference to a page, 16 bytes.
 *
 * Consecutive references to the SAME page from the same vCPU are collapsed by
 * the plugin: every replacement policy -- clock, LRU, FIFO and Belady's MIN
 * alike -- gives an identical answer whether a page is touched once or a
 * thousand times in a row, so the collapse removes ~99% of the volume and
 * changes no result. The collapse window is exactly ONE, so an A,B,A,B
 * alternation survives intact; a wider window would destroy recency order,
 * which is the one thing this file exists to record. */
struct mmt_rec {
    /* bits  0..35  virtual page number (48-bit VA >> 12)
     * bits 36..37  kind: 0 = read, 1 = write, 2 = instruction fetch
     * bits 38..41  vCPU index
     * bits 42..63  reserved, zero */
    uint64_t a;
    /* bits  0..23  address-space id = CR3 >> 12 (0 if CR3 is unreadable)
     * bits 24..47  physical frame number backing the access at that instant
     *              (0 for instruction fetches, which carry no hwaddr)
     * bits 48..63  reserved, zero */
    uint64_t b;
};

#define MMT_KIND_READ  0u
#define MMT_KIND_WRITE 1u
#define MMT_KIND_EXEC  2u

#define MMT_VPN(r)   ((r).a & 0xFFFFFFFFFull)
#define MMT_KIND(r)  (unsigned)(((r).a >> 36) & 3u)
#define MMT_CPU(r)   (unsigned)(((r).a >> 38) & 15u)
#define MMT_SPACE(r) (uint32_t)((r).b & 0xFFFFFFu)
#define MMT_PFN(r)   (uint32_t)(((r).b >> 24) & 0xFFFFFFu)

#define MMT_MK_A(vpn, kind, cpu) \
    (((uint64_t)(vpn) & 0xFFFFFFFFFull) | ((uint64_t)((kind) & 3u) << 36) | \
     ((uint64_t)((cpu) & 15u) << 38))
#define MMT_MK_B(space, pfn) \
    (((uint64_t)(space) & 0xFFFFFFull) | (((uint64_t)(pfn) & 0xFFFFFFull) << 24))

/* 64 bytes, at offset 0. */
struct mmt_hdr {
    char     magic[8];
    uint32_t version;
    uint32_t recsize;       /* sizeof(struct mmt_rec); a mismatch is fatal */
    uint64_t va_lo, va_hi;  /* the virtual window the plugin filtered to */
    uint64_t flags;
    uint64_t nrec;          /* records written; 0 if the run was cut short */
    uint64_t reserved[2];
};

#define MMT_F_EXEC  (1ull << 0)   /* instruction fetches are in the trace */
#define MMT_F_CR3   (1ull << 1)   /* space ids are real CR3s, not zeroes */

#endif /* LOGIT_MMTRACE_FMT_H */
