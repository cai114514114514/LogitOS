/* /proc -- the namespace, the formats and the lifetime rules.
 *
 * NO KERNEL HEADER IS INCLUDED HERE, deliberately: every fact about the
 * machine arrives through the procfs_src_* seam in procfs.h, so this whole
 * file -- the pid parsing, the offset arithmetic, the latch and the rule for
 * what a read of a dead process's file returns -- compiles into
 * tests/unit/procfs_test.c and the code under test is the code that ships.
 * Same split, and the same reason, as c/fs/vfs_path.c.
 *
 * Read procfs.h before this file. The four-point lifetime argument is there,
 * and it is the design; what is below is its implementation. */

#include <stddef.h>
#include <stdint.h>
#include "procfs.h"
#include "vfs.h"
#include "vfs_path.h"

/* vfs_mount_at, weak, for exactly the reason c/fs/vfs.c declares kdiag weakly:
 * this file is linked into a host unit test that has no mount table and wants
 * none. Absent, procfs_mount() reports that it could not mount, which is the
 * truth in that build. */
int vfs_mount_at(const char *dir, struct filesystem *fs) __attribute__((weak));

/* --------------------------------------------------------------------------
 * Sizing, argued rather than rounded.
 *
 * The largest file here is /proc/<pid>/maps. c/kernel/mm/vma.h fixes
 * VMA_MAXAREA at 32, and a line is
 *
 *     0000000040000000-0000000040002000 r-x anon 0000000000000000
 *
 * = 66 bytes with the newline, so 32 areas is 2,112 bytes plus a header line.
 * 4,096 is that with room to spare and is also the page size, which is what a
 * caller will hand us. Overflow is NOT silent: the emitter stops at
 * RENDER_MAX - OVERFLOW_NOTE and appends a marker, because a truncated /proc
 * file that looks complete is the one failure mode of this design that a
 * reader cannot detect from the outside.
 *
 * NSNAP = 4 latches x 4,096 = 16,384 B of .bss plus one 4,096 B scratch for
 * size(). Measured with nm after the change; see the report.
 * ------------------------------------------------------------------------ */
#define PROCFS_RENDER_MAX 4096
#define PROCFS_OVERFLOW_NOTE 40
#define PROCFS_NSNAP 4

/* THE NEGATIVE CONTROL, and it is the PLAUSIBLE wrong implementation rather
 * than a mutilation: size() and the first read() of a file render the same
 * bytes microseconds apart, so sharing one render between them is an obvious
 * saving, and c/kernel/exec/file.c calls vfs_size() at OPEN. Take the saving
 * and every /proc file silently becomes a snapshot taken at open() -- which
 * still formats correctly, still has the right length, still passes every
 * test that only checks shape, and is wrong in the one way this filesystem
 * exists to be right about. `make test-procfs-negctl` builds exactly this and
 * requires the liveness checks -- and only those -- to redden. */
#ifdef PROCFS_SNAPSHOT_AT_OPEN
static const int g_size_latches = 1;
#else
static const int g_size_latches = 0;
#endif

/* --- tiny string helpers (no libc in the kernel's fs layer) --------------- */

static int  p_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  p_eq(const char *a, const char *b)
{ int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static void p_cpy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* --- the emitter ---------------------------------------------------------- */

struct em { char *b; int max, n; int over; };

static void e_ch(struct em *e, char c)
{
    if (e->n >= e->max) { e->over = 1; return; }
    e->b[e->n++] = c;
}
static void e_str(struct em *e, const char *s) { for (int i = 0; s && s[i]; i++) e_ch(e, s[i]); }
static void e_u64(struct em *e, uint64_t v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (k) e_ch(e, t[--k]);
}
static void e_i(struct em *e, int v)
{
    if (v < 0) { e_ch(e, '-'); e_u64(e, (uint64_t)(-(long)v)); return; }
    e_u64(e, (uint64_t)v);
}
static void e_hex16(struct em *e, uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    for (int s = 60; s >= 0; s -= 4) e_ch(e, d[(v >> s) & 0xf]);
}
/* A count of 4 KiB frames as kB, which is what every meminfo reader expects
 * and what makes the numbers comparable with a Linux one. */
static void e_kb(struct em *e, uint64_t bytes) { e_u64(e, bytes / 1024); e_str(e, " kB\n"); }

/* --------------------------------------------------------------------------
 * The namespace
 * ------------------------------------------------------------------------ */

enum {
    N_NONE = 0, N_ROOT, N_MEMINFO, N_UPTIME, N_VERSION,
    N_PIDDIR, N_STAT, N_STATUS, N_CMDLINE, N_MAPS
};

struct pnode { int kind; int pid; };

/* One component out of `p` (which points just past a '/'), into `out`.
 * Returns a pointer to the '/' or NUL that ended it. */
static const char *comp(const char *p, char *out, int max)
{
    int i = 0;
    for (; p[i] && p[i] != '/'; i++) if (i < max - 1) out[i] = p[i];
    out[i < max - 1 ? i : max - 1] = 0;
    return p + i;
}

/* Strictly positive decimal, or -1. Strict on purpose: "007" and "1x" and ""
 * must NOT resolve to pid 7, 1 and 0 -- a /proc that accepts sloppy pids
 * accepts two names for one process and lets a typo report on something real. */
static int pid_of(const char *s)
{
    if (!s || !s[0]) return -1;
    long v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        if (i > 0 && s[0] == '0') return -1;         /* no leading zeros */
        v = v * 10 + (s[i] - '0');
        if (v > 0x7fffffff) return -1;
    }
    return v > 0 ? (int)v : -1;
}

/* Parse a MOUNT-RELATIVE path. Does not consult the process table: this
 * answers "is that a well-formed name in this namespace", and whether the
 * process behind it exists is a separate question asked at render time -- see
 * the lifetime note in procfs.h, point 2. Keeping them apart is what lets
 * "/proc/9/stat where 9 has exited" and "/proc/9/nonsense" be different
 * errors. */
static void parse(const char *path, struct pnode *pn)
{
    pn->kind = N_NONE; pn->pid = 0;
    if (!path || path[0] != '/') return;
    if (!path[1]) { pn->kind = N_ROOT; return; }

    char c1[PROCFS_PATH_MAX];
    const char *rest = comp(path + 1, c1, (int)sizeof c1);

    if (p_eq(c1, "meminfo") || p_eq(c1, "uptime") || p_eq(c1, "version")) {
        if (rest[0]) return;                       /* "/meminfo/x" is not a thing */
        pn->kind = p_eq(c1, "meminfo") ? N_MEMINFO : p_eq(c1, "uptime") ? N_UPTIME : N_VERSION;
        return;
    }

    int pid = p_eq(c1, "self") ? procfs_src_self() : pid_of(c1);
    if (pid <= 0) return;
    pn->pid = pid;

    if (!rest[0]) { pn->kind = N_PIDDIR; return; }
    char c2[PROCFS_PATH_MAX];
    rest = comp(rest + 1, c2, (int)sizeof c2);
    if (rest[0]) return;                           /* nothing is three deep */

    if      (p_eq(c2, "stat"))    pn->kind = N_STAT;
    else if (p_eq(c2, "status"))  pn->kind = N_STATUS;
    else if (p_eq(c2, "cmdline")) pn->kind = N_CMDLINE;
    else if (p_eq(c2, "maps"))    pn->kind = N_MAPS;
    else                          pn->pid = 0;     /* unknown leaf: not a name here */
}

static const char *g_pidfiles[] = { "stat", "status", "cmdline", "maps" };
#define NPIDFILE ((int)(sizeof g_pidfiles / sizeof g_pidfiles[0]))
static const char *g_rootfiles[] = { "meminfo", "uptime", "version", "self" };
#define NROOTFILE ((int)(sizeof g_rootfiles / sizeof g_rootfiles[0]))

/* --------------------------------------------------------------------------
 * The renders. Each returns the byte count, or VFS_ENOENT if the process the
 * file describes is gone -- which is the whole of the lifetime rule in code:
 * the task is fetched HERE, by pid, at the moment the bytes are demanded, and
 * nothing survives the call.
 * ------------------------------------------------------------------------ */

static char state_letter(const struct procfs_task *t)
{
    return t->state == PROCFS_ZOMBIE ? 'Z' : t->dying ? 'K' : 'R';
}

/* /proc/<pid>/stat -- ONE LINE, for ps.
 *
 * NOT Linux's 52 fields. Linux's field 4 is the process group and field 5 the
 * session, and this kernel has neither (c/kernel/exec/file.c says so in as
 * many words: "there are no sessions and no process groups here"). Writing 0
 * into them would be a number nobody measured in a position every parser
 * believes. The fields below are the ones struct proc actually holds, in a
 * fixed order, with the comm in parentheses exactly where Linux puts it so
 * that a name containing a space still cannot be confused for a field. */
static int r_stat(struct em *e, int pid)
{
    struct procfs_task t;
    if (!procfs_src_task(pid, &t)) return VFS_ENOENT;
    e_i(e, t.pid);   e_str(e, " (");
    e_str(e, t.name[0] ? t.name : "?");
    e_str(e, ") ");  e_ch(e, state_letter(&t));
    e_ch(e, ' ');    e_i(e, t.ppid);
    e_ch(e, ' ');    e_i(e, t.tid);
    e_ch(e, ' ');    e_i(e, t.nfds);
    e_ch(e, ' ');    e_i(e, t.gui);
    e_ch(e, '\n');
    return e->n;
}

static int r_status(struct em *e, int pid)
{
    struct procfs_task t;
    if (!procfs_src_task(pid, &t)) return VFS_ENOENT;
    e_str(e, "Name:\t");   e_str(e, t.name[0] ? t.name : "?"); e_ch(e, '\n');
    e_str(e, "State:\t");  e_ch(e, state_letter(&t));
    e_str(e, t.state == PROCFS_ZOMBIE ? " (zombie)\n" : t.dying ? " (dying)\n" : " (running)\n");
    e_str(e, "Pid:\t");    e_i(e, t.pid);  e_ch(e, '\n');
    e_str(e, "PPid:\t");   e_i(e, t.ppid); e_ch(e, '\n');
    e_str(e, "Tid:\t");    e_i(e, t.tid);  e_ch(e, '\n');
    e_str(e, "FDs:\t");    e_i(e, t.nfds); e_ch(e, '/'); e_i(e, t.nfd_max); e_ch(e, '\n');
    e_str(e, "Gui:\t");    e_i(e, t.gui);  e_ch(e, '\n');
    e_str(e, "Cwd:\t");    e_str(e, t.cwd[0] ? t.cwd : "/"); e_ch(e, '\n');
    /* M28. Hex because it is a bitmap and CAP_* are bits; decimal would be the
     * one field in this file a reader has to convert before it means anything. */
    e_str(e, "Caps:\t0x");  e_hex16(e, (uint64_t)t.caps); e_ch(e, '\n');
    e_str(e, "FsPrefix:\t");
    e_str(e, t.fs_prefix[0] ? t.fs_prefix : "/");
    e_ch(e, '\n');
    return e->n;
}

/* argv[0] and no more, NUL-terminated as Linux's is.
 *
 * This kernel does not retain argv: c/kernel/exec/exec.c builds the SysV stack
 * for the new image and copies only the program name into `p->name`
 * (exec.c:387), so the arguments exist in the process's own stack and nowhere
 * the kernel can find them again. Reporting the name is honest; inventing a
 * command line is not, and retaining argv is a change to a file this line does
 * not own. */
static int r_cmdline(struct em *e, int pid)
{
    struct procfs_task t;
    if (!procfs_src_task(pid, &t)) return VFS_ENOENT;
    e_str(e, t.name[0] ? t.name : "?");
    e_ch(e, '\0');
    return e->n;
}

/* /proc/<pid>/maps -- the only way to see an address space from outside.
 *
 * Format: start-end prot backing offset. `prot` renders PROT_NONE as "---"
 * and that is a real state on this machine, not a placeholder: pthread_create
 * mprotects a stack's lowest page to PROT_NONE for the guard, and
 * c/kernel/mm/vma.h's comment on vma_protect explains that 0 is deliberately
 * distinguishable there. A guard page is exactly the thing somebody opens
 * this file to look for. */
static int r_maps(struct em *e, int pid)
{
    struct procfs_area a;
    int rc = procfs_src_area(pid, 0, &a);
    if (rc < 0) return VFS_ENOENT;
    for (int i = 0; rc == 1; i++, rc = procfs_src_area(pid, i, &a)) {
        e_hex16(e, a.start); e_ch(e, '-'); e_hex16(e, a.end); e_ch(e, ' ');
        e_ch(e, (a.prot & PROCFS_R) ? 'r' : '-');
        e_ch(e, (a.prot & PROCFS_W) ? 'w' : '-');
        e_ch(e, (a.prot & PROCFS_X) ? 'x' : '-');
        e_ch(e, ' ');
        if      (a.file >= 0) { e_str(e, "file:"); e_i(e, a.file); }
        else if (a.shm  >= 0) { e_str(e, "shm:");  e_i(e, a.shm);  }
        else                    e_str(e, "anon");
        e_ch(e, ' '); e_hex16(e, a.foff);
        e_ch(e, '\n');
    }
    return e->n;
}

static int r_meminfo(struct em *e)
{
    struct procfs_mem m;
    procfs_src_mem(&m);
    /* Frame counts x the frame size, reported in kB. The kernel counts FRAMES
     * and the units conversion happens once, here, rather than in each of the
     * three readers -- CLAUDE.md records two units bugs in the style path that
     * both had this shape (a plausible small number where there should have
     * been none), and both survived because the conversion was at the reader. */
    e_str(e, "MemTotal:\t");  e_kb(e, m.total_frames * m.frame_bytes);
    e_str(e, "MemFree:\t");   e_kb(e, m.free_frames  * m.frame_bytes);
    e_str(e, "MemUsed:\t");   e_kb(e, m.used_frames  * m.frame_bytes);
    e_str(e, "FrameSize:\t"); e_u64(e, m.frame_bytes); e_ch(e, '\n');
    e_str(e, "Frames:\t");    e_u64(e, m.total_frames); e_ch(e, '\n');
    e_str(e, "KHeapArena:\t");e_kb(e, m.heap_arena);
    e_str(e, "KHeapLive:\t"); e_kb(e, m.heap_live);
    e_str(e, "KHeapFree:\t"); e_kb(e, m.heap_free);
    e_str(e, "KHeapAllocs:\t"); e_u64(e, m.heap_allocs); e_ch(e, '\n');
    e_str(e, "KHeapFrees:\t");  e_u64(e, m.heap_frees);  e_ch(e, '\n');
    e_str(e, "KHeapGrows:\t");  e_u64(e, m.heap_grows);  e_ch(e, '\n');
    e_str(e, "AddrSpaces:\t");  e_i(e, m.spaces_live);   e_ch(e, '\n');
    return e->n;
}

/* Seconds with two decimals, and ONE column.
 *
 * Linux's second column is idle time, summed over cores. This kernel does not
 * account it -- the profiler measures halted cores by SAMPLING (CLAUDE.md's
 * "98.5% of the machine's samples are halted cores"), which is a statistic and
 * not a total, and there is no per-core idle accumulator anywhere in
 * c/kernel/sched. An invented second column would be read as one. */
static int r_uptime(struct em *e)
{
    uint64_t ms = procfs_src_uptime_ms();
    e_u64(e, ms / 1000);
    e_ch(e, '.');
    uint64_t cs = (ms % 1000) / 10;
    if (cs < 10) e_ch(e, '0');
    e_u64(e, cs);
    e_ch(e, '\n');
    return e->n;
}

static int r_version(struct em *e)
{
    e_str(e, procfs_src_version());
    e_ch(e, '\n');
    return e->n;
}

/* Render `pn` into `buf`. Returns bytes, or a negative VFS_E*. */
static int render(const struct pnode *pn, char *buf, int max)
{
    struct em e = { buf, max - PROCFS_OVERFLOW_NOTE, 0, 0 };
    int rc;
    switch (pn->kind) {
    case N_MEMINFO: rc = r_meminfo(&e); break;
    case N_UPTIME:  rc = r_uptime(&e);  break;
    case N_VERSION: rc = r_version(&e); break;
    case N_STAT:    rc = r_stat(&e, pn->pid);    break;
    case N_STATUS:  rc = r_status(&e, pn->pid);  break;
    case N_CMDLINE: rc = r_cmdline(&e, pn->pid); break;
    case N_MAPS:    rc = r_maps(&e, pn->pid);    break;
    default:        return VFS_ENOENT;
    }
    if (rc < 0) return rc;
    if (e.over) {
        /* Loud, in the file itself. A /proc file that is silently short is
         * indistinguishable from a machine with less to report, which is the
         * one way this design could mislead a reader who is doing everything
         * right. */
        e.max = max;
        e_str(&e, "[procfs: render truncated]\n");
    }
    return e.n;
}

/* --------------------------------------------------------------------------
 * The latch. See procfs.h point 4: one read pass is one instant.
 * ------------------------------------------------------------------------ */

struct snap {
    char     path[PROCFS_PATH_MAX];   /* "" = free */
    int      owner;                   /* the READER's pid */
    int      len;
    unsigned use;                     /* LRU clock */
    char     buf[PROCFS_RENDER_MAX];
};

static struct snap g_snap[PROCFS_NSNAP];
static unsigned    g_clock;

/* Keyed by (reader pid, path) and NOT by path alone. Two processes reading
 * different /proc files must not evict each other's pass -- that would put the
 * seam back exactly where a `ps` looping over pids while a monitor reads
 * meminfo would find it. Keying on the open file DESCRIPTION would be better
 * still and is not available: struct fs_iops hands a backend a path and an
 * offset and no handle, and widening it is a change to c/fs/vfs.h that every
 * backend would have to follow. */
static struct snap *snap_find(const char *path, int owner)
{
    for (int i = 0; i < PROCFS_NSNAP; i++)
        if (g_snap[i].path[0] && g_snap[i].owner == owner && p_eq(g_snap[i].path, path))
            return &g_snap[i];
    return NULL;
}

static struct snap *snap_take(const char *path, int owner)
{
    struct snap *s = snap_find(path, owner);
    if (!s) {
        s = &g_snap[0];
        for (int i = 1; i < PROCFS_NSNAP; i++) {
            if (!g_snap[i].path[0]) { s = &g_snap[i]; break; }
            if (g_snap[i].use < s->use) s = &g_snap[i];
        }
    }
    p_cpy(s->path, path, PROCFS_PATH_MAX);
    s->owner = owner;
    s->use = ++g_clock;
    s->len = 0;
    return s;
}

static void snap_drop(struct snap *s) { s->path[0] = 0; s->owner = 0; s->len = 0; }

/* size()'s scratch. SEPARATE from the latches on purpose and that separation
 * IS the shipping behaviour -- see g_size_latches above and the negative
 * control it drives. */
static char g_scratch[PROCFS_RENDER_MAX];

/* --------------------------------------------------------------------------
 * The filesystem operations
 * ------------------------------------------------------------------------ */

static int pf_mount(struct filesystem *f) { (void)f; return 0; }

static void pf_umount(struct filesystem *f)
{
    (void)f;
    for (int i = 0; i < PROCFS_NSNAP; i++) snap_drop(&g_snap[i]);
}

/* A directory answers count() >= 0 and size() < 0; c/fs/vfs.c decides which a
 * path is from exactly that, so the two must never both succeed. */
static int pf_size(struct filesystem *f, const char *path)
{
    (void)f;
    struct pnode pn; parse(path, &pn);
    if (pn.kind == N_NONE || pn.kind == N_ROOT || pn.kind == N_PIDDIR) return -1;

    if (g_size_latches) {
        /* THE CONTROL. Render into the latch so that the read at offset 0
         * finds it and serves the OPEN-time bytes. */
        struct snap *s = snap_take(path, procfs_src_self());
        int n = render(&pn, s->buf, PROCFS_RENDER_MAX);
        if (n < 0) { snap_drop(s); return -1; }
        s->len = n;
        return n;
    }
    int n = render(&pn, g_scratch, PROCFS_RENDER_MAX);
    return n < 0 ? -1 : n;
}

static int pf_pread(struct filesystem *f, const char *path, void *buf, int max, long long off)
{
    (void)f;
    if (max < 0 || off < 0) return VFS_EINVAL;
    struct pnode pn; parse(path, &pn);
    if (pn.kind == N_NONE || pn.kind == N_ROOT || pn.kind == N_PIDDIR) return VFS_ENOENT;

    int owner = procfs_src_self();
    struct snap *s = snap_find(path, owner);
    /* Offset 0 is the refresh point: a rewind re-takes the instant, a
     * continuation serves the one already taken. Under the control the latch
     * is reused even at 0, which is the whole difference. */
    if (!s || (off == 0 && !g_size_latches)) {
        s = snap_take(path, owner);
        int n = render(&pn, s->buf, PROCFS_RENDER_MAX);
        if (n < 0) { snap_drop(s); return n; }
        s->len = n;
    }
    s->use = ++g_clock;
    if (off >= (long long)s->len) return 0;                /* end of file */
    int n = s->len - (int)off;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) ((char *)buf)[i] = s->buf[(int)off + i];
    return n;
}

/* Whole-file read: pread from 0, truncated to the caller's buffer the way
 * ramfs does rather than refused the way logitfs does. A caller that sized its
 * buffer from size() gets everything; one that did not gets a prefix, and the
 * prefix is of ONE instant because it comes out of one render. */
static int pf_read(struct filesystem *f, const char *path, void *buf, int max)
{
    int n = pf_pread(f, path, buf, max, 0);
    return n < 0 ? -1 : n;
}

static int pf_count(struct filesystem *f, const char *dir)
{
    (void)f;
    struct pnode pn; parse(dir, &pn);
    if (pn.kind == N_ROOT) {
        int pids[64];
        int np = procfs_src_pids(pids, (int)(sizeof pids / sizeof pids[0]));
        return NROOTFILE + np;
    }
    if (pn.kind == N_PIDDIR) {
        struct procfs_task t;
        /* A directory for a process that has exited is not a directory. Asked
         * here as well as at render time because c/fs/vfs.c decides "is this a
         * directory" from this call, and a yes would make `ls /proc/<gone>`
         * report four entries that all fail to open. */
        return procfs_src_task(pn.pid, &t) ? NPIDFILE : -1;
    }
    return -1;
}

/* The nth name in `dir`. Static buffer, exactly as ramfs's ent_name does, and
 * for the same reason: struct fs_iops returns a `const char *` the VFS copies
 * immediately. */
static char g_namebuf[PROCFS_PATH_MAX];

static const char *pf_ent_name(struct filesystem *f, const char *dir, int i)
{
    (void)f;
    struct pnode pn; parse(dir, &pn);
    if (pn.kind == N_PIDDIR)
        return (i >= 0 && i < NPIDFILE) ? g_pidfiles[i] : "";
    if (pn.kind != N_ROOT || i < 0) return "";
    if (i < NROOTFILE) return g_rootfiles[i];

    int pids[64];
    int np = procfs_src_pids(pids, (int)(sizeof pids / sizeof pids[0]));
    int k = i - NROOTFILE;
    if (k >= np) return "";
    struct em e = { g_namebuf, (int)sizeof g_namebuf - 1, 0, 0 };
    e_i(&e, pids[k]);
    g_namebuf[e.n] = 0;
    return g_namebuf;
}

static int pf_ent_is_dir(struct filesystem *f, const char *dir, int i)
{
    struct pnode pn; parse(dir, &pn);
    if (pn.kind == N_PIDDIR) return 0;                 /* all four are files */
    if (pn.kind != N_ROOT) return 0;
    if (i < 0) return 0;
    if (i < NROOTFILE) return p_eq(g_rootfiles[i], "self");   /* self is the dir */
    (void)f;
    return 1;                                          /* a pid */
}

static int pf_ent_size(struct filesystem *f, const char *dir, int i)
{
    const char *nm = pf_ent_name(f, dir, i);
    if (!nm[0] || pf_ent_is_dir(f, dir, i)) return 0;
    char p[PROCFS_PATH_MAX];
    int n = 0;
    if (!p_eq(dir, "/")) { for (int k = 0; dir[k] && n < PROCFS_PATH_MAX - 2; k++) p[n++] = dir[k]; }
    p[n++] = '/';
    for (int k = 0; nm[k] && n < PROCFS_PATH_MAX - 1; k++) p[n++] = nm[k];
    p[n] = 0;
    int sz = pf_size(f, p);
    return sz < 0 ? 0 : sz;
}

/* Designated initialisers, NOT the positional form ramfs_iops uses. vfs.h's
 * own comment records why the positional form is a trap ("a member inserted
 * anywhere else silently re-aims every pointer after it") and the cost of not
 * falling into it is zero. Every op left out is NULL, which c/fs/vfs.c's
 * dispatchers already treat as "this backend cannot do that": write, del,
 * mkdir and rename are absent because /proc is read-only, and refusing by
 * ABSENCE rather than by a stub that returns -1 means there is no code here
 * that could ever be made to write. */
static const struct fs_iops procfs_iops = {
    .mount      = pf_mount,
    .umount     = pf_umount,
    .size       = pf_size,
    .read       = pf_read,
    .count      = pf_count,
    .ent_name   = pf_ent_name,
    .ent_size   = pf_ent_size,
    .ent_is_dir = pf_ent_is_dir,
    .pread      = pf_pread,
};

static struct filesystem g_procfs;
static char g_at[PROCFS_PATH_MAX] = "/proc";

struct filesystem *procfs_get(void)
{
    g_procfs.name = "procfs";
    g_procfs.iops = &procfs_iops;
    g_procfs.priv = NULL;
    return &g_procfs;
}

int procfs_owns_path(const char *abs)
{
    if (!abs || !g_at[0]) return 0;
    int l = p_len(g_at);
    for (int i = 0; i < l; i++) if (abs[i] != g_at[i]) return 0;
    /* Component boundary, the same rule c/fs/vfs.c's mount_for uses: "/procx"
     * is not under "/proc". */
    return abs[l] == '/' || abs[l] == 0;
}

const char *procfs_mountpoint(void) { return g_at; }

/* One call, so that the string the mount table holds and the string
 * procfs_owns_path() compares against are the same one. Two copies of a mount
 * point is how /proc comes to read live through one door and as a snapshot
 * through the other, with nothing to say which. */
int procfs_mount(const char *at)
{
    if (!vfs_mount_at) return VFS_ENOSYS;
    if (!at || at[0] != '/') return VFS_EINVAL;
    int rc = vfs_mount_at(at, procfs_get());
    if (rc == 0) p_cpy(g_at, at, PROCFS_PATH_MAX);
    return rc;
}
