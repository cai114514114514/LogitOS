#include <stdint.h>
#include <stddef.h>
#include "rng.h"
#include "pit.h"
#include "crypto.h"
#include "spinlock.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

static uint8_t rng_state[32];
static uint64_t rng_counter;
static int rng_seeded;
/* BKL-free callers can reach kernel_random_bytes concurrently: guard the state. */
static spinlock_t rng_lock = SPINLOCK_INIT;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static int cpu_has_rdrand(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1u << 30)) != 0;
}

static int cpu_has_rdseed(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7) return 0;
    cpuid(7, 0, &a, &b, &c, &d);
    return (b & (1u << 18)) != 0;
}

/* 1 when a hardware entropy source (RDSEED/RDRAND) is available, 0 when the
 * DRBG would fall back to rdtsc-only seeding. TLS refuses to handshake on 0:
 * session keys from a predictable seed are worse than a clear error. */
int rng_strong(void)
{
    return cpu_has_rdseed() || cpu_has_rdrand();
}

static int rdrand64(uint64_t *out)
{
    uint8_t ok;
    uint64_t v;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (!ok) return 0;
    *out = v;
    return 1;
}

static int rdseed64(uint64_t *out)
{
    uint8_t ok;
    uint64_t v;
    __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
    if (!ok) return 0;
    *out = v;
    return 1;
}

static void rng_absorb(const void *data, int len)
{
    struct sha256 h;
    sha256_init(&h);
    sha256_update(&h, rng_state, sizeof rng_state);
    sha256_update(&h, data, (size_t)len);
    sha256_final(&h, rng_state);
}

static void rng_seed(void)
{
    uint64_t seed[12];
    int n = 0;
    seed[n++] = rdtsc();
    seed[n++] = timer_ticks();
    seed[n++] = (uint64_t)&seed;
    seed[n++] = (uint64_t)&rng_state;

    int has_rdseed = cpu_has_rdseed();
    int has_rdrand = cpu_has_rdrand();
    if (!has_rdseed && !has_rdrand)
        kprintf("[rng] WARNING: no rdseed/rdrand; entropy falls back to rdtsc (weak)\n");
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        int got = 0;
        /* rdseed/rdrand can transiently fail (CF=0) under contention: retry a few
         * times before degrading to rdtsc. */
        for (int t = 0; t < 16 && !got; t++) {
            if (has_rdseed)      got = rdseed64(&v);
            else if (has_rdrand) got = rdrand64(&v);
            else break;
        }
        if (got) seed[n++] = v;
        else seed[n++] = rdtsc() ^ ((uint64_t)timer_ticks() << (i + 1));
    }

    rng_absorb(seed, n * (int)sizeof(seed[0]));
    rng_seeded = 1;
}

void kernel_random_bytes(uint8_t *out, int len)
{
    if (!out || len <= 0) return;
    uint64_t fl = spin_lock_irqsave(&rng_lock);
    if (!rng_seeded) rng_seed();

    int has_rdseed = cpu_has_rdseed();
    int has_rdrand = cpu_has_rdrand();
    for (int off = 0; off < len;) {
        uint64_t extra[3];
        extra[0] = ++rng_counter;
        extra[1] = rdtsc();
        extra[2] = timer_ticks();
        uint64_t hw;
        if (has_rdseed && rdseed64(&hw)) extra[2] ^= hw;
        else if (has_rdrand && rdrand64(&hw)) extra[2] ^= hw;
        rng_absorb(extra, sizeof extra);

        int n = len - off;
        if (n > (int)sizeof rng_state) n = (int)sizeof rng_state;
        memcpy(out + off, rng_state, (size_t)n);
        off += n;
    }
    spin_unlock_irqrestore(&rng_lock, fl);
}
