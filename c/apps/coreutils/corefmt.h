#ifndef LOGIT_COREFMT_H
#define LOGIT_COREFMT_H

/* Reading an ELF64 ET_CORE file produced by c/kernel/exec/coredump.c.
 *
 * A SECOND, INDEPENDENT PARSER, and that is the point rather than an accident
 * of where the code had to live. coredump.c already contains a walk of its own
 * output (coredump_read_gregs), used so the kernel's [core] line can quote the
 * FILE instead of quoting the trap frame twice. If /bin/readcore called that
 * one, the reader and the writer would share every assumption -- note padding,
 * name length, descriptor alignment, the order of pr_reg -- and could only
 * disagree about arithmetic, never about interpretation. Written separately,
 * from coredump.h's declarations and nothing else, the two are a differential.
 * gdb is the third, and the only one that came from outside this tree.
 *
 * Header-only, no libc, no allocation: c/apps/coreutils programs link crt0 and
 * clib.h and nothing else (CLI_RULE compiles exactly one .c), and the host gate
 * includes this same file to run the same parse over the same bytes.
 *
 * Every field is read with an explicit little-endian byte load rather than a
 * struct cast. Not portability theatre -- a dump is a FILE, and a file whose
 * fields are read by pointing a struct at a buffer is a file whose parser
 * silently depends on the compiler having chosen the same padding the writer
 * did. That is the one failure this format's whole gate is aimed at. */

#include <stdint.h>
#include "coredump.h"   /* the format's single definition site */

#define CF_OK          0
#define CF_E_SHORT   (-1)
#define CF_E_MAGIC   (-2)
#define CF_E_TYPE    (-3)   /* not ET_CORE                                    */
#define CF_E_PHDR    (-4)   /* the program-header table is out of the file     */
#define CF_E_NONOTE  (-5)   /* no PT_NOTE                                      */
#define CF_E_NOREGS  (-6)   /* PT_NOTE carries no NT_PRSTATUS                  */
#define CF_E_NOLOGIT (-7)   /* no LOGIT note: not one of ours                  */

struct cf_seg {
    uint64_t vaddr, filesz, memsz, offset;
    uint32_t flags;
};

/* Everything a reader wants, extracted in one pass. `seg` is the caller's
 * storage; `nseg` comes back as the number that EXIST, which may exceed
 * `maxseg` -- the reader prints that difference rather than a prefix that
 * looks complete. */
struct cf_dump {
    const unsigned char *b;
    int32_t  n;
    uint32_t phnum;
    int32_t  nseg, nseg_stored;
    struct cf_seg *seg;
    /* NT_PRSTATUS */
    uint64_t greg[27];
    int32_t  signo, pid, ppid, fpvalid;
    /* NT_PRPSINFO */
    char     fname[17];
    /* NT_SIGINFO */
    int32_t  fault_code;
    uint64_t fault_addr;
    /* the LOGIT note */
    struct core_logit_note logit;
    int32_t  has_logit, has_fpregs;
};

static inline uint32_t cf_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t cf_u64(const unsigned char *p)
{
    return (uint64_t)cf_u32(p) | ((uint64_t)cf_u32(p + 4) << 32);
}

static inline void cf_notes(struct cf_dump *d, uint64_t at, uint64_t end)
{
    const unsigned char *b = d->b;
    while (at + 12 <= end) {
        uint32_t namesz = cf_u32(b + at);
        uint32_t descsz = cf_u32(b + at + 4);
        uint32_t type   = cf_u32(b + at + 8);
        uint64_t nm     = ((uint64_t)namesz + 3u) & ~3ull;
        uint64_t ds     = ((uint64_t)descsz + 3u) & ~3ull;
        uint64_t name   = at + 12;
        uint64_t desc   = name + nm;
        if (desc + ds > end) return;
        int is_core = (namesz == 5 && b[name] == 'C' && b[name+1] == 'O' &&
                       b[name+2] == 'R' && b[name+3] == 'E' && b[name+4] == 0);
        int is_logit = (namesz == 6 && b[name] == 'L' && b[name+1] == 'O' &&
                        b[name+2] == 'G' && b[name+3] == 'I' &&
                        b[name+4] == 'T' && b[name+5] == 0);
        if (is_core && type == CORE_NT_PRSTATUS && descsz >= 336) {
            d->signo   = (int32_t)cf_u32(b + desc);        /* pr_info.si_signo */
            d->pid     = (int32_t)cf_u32(b + desc + 32);
            d->ppid    = (int32_t)cf_u32(b + desc + 36);
            for (int i = 0; i < 27; i++)
                d->greg[i] = cf_u64(b + desc + 112 + i * 8);
            d->fpvalid = (int32_t)cf_u32(b + desc + 328);
        } else if (is_core && type == CORE_NT_PRPSINFO && descsz >= 136) {
            for (int i = 0; i < 16; i++) d->fname[i] = (char)b[desc + 40 + i];
            d->fname[16] = 0;
        } else if (is_core && type == CORE_NT_SIGINFO && descsz >= 24) {
            d->fault_code = (int32_t)cf_u32(b + desc + 8);
            d->fault_addr = cf_u64(b + desc + 16);
        } else if (is_core && type == CORE_NT_FPREGSET && descsz == 512) {
            d->has_fpregs = 1;
        } else if (is_logit && type == CORE_NT_LOGIT &&
                   descsz >= sizeof(struct core_logit_note)) {
            /* The one place a struct copy is honest: this note is written and
             * read by two files in the same tree, compiled by the same
             * compiler for the same target, and its layout is not a published
             * contract the way NT_PRSTATUS's is. The FIELDS ARE STILL CHECKED
             * -- magic and version below -- because a stale dump left on the
             * disk by an older kernel is exactly what a reader will meet. */
            const unsigned char *p = b + desc;
            if (cf_u32(p) == CORE_LOGIT_MAGIC && cf_u32(p + 4) == CORE_LOGIT_VER) {
                struct core_logit_note *L = &d->logit;
                L->magic = cf_u32(p); L->version = cf_u32(p + 4);
                L->flags = cf_u32(p + 8); L->signo = cf_u32(p + 12);
                L->trapno = cf_u64(p + 16); L->err = cf_u64(p + 24);
                L->cr2 = cf_u64(p + 32); L->cr3 = cf_u64(p + 40);
                L->want_bytes = cf_u64(p + 48); L->got_bytes = cf_u64(p + 56);
                L->want_regions = cf_u32(p + 64); L->got_regions = cf_u32(p + 68);
                L->nregion = cf_u32(p + 72);
                uint32_t nr = L->nregion > CORE_RGN_MAX ? CORE_RGN_MAX : L->nregion;
                for (uint32_t i = 0; i < nr; i++) {
                    const unsigned char *q = p + 80 + i * 32;
                    L->region[i].start = cf_u64(q);
                    L->region[i].end = cf_u64(q + 8);
                    L->region[i].dumped = cf_u64(q + 16);
                    L->region[i].prot = cf_u32(q + 24);
                    L->region[i].kind = cf_u32(q + 28);
                }
                d->has_logit = 1;
            }
        }
        at = desc + ds;
    }
}

/* Parse `buf`. Returns CF_OK or a CF_E_*. */
static inline int cf_parse(struct cf_dump *d, const void *buf, int n,
                           struct cf_seg *seg, int maxseg)
{
    const unsigned char *b = (const unsigned char *)buf;
    for (unsigned i = 0; i < sizeof *d; i++) ((unsigned char *)d)[i] = 0;
    d->b = b; d->n = n; d->seg = seg;
    if (n < 64) return CF_E_SHORT;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return CF_E_MAGIC;
    if (b[4] != 2 || b[5] != 1) return CF_E_MAGIC;
    uint16_t etype = (uint16_t)(b[16] | (b[17] << 8));
    if (etype != 4) return CF_E_TYPE;

    uint64_t phoff = cf_u64(b + 32);
    uint16_t phentsize = (uint16_t)(b[54] | (b[55] << 8));
    uint16_t phnum = (uint16_t)(b[56] | (b[57] << 8));
    if (phentsize != 56) return CF_E_PHDR;
    if (phoff + (uint64_t)phnum * 56 > (uint64_t)n) return CF_E_PHDR;
    d->phnum = phnum;

    int notes = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const unsigned char *p = b + phoff + (uint64_t)i * 56;
        uint32_t type = cf_u32(p);
        uint64_t off = cf_u64(p + 8), va = cf_u64(p + 16);
        uint64_t fsz = cf_u64(p + 32), msz = cf_u64(p + 40);
        if (off + fsz > (uint64_t)n) return CF_E_PHDR;
        if (type == 4) { cf_notes(d, off, off + fsz); notes++; continue; }
        if (type != 1) continue;
        if (d->nseg_stored < maxseg) {
            seg[d->nseg_stored].vaddr = va;
            seg[d->nseg_stored].filesz = fsz;
            seg[d->nseg_stored].memsz = msz;
            seg[d->nseg_stored].offset = off;
            seg[d->nseg_stored].flags = cf_u32(p + 4);
            d->nseg_stored++;
        }
        d->nseg++;
    }
    if (!notes) return CF_E_NONOTE;
    /* "greg is all zero" is not a legitimate register file for a program that
     * was executing, so it is treated as an absent note rather than reported
     * as a program stopped at address zero. */
    if (!d->greg[CORE_RIP] && !d->greg[CORE_RSP]) return CF_E_NOREGS;
    if (!d->has_logit) return CF_E_NOLOGIT;
    return CF_OK;
}

#endif /* LOGIT_COREFMT_H */
