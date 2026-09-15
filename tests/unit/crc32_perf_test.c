#define _POSIX_C_SOURCE 200809L

/* Correctness, watched structural negative control and host attribution bench
 * for c/drivers/block/crc32.c.  The bitwise oracle shares only the published
 * polynomial, and the benchmark compares separately compiled byte-table and
 * exact former nibble-table objects in alternating order.
 *
 * Host timing attributes this isolated CRC path.  Guest AEX launch cycles are
 * required before using it as an OS/browser launch result.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crc32.h"

#ifdef CRC32_PERF_COMPARE
extern uint32_t crc32_update_current(uint32_t, const void *, size_t);
extern uint32_t crc32_update_nibble(uint32_t, const void *, size_t);

typedef uint32_t (*update_fn)(uint32_t, const void *, size_t);

static volatile uint32_t bench_sink;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t time_one(update_fn fn, const unsigned char *buf, size_t len,
                         unsigned calls, uint32_t *answer)
{
    uint32_t crc = CRC32_INIT;
    uint64_t begin = now_ns();
    for (unsigned i = 0; i < calls; i++)
        crc = fn(crc ^ i, buf, len);
    uint64_t elapsed = now_ns() - begin;
    bench_sink ^= crc;
    *answer = crc;
    return elapsed;
}

static void sort_u64(uint64_t *a, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t v = a[i];
        unsigned j = i;
        while (j && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

int main(void)
{
    enum { ROUNDS = 11, CALLS = 8 };
    const size_t len = 16u * 1024u * 1024u;
    unsigned char *buf = malloc(len);
    if (!buf) return 2;
    uint32_t s = 0x13579bdfu;
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (unsigned char)s;
    }

    uint32_t warm;
    (void)time_one(crc32_update_current, buf, len, 1, &warm);
    (void)time_one(crc32_update_nibble, buf, len, 1, &warm);

    uint64_t current[ROUNDS], nibble[ROUNDS];
    int same = 1;
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint32_t a, b;
        if (r & 1) {
            nibble[r] = time_one(crc32_update_nibble, buf, len, CALLS, &b);
            current[r] = time_one(crc32_update_current, buf, len, CALLS, &a);
        } else {
            current[r] = time_one(crc32_update_current, buf, len, CALLS, &a);
            nibble[r] = time_one(crc32_update_nibble, buf, len, CALLS, &b);
        }
        if (a != b) same = 0;
        printf("round=%u current_ns=%llu nibble_ns=%llu\n", r + 1,
               (unsigned long long)current[r],
               (unsigned long long)nibble[r]);
    }
    sort_u64(current, ROUNDS);
    sort_u64(nibble, ROUNDS);
    uint64_t cm = current[ROUNDS / 2], nm = nibble[ROUNDS / 2];
    double ratio = (double)cm / (double)nm;
    double mib = (double)len * CALLS / 1048576.0;
    printf("median current_ns=%llu nibble_ns=%llu ratio=%.4f "
           "current_mib_s=%.2f nibble_mib_s=%.2f sink=%08x\n",
           (unsigned long long)cm, (unsigned long long)nm, ratio,
           mib * 1e9 / (double)cm, mib * 1e9 / (double)nm,
           (unsigned)bench_sink);
    free(buf);
    if (!same) { puts("FAIL: byte and nibble implementations agree"); return 1; }
    if (!(ratio < 0.80)) {
        puts("FAIL: byte-table median is at least 20% faster than nibble control");
        return 1;
    }
    puts("crc32 interleaved host bench: PASS");
    return 0;
}

#else
static int checks, failures;
static volatile uint64_t lookup_count;

void crc32_test_count_lookup(void) { lookup_count++; }

static void check(int ok, const char *why)
{
    checks++;
    if (ok) printf("ok: %s\n", why);
    else { printf("FAIL: %s\n", why); failures++; }
}

static uint32_t bitwise_update(uint32_t crc, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

int main(void)
{
    unsigned char buf[4096];
    uint32_t s = 0x9e3779b9u;
    for (size_t i = 0; i < sizeof buf; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (unsigned char)s;
    }

    check(crc32("123456789", 9) == 0xcbf43926u,
          "IEEE CRC-32 check value");
    check(crc32("", 0) == 0u, "empty input");

    int all = 1;
    for (size_t n = 0; n <= sizeof buf; n++)
        if (crc32_update(CRC32_INIT, buf, n) !=
            bitwise_update(CRC32_INIT, buf, n)) all = 0;
    check(all, "all lengths through 4096 match independent bitwise oracle");

    all = 1;
    s = 0x243f6a88u;
    for (unsigned i = 0; i < 1024; i++) {
        s = s * 1664525u + 1013904223u;
        size_t off = (s >> 12) & 2047u;
        size_t n = (s >> 3) & 2047u;
        if (crc32_update(s, buf + off, n) != bitwise_update(s, buf + off, n))
            all = 0;
    }
    check(all, "random initial states and slices match bitwise oracle");

    uint32_t one = crc32_update(CRC32_INIT, buf, sizeof buf);
    uint32_t split = CRC32_INIT;
    for (size_t off = 0; off < sizeof buf;) {
        size_t n = ((off * 17u + 11u) % 61u) + 1u;
        if (n > sizeof buf - off) n = sizeof buf - off;
        split = crc32_update(split, buf + off, n);
        off += n;
    }
    check(split == one, "fragmented streaming equals one-shot update");

    lookup_count = 0;
    (void)crc32_update(CRC32_INIT, buf, sizeof buf);
    check(lookup_count == sizeof buf,
          "one table lookup per byte invariant");

    printf("crc32 fast path: %d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif
