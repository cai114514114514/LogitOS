/* /bin/libctest2 -- on-target battery for the syscall-backed additions that
 * make c/apps/libc more than mini-libc: glob/fnmatch, pwd/grp, sys/utsname,
 * sys/mman, sched.h, sys/resource, syslog, poll. These cannot be host-diffed
 * (tests/libc.mk does that for the pure-computation additions -- fnmatch,
 * inet_ntop/pton, regex) because they talk to the real kernel: SYS_MMAP,
 * SYS_YIELD, SYS_DIR_COUNT/SYS_DIR_NAME, the real /bin and / filesystem.
 * Run by `make test-libc2` (tests/boot/run-libc2-test.sh), which asserts
 * "LIBC2_OK" on serial -- same convention as /bin/libctest (LIBC_OK). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <fnmatch.h>
#include <pwd.h>
#include <grp.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sched.h>
#include <syslog.h>
#include <poll.h>
#include <limits.h>

static int checks, fails;
#define CHK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", msg); } } while (0)
#define CHK_STR(got, want, msg) do { checks++; if (strcmp((got),(want)) != 0) { fails++; \
    printf("FAIL: %s -- got '%s' want '%s'\n", msg, (got), (want)); } } while (0)

static void t_uname(void)
{
    struct utsname u;
    CHK(uname(&u) == 0, "uname ok");
    CHK_STR(u.sysname, "LogitOS", "uname sysname");
    CHK_STR(u.machine, "x86_64", "uname machine");
    CHK(u.nodename[0] != 0, "uname nodename set");
    CHK(uname(0) == -1 && errno == EFAULT, "uname null -> EFAULT");
}

/* P1: SYS_READ_FILE hands the caller's buffer to the block device VERBATIM.
 *
 * The chain, verified jump by jump (docs/BUG_BACKLOG.md): syscall.c passes the
 * user pointer into vfs_read -> logitfs -> bcache_read_run, whose miss runs go
 * straight to blk_read_n -> virtio_blk.c casts the pointer into the virtqueue
 * descriptor -> the device treats it as a PHYSICAL address. boot.asm identity-
 * maps only the first 1 GiB, and user images link at exactly 1 GiB -- so for a
 * user buffer the device DMA-writes past the end of RAM, the data lands
 * nowhere, and the syscall still RETURNS SUCCESS because the status byte (a
 * kernel address) completed fine. Nothing ever failed loudly; files under
 * 4 KiB worked because the single-block path happens to bounce through a
 * kernel buffer -- while the pool has a free slot, which is one more "usually".
 *
 * The oracle is TWO READ PATHS THAT MUST AGREE. fread() goes through F_VFS,
 * which slurps into a KERNEL buffer and memcpys out -- immune by construction.
 * SYS_READ_FILE into our own .bss (a CLI app links at 0x50000000, comfortably
 * past the identity map) is the DMA path. Same file, byte-for-byte: if fread's
 * copy is right, the bytes on disk are right, so any raw-read mismatch is the
 * read path and not a write-side accident. The buffer is pre-filled with a
 * sentinel so "the device never wrote" shows as the sentinel surviving, which
 * is distinguishable from "the device wrote the wrong bytes".
 *
 * THE SIZE IS THE REPRODUCER, and 16 KiB was not enough -- the first version of
 * this test PASSED on the broken kernel, because a file just written is still
 * RESIDENT in the buffer cache (BC_NBUF = 256 blocks = 1 MiB), and resident
 * blocks are served by memcpy from kernel buffers; the DMA path never ran. So:
 * 2 MiB, twice the pool. Writing it evicts its own head, so reading the head
 * back MUST miss; and a big read run trips bcache_read_run's stream bypass
 * (install = n <= nbuf/4), which never touches a kernel buffer at all. The
 * pattern depends on the offset with an odd multiplier (the mm suite's
 * discipline) so an off-by-one-block transfer mismatches immediately. */
static long raw_sys3(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }
#define RAW_SYS_READ_FILE 11   /* include/abi/logit_abi.h; mirrored here because
                                * mini-libc deliberately does not re-export the
                                * SYS_* numbers and this test wants the RAW
                                * syscall, not a wrapper that might change path */

#define P1_BYTES (2u * 1024 * 1024)
static unsigned char p1_dma[P1_BYTES];     /* .bss at ~0x50xxxxxx: past 1 GiB */

static unsigned char p1_pat(int i) { return (unsigned char)(i * 131 + (i >> 5) * 7 + 0x5B); }

static void t_readfile_dma(void)
{
    const char *path = "/p1probe.dat";
    unsigned char *ref = malloc(P1_BYTES);
    CHK(ref != 0, "P1: reference buffer");
    if (!ref) return;

    /* Write the pattern through stdio: F_VFS buffers in the KERNEL and flushes
     * on close, so the on-disk bytes do not depend on the path under test. */
    for (int i = 0; i < P1_BYTES; i++) ref[i] = p1_pat(i);
    FILE *f = fopen(path, "w");
    CHK(f != 0, "P1: create probe file");
    if (!f) { free(ref); return; }
    CHK(fwrite(ref, 1, P1_BYTES, f) == P1_BYTES, "P1: write pattern");
    CHK(fclose(f) == 0, "P1: close flushes");

    /* Path 1 (immune): fread back through the same kernel-buffered route and
     * prove the DISK holds the pattern -- isolating the read path from any
     * write-side accident before blaming it. */
    f = fopen(path, "r");
    CHK(f != 0, "P1: reopen probe");
    if (f) {
        memset(ref, 0, P1_BYTES);
        CHK(fread(ref, 1, P1_BYTES, f) == P1_BYTES, "P1: fread full");
        fclose(f);
        int bad = -1;
        for (int i = 0; i < P1_BYTES && bad < 0; i++)
            if (ref[i] != p1_pat(i)) bad = i;
        CHK(bad < 0, "P1: disk holds the pattern (kernel-buffer path)");
    }

    /* Path 2 (under test): the raw syscall, DMA straight at our .bss. */
    memset(p1_dma, 0xAA, P1_BYTES);          /* sentinel; also faults the pages in */
    long n = raw_sys3(RAW_SYS_READ_FILE, (long)path, (long)p1_dma, P1_BYTES);
    CHK(n == P1_BYTES, "P1: SYS_READ_FILE returns full length");

    int first_bad = -1, sentinel = 0;
    for (int i = 0; i < P1_BYTES; i++) {
        if (p1_dma[i] != p1_pat(i)) { if (first_bad < 0) first_bad = i; if (p1_dma[i] == 0xAA) sentinel++; }
    }
    checks++;
    if (first_bad >= 0) {
        fails++;
        printf("FAIL: P1 DMA read corrupt: first bad byte at %d, %d sentinel bytes untouched of %d\n",
               first_bad, sentinel, P1_BYTES);
    }

    remove(path);
    free(ref);
}

static void t_pwgrp(void)
{
    struct passwd *pw = getpwuid(0);
    CHK(pw && strcmp(pw->pw_name, "root") == 0, "getpwuid(0) is root");
    CHK(getpwuid(1) == 0, "getpwuid(1) -> NULL (no such user)");
    pw = getpwnam("root");
    CHK(pw && pw->pw_uid == 0, "getpwnam root");
    CHK(getpwnam("nobody") == 0, "getpwnam nobody -> NULL");
    struct group *gr = getgrgid(0);
    CHK(gr && strcmp(gr->gr_name, "root") == 0, "getgrgid(0) is root");
    CHK(getuid() == 0 && geteuid() == 0 && getgid() == 0, "one-user identity");
}

static void t_mmap(void)
{
    void *p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHK(p != MAP_FAILED, "mmap anon ok");
    if (p != MAP_FAILED) {
        unsigned char *b = (unsigned char *)p;
        b[0] = 0xAB; b[65535] = 0xCD;   /* first-touch both ends of the reservation */
        CHK(b[0] == 0xAB && b[65535] == 0xCD, "mmap read back what was written");
        CHK(munmap(p, 65536) == 0, "munmap ok");
    }
    void *bad = mmap(0, 4096, PROT_READ, MAP_PRIVATE, 3 /* a real fd */, 0);
    CHK(bad == MAP_FAILED && errno == ENODEV, "mmap file-backed refused honestly");
    CHK(mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED, "mmap len 0 refused");
}

static void t_sched(void)
{
    CHK(sched_yield() == 0, "sched_yield ok");
    int cpu = sched_getcpu();
    CHK(cpu >= 0, "sched_getcpu non-negative");
    CHK(sched_get_priority_max(SCHED_OTHER) == 0, "SCHED_OTHER max priority is 0");
    CHK(sched_get_priority_max(SCHED_FIFO) == -1, "SCHED_FIFO refused (no realtime scheduler)");
}

static void t_resource(void)
{
    struct rlimit rl;
    CHK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == OPEN_MAX, "RLIMIT_NOFILE == OPEN_MAX");
    CHK(setrlimit(RLIMIT_NOFILE, &rl) == 0, "setrlimit to the current value succeeds");
    rl.rlim_cur = 4;
    CHK(setrlimit(RLIMIT_NOFILE, &rl) == -1 && errno == EPERM, "setrlimit to a different value refused");
    struct rusage ru;
    CHK(getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_utime.tv_sec == 0, "getrusage: honest zeros");
}

static void t_glob_fnmatch(void)
{
    CHK(fnmatch("*.txt", "readme.txt", 0) == 0, "fnmatch hit");
    CHK(fnmatch("*.txt", "readme.bin", 0) == FNM_NOMATCH, "fnmatch miss");

    glob_t g;
    memset(&g, 0, sizeof g);
    int rc = glob("/bin/*", 0, 0, &g);
    CHK(rc == 0, "glob /bin/* succeeds");
    CHK(g.gl_pathc > 0, "glob /bin/* found entries");
    int saw_sh = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) if (strcmp(g.gl_pathv[i], "/bin/sh") == 0) saw_sh = 1;
    CHK(saw_sh, "glob /bin/* found /bin/sh");
    globfree(&g);

    memset(&g, 0, sizeof g);
    rc = glob("/no/such/dir/*", 0, 0, &g);
    CHK(rc == GLOB_NOMATCH, "glob on a missing directory -> GLOB_NOMATCH");
    globfree(&g);

    memset(&g, 0, sizeof g);
    rc = glob("/no/such/thing*", GLOB_NOCHECK, 0, &g);
    CHK(rc == 0 && g.gl_pathc == 1 && strcmp(g.gl_pathv[0], "/no/such/thing*") == 0,
        "glob GLOB_NOCHECK returns the literal pattern");
    globfree(&g);
}

static void t_poll(void)
{
    int fd = open("/bin/sh", O_RDONLY);
    CHK(fd >= 0, "open /bin/sh for poll test");
    if (fd >= 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
        int r = poll(&pfd, 1, 0);
        CHK(r == 1 && (pfd.revents & POLLIN), "poll: a regular file is always readable");
        close(fd);
    }
}

static void t_syslog(void)
{
    /* Real functionality (writes to fd 1/2), not a stub -- there is nothing
     * to assert on the OUTPUT (no syslogd to read it back from), only that
     * it does not crash and openlog/closelog round-trip cleanly. */
    openlog("libctest2", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "libctest2 syslog smoke test, pid via LOG_PID");
    closelog();
    checks++;   /* reaching here without a fault is the assertion */
}

/* The environment the kernel actually pushed.
 *
 * getenv() was internally consistent and permanently empty: c/kernel/exec/exec.c
 * builds a full SysV stack with argv, envp and 14 auxv pairs, and crt0 read
 * argc/argv and threw the envp pointer away. Every program on this machine saw
 * an environment of nothing -- getenv("HOME"), getenv("PATH"), getenv("TERM")
 * all NULL -- which is exactly the shape of gap that a self-consistent API
 * hides. So the assertion has to come from OUTSIDE the process: the runner
 * exports LOGIT_ENVTEST in the shell before launching this, and the value has
 * to survive fork + execve + crt0 + env_init to be read back here. */
extern char **environ;

static void t_environ(void)
{
    CHK(environ != 0, "environ is a real vector");
    CHK(environ && environ[0] != 0, "environ is not empty at startup");

    const char *v = getenv("LOGIT_ENVTEST");
    CHK(v != 0, "getenv sees a variable exported by the shell");
    if (v) CHK_STR(v, "42", "the exported value survived execve");

    /* And the pre-existing half still works on top of an adopted vector -- a
     * setenv that lands in the middle of crt0's strings rather than in an
     * empty one is where an adoption bug would show. */
    CHK(setenv("LOGIT_ENVTEST", "43", 1) == 0, "setenv overwrite");
    v = getenv("LOGIT_ENVTEST");
    if (v) CHK_STR(v, "43", "setenv overwrote the inherited value");
    CHK(setenv("LOGIT_ENVNEW", "x", 0) == 0, "setenv new");
    v = getenv("LOGIT_ENVNEW");
    if (v) CHK_STR(v, "x", "setenv new readable");
    CHK(unsetenv("LOGIT_ENVNEW") == 0 && getenv("LOGIT_ENVNEW") == 0, "unsetenv");
}

int main(void)
{
    t_environ();
    t_readfile_dma();
    t_uname();
    t_pwgrp();
    t_mmap();
    t_sched();
    t_resource();
    t_glob_fnmatch();
    t_poll();
    t_syslog();
    if (fails == 0) printf("LIBC2_OK %d/%d\n", checks - fails, checks);
    else            printf("LIBC2_FAIL %d/%d (%d failed)\n", checks - fails, checks, fails);
    return fails ? 1 : 0;
}
