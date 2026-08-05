/* Structural tests for the kernel Hash_DRBG (c/kernel/core/rng.c), built on
 * the host with stub headers (tests/unit/rngstub). These assert the security
 * *structure*, not statistical quality:
 *   - outputs are unique (counter-domain separation works)
 *   - an output block never equals the state that produced it (state/output
 *     separation -- the pre-refactor design emitted the state verbatim)
 *   - the state evolves on every block (forward secrecy within an epoch)
 *   - the periodic reseed actually fires inside 1 MiB of output
 *   - a forced reseed changes the state even with no output in between */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "rng.h"            /* c/kernel/core via -I */
void rng_test_state(uint8_t out[32], uint64_t *counter);
void rng_test_force_reseed(void);

/* --- stub implementations --- */
static uint64_t fake_ticks;
uint64_t timer_ticks(void) { return ++fake_ticks; }
void kprintf(const char *fmt, ...) { (void)fmt; }

static int fails;

#define CHECK(cond, name) do { \
    if (cond) { printf("ok   %s\n", name); } \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

int main(void)
{
    uint8_t s0[32], s1[32], out[32];
    uint64_t c0, c1;

    /* 1. consecutive blocks differ, and an output never equals the state
     *    that was in place when it was generated */
    rng_test_state(s0, &c0);
    kernel_random_bytes(out, 32);
    rng_test_state(s1, &c1);
    CHECK(memcmp(out, s0, 32) != 0, "output != generating state");
    CHECK(memcmp(out, s1, 32) != 0, "output != evolved state");
    CHECK(memcmp(s1, s0, 32) != 0, "state evolves per block");
    CHECK(c1 == c0 + 1, "block counter advances");

    /* 2. 4096 consecutive 32-byte blocks are pairwise distinct */
    {
        static uint8_t blocks[4096][32];
        int dup = 0;
        for (int i = 0; i < 4096; i++) {
            kernel_random_bytes(blocks[i], 32);
            for (int j = 0; j < i && !dup; j++)
                if (memcmp(blocks[i], blocks[j], 32) == 0) dup = 1;
        }
        CHECK(!dup, "4096 blocks pairwise distinct");
    }

    /* 3. forced reseed changes the state even with no output in between */
    {
        uint8_t a[32], b[32]; uint64_t ca, cb;
        rng_test_force_reseed();
        rng_test_state(a, &ca);
        rng_test_force_reseed();
        rng_test_state(b, &cb);
        CHECK(memcmp(a, b, 32) != 0, "forced reseed changes state");
        CHECK(ca == 0 && cb == 0, "reseed resets block counter");
    }

    /* 4. periodic reseed fires within 1 MiB: after >1 MiB of output the
     *    epoch counter must have wrapped (i.e. be below the total number of
     *    blocks produced since we started counting) */
    {
        uint8_t buf[4096]; uint64_t cstart, cend;
        rng_test_force_reseed();                 /* known epoch start */
        rng_test_state(buf, &cstart);
        for (int i = 0; i < 300; i++)            /* 300*4K = 1.17 MiB */
            kernel_random_bytes(buf, sizeof buf);
        rng_test_state(buf, &cend);
        CHECK(cend < 300ULL * 4096 / 32, "periodic reseed fired within 1 MiB");
    }

    /* 5. sub-block requests: 7 bytes then 57 bytes are distinct and the
     *    counter advances by whole blocks consumed */
    {
        uint8_t a[7], b[57]; uint64_t x0, x1;
        rng_test_state(s0, &x0);
        kernel_random_bytes(a, sizeof a);
        kernel_random_bytes(b, sizeof b);
        rng_test_state(s1, &x1);
        CHECK(x1 == x0 + 3, "partial blocks consume whole counter steps");
        CHECK(memcmp(a, b, sizeof a) != 0, "partial outputs distinct");
    }

    /* 6. degenerate calls are safe */
    kernel_random_bytes(0, 32);
    kernel_random_bytes(out, 0);
    kernel_random_bytes(out, -5);
    CHECK(1, "null/zero/negative requests ignored");

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
