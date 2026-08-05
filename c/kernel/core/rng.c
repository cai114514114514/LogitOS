#include <stdint.h>
#include <stddef.h>
#include "rng.h"
#include "pit.h"
#include "crypto.h"
#include "spinlock.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

/* SHA-256 Hash_DRBG-style kernel PRNG with state/output separation.
 *
 * Structure:
 *   reseed:    state = H(state || 0x00 || entropy)
 *   generate:  block = H(state || 0x01 || counter)   -> 32 bytes of output
 *              state = H(state || 0x02 || counter)   -> state evolves per block
 *
 * The output never IS the internal state (the pre-refactor design hashed the
 * state and emitted it verbatim, so one state compromise exposed every past
 * and future output). Here a compromised state cannot rewind: earlier states
 * are one hash pre-image away, so earlier outputs stay sealed (forward
 * secrecy within a reseed epoch).
 *
 * Entropy enters at first use and then periodically -- every
 * RNG_RESEED_REQUESTS generate calls or RNG_RESEED_BYTES of output,
 * whichever comes first -- from RDSEED/RDRAND when the CPU offers them. */

static uint8_t rng_state[32];
static uint64_t rng_counter;                 /* block counter within an epoch */
static uint64_t rng_requests;                /* generate calls this epoch */
static uint64_t rng_bytes;                   /* bytes produced this epoch */
static int rng_seeded;
/* BKL-free callers can reach kernel_random_bytes concurrently: guard the state. */
static spinlock_t rng_lock = SPINLOCK_INIT;

#define RNG_RESEED_REQUESTS 1024ULL
#define RNG_RESEED_BYTES    (1024ULL*1024ULL)

/* Overwrite key material through a volatile pointer so the compiler cannot
 * elide it as a dead store. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

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

/* out = H(state || tag || data). `out` may alias rng_state: the state is
 * absorbed into the hash context before anything is written. */
static void rng_hash(uint8_t tag, const void *data, size_t len, uint8_t out[32])
{
    struct sha256 h;
    sha256_init(&h);
    sha256_update(&h, rng_state, sizeof rng_state);
    sha256_update(&h, &tag, 1);
    if (data && len) sha256_update(&h, data, len);
    sha256_final(&h, out);
    wipe(&h, sizeof h);          /* the context held the old state */
}

/* Collect entropy: up to 4 hardware words (RDSEED preferred, RDRAND else,
 * each retried against transient CF=0 under contention), then rdtsc + tick
 * count as a cheap always-available (but weak) supplement. */
static int rng_gather(uint64_t *buf)
{
    int n = 0;
    int has_rdseed = cpu_has_rdseed();
    int has_rdrand = cpu_has_rdrand();
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        int got = 0;
        for (int t = 0; t < 16 && !got; t++) {
            if (has_rdseed)      got = rdseed64(&v);
            else if (has_rdrand) got = rdrand64(&v);
            else break;
        }
        if (got) buf[n++] = v;
    }
    buf[n++] = rdtsc();
    buf[n++] = timer_ticks();
    return n;
}

/* state = H(state || 0x00 || fresh entropy); starts a new epoch. */
static void rng_reseed(void)
{
    uint64_t ent[8];
    int n = rng_gather(ent);
    rng_hash(0x00, ent, (size_t)n * sizeof(uint64_t), rng_state);
    wipe(ent, sizeof ent);
    rng_counter = 0;
    rng_requests = 0;
    rng_bytes = 0;
}

static void rng_seed(void)
{
    if (!cpu_has_rdseed() && !cpu_has_rdrand())
        kprintf("[rng] WARNING: no rdseed/rdrand; entropy falls back to rdtsc (weak)\n");
    rng_reseed();
    rng_seeded = 1;
}

void kernel_random_bytes(uint8_t *out, int len)
{
    if (!out || len <= 0) return;
    uint64_t fl = spin_lock_irqsave(&rng_lock);
    if (!rng_seeded) rng_seed();

    /* periodic reseed: bound how much output one epoch of entropy covers */
    if (rng_requests >= RNG_RESEED_REQUESTS || rng_bytes >= RNG_RESEED_BYTES)
        rng_reseed();
    rng_requests++;
    rng_bytes += (uint64_t)len;

    for (int off = 0; off < len; ) {
        uint8_t ctr[8];
        for (int i = 0; i < 8; i++) ctr[i] = (uint8_t)(rng_counter >> (8*i));
        uint8_t block[32];
        rng_hash(0x01, ctr, sizeof ctr, block);       /* the output block */
        rng_hash(0x02, ctr, sizeof ctr, rng_state);   /* evolve the state */
        rng_counter++;
        int n = len - off;
        if (n > (int)sizeof block) n = (int)sizeof block;
        memcpy(out + off, block, (size_t)n);
        off += n;
        wipe(block, sizeof block);                    /* block is pre-image of nothing public */
    }
    spin_unlock_irqrestore(&rng_lock, fl);
}

/* Test hooks (tests/unit/rng_test.c): let the host-side structural test peek
 * at the internals so it can assert output != state, state evolution, and
 * that the periodic reseed really fires. Not used by the kernel. */
void rng_test_state(uint8_t out[32], uint64_t *counter)
{
    uint64_t fl = spin_lock_irqsave(&rng_lock);
    memcpy(out, rng_state, 32);
    *counter = rng_counter;
    spin_unlock_irqrestore(&rng_lock, fl);
}

void rng_test_force_reseed(void)
{
    uint64_t fl = spin_lock_irqsave(&rng_lock);
    if (!rng_seeded) rng_seed();
    rng_reseed();
    spin_unlock_irqrestore(&rng_lock, fl);
}
