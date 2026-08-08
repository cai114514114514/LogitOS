/* js_hash_test -- the atom hash patch computes the SAME number as the code it
 * replaced.
 *
 * WHY THIS IS A TEST AND NOT A CODE REVIEW
 * QuickJS stores hash_string8()'s result inside every JSString and compares it
 * against hash_string16()'s result for the same characters when deciding
 * whether two strings are the same atom. A "faster hash" that hashes just as
 * WELL but differently would not crash and would not fail any language test --
 * it would quietly let the same identifier become two atoms in the narrow and
 * wide paths, and the only symptom would be a wrong answer somewhere deep in a
 * page months later. So the requirement is not "a good hash", it is "bit for
 * bit the old hash", and that is what this checks: every length from 0 to 64
 * (all four residues of the unrolled loop), the byte extremes, and a large
 * random corpus, against the upstream recurrence written out longhand.
 *
 * It also checks the invariant the patch's comment turns on: hash_string8 and
 * hash_string16 agree for the same characters.
 *
 * Includes quickjs.c directly -- hash_string8 is static and this test is about
 * an internal, not an API.
 */
#include "quickjs.c"
#undef malloc
#undef free
#undef realloc
#include <stdio.h>

/* upstream QuickJS 2024-01-13, verbatim */
static uint32_t hash_ref(const uint8_t *str, size_t len, uint32_t h)
{
    size_t i;
    for (i = 0; i < len; i++)
        h = h * 263 + str[i];
    return h;
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    uint8_t buf[128];
    uint16_t wide[128];
    long checks = 0, bad = 0;

    static const uint32_t seeds[] = {
        0, 1, JS_ATOM_TYPE_STRING, JS_ATOM_TYPE_SYMBOL,
        0xffffffffu, 0x80000000u, 263u, 489383265u,
    };

    /* every length 0..64 (covers all four residues of the unrolled loop),
     * with several byte patterns each */
    for (size_t len = 0; len <= 64; len++) {
        for (int pat = 0; pat < 5; pat++) {
            for (size_t i = 0; i < len; i++) {
                switch (pat) {
                case 0: buf[i] = 0; break;
                case 1: buf[i] = 0xff; break;
                case 2: buf[i] = (uint8_t)('a' + (i % 26)); break;
                case 3: buf[i] = (uint8_t)i; break;
                default: buf[i] = (uint8_t)rng(); break;
                }
                wide[i] = buf[i];
            }
            for (size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
                uint32_t want = hash_ref(buf, len, seeds[s]);
                uint32_t got = hash_string8(buf, len, seeds[s]);
                uint32_t got16 = hash_string16(wide, len, seeds[s]);
                checks += 2;
                if (got != want) {
                    if (++bad < 10)
                        printf("FAIL len=%zu pat=%d seed=%u: hash_string8 %u, "
                               "upstream %u\n", len, pat, seeds[s], got, want);
                }
                if (got16 != want) {
                    if (++bad < 10)
                        printf("FAIL len=%zu pat=%d seed=%u: hash_string16 %u "
                               "disagrees with the narrow path %u\n",
                               len, pat, seeds[s], got16, want);
                }
            }
        }
    }

    /* a large random corpus, random lengths */
    for (long n = 0; n < 200000; n++) {
        size_t len = rng() % 65;
        for (size_t i = 0; i < len; i++) { buf[i] = (uint8_t)rng(); wide[i] = buf[i]; }
        uint32_t seed = rng();
        uint32_t want = hash_ref(buf, len, seed);
        checks += 2;
        if (hash_string8(buf, len, seed) != want) {
            if (++bad < 10) printf("FAIL random len=%zu\n", len);
        }
        if (hash_string16(wide, len, seed) != want) {
            if (++bad < 10) printf("FAIL random wide len=%zu\n", len);
        }
    }

    if (bad) {
        printf("js_hash_test: %ld of %ld comparisons FAILED -- the atom hash is "
               "not the one QuickJS stored in its strings\n", bad, checks);
        return 1;
    }
    printf("js_hash_test: %ld comparisons pass (hash_string8 == upstream == "
           "hash_string16)\n", checks);
    return 0;
}
