/* /proc, on the host: the namespace, the formats, and the LIVENESS rule.
 *
 * This links THE REAL c/fs/procfs.c. Nothing here is a re-implementation --
 * procfs.c includes no kernel header at all (see its own file comment), so
 * everything that decides what /proc answers is under test, and the only thing
 * this file supplies is the procfs_src_* seam declared in c/fs/procfs.h.
 *
 * WHAT THAT BUYS, and it is the whole reason the seam exists: a fake process
 * table can be MUTATED BETWEEN TWO READS, and can make a process exit, at
 * exactly the instant a test wants. Neither is possible on a real machine
 * without a race, so the two properties this filesystem is built around --
 * a read is answered at read() time, and a read of a gone process FAILS --
 * would otherwise be untestable and would be assertions rather than gates.
 *
 * THE NEGATIVE CONTROL is -DPROCFS_SNAPSHOT_AT_OPEN (tests/procfs.mk). It
 * makes pf_size() latch its render so the first read serves it -- and
 * pf_size() is what c/kernel/exec/file.c calls at open(). Under it every
 * FORMAT and NAMESPACE check still passes and exactly the five checks whose
 * names begin "LIVE:" must fail.
 *
 * Every liveness check below OPENS (size) and then READS, in that order and
 * with nothing in between, because that is the sequence file_open_vfs() and
 * file_read() perform -- and because it makes the control's behaviour
 * independent of which of the four latches happens to have been evicted. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "procfs.h"
#include "vfs.h"
#include "vfs_path.h"

/* ---------------------------------------------------------------------------
 * The fake machine.
 * ------------------------------------------------------------------------ */

#define FT_MAX 8

static struct procfs_task ft[FT_MAX];
static int  ft_live[FT_MAX];
static int  ft_n;
static int  ft_self;

static struct procfs_area fa[FT_MAX][4];
static int fa_n[FT_MAX];

static struct procfs_mem fm;
static uint64_t f_uptime_ms;

static int slot_of(int pid)
{
    for (int i = 0; i < ft_n; i++) if (ft_live[i] && ft[i].pid == pid) return i;
    return -1;
}

int procfs_src_pids(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ft_n && n < max; i++) if (ft_live[i]) out[n++] = ft[i].pid;
    return n;
}

int procfs_src_task(int pid, struct procfs_task *out)
{
    int s = slot_of(pid);
    if (s < 0) return 0;
    *out = ft[s];
    return 1;
}

int procfs_src_self(void) { return ft_self; }

int procfs_src_area(int pid, int i, struct procfs_area *out)
{
    int s = slot_of(pid);
    if (s < 0) return -1;
    if (i < 0 || i >= fa_n[s]) return 0;
    *out = fa[s][i];
    return 1;
}

void procfs_src_mem(struct procfs_mem *out) { *out = fm; }
uint64_t procfs_src_uptime_ms(void) { return f_uptime_ms; }
const char *procfs_src_version(void) { return "LogitOS version 0.29 (x86_64)"; }

/* ---------------------------------------------------------------------------
 * Harness
 * ------------------------------------------------------------------------ */

static int checks, fails;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("FAIL %s\n", what); }
}

static void ok_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("FAIL %s\n  got  '%s'\n  want '%s'\n", what, got ? got : "(null)", want);
    }
}

/* Whole-file read through the SAME path a program takes: pread from 0. */
static int slurp(struct filesystem *fs, const char *path, char *out, int max)
{
    int n = fs->iops->pread(fs, path, out, max, 0);
    if (n >= 0) out[n] = 0;
    return n;
}

static void mk_task(int slot, int pid, int ppid, const char *name)
{
    memset(&ft[slot], 0, sizeof ft[slot]);
    ft[slot].pid = pid; ft[slot].ppid = ppid; ft[slot].tid = pid + 10;
    ft[slot].state = PROCFS_RUN;
    ft[slot].nfds = 3; ft[slot].nfd_max = 32;
    ft[slot].caps = 0x1f;
    ft[slot].cr3 = 0x100000ull + (unsigned)slot * 0x1000;
    strcpy(ft[slot].name, name);
    strcpy(ft[slot].cwd, "/");
    ft_live[slot] = 1;
    fa_n[slot] = 0;
}

static void setup(void)
{
    memset(ft, 0, sizeof ft);
    memset(ft_live, 0, sizeof ft_live);
    memset(fa_n, 0, sizeof fa_n);
    ft_n = 4;
    mk_task(0, 1,  0, "sh");
    mk_task(1, 4,  1, "ps");
    mk_task(2, 9,  1, "browser");
    mk_task(3, 12, 1, "victim");
    ft[2].gui = 1;
    ft[2].state = PROCFS_ZOMBIE;    /* the Z case, set here so its file is read once */
    ft[3].dying = 1;                /* the K case, likewise */
    ft_self = 4;

    fa[1][0] = (struct procfs_area){ 0x50000000, 0x50002000, 0, PROCFS_R | PROCFS_X, -1, -1 };
    fa[1][1] = (struct procfs_area){ 0x50002000, 0x50003000, 0,          0,          -1, -1 };
    fa[1][2] = (struct procfs_area){ 0x60000000, 0x60004000, 0x2000,
                                     PROCFS_R | PROCFS_W, 7, -1 };
    fa_n[1] = 3;

    memset(&fm, 0, sizeof fm);
    /* 130944 frames x 4096 = 536,346,624 B = 523,776 kB. Every expected number
     * below is that arithmetic done by hand, which is the point: a units bug in
     * r_meminfo would otherwise agree with itself. */
    fm.total_frames = 130944; fm.free_frames = 124929; fm.used_frames = 6015;
    fm.frame_bytes = 4096;
    fm.heap_arena = 20971520; fm.heap_live = 6230016; fm.heap_free = 14741504;
    fm.heap_allocs = 4211; fm.heap_frees = 3900; fm.heap_grows = 5;
    fm.spaces_live = 3;

    f_uptime_ms = 252030;           /* 252.03 -- the hundredths need a leading 0 */
}

int main(void)
{
    setup();
    struct filesystem *fs = procfs_get();
    char b[8192];
    int n;

    /* --- 1. the namespace ------------------------------------------------ */

    ok(fs->iops->count(fs, "/") == 4 + 4, "root has meminfo/uptime/version/self + 4 pids");
    ok(fs->iops->size(fs, "/") < 0, "a directory has no size");
    ok(fs->iops->count(fs, "/4") == 4, "a pid dir has stat/status/cmdline/maps");
    ok(fs->iops->count(fs, "/4/stat") < 0, "a file is not a directory");
    ok(fs->iops->size(fs, "/4/stat") > 0, "a file has a size");

    ok_str(fs->iops->ent_name(fs, "/", 0), "meminfo", "root entry 0");
    ok_str(fs->iops->ent_name(fs, "/", 3), "self", "root entry 3");
    ok_str(fs->iops->ent_name(fs, "/", 4), "1", "root entry 4 is the first pid");
    ok_str(fs->iops->ent_name(fs, "/", 7), "12", "root entry 7 is the last pid");
    ok_str(fs->iops->ent_name(fs, "/", 8), "", "root has no entry 8");
    ok(fs->iops->ent_is_dir(fs, "/", 0) == 0, "meminfo is a file");
    ok(fs->iops->ent_is_dir(fs, "/", 3) == 1, "self is a directory");
    ok(fs->iops->ent_is_dir(fs, "/", 4) == 1, "a pid is a directory");
    ok_str(fs->iops->ent_name(fs, "/9", 0), "stat", "pid dir entry 0");
    ok(fs->iops->ent_is_dir(fs, "/9", 0) == 0, "everything in a pid dir is a file");
    ok(fs->iops->ent_size(fs, "/", 0) == fs->iops->size(fs, "/meminfo"),
       "ent_size of meminfo agrees with size(/meminfo)");

    /* Names that must NOT resolve. A /proc that accepts a sloppy pid accepts
     * two names for one process, and a typo then reports on something real. */
    ok(fs->iops->size(fs, "/007/stat") < 0, "leading zeros are not a pid");
    ok(fs->iops->size(fs, "/4x/stat") < 0, "trailing junk is not a pid");
    ok(fs->iops->size(fs, "/0/stat") < 0, "pid 0 is not a pid");
    ok(fs->iops->size(fs, "/-1/stat") < 0, "a negative is not a pid");
    ok(fs->iops->size(fs, "/4/nonsense") < 0, "an unknown leaf is not a file");
    ok(fs->iops->size(fs, "/4/stat/more") < 0, "nothing is three deep");
    ok(fs->iops->size(fs, "/meminfo/x") < 0, "meminfo is not a directory");
    ok(fs->iops->count(fs, "/77") < 0, "a dir for a pid that does not exist is not a dir");
    ok(fs->iops->pread(fs, "/77/stat", b, sizeof b, 0) == VFS_ENOENT,
       "a file under a pid that does not exist is ENOENT");

    /* --- 2. /proc/self is the CALLER ------------------------------------- */

    n = slurp(fs, "/self/stat", b, sizeof b);
    ok(n > 0 && strncmp(b, "4 (ps)", 6) == 0, "self resolves to the caller (pid 4)");
    ft_self = 1;
    n = slurp(fs, "/self/stat", b, sizeof b);
    ok(n > 0 && strncmp(b, "1 (sh)", 6) == 0, "self follows the caller, it is not a fixed link");
    ft_self = 4;

    /* --- 3. the formats. Each path is read ONCE here, so nothing in this
     *        section can redden under a control about staleness. ---------- */

    slurp(fs, "/1/stat", b, sizeof b);
    ok_str(b, "1 (sh) R 0 11 3 0\n", "stat: pid (comm) state ppid tid fds gui");

    slurp(fs, "/9/stat", b, sizeof b);
    ok_str(b, "9 (browser) Z 1 19 3 1\n", "stat: a zombie reads Z and keeps its gui flag");

    slurp(fs, "/12/stat", b, sizeof b);
    ok_str(b, "12 (victim) K 1 22 3 0\n", "stat: an accepted kill reads K, distinct from Z");

    slurp(fs, "/4/status", b, sizeof b);
    ok_str(b,
           "Name:\tps\n"
           "State:\tR (running)\n"
           "Pid:\t4\n"
           "PPid:\t1\n"
           "Tid:\t14\n"
           "FDs:\t3/32\n"
           "Gui:\t0\n"
           "Cwd:\t/\n"
           "Caps:\t0x000000000000001f\n"
           "FsPrefix:\t/\n",
           "status: every field, in order");

    n = slurp(fs, "/4/cmdline", b, sizeof b);
    ok(n == 3 && memcmp(b, "ps\0", 3) == 0, "cmdline: argv[0], NUL-terminated");

    slurp(fs, "/4/maps", b, sizeof b);
    ok_str(b,
           "0000000050000000-0000000050002000 r-x anon 0000000000000000\n"
           "0000000050002000-0000000050003000 --- anon 0000000000000000\n"
           "0000000060000000-0000000060004000 rw- file:7 0000000000002000\n",
           "maps: three areas, PROT_NONE renders --- and the file handle shows");

    n = slurp(fs, "/1/maps", b, sizeof b);
    ok(n == 0, "maps of a process with no areas is EMPTY, not an error");

    slurp(fs, "/meminfo", b, sizeof b);
    ok(strstr(b, "MemTotal:\t523776 kB\n") != NULL, "meminfo: frames x 4096, as kB");
    ok(strstr(b, "MemFree:\t499716 kB\n")  != NULL, "meminfo: MemFree");
    ok(strstr(b, "MemUsed:\t24060 kB\n")   != NULL, "meminfo: MemUsed");
    ok(strstr(b, "FrameSize:\t4096\n")     != NULL, "meminfo: the frame size is published");
    ok(strstr(b, "KHeapArena:\t20480 kB\n")!= NULL, "meminfo: the kernel heap arena");
    ok(strstr(b, "KHeapLive:\t6084 kB\n")  != NULL, "meminfo: the kernel heap live bytes");
    ok(strstr(b, "AddrSpaces:\t3\n")       != NULL, "meminfo: live address spaces");

    slurp(fs, "/uptime", b, sizeof b);
    ok_str(b, "252.03\n", "uptime: seconds, two decimals, hundredths zero-padded");

    slurp(fs, "/version", b, sizeof b);
    ok_str(b, "LogitOS version 0.29 (x86_64)\n", "version");

    /* --- 4. LIVENESS. Exactly these five redden under the control. ------- */

    /* (a) open, the machine moves, read. The answer must be the machine's,
     *     not the one that was true when the file was opened. */
    ok(fs->iops->size(fs, "/1/stat") > 0, "size() answers before the change");
    strcpy(ft[0].name, "bash");
    ft[0].nfds = 7;
    slurp(fs, "/1/stat", b, sizeof b);
    ok_str(b, "1 (bash) R 0 11 7 0\n", "LIVE: the read reflects the change, not the open");

    /* (b) two reads at offset 0, back to back, are two instants. Compared to
     *     each other rather than to a literal, so this says the same thing
     *     whichever of the two the first read happened to get. */
    {
        char first[64], second[64];
        slurp(fs, "/uptime", first, sizeof first);
        f_uptime_ms = 998690;
        slurp(fs, "/uptime", second, sizeof second);
        ok(strcmp(first, second) != 0, "LIVE: two reads at offset 0 are two instants");
        f_uptime_ms = 252030;
    }

    /* (c) the same, through the file /bin/free reads. */
    ok(fs->iops->size(fs, "/meminfo") > 0, "meminfo has a size at open");
    fm.free_frames = 100;
    slurp(fs, "/meminfo", b, sizeof b);
    ok(strstr(b, "MemFree:\t400 kB\n") != NULL, "LIVE: meminfo re-reads the allocator");
    fm.free_frames = 124929;

    /* (d) THE LIFETIME RULE. A process exits while a reader holds its name.
     *     The read must FAIL -- not return the snapshot taken at open, and not
     *     return 0 bytes, which is indistinguishable from an empty file. */
    ok(fs->iops->size(fs, "/9/stat") > 0, "pid 9's stat has a size while it lives");
    ft_live[2] = 0;                                    /* pid 9 exits */
    ok(fs->iops->pread(fs, "/9/stat", b, sizeof b, 0) == VFS_ENOENT,
       "LIVE: reading a gone process is ENOENT, not the bytes taken at open");
    ok(fs->iops->pread(fs, "/9/status", b, sizeof b, 0) == VFS_ENOENT,
       "status of a gone process is ENOENT");
    ok(fs->iops->pread(fs, "/9/maps", b, sizeof b, 0) == VFS_ENOENT,
       "maps of a gone process is ENOENT -- distinct from the EMPTY file a live "
       "process with no areas gives");
    ok(fs->iops->size(fs, "/9/stat") < 0, "and it has no size either");
    ok(fs->iops->count(fs, "/") == 4 + 3, "it is out of the root listing");
    ok(fs->iops->count(fs, "/9") < 0, "its directory is no longer a directory");
    ft_live[2] = 1;

    /* --- 5. ONE READ PASS IS ONE INSTANT (the latch) --------------------- */
    /* A `cat` takes a file in bites. The bites must not straddle two renders:
     * the front of one instant stitched to the back of another is a file that
     * never existed -- which is exactly what c/fs/vfs.c's vfs_pread comment
     * refuses to do for the synthetic nodes. */
    {
        char part[4096];
        int total = fs->iops->size(fs, "/4/status");
        ok(total > 20, "latch: status is long enough to take in bites");

        int got = fs->iops->pread(fs, "/4/status", part, 12, 0);
        ok(got == 12, "latch: the first bite is 12 bytes");
        memcpy(b, part, 12);

        strcpy(ft[1].name, "CHANGED");                 /* the machine moves mid-pass */
        ft[1].nfds = 31;

        int off = 12;
        while (off < total) {
            got = fs->iops->pread(fs, "/4/status", part, 12, off);
            if (got <= 0) break;
            memcpy(b + off, part, (size_t)got);
            off += got;
        }
        b[off] = 0;
        ok(off == total, "latch: the pass read exactly the length it started with");
        ok(strstr(b, "Name:\tps\n") != NULL,
           "latch: the whole pass is ONE instant -- the mid-pass change is not in it");
        ok(strstr(b, "CHANGED") == NULL, "latch: and nothing of the second instant leaked in");

        /* Rewinding to 0 is the refresh point, and it must show the change. */
        slurp(fs, "/4/status", b, sizeof b);
        ok(strstr(b, "Name:\tCHANGED\n") != NULL, "LIVE: a rewind to 0 re-takes the instant");
        strcpy(ft[1].name, "ps");
        ft[1].nfds = 3;
    }

    /* --- 6. offsets and ends --------------------------------------------- */
    {
        int total = fs->iops->size(fs, "/1/stat");
        ok(fs->iops->pread(fs, "/1/stat", b, sizeof b, total) == 0, "a read AT eof is 0");
        ok(fs->iops->pread(fs, "/1/stat", b, sizeof b, total + 100) == 0, "a read PAST eof is 0");
        ok(fs->iops->pread(fs, "/1/stat", b, sizeof b, -1) == VFS_EINVAL,
           "a negative offset is EINVAL");
        ok(fs->iops->pread(fs, "/1/stat", b, 3, 0) == 3,
           "a short buffer gets a short read, not a refusal");
    }

    /* --- 7. read-only, structurally rather than by refusal ---------------- */
    ok(fs->iops->write == NULL,  "there is no write op to call");
    ok(fs->iops->del == NULL,    "there is no unlink op to call");
    ok(fs->iops->mkdir == NULL,  "there is no mkdir op to call");
    ok(fs->iops->rename == NULL, "there is no rename op to call");

    /* --- 8. the mount-point test c/kernel/exec/file.c uses at open() ------ */
    ok(procfs_owns_path("/proc") == 1,          "owns /proc itself");
    ok(procfs_owns_path("/proc/1/stat") == 1,   "owns a file under it");
    ok(procfs_owns_path("/procx") == 0,         "component boundary: /procx is not /proc");
    ok(procfs_owns_path("/etc/passwd") == 0,    "owns nothing else");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
