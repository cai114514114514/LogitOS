#include "clib.h"

/* free -- physical memory and the kernel heap, read out of /proc/meminfo.
 *
 * Same claim as ps: no syscall here that `cat` does not make. SYS_MEMINFO (94)
 * exists and is not called; it prints to the KERNEL LOG rather than returning
 * numbers, so before /proc there was no way for a program to learn how much
 * memory this machine had at all.
 *
 *              total       used       free
 * Mem:        523712      23996     499716
 * KHeap:       20480       6084      14396
 *
 * Values are kB, as in /proc/meminfo, because that is the unit the file
 * publishes and converting here would put the units decision in the reader --
 * which is exactly how CLAUDE.md's two style-path units bugs happened. -b
 * prints the raw file instead, so a person who distrusts this table can see
 * what it was computed from without another program. */

static int slurp(const char *path, char *buf, int max)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    for (;;) {
        int r = sys_read(fd, buf + n, max - 1 - n);
        if (r <= 0) break;
        n += r;
        if (n >= max - 1) break;
    }
    sys_close(fd);
    buf[n] = 0;
    return n;
}

/* The value on the line whose key is `key`, or -1 if the key is absent.
 *
 * ABSENT IS -1 AND NOT 0. A meminfo that stopped publishing MemFree would
 * otherwise print a machine with no free memory, which is a number somebody
 * would act on. */
static long field(const char *buf, const char *key)
{
    int kl = c_strlen(key);
    for (int i = 0; buf[i]; i++) {
        if (i && buf[i - 1] != '\n') continue;
        if (c_strncmp(buf + i, key, kl) != 0) continue;
        int j = i + kl;
        while (buf[j] == ' ' || buf[j] == '\t' || buf[j] == ':') j++;
        if (buf[j] < '0' || buf[j] > '9') return -1;
        long v = 0;
        while (buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (buf[j++] - '0');
        return v;
    }
    return -1;
}

static void col(long v, int w)
{
    if (v < 0) { for (int i = 1; i < w; i++) outc(' '); outc('?'); return; }
    int d = 1; long t = v;
    while (t >= 10) { t /= 10; d++; }
    for (int i = d; i < w; i++) outc(' ');
    outn(v);
}

static void row(const char *label, long total, long used, long freeb)
{
    outs(label);
    for (int i = c_strlen(label); i < 7; i++) outc(' ');
    col(total, 11); col(used, 11); col(freeb, 11);
    outc('\n');
}

int main(int argc, char **argv)
{
    char buf[1024];
    int n = slurp("/proc/meminfo", buf, sizeof buf);
    if (n <= 0) { errs("free: cannot read /proc/meminfo\n"); return 1; }

    for (int i = 1; i < argc; i++)
        if (c_streq(argv[i], "-b")) { sys_write(1, buf, n); return 0; }

    long mt = field(buf, "MemTotal"), mf = field(buf, "MemFree");
    long mu = field(buf, "MemUsed");
    long ha = field(buf, "KHeapArena"), hl = field(buf, "KHeapLive");
    long hf = field(buf, "KHeapFree");

    /* 13 spaces + "total" puts its last column at 18, which is where row()'s
     * first number ends (7 for the label + 11 for the column). Counted, not
     * eyeballed -- a header one space off is the kind of thing that gets
     * "fixed" by widening the data column. */
    outs("             total       used       free\n");
    row("Mem:", mt, mu, mf);
    row("KHeap:", ha, hl, hf);
    return 0;
}
