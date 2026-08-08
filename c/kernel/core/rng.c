#include <stdint.h>
#include <stddef.h>
#include "rng.h"
#include "pit.h"
#include "crypto.h"
#include "cpufeat.h"
#include "spinlock.h"
#include "kprintf.h"
#include "usercopy.h"

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

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* RDRAND/RDSEED availability now comes from the shared CPUID decode in
 * c/kernel/cpu/cpufeat.c rather than a private copy of the leaf-1/leaf-7
 * query that used to live here. Same answers -- the leaf-7-only-if-max-leaf>=7
 * guard that made the old rdseed check correct is the general rule there --
 * but they are now cached, so rng_gather() no longer executes two CPUIDs (a
 * serialising instruction) per reseed, and there is one place to look when a
 * feature bit is wrong. */
#define cpu_has_rdrand() cpu_has(CPU_RDRAND)
#define cpu_has_rdseed() cpu_has(CPU_RDSEED)

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
    crypto_wipe(&h, sizeof h);          /* the context held the old state */
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
    crypto_wipe(ent, sizeof ent);
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
        crypto_wipe(block, sizeof block);                    /* block is pre-image of nothing public */
    }
    spin_unlock_irqrestore(&rng_lock, fl);
}

/* --- SYS_GETRANDOM: the DRBG, reachable from ring 3 ------------------------
 *
 * Until this existed the kernel had a real Hash_DRBG and NOTHING in userland
 * could reach it. There is no /dev/urandom here, no RDRAND wrapper in libc and
 * no pool syscall, so `crypto.getRandomValues` in the browser was xorshift128+
 * seeded from clocks and heap addresses -- and said so, in capitals, in
 * c/apps/browser/js_platform.c. Every CSRF token, UUID and WebCrypto shim on
 * every page was predictable from a few observations. This is the syscall that
 * makes that excuse unnecessary.
 *
 * Contract. The NUMBER (SYS_GETRANDOM = 114) and the flag names were
 * arbitrated by the threads line in include/abi/logit_abi.h; this is the back
 * end behind it, and it implements what that header publishes:
 *   getrandom(buf, len, flags)
 *     buf != NULL : fills min(len, GRND_MAX) bytes and returns that count.
 *                   -1 on a bad range or a negative len. GRND_NONBLOCK and
 *                   GRND_RANDOM are accepted and change nothing -- the DRBG is
 *                   seeded before ring 3 exists, so there is no "not ready"
 *                   state to block on.
 *     buf == NULL : returns 1 if a hardware entropy source (RDSEED/RDRAND) is
 *                   backing the DRBG, 0 if it has fallen back to rdtsc. A
 *                   caller about to generate a long-term key can refuse to; a
 *                   caller that wants a request id does not care.
 *
 * THE SHORT READ, and what to do about it. The published ABI caps one call at
 * GRND_MAX and returns the clamped count rather than an error -- the same shape
 * as Linux's getrandom(), and the same footgun: the failure mode of ignoring
 * the return value is a key with a zero tail, and it is invisible in testing
 * because the truncation only bites above 4 KiB. The cap itself is right (one
 * call must not hold the RNG lock for an unbounded time). The mitigation is
 * that EVERY caller-side wrapper loops -- see the getrandom() wrapper in
 * userland and js_platform.c's fill -- so no caller in this tree can observe a
 * short count. A new caller that reads the return value as "success" is the
 * bug this paragraph exists to make findable.
 *
 * The copy-out goes through a bounded kernel staging buffer rather than
 * generating straight into the user page: kernel_random_bytes runs with the RNG
 * spinlock held and IF off, and a user page can fault (copy-on-write, or an
 * mmap'd page not yet materialised). user_range_ok resolves those faults BEFORE
 * the lock is taken; the staging buffer means that even if it did not, the
 * generator never touches a user address. */
#define GETRANDOM_CHUNK 256
#define GETRANDOM_MAX   4096                         /* == GRND_MAX in the ABI */

long rng_syscall(long ubuf, long len, long flags)
{
    (void)flags;                                     /* GRND_* are all no-ops */
    if (!ubuf) return rng_strong();                  /* the "is it strong?" query */
    if (len < 0) return -1;
    if (len > GETRANDOM_MAX) len = GETRANDOM_MAX;    /* clamp, per the ABI */
    if (len == 0) return 0;
    if (!user_range_ok((const void *)(uintptr_t)ubuf, (uint64_t)len, 1)) return -1;

    uint8_t chunk[GETRANDOM_CHUNK];
    long done = 0;
    while (done < len) {
        int n = (int)(len - done);
        if (n > GETRANDOM_CHUNK) n = GETRANDOM_CHUNK;
        kernel_random_bytes(chunk, n);
        if (user_copy_to((void *)(uintptr_t)(ubuf + done), chunk, (uint64_t)n) != 0) {
            crypto_wipe(chunk, sizeof chunk);
            return -1;
        }
        done += n;
    }
    /* The staging buffer held generator output that the caller now owns; it
     * must not stay legible in a kernel stack frame the next syscall reuses. */
    crypto_wipe(chunk, sizeof chunk);
    return done;
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
