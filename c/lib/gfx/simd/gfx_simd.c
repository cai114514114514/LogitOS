#include "gfx_simd.h"

static uint64_t (*glass_clock)(void);
static struct gfx_glass_prof glass_profile;

/* The library also runs in ring 3 and host tests: inject the clock rather than
 * importing time_mono_ns. Publishing atomically keeps band readers race-free. */
void gfx_set_clock(uint64_t (*ns)(void))
{
    __atomic_store_n(&glass_clock, ns, __ATOMIC_RELEASE);
}

/* Read once per stage boundary. An unconfigured library keeps counting calls
 * and pixels with zero durations; it never invents a timing source. */
uint64_t gfx_now_ns(void)
{
    uint64_t (*ns)(void) = __atomic_load_n(&glass_clock, __ATOMIC_ACQUIRE);
    return ns ? ns() : 0;
}

/* Band workers finish independently. Relaxed adds preserve every contribution
 * without imposing ordering on pixels; their existing completion barrier owns
 * that ordering. Memo counters deliberately stay zero until S4 exists. */
void gfx_glass_record(uint64_t px, uint64_t blur_ns, uint64_t render_ns)
{
    __atomic_fetch_add(&glass_profile.calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.px, px, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.blur_ns, blur_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.render_ns, render_ns, __ATOMIC_RELAXED);
}

/* Return cumulative storage so reporting can take relaxed atomic field loads
 * while workers run. A plain struct copy would race with their atomic adds. */
const struct gfx_glass_prof *gfx_glass_prof(void)
{
    return &glass_profile;
}
