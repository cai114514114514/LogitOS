/* A fuzz pass over the loader.
 *
 * elf.c parses disk-controlled input IN RING 0. The author of the old loader
 * knew it -- the program-header bound is written in subtraction form
 * specifically so the addition cannot overflow -- and everything added since
 * has to hold to the same standard. This is how that is checked rather than
 * asserted.
 *
 * The corpus is the REAL binaries this tree builds (their .aex bytes), plus a
 * hand-built reference image, and the mutations are the ones that matter for a
 * header parser: smash a field to a random 64-bit value, flip bits, truncate.
 * A field smash is worth far more than a bit flip here, because the dangerous
 * values are not near the valid ones -- 0xFFFFFFFFFFFFFFF0 as a p_offset is one
 * mutation away from correct and a billion bit-flips away.
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
            switch (rrange(5)) {
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
            /* The container refused before the ELF parser was reached. That is
             * a legitimate outcome and nothing can be mapped, so there is
             * nothing more to check this iteration. */
            container_refused++;
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
