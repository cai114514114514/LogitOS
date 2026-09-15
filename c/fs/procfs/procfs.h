#ifndef LOGIT_PROCFS_H
#define LOGIT_PROCFS_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * /proc -- the kernel's state as a NAMESPACE instead of as a syscall each.
 *
 * WHY THIS EXISTS, stated as the measurement that produced it. Every number
 * below was already computed somewhere in this kernel and reachable only
 * through a bespoke door:
 *
 *   the PCB table      SYS_PROCS (95), whose ONLY caller in the whole tree is
 *                      the Monitor GUI app. 44 programs in c/apps/coreutils
 *                      and not one of them was ps, because there was no way to
 *                      write ps without adding a syscall to it.
 *   memory             SYS_MEMINFO with a NULL buffer and an MMCTL_* selector
 *                      (c/kernel/mm/mmsys.c), which prints to the kernel log.
 *   uptime             time_mono_ns(), kernel-internal.
 *   an address space   nothing at all. vma.c knew, and nobody could ask.
 *
 * That shape -- one syscall per question, one app per syscall -- is the same
 * shape as SYS_NET_PING, which made ICMP unreachable from userland by any
 * general mechanism until a raw socket existed. /proc is the general
 * mechanism: a SECOND READER of facts that already exist, not new accounting.
 * Nothing here samples anything the kernel was not already sampling, and
 * NOTHING is retired -- SYS_PROCS/SYS_MEMINFO/SYS_SYSINFO keep their live
 * consumer (Monitor) and keep working unchanged.
 *
 * WHY A MOUNTED FILESYSTEM AND NOT A SYNTHETIC NODE. /dev/vfsctl and /dev/kmsg
 * are single paths matched by string compare (the KDIAG_NOT_MINE protocol in
 * c/fs/vfs.c). That works for a fixed handful of names and does not work for a
 * TREE whose middle level is the set of live pids -- readdir, mount crossing,
 * "." and "..", and the permission walk all come free from the mount table and
 * would each have to be re-implemented inside a NOT_MINE hook. procfs is a
 * struct filesystem for the same reason ramfs is one, and it needed no change
 * to c/fs/vfs.c whatsoever.
 *
 * THE NAMESPACE
 *   /proc/meminfo          pmm frames + the kernel heap
 *   /proc/uptime           monotonic seconds since time_init()
 *   /proc/version          the kernel version string
 *   /proc/self             a DIRECTORY, resolved to the CALLER's pid at the
 *                          moment of the call -- not a symlink. A symlink
 *                          would have to be a record in the VFS metadata store
 *                          (vmeta_readlink, consulted by vfs.c's walker) and
 *                          that store is one table for the whole machine, so
 *                          the single record could only ever name ONE pid.
 *                          Resolving the name inside the filesystem is both
 *                          correct for every caller at once and consistent
 *                          with the rest of this design, in which every answer
 *                          is computed at the moment it is demanded.
 *   /proc/<pid>/stat       one line, machine-readable, for ps
 *   /proc/<pid>/status     key/value, for a person
 *   /proc/<pid>/cmdline    argv[0]. See procfs_src_task() on why only that.
 *   /proc/<pid>/maps       the address space, from c/kernel/mm/vma.c
 *
 * ===========================================================================
 * LIFETIME -- the hard part, and the part that is an argument rather than a
 * format. A process can exit while something holds an open fd on its entry.
 *
 *  1. procfs HOLDS NO POINTER TO A PROCESS, EVER. There is no `struct proc *`
 *     in this filesystem, no cached cr3, no borrowed name. Every render
 *     resolves the pid through the source seam below, which copies what it
 *     needs out of the process table under that table's own lock and returns.
 *     There is no reference here that could dangle, because there is no
 *     reference. That is what makes the use-after-free structurally
 *     impossible rather than carefully avoided.
 *
 *  2. THE NAME IS THE REFERENCE. An open fd on /proc/7/stat carries a PATH and
 *     nothing else (c/kernel/exec/file.c's `char path[128]`), so every read
 *     re-asks the question. When pid 7 is gone the answer is VFS_ENOENT, which
 *     surfaces as read() == -1. That is chosen over the two alternatives:
 *       - a STALE SNAPSHOT presents a dead process as alive, and a monitor
 *         polling it would never learn of the exit. It is the answer that
 *         looks like it works.
 *       - RETURNING 0 is indistinguishable from an empty file, so a reader
 *         cannot tell "gone" from "nothing to say".
 *     "The thing this file describes no longer exists" is a fact, and an error
 *     is how a file reports one.
 *
 *  3. PID REUSE CANNOT ALIAS, and this is what makes (2) sound rather than
 *     merely convenient. c/kernel/exec/proc.c allocates from `static int
 *     next_pid = 1` with `p->pid = next_pid++` -- a MONOTONE COUNTER, not a
 *     slot index -- so a pid names at most one process in the life of a boot.
 *     Linux needs a pinned dentry holding a task reference precisely because
 *     its pids DO recycle; this kernel has already paid for the property, and
 *     resolve-by-name-at-read-time is safe here for a reason specific to this
 *     machine. IF THAT EVER CHANGES -- a pid table that wraps, a pid_max --
 *     this whole argument fails and the fd needs to carry an incarnation
 *     number. Said here because the change would be made in another file and
 *     would break this one silently.
 *
 *  4. ONE READ PASS IS ONE INSTANT. A file is rendered whole into a latch
 *     (procfs.c) when it is read at offset 0, and later offsets are served
 *     from that latch. Without it a `cat` taking the file 512 bytes at a time
 *     would stitch the front of one instant onto the back of another and
 *     produce a file that never existed -- exactly the failure c/fs/vfs.c's
 *     vfs_pread comment refuses to emulate for the synthetic nodes. Rewinding
 *     to offset 0 re-renders, so the refresh point is explicit.
 * ===========================================================================
 *
 * THE SOURCE SEAM. Everything this filesystem knows about the machine arrives
 * through the procfs_src_* functions below, and procfs.c includes no kernel
 * header at all. That is the same split c/fs/vfs_path.c makes and for the same
 * reason: the namespace rules, the pid parsing, the offset arithmetic and the
 * lifetime rules are where the bugs are, and none of them needs a process, a
 * disk or a boot to exercise. tests/unit/procfs_test.c implements this seam
 * over a table it controls and links THE REAL procfs.c, so the code under test
 * is the code that ships. c/fs/procfs_src.c is the kernel's implementation.
 * ------------------------------------------------------------------------ */

struct filesystem;   /* vfs.h */

/* Mount-relative paths only; /proc is mounted, so nothing here ever sees more
 * than "/<pid>/<name>". 64 is four times the longest such string. */
#define PROCFS_PATH_MAX  64
#define PROCFS_NAME_MAX  32
#define PROCFS_CWD_MAX  128
#define PROCFS_PREFIX_MAX 64

/* Mirrors enum proc_state in c/kernel/exec/proc.h. Restated rather than
 * included for the reason above: this header must not drag proc.h into a host
 * test. procfs_src.c is where the two meet and it asserts nothing -- it
 * TRANSLATES, in four lines, so a divergence is a compile error there instead
 * of a wrong letter in every ps listing. */
#define PROCFS_RUN     1
#define PROCFS_ZOMBIE  2

#define PROCFS_R  0x1
#define PROCFS_W  0x2
#define PROCFS_X  0x4

struct procfs_task {
    int      pid, ppid, tid;
    int      state;                 /* PROCFS_RUN / PROCFS_ZOMBIE */
    int      nfds, nfd_max;
    int      gui;                   /* 1 = owns a window */
    int      dying;                 /* a kill was accepted, it has not exited yet */
    unsigned long caps;             /* M28 CAP_* bitmap */
    /* The address space, for procfs_src_area() alone. DELIBERATELY NOT
     * RENDERED into any file here: it is a physical address, and handing ring
     * 3 one turns a read-only introspection surface into a hint for anybody
     * trying to aim at the page tables. It is in this struct because the ONE
     * caller that needs it (the maps source) already has to fetch the task to
     * learn the process still exists, and a second lookup would be a second
     * chance for the answer to change between them. */
    uint64_t cr3;
    char     name[PROCFS_NAME_MAX];
    char     cwd[PROCFS_CWD_MAX];
    char     fs_prefix[PROCFS_PREFIX_MAX];
};

struct procfs_area {
    uint64_t start, end, foff;
    unsigned prot;                  /* PROCFS_R|W|X; 0 is PROT_NONE and is real */
    int      file, shm;             /* backing handles, -1 = not that kind */
};

struct procfs_mem {
    uint64_t total_frames, free_frames, used_frames, frame_bytes;
    uint64_t heap_arena, heap_live, heap_free;
    uint64_t heap_allocs, heap_frees, heap_grows;
    int      spaces_live;
};

/* --- the seam ------------------------------------------------------------
 * All of these are pure reads. None allocates, none blocks, none can fail
 * other than by saying "no such thing".
 *
 * procfs_src_pids     the live pids, oldest slot first. Returns the count.
 *                     Pids and not tasks: 32 ints is a stack cost this can
 *                     pay inside a syscall, 32 struct procfs_task is 12 KiB
 *                     against a 32 KiB kernel stack that the TLS path already
 *                     runs deep in.
 * procfs_src_task     1 = filled, 0 = no such pid. THE ONLY WAY a pid becomes
 *                     a task, and therefore the single place the lifetime
 *                     rule above is enforced.
 * procfs_src_self     the calling process's pid, or 0 if there is no calling
 *                     process (the boot path, a host test).
 * procfs_src_area     area `i` of pid's address space: 1 = filled, 0 = past
 *                     the end, -1 = no such pid. Three answers because "the
 *                     process is gone" and "it has no more areas" must not
 *                     collapse -- the first has to become ENOENT and the
 *                     second is an ordinary end of file.
 * procfs_src_mem      the machine's memory, from pmm and the kernel heap.
 * procfs_src_uptime_ms  monotonic ms since time_init().
 * procfs_src_version    the kernel version line, NUL-terminated.
 */
int         procfs_src_pids(int *out, int max);
int         procfs_src_task(int pid, struct procfs_task *out);
int         procfs_src_self(void);
int         procfs_src_area(int pid, int i, struct procfs_area *out);
void        procfs_src_mem(struct procfs_mem *out);
uint64_t    procfs_src_uptime_ms(void);
const char *procfs_src_version(void);

/* The singleton instance, ready for vfs_mount_at("/proc", ...). A singleton
 * and not a pool like ramfs_create(): a second /proc would report the same
 * machine, so there is nothing for an instance to distinguish. */
struct filesystem *procfs_get(void);

/* Does `abs` (an ABSOLUTE, already-resolved path) name something this
 * filesystem generates? c/kernel/exec/file.c asks at open() so that it does
 * NOT slurp the file into a buffer -- see the comment at that call site, and
 * point 4 above for what would otherwise be a snapshot taken at open. Answers
 * against the path this filesystem was actually mounted at, so it is a fact
 * and not a hardcoded "/proc". */
int procfs_owns_path(const char *abs);

/* Mount /proc at `at` AND record `at` as the prefix procfs_owns_path()
 * answers for. One call rather than a mount plus a setter, so the mount table
 * and the ownership test cannot disagree -- see the comment on the definition.
 * Returns 0, or a negative VFS_E*. */
int         procfs_mount(const char *at);
const char *procfs_mountpoint(void);

#endif /* LOGIT_PROCFS_H */
