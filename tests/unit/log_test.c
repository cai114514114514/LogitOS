/* Host unit test for the kernel log ring (c/kernel/core/klog.c) and the
 * formatter it shares with kprintf (c/kernel/core/kprintf.c).
 *
 * The ring is the one part of the diagnostic subsystem whose logic is fully
 * isolable: it is static memory, a leaf lock and a per-CPU line buffer, with
 * no device in it. So it is tested here rather than only on the machine --
 * wraparound, truncation, level filtering, full-ring behaviour and the
 * interleaving of two producers all take one second here and one boot there.
 *
 * tests/unit/klogstub/ shadows five kernel headers (the same trick
 * tests/unit/kheapstub uses): the interrupt guard, the spinlock, per-CPU
 * identity, the timer and the two console sinks become things this file
 * implements and can observe.
 *
 * THE NEGATIVE CONTROL. Build this with -DKLOG_UNSAFE (`make test-klog-control`)
 * and klog.c loses its interrupt guard and its per-CPU line buffers -- the
 * naive logger. Case 7 then fails: two producers interleaving mid-line
 * scramble each other's records. That is the control for the interrupt-context
 * claim; the on-device half is `echo irqstorm > /dev/ktrigger`.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "klog.h"
#include "kprintf.h"
#include "spinlock.h"     /* all five of these resolve to tests/unit/klogstub/ */
#include "percpu.h"
#include "kirq.h"
#include "pit.h"
#include "vga.h"
#include "serial.h"

/* ---- stub implementations ------------------------------------------------ */

static struct cpu g_cpu = { 0 };
struct cpu *this_cpu(void) { return &g_cpu; }
void klogtest_set_cpu(int i) { g_cpu.index = i; }

static int g_lock_depth, g_lock_maxdepth, g_lock_taken;
uint64_t spin_lock_irqsave(spinlock_t *l)
{
    (void)l;
    g_lock_depth++;
    g_lock_taken++;
    if (g_lock_depth > g_lock_maxdepth) g_lock_maxdepth = g_lock_depth;
    return 0x202;
}
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; g_lock_depth--; }
void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }

static int g_irq_depth, g_irq_maxdepth;
uint64_t kirq_save(void) { g_irq_depth++; if (g_irq_depth > g_irq_maxdepth) g_irq_maxdepth = g_irq_depth; return 0x202; }
void kirq_restore(uint64_t f) { (void)f; g_irq_depth--; }

static uint64_t g_ms = 0;
uint64_t timer_ms(void) { return g_ms; }
uint64_t timer_ticks(void) { return g_ms / 10; }
void pit_init(uint32_t hz) { (void)hz; }
void timer_tick(void) {}

/* Console sinks: what a human watching the serial port would have seen. */
static char g_con[1 << 20];
static int  g_con_n;
static void con_putc(char c) { if (g_con_n < (int)sizeof g_con - 1) g_con[g_con_n++] = c; g_con[g_con_n] = 0; }
void vga_putc(char c) { (void)c; }
void vga_puts(const char *s) { (void)s; }
void vga_clear(void) {}
void vga_set_color(enum vga_color f, enum vga_color b) { (void)f; (void)b; }
void serial_putc(char c) { con_putc(c); }
void serial_puts(const char *s) { while (*s) con_putc(*s++); }
void serial_init(void) {}
int  serial_getc(void) { return -1; }

static void con_reset(void) { g_con_n = 0; g_con[0] = 0; }

/* ---- test harness -------------------------------------------------------- */

static int g_checks, g_fails;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static int rec_is(const struct klog_rec *r, const char *want)
{
    int n = (int)strlen(want);
    return r->len == n && memcmp(r->text, want, n) == 0;
}

static void rec_str(const struct klog_rec *r, char *out, int max)
{
    int n = r->len < max - 1 ? r->len : max - 1;
    memcpy(out, r->text, n);
    out[n] = 0;
}

/* ---- cases --------------------------------------------------------------- */

static void t_basic(void)
{
    printf("1. a kprintf line reaches BOTH the console and the ring\n");
    con_reset();
    uint64_t before = klog_next_seq();
    g_ms = 1234;
    kprintf("hello %d %s\n", 42, "world");

    CHECK(strcmp(g_con, "hello 42 world\n") == 0, "console got '%s'", g_con);
    CHECK(klog_next_seq() == before + 1, "one record expected, seq moved by %llu",
          (unsigned long long)(klog_next_seq() - before));

    struct klog_rec r;
    CHECK(klog_get(before, &r), "record %llu readable", (unsigned long long)before);
    CHECK(rec_is(&r, "hello 42 world"), "record text");
    CHECK(r.level == KL_INFO, "kprintf records at KL_INFO, got %d", r.level);
    CHECK(r.ms == 1234, "timestamp %llu", (unsigned long long)r.ms);
    CHECK(r.cpu == 0, "cpu %d", r.cpu);

    /* A record is one LINE, even when it took several kprintf calls to build
     * -- which is how most of this kernel prints. */
    before = klog_next_seq();
    kprintf("part one, ");
    kprintf("part two %x\n", 0xbeef);
    CHECK(klog_next_seq() == before + 1, "partial lines join into one record");
    CHECK(klog_get(before, &r) && rec_is(&r, "part one, part two beef"),
          "joined text");

    /* CR is transport, not content: the tty path emits \r\n. */
    before = klog_next_seq();
    kprintf("crlf\r\n");
    CHECK(klog_get(before, &r) && rec_is(&r, "crlf"), "carriage return dropped");
}

static void t_truncate(void)
{
    printf("2. an over-long line is TRUNCATED and the loss is counted\n");
    struct klog_stats a, b;
    klog_get_stats(&a);
    uint64_t seq = klog_next_seq();

    char big[400];
    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = 0;
    kprintf("%s\n", big);

    struct klog_rec r;
    CHECK(klog_get(seq, &r), "the over-long line still produced a record");
    CHECK(r.len == KLOG_TEXT, "kept %u bytes, expected %u", r.len, KLOG_TEXT);
    CHECK(r.dropped == (uint32_t)(sizeof big - 1 - KLOG_TEXT),
          "dropped %u, expected %u", r.dropped,
          (unsigned)(sizeof big - 1 - KLOG_TEXT));

    klog_get_stats(&b);
    CHECK(b.truncated_lines == a.truncated_lines + 1, "truncated_lines counted");
    CHECK(b.truncated_chars == a.truncated_chars + r.dropped, "truncated_chars counted");

    /* The rendered line has to SAY it was truncated -- silently short output
     * is how you spend an afternoon believing a driver stopped printing. */
    char line[512];
    klog_format_rec(&r, line, sizeof line);
    CHECK(strstr(line, "truncated") != NULL, "render marks truncation: %s", line);

    /* Exactly-full and one-over boundaries. */
    char exact[KLOG_TEXT + 1];
    memset(exact, 'e', KLOG_TEXT);
    exact[KLOG_TEXT] = 0;
    seq = klog_next_seq();
    kprintf("%s\n", exact);
    CHECK(klog_get(seq, &r) && r.len == KLOG_TEXT && r.dropped == 0,
          "a line of exactly KLOG_TEXT loses nothing (len=%u dropped=%u)",
          r.len, r.dropped);
}

static void t_wrap(void)
{
    printf("3. the ring wraps: oldest aged out, newest intact, nothing blocks\n");
    uint64_t start = klog_next_seq();
    const int N = KLOG_SLOTS + 137;
    for (int i = 0; i < N; i++)
        kprintf("wrap %d\n", i);

    CHECK(klog_next_seq() == start + N, "all %d writes committed", N);

    uint64_t first = klog_first_seq();
    CHECK(first == klog_next_seq() - KLOG_SLOTS,
          "first_seq trails next_seq by exactly the capacity");

    struct klog_rec r;
    CHECK(!klog_get(start, &r), "the oldest of this batch has aged out");
    CHECK(!klog_get(first - 1, &r), "the record before first_seq is gone");
    CHECK(klog_get(first, &r), "first_seq itself is readable");

    /* Content check, not just a count: an off-by-one in the slot index gives
     * you the right number of readable records with the wrong text in them. */
    char want[32];
    snprintf(want, sizeof want, "wrap %d", (int)(first - start));
    CHECK(rec_is(&r, want), "oldest retained is '%s'", want);

    CHECK(klog_get(klog_next_seq() - 1, &r), "newest is readable");
    snprintf(want, sizeof want, "wrap %d", N - 1);
    CHECK(rec_is(&r, want), "newest is '%s'", want);

    /* Every retained record must be whole -- a partially overwritten slot is
     * the failure mode a naive ring has. */
    int bad = 0;
    for (uint64_t s = klog_first_seq(); s < klog_next_seq(); s++) {
        struct klog_rec x;
        if (!klog_get(s, &x)) { bad++; continue; }
        if (x.seq != s || x.len > KLOG_TEXT || x.level >= KL_NLEVELS) bad++;
    }
    CHECK(bad == 0, "%d malformed records after wrap", bad);
}

static void t_full(void)
{
    printf("4. a FULL ring overwrites and counts; it never blocks or drops silently\n");
    struct klog_stats a, b;
    klog_get_stats(&a);
    const int N = KLOG_SLOTS * 8;
    for (int i = 0; i < N; i++)
        kprintf("flood %d\n", i);
    klog_get_stats(&b);

    CHECK(b.records == a.records + N, "every write was accepted (%llu -> %llu)",
          (unsigned long long)a.records, (unsigned long long)b.records);
    CHECK(b.overwritten == b.records - KLOG_SLOTS,
          "aged-out count = records - capacity (%llu vs %llu)",
          (unsigned long long)b.overwritten,
          (unsigned long long)(b.records - KLOG_SLOTS));
    CHECK(klog_next_seq() - klog_first_seq() == KLOG_SLOTS,
          "exactly `capacity` records are live");

    /* The lock is a LEAF: it must never be entered recursively, or the claim
     * that it cannot participate in an inversion is worth nothing. */
    CHECK(g_lock_maxdepth == 1, "ring lock nesting depth reached %d", g_lock_maxdepth);
    CHECK(g_irq_maxdepth == 1, "interrupt guard nesting depth reached %d", g_irq_maxdepth);
}

static void t_levels(void)
{
    printf("5. level filtering: debug reaches the ring but not the console\n");
    CHECK(klog_console_level() == KL_INFO, "default console level is KL_INFO");

    con_reset();
    uint64_t seq = klog_next_seq();
    kdebug("a driver trace %d", 7);
    CHECK(g_con_n == 0, "kdebug printed '%s' to the console", g_con);
    struct klog_rec r;
    CHECK(klog_get(seq, &r), "kdebug still reached the ring");
    CHECK(r.level == KL_DEBUG, "recorded at KL_DEBUG (%d)", r.level);
    CHECK(rec_is(&r, "a driver trace 7"), "kdebug text");

    con_reset();
    seq = klog_next_seq();
    kwarn("something odd: %d", 3);
    CHECK(strcmp(g_con, "[warn] something odd: 3\n") == 0,
          "warn console line '%s'", g_con);
    CHECK(klog_get(seq, &r) && r.level == KL_WARN, "warn recorded at KL_WARN");

    /* Raising the threshold lets debug through; lowering it silences info. */
    klog_set_console_level(KL_DEBUG);
    con_reset();
    kdebug("now visible");
    CHECK(strcmp(g_con, "now visible\n") == 0, "debug at level DEBUG: '%s'", g_con);

    klog_set_console_level(KL_ERR);
    con_reset();
    kwarn("hidden");
    kinfo("hidden too");
    kerr("shown");
    CHECK(strcmp(g_con, "[err] shown\n") == 0, "level ERR console: '%s'", g_con);

    klog_set_console_level(KL_INFO);

    /* Out-of-range levels clamp rather than indexing off the end of a table. */
    klog_set_console_level(-5);
    CHECK(klog_console_level() == KL_PANIC, "clamped low");
    klog_set_console_level(99);
    CHECK(klog_console_level() == KL_DEBUG, "clamped high");
    klog_set_console_level(KL_INFO);

    /* A line's severity is its WORST part: kprintf (INFO) then an error
     * finishing the same line must record as an error. */
    seq = klog_next_seq();
    klog_putc(KL_INFO, 'a');
    klog_putc(KL_ERR, 'b');
    klog_putc(KL_INFO, '\n');
    CHECK(klog_get(seq, &r) && r.level == KL_ERR, "mixed line takes the worst level");
}

static void t_interleave(void)
{
    printf("6. two cores logging AT THE SAME TIME do not scramble each other\n");
    printf("   (this is the case -DKLOG_UNSAFE is expected to fail)\n");
    uint64_t seq = klog_next_seq();

    /* Character-by-character interleaving is not a contrivance: it is exactly
     * what happens when an interrupt handler logs in the middle of another
     * klog call, which is the situation the per-CPU line buffer exists for. */
    const char *a = "AAAAAAAAAAAAAAAA";
    const char *b = "BBBBBBBBBBBBBBBB";
    for (int i = 0; a[i]; i++) {
        klogtest_set_cpu(0); klog_putc(KL_INFO, a[i]);
        klogtest_set_cpu(1); klog_putc(KL_INFO, b[i]);
    }
    klogtest_set_cpu(0); klog_putc(KL_INFO, '\n');
    klogtest_set_cpu(1); klog_putc(KL_INFO, '\n');
    klogtest_set_cpu(0);

    CHECK(klog_next_seq() == seq + 2, "two producers produced two records, got %llu",
          (unsigned long long)(klog_next_seq() - seq));

    struct klog_rec r0, r1;
    char s0[KLOG_TEXT + 1] = "", s1[KLOG_TEXT + 1] = "";
    if (klog_get(seq, &r0)) rec_str(&r0, s0, sizeof s0);
    if (klog_get(seq + 1, &r1)) rec_str(&r1, s1, sizeof s1);

    CHECK(strcmp(s0, a) == 0, "cpu0's record is its own line, got '%s'", s0);
    CHECK(strcmp(s1, b) == 0, "cpu1's record is its own line, got '%s'", s1);
    CHECK(r0.cpu == 0 && r1.cpu == 1, "records carry the originating cpu (%d,%d)",
          r0.cpu, r1.cpu);
}

static void t_render(void)
{
    printf("7. rendering: what `cat /dev/kmsg` actually shows\n");
    uint64_t seq = klog_next_seq();
    g_ms = 65432;
    klogtest_set_cpu(3);
    kerr("disk %d fell off the bus", 1);
    klogtest_set_cpu(0);

    struct klog_rec r;
    CHECK(klog_get(seq, &r), "record present");
    char line[256];
    int n = klog_format_rec(&r, line, sizeof line);
    CHECK(n > 0 && line[n - 1] == '\n', "rendered line ends in a newline");
    CHECK(strstr(line, "[   65.432]") != NULL, "timestamp rendered: %s", line);
    CHECK(strstr(line, " E ") != NULL, "severity rendered: %s", line);
    CHECK(strstr(line, "cpu3") != NULL, "cpu rendered: %s", line);
    CHECK(strstr(line, "[err] disk 1 fell off the bus") != NULL, "text: %s", line);

    /* A small buffer must truncate, not overrun. */
    char tiny[24];
    memset(tiny, 0x7f, sizeof tiny);
    int m = klog_format_rec(&r, tiny, (int)sizeof tiny);
    CHECK(m < (int)sizeof tiny, "format honours max (%d)", m);
    CHECK(tiny[sizeof tiny - 1] == 0 || m < (int)sizeof tiny - 1, "no overrun");

    /* The whole-ring render is what the synthetic file serves. */
    static char big[1 << 20];
    int total = klog_render(big, (int)sizeof big);
    CHECK(total > 0, "render produced %d bytes", total);
    CHECK(strstr(big, "disk 1 fell off the bus") != NULL, "render contains the record");
    int lines = 0;
    for (int i = 0; i < total; i++) if (big[i] == '\n') lines++;
    CHECK(lines == (int)(klog_next_seq() - klog_first_seq()),
          "render emitted one line per live record (%d vs %llu)", lines,
          (unsigned long long)(klog_next_seq() - klog_first_seq()));

    /* Render into a buffer far too small: it must stop cleanly. */
    char small[200];
    int t2 = klog_render(small, (int)sizeof small);
    CHECK(t2 < (int)sizeof small, "bounded render stayed in bounds (%d)", t2);
}

static void t_format(void)
{
    printf("8. the shared formatter (ksnprintf), including the 64-bit widths\n");
    char b[128];

    ksnprintf(b, sizeof b, "%d %u %x %s %c%%", -5, 7u, 0xabcu, "str", 'q');
    CHECK(strcmp(b, "-5 7 abc str q%") == 0, "basic conversions: '%s'", b);

    ksnprintf(b, sizeof b, "[%5d][%-5d][%05d]", 42, 42, 42);
    CHECK(strcmp(b, "[   42][42   ][00042]") == 0, "width and flags: '%s'", b);

    ksnprintf(b, sizeof b, "%llu %llx", 12345678901234ull, 0xdeadbeefcafeull);
    CHECK(strcmp(b, "12345678901234 deadbeefcafe") == 0, "64-bit: '%s'", b);

    ksnprintf(b, sizeof b, "%lu", (unsigned long)0x100000000ul);
    CHECK(strcmp(b, "4294967296") == 0, "long: '%s'", b);

    ksnprintf(b, sizeof b, "%p", (void *)0x1234abcd);
    CHECK(strcmp(b, "0x1234abcd") == 0, "pointer: '%s'", b);

    ksnprintf(b, sizeof b, "%s", (char *)NULL);
    CHECK(strcmp(b, "(null)") == 0, "null string: '%s'", b);

    /* Truncation must be exact and NUL-terminated -- the panic path formats
     * into fixed buffers and cannot afford a runaway. */
    char t[8];
    int n = ksnprintf(t, sizeof t, "abcdefghijklmno");
    CHECK(n == 7 && strcmp(t, "abcdefg") == 0, "truncated to '%s' (n=%d)", t, n);

    /* A trailing bare '%' must not read past the end of the format string. */
    ksnprintf(b, sizeof b, "end%");
    CHECK(strcmp(b, "end") == 0, "dangling %% handled: '%s'", b);
}

static void t_partial_flush(void)
{
    printf("9. the partial line the machine died in the middle of is not lost\n");
    uint64_t seq = klog_next_seq();
    kprintf("about to fault: ");         /* no newline -- this is the real case */
    CHECK(klog_next_seq() == seq, "an unterminated line has not committed yet");
    klog_flush_partial();
    struct klog_rec r;
    CHECK(klog_next_seq() == seq + 1, "flush committed it");
    CHECK(klog_get(seq, &r) && rec_is(&r, "about to fault: "), "partial text kept");
    CHECK(klog_next_seq() == seq + 1, "a second flush adds nothing");
    klog_flush_partial();
    CHECK(klog_next_seq() == seq + 1, "flushing an empty assembler is a no-op");
}

int main(void)
{
    printf("klog host tests (capacity %d records x %d bytes)\n\n",
           KLOG_SLOTS, KLOG_TEXT);
#ifdef KLOG_UNSAFE
    printf("*** built with -DKLOG_UNSAFE: this is the NEGATIVE CONTROL and it\n"
           "*** is expected to FAIL case 6.\n\n");
#endif

    t_basic();
    t_truncate();
    t_wrap();
    t_full();
    t_levels();
    t_interleave();
    t_render();
    t_format();
    t_partial_flush();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0)
        printf("KLOG_TEST_OK\n");
    return g_fails ? 1 : 0;
}
