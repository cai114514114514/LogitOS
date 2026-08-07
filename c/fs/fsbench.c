/* /dev/fsbench -- see fsbench.h for why this is a node on the machine rather
 * than a script on the host. */

#include <stdint.h>
#include <stddef.h>
#include "fsbench.h"
#include "bcache.h"
#include "blkdev.h"
#include "logitfs.h"
#include "vfs.h"
#include "ktime.h"
#include "kheap.h"
#include "kprintf.h"

void *memset(void *, int, size_t);

#define BENCH_MAX   4096
#define MAXREP      32

static char g_out[BENCH_MAX];
static int  g_len;

/* --- output ---------------------------------------------------------------
 * Everything is written twice: into the node's buffer for `cat`, and to the
 * serial log with a [bench] prefix so a boot harness can grep it without
 * needing a shell. */
static void emit(const char *line)
{
    kprintf("[bench] %s\n", line);
    for (int i = 0; line[i] && g_len < BENCH_MAX - 2; i++) g_out[g_len++] = line[i];
    if (g_len < BENCH_MAX - 1) g_out[g_len++] = '\n';
    g_out[g_len] = 0;
}

/* --- the statistic --------------------------------------------------------
 * A median and a spread, never a single sample. This host runs several QEMU
 * instances at once and TCG's throughput swings by a factor of two between
 * them; a lone number from that environment is a coin flip presented as
 * evidence. Insertion sort over <=32 samples costs nothing worth naming next
 * to the milliseconds being measured. */
struct stat { uint64_t med, min, max; int n; };

static void summarize(uint64_t *v, int n, struct stat *s)
{
    for (int i = 1; i < n; i++) {
        uint64_t x = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    s->n = n;
    s->min = n ? v[0] : 0;
    s->max = n ? v[n - 1] : 0;
    s->med = n ? v[n / 2] : 0;
}

/* us with three decimals, from ns, without floating point. */
static int fmt_us(char *b, int max, uint64_t ns)
{
    return ksnprintf(b, max, "%u.%u%u%u", (unsigned)(ns / 1000),
                     (unsigned)((ns / 100) % 10), (unsigned)((ns / 10) % 10),
                     (unsigned)(ns % 10));
}

static void report(const char *label, struct stat *s, uint64_t bytes)
{
    char b[192]; int n = 0;
    n += ksnprintf(b + n, (int)sizeof b - n, "%s med=", label);
    n += fmt_us(b + n, (int)sizeof b - n, s->med);
    n += ksnprintf(b + n, (int)sizeof b - n, "us min=");
    n += fmt_us(b + n, (int)sizeof b - n, s->min);
    n += ksnprintf(b + n, (int)sizeof b - n, "us max=");
    n += fmt_us(b + n, (int)sizeof b - n, s->max);
    n += ksnprintf(b + n, (int)sizeof b - n, "us n=%d", s->n);
    if (bytes && s->med) {
        /* KiB/s. bytes/(ns/1e9) = bytes*1e9/ns; 1e9 * 16 MiB still fits a u64
         * with 47 bits to spare, so the multiply comes first and the division
         * does not throw the answer away before it is asked for. */
        uint64_t kibs = (bytes * 1000000000ull) / s->med / 1024ull;
        n += ksnprintf(b + n, (int)sizeof b - n, " %u.%uMiB/s",
                       (unsigned)(kibs / 1024), (unsigned)((kibs % 1024) * 10 / 1024));
    }
    emit(b);
}

/* --- helpers --------------------------------------------------------------- */

static int  c_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }

static long num(const char *s)
{
    long v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* Cold means the buffer cache does not hold the answer. Sync FIRST: dropping a
 * dirty buffer to make a read look slower would be a benchmark that loses data,
 * which is not a trade this tree makes for a number. */
static void go_cold(void)
{
    (void)bcache_sync();
    bcache_drop();
}

/* --- blk: what one device round trip costs, as a function of its size -------
 * This is the measurement that separates the fixed per-command cost (queue the
 * descriptor, notify, wait for the other thread to run, take the completion)
 * from the per-byte cost. If the fixed part dominates at 8 sectors, then
 * reading a 3 MB file as 741 separate 8-sector commands is paying that fixed
 * cost 741 times, and the fix is a bigger request rather than a faster one. */
static void bench_blk(uint32_t sectors, int reps)
{
    struct blkdev *d = blk_root();
    if (!d) { emit("blk: no root device"); return; }
    if (reps <= 0 || reps > MAXREP) reps = 9;
    uint32_t bytes = sectors * 512u;
    void *buf = kmalloc(bytes);
    if (!buf) { emit("blk: kmalloc failed"); return; }

    uint64_t v[MAXREP];
    int got = 0;
    for (int r = 0; r < reps; r++) {
        /* Walk the LBA forward every rep so no device-side cache can answer
         * twice, and stay inside the device. */
        uint64_t lba = (uint64_t)(r + 1) * sectors * 8u;
        if (lba + sectors > d->nsectors) lba = 0;
        uint64_t t0 = time_mono_ns();
        int rc = blk_dev_read(d, lba, sectors, buf);
        uint64_t t1 = time_mono_ns();
        if (rc == 0) v[got++] = t1 - t0;
    }
    kfree(buf);
    if (!got) { emit("blk: every read failed"); return; }

    struct stat s; summarize(v, got, &s);
    char label[64];
    ksnprintf(label, (int)sizeof label, "blk %s %u sec/req (%uKiB)",
              d->name, sectors, bytes / 1024);
    report(label, &s, bytes);
}

/* --- file: the read half of a launch, cold and warm ------------------------ */
static void bench_file(const char *path, int reps, int cold)
{
    int sz = vfs_size(path);
    if (sz <= 0) { char b[96]; ksnprintf(b, 96, "file %s: not found", path); emit(b); return; }
    if (reps <= 0 || reps > MAXREP) reps = 5;

    int bytes = ((sz + 511) / 512) * 512;          /* what wm_launch allocates */
    void *img = kmalloc((size_t)bytes);
    if (!img) { emit("file: kmalloc failed"); return; }

    uint64_t vr[MAXREP], vs[MAXREP];
    int got = 0;
    for (int r = 0; r < reps; r++) {
        if (cold) go_cold();
        uint64_t t0 = time_mono_ns();
        int n1 = vfs_size(path);                    /* phase: path resolve + stat */
        uint64_t t1 = time_mono_ns();
        int n2 = vfs_read(path, img, bytes);        /* phase: the data */
        uint64_t t2 = time_mono_ns();
        if (n1 > 0 && n2 > 0) { vs[got] = t1 - t0; vr[got] = t2 - t1; got++; }
    }
    kfree(img);
    if (!got) { emit("file: every read failed"); return; }

    struct stat ss, sr;
    summarize(vs, got, &ss);
    summarize(vr, got, &sr);
    char label[128];
    ksnprintf(label, (int)sizeof label, "%s %s size=%d resolve", cold ? "cold" : "warm", path, sz);
    report(label, &ss, 0);
    ksnprintf(label, (int)sizeof label, "%s %s size=%d read   ", cold ? "cold" : "warm", path, sz);
    report(label, &sr, (uint64_t)sz);
}

/* --- launch: everything wm_launch does before elf_load --------------------
 * Deliberately the same sequence and the same allocation size as c/kernel/gui/
 * wm.c: vfs_size, kmalloc of the 512-rounded size, vfs_read. What it does NOT
 * include is elf_load and the first paint, which live in files this line does
 * not own -- those are attributed with kprof's sampler instead, and the report
 * says so rather than quietly presenting three phases as four. */
static void bench_launch(const char *path, int reps)
{
    int sz = vfs_size(path);
    if (sz <= 0) { char b[96]; ksnprintf(b, 96, "launch %s: not found", path); emit(b); return; }
    if (reps <= 0 || reps > MAXREP) reps = 5;
    int bytes = ((sz + 511) / 512) * 512;

    uint64_t va[MAXREP], vr[MAXREP], vt[MAXREP], vz[MAXREP];
    struct bcache_stats c0, c1;
    int got = 0;
    for (int r = 0; r < reps; r++) {
        go_cold();
        bcache_getstats(&c0);                 /* the LAST rep's device traffic is
                                               * what gets reported: one cold read
                                               * of this file, nothing else */
        uint64_t t0 = time_mono_ns();
        int n1 = vfs_size(path);
        uint64_t t1 = time_mono_ns();
        void *img = kmalloc((size_t)bytes);
        uint64_t t2 = time_mono_ns();
        int n2 = img ? vfs_read(path, img, bytes) : -1;
        uint64_t t3 = time_mono_ns();
        bcache_getstats(&c1);
        kfree(img);
        if (n1 > 0 && n2 > 0) {
            vz[got] = t1 - t0; va[got] = t2 - t1; vr[got] = t3 - t2; vt[got] = t3 - t0;
            got++;
        }
    }
    if (!got) { emit("launch: failed"); return; }

    struct stat s;
    char label[128];
    summarize(vz, got, &s);
    ksnprintf(label, (int)sizeof label, "launch %s size=%d  1.size ", path, sz); report(label, &s, 0);
    summarize(va, got, &s);
    ksnprintf(label, (int)sizeof label, "launch %s size=%d  2.alloc", path, sz); report(label, &s, 0);
    summarize(vr, got, &s);
    ksnprintf(label, (int)sizeof label, "launch %s size=%d  3.read ", path, sz); report(label, &s, (uint64_t)sz);
    summarize(vt, got, &s);
    ksnprintf(label, (int)sizeof label, "launch %s size=%d  TOTAL ", path, sz); report(label, &s, (uint64_t)sz);

    unsigned long cmds = c1.dev_reads - c0.dev_reads, blks = c1.dev_blocks - c0.dev_blocks;
    ksnprintf(label, (int)sizeof label,
              "launch %s              devcmds=%u devblocks=%u blocks/cmd=%u",
              path, (unsigned)cmds, (unsigned)blks,
              (unsigned)(cmds ? blks / cmds : 0));
    emit(label);
}

static void bench_cache(void)
{
    struct bcache_stats c;
    bcache_getstats(&c);
    char b[192];
    ksnprintf(b, (int)sizeof b,
              "cache hits=%u misses=%u evict=%u dirty-evict=%u wb=%u syncs=%u resident=%u dirty=%u",
              (unsigned)c.hits, (unsigned)c.misses, (unsigned)c.evictions,
              (unsigned)c.dirty_evictions, (unsigned)c.writebacks, (unsigned)c.syncs,
              (unsigned)c.resident, (unsigned)c.dirty);
    emit(b);
    /* The coalescing factor, which is the number the whole change is about:
     * blocks delivered per device command. 1.0 means one round trip per block. */
    ksnprintf(b, (int)sizeof b, "cache devcmds=%u devblocks=%u blocks/cmd=%u bypassed=%u",
              (unsigned)c.dev_reads, (unsigned)c.dev_blocks,
              (unsigned)(c.dev_reads ? c.dev_blocks / c.dev_reads : 0),
              (unsigned)c.bypassed);
    emit(b);
}

/* --- the standard table ---------------------------------------------------- */
static void bench_all(int reps)
{
    struct blkdev *d = blk_root();
    char b[128];
    ksnprintf(b, (int)sizeof b, "root=%s sectors=%u", d ? d->name : "(none)",
              d ? (unsigned)d->nsectors : 0u);
    emit(b);

    /* The size sweep is the whole argument for coalescing: if 1 sector and 1024
     * sectors cost nearly the same, then the cost is per COMMAND and the number
     * of commands is the only thing worth changing. 1024 sectors is 512 KiB,
     * which is what inode_read's READ_RUN issues. */
    int r = reps ? reps : 9;
    bench_blk(1, r);
    bench_blk(8, r);
    bench_blk(64, r);
    bench_blk(255, r);
    bench_blk(1024, r);
    bench_blk(2048, r);

    /* A and B, back to back, in one boot. read_run=1 IS the old read path: one
     * device command per 4 KiB block, cache installing every block it touches.
     * Anything that differs between the two halves below is the change and
     * nothing else -- same device, same image, same host load, same minute. */
    uint32_t keep = logitfs_read_run();

    emit("--- A: read_run=1 (the block-at-a-time path this replaced) ---");
    logitfs_set_read_run(1);
    bench_launch("/browser.aex", reps);
    bench_launch("/clock.aex", reps);
    bench_file("/browser.aex", reps, 0);
    bench_file("/clock.aex", reps, 0);
    bench_cache();

    emit("--- B: read_run=128 (512 KiB per command) ---");
    logitfs_set_read_run(128);
    bench_launch("/browser.aex", reps);
    bench_launch("/clock.aex", reps);
    bench_file("/browser.aex", reps, 0);
    bench_file("/clock.aex", reps, 0);
    bench_cache();

    logitfs_set_read_run(keep);
    emit("BENCH-DONE");
}

/* --- command surface ------------------------------------------------------- */

int fsbench_command(const char *buf, int len)
{
    char a[3][128];
    int na = 0, i = 0;
    while (i < len && na < 3) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= len || !buf[i]) break;
        int k = 0;
        while (i < len && buf[i] && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < 127) a[na][k++] = buf[i];
            i++;
        }
        a[na][k] = 0; na++;
    }
    if (!na) return -1;
    for (int k = na; k < 3; k++) a[k][0] = 0;

    g_len = 0; g_out[0] = 0;

    if (c_eq(a[0], "blk"))         { bench_blk((uint32_t)num(a[1]), (int)num(a[2])); return 0; }
    if (c_eq(a[0], "file"))        { bench_file(a[1], (int)num(a[2]), 1);
                                     bench_file(a[1], (int)num(a[2]), 0); return 0; }
    if (c_eq(a[0], "launch"))      { bench_launch(a[1], (int)num(a[2])); return 0; }
    if (c_eq(a[0], "cache"))       { bench_cache(); return 0; }
    if (c_eq(a[0], "run")) {                     /* the A/B knob, see logitfs.h */
        logitfs_set_read_run((uint32_t)num(a[1]));
        char b[64];
        ksnprintf(b, (int)sizeof b, "read_run=%u", (unsigned)logitfs_read_run());
        emit(b);
        return 0;
    }
    if (c_eq(a[0], "all"))         { bench_all((int)num(a[1])); return 0; }

    emit("usage: blk <sectors> <reps> | file <path> [reps] | launch <path> [reps] | cache | all [reps]");
    return -1;
}

int fsbench_render(char *out, int max)
{
    int n = g_len < max ? g_len : max;
    for (int i = 0; i < n; i++) out[i] = g_out[i];
    return n;
}

int fsbench_len(void) { return g_len; }
