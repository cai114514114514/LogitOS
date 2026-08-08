/* /bin/entropy -- prove that ring 3 reaches the kernel DRBG, and that the
 * thing it reaches is not the old clock-seeded PRNG wearing its name.
 *
 * A statistical smoke test would not do. xorshift128+ passes every statistical
 * test this machine could run; what was wrong with it was never the
 * distribution, it was that the SEED was two clocks and two heap addresses.
 * So the assertions here are about REACHABILITY and UNPREDICTABILITY, and each
 * one is a property the old generator visibly fails:
 *
 *   A. two reads in one process differ            (a constant would fail)
 *   B. a forked child and its parent differ       -- THE classic bug: a
 *      userland PRNG seeded once at process start is copied by fork(), and
 *      both halves then produce the identical stream. This is the assertion
 *      that says the state lives in the kernel and not in this address space.
 *   C. two SEPARATE RUNS of this same program differ. This is the one the
 *      negative control turns red: the old seed was SYS_GET_TIME (whole
 *      seconds) mixed with malloc addresses, and two runs of the same binary
 *      inside the same second have the same second and the same allocator
 *      layout -- so they produce byte-identical output. That is not a
 *      contrived weakness, it is the defect, and it is fully deterministic.
 *
 * Output is greppable, one line per fact, so tests/boot/run-entropy-test.sh
 * does the comparing rather than trusting a verdict this program prints about
 * itself.
 *
 * ENTROPY_CONTROL_XORSHIFT builds the SAME program against the generator that
 * c/apps/browser/js_platform.c used to ship -- same seeding, same algorithm --
 * so the control is the code that was really there, not a strawman. */
#include "clib.h"

#define N 32

#ifdef ENTROPY_CONTROL_XORSHIFT
/* --- the NEGATIVE CONTROL: js_platform.c's generator, verbatim ------------ */
static unsigned long long g_s0, g_s1;
static int g_seeded;

static void ctl_seed(void)
{
    /* Same three ingredients the old code used: the wall clock, the monotonic
     * clock, and heap addresses. There is no malloc in clib, so the stack and
     * a static address stand in for the allocator layout -- which if anything
     * makes the control HARDER to fail, since a stack address is at least as
     * variable across runs as a fresh-process heap address is. */
    struct logit_time wt; get_time(&wt);
    unsigned long long secs = (unsigned long long)wt.hour * 3600u +
                              (unsigned long long)wt.minute * 60u + (unsigned long long)wt.second;
    unsigned long long x = secs * 0x9E3779B97F4A7C15ull;
    x ^= (unsigned long long)(unsigned long)&g_s0 * 0xBF58476D1CE4E5B9ull;
    x ^= (unsigned long long)(unsigned long)&x << 17;
    x ^= (unsigned long long)_sys(SYS_MONOTONIC_MS, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        x += 0x9E3779B97F4A7C15ull;
        unsigned long long z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        if (i == 0) g_s0 = z | 1; else g_s1 = z | 1;
    }
    g_seeded = 1;
}

static unsigned long long ctl_next(void)
{
    unsigned long long s1 = g_s0, s0 = g_s1;
    g_s0 = s0;
    s1 ^= s1 << 23;
    g_s1 = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return g_s1 + s0;
}

static int fill(unsigned char *b, int n)
{
    if (!g_seeded) ctl_seed();
    for (int i = 0; i < n; ) {
        unsigned long long r = ctl_next();
        for (int k = 0; k < 8 && i < n; k++, i++) b[i] = (unsigned char)(r >> (k * 8));
    }
    return 0;
}
#else
static int fill(unsigned char *b, int n) { return getrandom_bytes(b, n); }
#endif

static void puthex(const unsigned char *b, int n)
{
    static const char H[] = "0123456789abcdef";
    char line[2 * N + 1];
    for (int i = 0; i < n; i++) { line[2*i] = H[b[i] >> 4]; line[2*i+1] = H[b[i] & 15]; }
    line[2*n] = 0;
    outs(line);
}

static void emit(const char *tag, const unsigned char *b, int n)
{ outs(tag); outs(" "); puthex(b, n); outs("\n"); }

static int same(const unsigned char *a, const unsigned char *b, int n)
{ for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned char a[N], b[N], c[N];
    int bad = 0;

    /* A: two reads, one process. */
    if (fill(a, N) != 0) { outs("ENT_FAIL getrandom refused\n"); return 1; }
    if (fill(b, N) != 0) { outs("ENT_FAIL getrandom refused\n"); return 1; }
    emit("ENT_A", a, N);
    emit("ENT_B", b, N);
    if (same(a, b, N)) { outs("ENT_FAIL two reads were identical\n"); bad = 1; }

    /* An all-zero read is what a stub returns; it would pass "A != B" only by
     * accident, so it is checked outright. */
    {
        int z = 0; for (int i = 0; i < N; i++) z |= a[i];
        if (!z) { outs("ENT_FAIL first read was all zero\n"); bad = 1; }
    }

    /* Bounds: the syscall must refuse a range that is not ours rather than
     * writing into it. A NULL buffer is the "is it strong?" query, so the
     * out-of-range probe uses a kernel-side address instead. */
    {
        long r = _sys(SYS_GETRANDOM, (long)0xFFFF800000000000ULL, 16, 0);
        outs("ENT_BADPTR "); outn(r); outs("\n");
        if (r >= 0) { outs("ENT_FAIL a kernel address was accepted\n"); bad = 1; }
    }

    /* Is the DRBG hardware-seeded on this machine? Reported, never asserted:
     * QEMU/TCG may or may not offer RDSEED, and the test must not depend on
     * which host it runs on. */
    outs("ENT_STRONG "); outn(getrandom_strong()); outs("\n");

    /* B: fork. The child reads first, the parent second; a per-process PRNG
     * copied by fork would hand both the same bytes regardless of order. */
    {
        int pid = sys_fork();
        if (pid == 0) {
            if (fill(c, N) != 0) { outs("ENT_FAIL child getrandom refused\n"); return 1; }
            emit("ENT_CHILD", c, N);
            return 0;
        }
        int st = 0;
        sys_waitpid(pid, &st);
        if (fill(c, N) != 0) { outs("ENT_FAIL parent getrandom refused\n"); return 1; }
        emit("ENT_PARENT", c, N);
    }

    outs(bad ? "ENTROPY_SELF_BAD\n" : "ENTROPY_SELF_OK\n");
    return bad;
}
