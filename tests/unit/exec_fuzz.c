/* A fuzz pass over the loader.
 *
 * elf.c parses disk-controlled input IN RING 0. The author of the old loader
 * knew it -- the program-header bound is written in subtraction form
 * specifically so the addition cannot overflow -- and everything added since
 * has to hold to the same standard. This is how that is checked rather than
 * asserted.
 *
 * The corpus is the REAL binaries this tree builds (their .aex bytes), and the
 * mutations are the ones that matter for a header parser: smash a field to a
 * random 64-bit value, flip bits, truncate, and -- the one that earns its keep
 * -- reach INTO the program-header table and smash a chosen field of a chosen
 * segment. A value smash is worth far more than a bit flip here, because the
 * dangerous values are not near the valid ones: 0xFFFFFFFFFFFFF000 as a p_memsz
 * is one mutation away from correct and a billion bit-flips away.
 *
 * Two things this had to learn the hard way, both recorded at the code:
 *   - the v2 container's CRC means any mutation inside the ELF is refused
 *     before the ELF parser runs, so the CRC is RE-STAMPED on three iterations
 *     in four. Without that, 40000 iterations reached the ring-0 parser 147
 *     times.
 *   - blind mutation could not re-find the bug this fuzzer originally found,
 *     because that bug needs two coordinated values in one program header.
 *     Hence the structure-aware case. The negative control is what measured
 *     both: a fuzzer that survives the loader it was written against is not
 *     running.
 *
 * FOUR invariants, checked after EVERY iteration:
 *   1. it returns. No crash, no hang, no infinite loop.
 *   2. it returns ELF_OK or a known negative code, never anything else.
 *   3. nothing is mapped outside [USER_VA_BASE, USER_VA_END). This is the one
 *      that matters: a mapping below the base lands in the SHARED kernel page
 *      tables with the USER bit set, which is a ring-3 window into the kernel.
 *   4. a refusal always printed a line. A silent refusal is how a loader
 *      becomes impossible to debug on a machine with no debugger.
 *
 * Usage: exec_fuzz <iterations> <seed> <file.aex>...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "space.h"
#include "elf.h"
#include "aex.h"
#include "crc32.h"

static uint64_t rs;
static uint64_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static uint64_t rrange(uint64_t n) { return n ? rnd() % n : 0; }

/* Values that are interesting to a bounds check, in the sense that they are the
 * ones that break a check written the wrong way round. */
static const uint64_t nasty[] = {
    0, 1, 2, 7, 8, 0xF, 0x10, 0x38, 0x40, 0xFFF, 0x1000, 0x1001,
    0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull,
    0x100000000ull, 0x3FFFFFFFull, 0x40000000ull, 0x7FFFFFFFull, 0x80000000ull,
    0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
    0x7FFFFFFFFFFFFFFFull, 0xDEADBEEFull,
};
#define NNASTY ((int)(sizeof nasty / sizeof nasty[0]))

struct seed { uint8_t *buf; long len; const char *name; };

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: exec_fuzz <iters> <seed> <file>...\n"); return 2; }
    long iters = atol(argv[1]);
    rs = strtoull(argv[2], 0, 0);
    if (!rs) rs = 0x2545F4914F6CDD1Dull;

    struct seed seeds[64];
    int nseed = 0;
    for (int i = 3; i < argc && nseed < 64; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n < 128 || n > 4 * 1024 * 1024) { fclose(f); continue; }
        uint8_t *b = malloc((size_t)n);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); continue; }
        fclose(f);
        seeds[nseed].buf = b; seeds[nseed].len = n; seeds[nseed].name = argv[i];
        nseed++;
    }
    if (!nseed) { fprintf(stderr, "exec_fuzz: no usable seeds\n"); return 2; }

    /* A frame budget the corpus fits in but a fabricated p_memsz does not, so
     * "an image asking for 200 MiB" exercises the OOM path instead of the host
     * OOM killer. */
    space_set_budget(20000);
    space_quiet(1);

    long accepted = 0, refused = 0, container_refused = 0;
    long by_code[64];
    memset(by_code, 0, sizeof by_code);

    uint8_t *work = malloc(4 * 1024 * 1024 + 64);

    for (long it = 0; it < iters; it++) {
        struct seed *s = &seeds[rrange((uint64_t)nseed)];
        long len = s->len;
        memcpy(work, s->buf, (size_t)len);

        int nmut = 1 + (int)rrange(4);
        for (int m = 0; m < nmut; m++) {
            switch (rrange(7)) {
            case 0: {                                   /* smash an aligned u64 */
                long off = (long)rrange((uint64_t)len);
                off &= ~7L;
                if (off + 8 <= len) {
                    uint64_t v = (rnd() & 1) ? nasty[rrange(NNASTY)] : rnd();
                    memcpy(work + off, &v, 8);
                }
                break;
            }
            case 1: {                                   /* smash an aligned u32 */
                long off = (long)rrange((uint64_t)len);
                off &= ~3L;
                if (off + 4 <= len) {
                    uint32_t v = (uint32_t)((rnd() & 1) ? nasty[rrange(NNASTY)] : rnd());
                    memcpy(work + off, &v, 4);
                }
                break;
            }
            case 2: {                                   /* smash a u16 */
                long off = (long)rrange((uint64_t)len);
                off &= ~1L;
                if (off + 2 <= len) {
                    uint16_t v = (uint16_t)rnd();
                    memcpy(work + off, &v, 2);
                }
                break;
            }
            case 3: {                                   /* flip a bit */
                long off = (long)rrange((uint64_t)len);
                work[off] ^= (uint8_t)(1u << rrange(8));
                break;
            }
            case 4:                                     /* truncate */
                len = (long)rrange((uint64_t)len + 1);
                break;

            case 5: case 6: {
                /* STRUCTURE-AWARE: find the ELF, pick a program header, and
                 * smash one of ITS fields. Twice as likely as the others
                 * because it is worth far more.
                 *
                 * A blind byte mutator reaches a program header roughly in
                 * proportion to how much of the file the table is, which for a
                 * 100 KiB binary is well under one percent -- and the bug this
                 * fuzzer found needs TWO coordinated values in the same header
                 * (p_memsz = -0x1000 AND a p_vaddr that is not page aligned).
                 * Blind, that is a coincidence; here it is one iteration in a
                 * few hundred. The measurement that forced this: with the v2
                 * container in place the negative control -- the loader with
                 * the old overflow check -- SURVIVED 40000 blind iterations. A
                 * fuzzer that cannot re-find the bug it found is not a
                 * regression test for it. */
                long hs = 64;
                if (len >= 64 && !memcmp(work, "AEX1", 4)) {
                    uint16_t ver = (uint16_t)(work[4] | (work[5] << 8));
                    hs = (ver >= 2) ? (work[52] | (work[53] << 8)) : 64;
                }
                if (hs < 0 || hs + 64 > len) break;
                uint8_t *e = work + hs;
                uint64_t phoff; uint16_t phentsize, phnum;
                memcpy(&phoff, e + 32, 8);
                memcpy(&phentsize, e + 54, 2);
                memcpy(&phnum, e + 56, 2);
                if (phentsize != 56 || phnum == 0 || phnum > 64) break;
                if (phoff > (uint64_t)(len - hs) ||
                    (uint64_t)phnum * 56 > (uint64_t)(len - hs) - phoff) break;
                uint8_t *ph = e + phoff + (rrange(phnum) * 56);
                switch (rrange(7)) {
                case 0: { uint32_t v = (uint32_t)rnd(); memcpy(ph + 0, &v, 4); break; }   /* p_type  */
                case 1: { uint32_t v = (uint32_t)rrange(8); memcpy(ph + 4, &v, 4); break; } /* p_flags */
                case 2: { uint64_t v = nasty[rrange(NNASTY)]; memcpy(ph + 8, &v, 8); break; }  /* p_offset */
                case 3: { uint64_t v; memcpy(&v, ph + 16, 8);
                          v += rrange(0x2000) - 0x1000; memcpy(ph + 16, &v, 8); break; }  /* p_vaddr nudge */
                case 4: { uint64_t v = nasty[rrange(NNASTY)]; memcpy(ph + 32, &v, 8); break; } /* p_filesz */
                case 5: { uint64_t v = nasty[rrange(NNASTY)]; memcpy(ph + 40, &v, 8); break; } /* p_memsz */
                case 6: { uint64_t v = nasty[rrange(NNASTY)]; memcpy(ph + 48, &v, 8); break; } /* p_align */
                }
                break;
            }
            }
        }

        /* RE-STAMP THE CRC, most of the time -- and this is the difference
         * between a fuzz run that means something and one that does not.
         *
         * The v2 container carries a CRC-32 of the ELF image, so after any
         * mutation inside the ELF the container refuses the file and the ELF
         * PARSER IS NEVER REACHED. Measured, the first time this ran against a
         * v2 corpus: 40000 iterations, 147 accepted, ZERO ELF-level refusals.
         * The parser that runs in ring 0 had stopped being fuzzed at all, and
         * the only sign of it was that the refusal-reason count collapsed.
         *
         * The CRC guards against a disk that lost bytes, not against a hostile
         * producer -- anything that can write a .aex can write a correct CRC --
         * so re-stamping it is not weakening the test, it is the threat model.
         * One iteration in four is left un-stamped so the container's own
         * validation keeps getting hit too. */
        int restamp = (rnd() & 3) != 0;
        if (restamp && len >= 64 && !memcmp(work, "AEX1", 4)) {
            uint16_t ver = (uint16_t)(work[4] | (work[5] << 8));
            uint16_t hs  = (uint16_t)(work[52] | (work[53] << 8));
            uint32_t es;
            memcpy(&es, work + 60, 4);
            if (ver == 2 && hs >= 64 && hs <= 4096 && !(hs & 7) &&
                (long)hs <= len && (uint64_t)es <= (uint64_t)(len - hs)) {
                for (uint32_t o = 64; o + 8 <= hs; ) {
                    uint32_t tag, tl;
                    memcpy(&tag, work + o, 4);
                    memcpy(&tl, work + o + 4, 4);
                    if (tl > hs - o - 8) break;
                    if (tag == 0x43524341u && tl == 4) {           /* "ACRC" */
                        uint32_t c = crc32(work + hs, es);
                        memcpy(work + o + 8, &c, 4);
                        break;
                    }
                    o += (8 + tl + 7u) & ~7u;
                }
            }
        }

        space_reset();
        space_msgs_reset();
        space_set_nx((int)(rnd() & 1));

        /* Through the container, exactly as the kernel enters it. The AEX
         * header is part of the untrusted input and is fuzzed with the rest. */
        struct elf_image img;
        const void *elf = 0; uint64_t elfsz = 0;
        if (aex_elf_range(work, (uint64_t)len, &elf, &elfsz) != 0) {
            /* The container refused before the ELF parser was reached. Nothing
             * can have been mapped, but the refusal is held to the same rule as
             * the ELF parser's: it has to have said why. */
            container_refused++;
            if (space_msgs() == 0) {
                printf("FAIL: iteration %ld: the container refused silently\n", it);
                return 1;
            }
            if (space_pages_mapped() != 0) {
                printf("FAIL: iteration %ld: the container refused but pages were "
                       "mapped anyway\n", it);
                return 1;
            }
            continue;
        }
#ifdef FUZZ_DUMP_LAST
        /* The last input attempted, on disk, before it is attempted. A crash
         * in ring-0 parser code is worth a reproducer, and "it was iteration
         * 8317 of seed 1" stops being one the moment the corpus changes. */
        { FILE *d = fopen("build/exec_fuzz_last.bin", "wb");
          if (d) { fwrite(work, 1, (size_t)len, d); fclose(d); } }
#endif
        int rc = elf_load_image((void *)elf, elfsz, &img);

        if (rc > 0 || rc < -40) {
            printf("FAIL: iteration %ld returned %d, which is not a known code\n", it, rc);
            return 1;
        }
        if (rc == ELF_OK) accepted++;
        else { refused++; by_code[-rc & 63]++; }

        uint64_t outside = space_pages_outside(USER_VA_BASE, USER_VA_END);
        if (outside) {
            printf("FAIL: iteration %ld mapped %llu page(s) OUTSIDE the user "
                   "region (seed=%s rc=%d)\n", it, (unsigned long long)outside,
                   s->name, rc);
            return 1;
        }
        if (rc != ELF_OK && space_msgs() == 0) {
            printf("FAIL: iteration %ld refused with %d and printed nothing\n", it, rc);
            return 1;
        }
        if (rc == ELF_OK) {
            /* An accepted image must be usable: the entry point has to be in a
             * mapped, present page. An "accepted" load that hands ring 3 an
             * unmapped rip is a worse outcome than a refusal. */
            if (!(space_pte(img.entry) & 1)) {
                printf("FAIL: iteration %ld accepted an image whose entry point "
                       "is not mapped\n", it);
                return 1;
            }
            if (!(space_pte(img.phdr_va) & 1) || !(space_pte(img.random_va) & 1)) {
                printf("FAIL: iteration %ld accepted an image whose auxv pages "
                       "are not mapped\n", it);
                return 1;
            }
        }
    }

    space_reset();
    space_quiet(0);
    printf("ok: %ld fuzz iterations over %d seeds: %ld accepted, %ld refused, "
           "0 crashes, 0 escapes from the user region, 0 silent refusals\n",
           iters, nseed, accepted, refused);
    printf("    (%ld more were refused by the AEX container before the ELF"
           " parser was reached)\n", container_refused);
    printf("    refusal codes seen:");
    int distinct = 0;
    for (int i = 1; i < 40; i++)
        if (by_code[i]) { printf(" -%d(%ld)", i, by_code[i]); distinct++; }
    printf("\n");
    /* A fuzz run that only ever produces one refusal reason is not reaching the
     * parser. This is the check that the corpus and the mutations are doing
     * their job, and it has caught a broken seed list before it caught a bug. */
    if (distinct < 8) {
        printf("FAIL: only %d distinct refusal reasons -- the mutations are not "
               "reaching the parser\n", distinct);
        return 1;
    }
    if (accepted == 0) {
        printf("FAIL: nothing was accepted -- the corpus never survives, so the "
               "LOADING path was never fuzzed at all\n");
        return 1;
    }
    return 0;
}
