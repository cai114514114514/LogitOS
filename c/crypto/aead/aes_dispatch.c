/* Runtime implementation selection.
 *
 * The mechanism, deliberately boring: a table of candidate backends in
 * preference order, each of which reports at runtime whether it can run on
 * this CPU; the first that says yes wins; the last entry is the portable C
 * reference and always says yes. Selection happens once and stores a pointer
 * to a const struct, so the steady-state cost is one indirect call per AES
 * block and there is nothing to lock.
 *
 * Three properties are on purpose:
 *
 *  - The baseline is never deleted and never unreachable. It is the reference
 *    the accelerated path is tested against, it is what runs on a CPU without
 *    AES-NI, and crypto_simd_force_baseline() lets a test exercise it on a
 *    machine that does have AES-NI. A fallback that has silently never run is
 *    not a fallback.
 *  - Selection is explicit and reportable: crypto_simd_backend_name() is what
 *    the boot line prints and what the tests assert on. "It probably picked
 *    the fast one" is not a claim anyone can check.
 *  - Nothing here is AES-specific except the table. Adding a second
 *    dispatched routine (the obvious next one is mini-libc's memcpy/strlen)
 *    means another table and another pointer, not another mechanism.
 *
 * Init ordering: crypto_simd_init() is called from the boot path, but every
 * entry point also initialises lazily, because host tests and any early
 * caller must not depend on someone else having gone first. */

#include "crypto.h"
#include "aes_backend.h"

static const struct aes_backend *g_aes;      /* NULL until selected */
static int g_force_baseline;

static void select_aes(void)
{
    const struct aes_backend *pick = 0;
    if (!g_force_baseline)
        pick = aes_backend_ni();
    if (!pick)
        pick = aes_backend_c();
    g_aes = pick;
}

const struct aes_backend *aes_current_backend(void)
{
    if (!g_aes) select_aes();
    return g_aes;
}

void crypto_simd_init(void)
{
    select_aes();
}

const char *crypto_simd_backend_name(void)
{
    return aes_current_backend()->name;
}

int crypto_simd_constant_time(void)
{
    return aes_current_backend()->constant_time;
}

void crypto_simd_force_baseline(int on)
{
    g_force_baseline = on ? 1 : 0;
    select_aes();
}

/* Cross-check the selected backend against the portable reference on a fixed
 * input, covering all three primitives (key schedule, block encrypt, GF
 * multiply) for both key lengths. Returns 0 when they agree, or the 1-based
 * index of the first check that did not.
 *
 * This is not a substitute for the vectors -- two identically wrong backends
 * would agree -- so the caller runs it alongside a known-answer check. What it
 * catches is the case the vectors cannot: a backend that is correct when the
 * machine is quiet and wrong when an interrupt lands in the middle of it. That
 * is why the boot selftest calls this in a loop with interrupts enabled. */
int crypto_simd_selftest(void)
{
    const struct aes_backend *ref = aes_backend_c();
    const struct aes_backend *cur = aes_current_backend();

    static const uint8_t k32[32] = {
        0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
        0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4 };
    static const uint8_t blk[16] = {
        0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34 };

    uint8_t ra[240], rb[240], oa[16], ob[16];
    int step = 0;

    for (int two = 0; two < 2; two++) {
        int keylen = two ? 32 : 16;
        int nr = two ? 14 : 10;
        int n = 16 * (nr + 1);

        for (int i = 0; i < 240; i++) { ra[i] = 0; rb[i] = 0; }
        ref->key_expand(k32, keylen, ra);
        cur->key_expand(k32, keylen, rb);
        step++;
        for (int i = 0; i < n; i++) if (ra[i] != rb[i]) return step;

        ref->encrypt(ra, nr, blk, oa);
        cur->encrypt(rb, nr, blk, ob);
        step++;
        for (int i = 0; i < 16; i++) if (oa[i] != ob[i]) return step;
    }

    /* GF multiply: chain it so an error in any bit position propagates. */
    for (int i = 0; i < 16; i++) { oa[i] = blk[i]; ob[i] = blk[i]; }
    for (int r = 0; r < 8; r++) {
        ref->gf_mul(oa, k32);
        cur->gf_mul(ob, k32);
    }
    step++;
    for (int i = 0; i < 16; i++) if (oa[i] != ob[i]) return step;

    return 0;
}
