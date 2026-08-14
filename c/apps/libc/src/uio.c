/* <sys/uio.h> readv/writev/preadv/pwritev, <sys/file.h> flock(), and
 * <sys/ioctl.h> ioctl() -- three "thin POSIX syscall-shaped surfaces" grouped
 * into one file because none of them needed a kernel change: each is built
 * entirely out of read()/write()/lseek()/fstat(), which already exist.
 *
 * THE TWO BUGS EVERY NAIVE readv/writev GETS WRONG (both are handled below,
 * in readv_core()/writev_core(), and both are load-bearing -- removing
 * either changes what the function returns, not just how it gets there):
 *
 *   1. A SHORT transfer must stop the loop right there and return bytes moved
 *      so far. POSIX defines readv() as behaving like a single read() into the
 *      logical concatenation of the buffers, so a short result means the data
 *      (or, for a write, the room) ran out -- continuing on to the next iovec
 *      would read/write bytes a real readv()/writev() never would, into a
 *      buffer region the shortfall proves isn't ready yet.
 *
 *   2. An error AFTER some bytes already moved returns the COUNT, not -1.
 *      Those bytes are real: they are already sitting in the caller's buffer
 *      (readv) or already accepted by the fd (writev). Reporting -1 here
 *      would make a partially successful call indistinguishable from one that
 *      did nothing, which is a worse answer than the truth and one no
 *      constructive proof needs to lose.
 *
 * ERRNO DISCIPLINE follows io.c (read this file's header comment too): each
 * wrapper here picks the errno a C program would expect from THAT call, and
 * never fabricates state to avoid returning one.
 *
 * A THIRD GAP, found later (by an adversarial review adding host-diff cases
 * against real glibc) and NOT fixable inside this file: writev()'s LOOP
 * breaks the atomicity a real single writev() syscall has on a pipe when
 * the vector does not entirely fit the available room. Real writev() is one
 * kernel operation that checks room against the WHOLE requested transfer
 * before moving anything; if it does not all fit (nonblocking), NOTHING
 * moves and the call fails EAGAIN as a unit -- confirmed empirically for a
 * total both under AND over PIPE_BUF, so this is broader than the
 * well-known "writes <= PIPE_BUF are atomic" rule specifically. This file's
 * writev(), built out of separate write() calls (there is no vectored
 * SYS_WRITE to make one real atomic call with -- see <sys/uio.h>), can let
 * an EARLIER iovec that individually fits land for real before a LATER one
 * fails, returning a PARTIAL count where a genuine writev() would have
 * returned -1 with nothing moved. This is real (not a test artifact) and
 * asymmetric: readv() has no matching gap, because reading never has an
 * analogous "not enough room for the WHOLE request" refusal to be atomic
 * about -- a short read is *supposed* to be partial. There is no fix
 * available here short of a real vectored SYS_WRITE; a caller that depends
 * on writev()'s pipe-insufficient-room atomicity (rare -- it matters only
 * when a SHORT-of-full multi-iovec write to a nonblocking pipe must never
 * partially land) is not served correctly by this implementation. Left
 * undiffed on purpose: tests/unit/libc_uio_test.c's case 25 documents this
 * investigation and the probe evidence in place rather than asserting
 * either side's behaviour as if it matched.
 */
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>

/* ------------------------------------------------------------------------
 * readv / writev / preadv / pwritev
 * ---------------------------------------------------------------------- */

/* Sum iov[].iov_len, refusing (EINVAL, like real readv/writev) BOTH an
 * iovcnt outside [0, IOV_MAX] and a total that would overflow ssize_t --
 * checked in full, up front, before any I/O happens. Doing this first means
 * a malformed vector fails atomically: no bytes moved, no fd position
 * touched (important for preadv/pwritev below, which must not seek at all
 * if the vector itself is bad). *out_total is only meaningful on success. */
static int vec_check(const struct iovec *iov, int iovcnt, size_t *out_total)
{
    if (iovcnt < 0 || iovcnt > IOV_MAX) { errno = EINVAL; return -1; }
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len > (size_t)SSIZE_MAX - total) { errno = EINVAL; return -1; }
        total += iov[i].iov_len;
    }
    *out_total = total;
    return 0;
}

/* An EMPTY vector (iovcnt == 0) is not a free pass: a real vectored syscall
 * still validates the fd even when there is nothing to transfer. Checked
 * empirically against a real Linux kernel (tests/unit/libc_uio_test.c case
 * 9 diffs it): writev(fd, v, 0) on a fd opened O_RDONLY fails EBADF, it does
 * not silently return 0. So this issues one zero-length read()/write()
 * purely to trigger that same validation; per POSIX, a call of length 0
 * never touches the buffer pointer, so `NULL` here is safe and deliberate --
 * there is nothing to write through it. */
static ssize_t readv_core(int fd, const struct iovec *iov, int iovcnt)
{
    size_t total;
    if (vec_check(iov, iovcnt, &total) < 0) return -1;
    if (iovcnt == 0) return read(fd, NULL, 0) < 0 ? -1 : 0;

    ssize_t moved = 0;
    for (int i = 0; i < iovcnt; i++) {
        /* Every entry is issued as a real call, INCLUDING zero-length ones
         * (no "skip if iov_len==0" shortcut): POSIX guarantees a 0-length
         * read/write never dereferences the buffer, so a zero-length entry
         * with a NULL base is exactly as safe to call through as to skip --
         * but calling through is what lets a permission failure on an
         * all-zero-length vector surface at all, for the same reason the
         * iovcnt==0 case above needs its own probe call. */
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return moved > 0 ? moved : -1;             /* rule 2 */
        moved += n;
        if ((size_t)n < iov[i].iov_len) return moved;         /* rule 1: short read, e.g. EOF */
    }
    return moved;
}

static ssize_t writev_core(int fd, const struct iovec *iov, int iovcnt)
{
    size_t total;
    if (vec_check(iov, iovcnt, &total) < 0) return -1;
    if (iovcnt == 0) return write(fd, NULL, 0) < 0 ? -1 : 0;

    ssize_t moved = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return moved > 0 ? moved : -1;              /* rule 2 */
        moved += n;
        if ((size_t)n < iov[i].iov_len) return moved;          /* rule 1: short write, e.g. a full pipe */
    }
    return moved;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)  { return readv_core(fd, iov, iovcnt); }
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) { return writev_core(fd, iov, iovcnt); }

/* preadv/pwritev must do their I/O at `offset` WITHOUT disturbing the fd's
 * ordinary file position, and there is no kernel primitive that does that
 * atomically -- SYS_READ/SYS_WRITE always act at the fd's current offset,
 * there is no "at" variant. So this saves the position, seeks to `offset`,
 * runs the same vec engine, and seeks back.
 *
 * WHAT THIS DOES NOT GIVE YOU, said plainly rather than left to be
 * discovered: a real preadv(2) is one atomic kernel operation, so a
 * concurrent lseek()/read()/write() on the SAME fd from another thread of
 * the same process (SYS_THREAD_CREATE threads share one fd table) cannot
 * land between "save" and "restore" and steal or corrupt the position. This
 * implementation has no such guarantee -- the three lseeks and the transfer
 * are four separate syscalls a preemption point can land between. A
 * single-threaded caller (the overwhelmingly common case for preadv/pwritev
 * -- reading a record out of a file opened once) is unaffected. A kernel fix
 * that would close this gap: real SYS_PREAD/SYS_PWRITE taking an explicit
 * offset, the same shape real pread(2)/pwrite(2) already have.
 *
 * On a fd that cannot seek at all (a pipe or the tty), the first lseek fails
 * with ESPIPE -- which is exactly the errno a real preadv/pwritev reports for
 * the same fd types, so this composition happens to match Linux's behaviour
 * for that case even though it is not trying to. */
static ssize_t at_offset(int fd, const struct iovec *iov, int iovcnt, off_t offset, int do_write)
{
    size_t total;
    if (vec_check(iov, iovcnt, &total) < 0) return -1;   /* bad vector: fail before touching the cursor */

    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0) return -1;                             /* not seekable -- lseek already set ESPIPE */
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;        /* cursor unmoved: lseek leaves it alone on failure */

    ssize_t r = do_write ? writev_core(fd, iov, iovcnt) : readv_core(fd, iov, iovcnt);
    int saved_errno = errno;         /* the restoring lseek below must not clobber a real error */
    lseek(fd, saved, SEEK_SET);
    errno = saved_errno;
    return r;
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{ return at_offset(fd, iov, iovcnt, offset, 0); }
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{ return at_offset(fd, iov, iovcnt, offset, 1); }

/* ------------------------------------------------------------------------
 * flock() -- see <sys/file.h> for the short version; this is the long one.
 * ------------------------------------------------------------------------
 * There is no lock manager in this kernel: no syscall records "fd N holds an
 * exclusive lock on this file" anywhere a SECOND caller could observe it, and
 * no shared-memory or other kernel object ring 3 could use to build one.
 *
 * THE ALTERNATIVE THIS FILE DID NOT TAKE, and why not: a process-local
 * advisory table, (dev,ino) -> lock state, kept in this library's own .data.
 * It was considered and rejected, not overlooked.
 *   - It could only ever mean something to THREADS of the SAME process
 *     (SYS_THREAD_CREATE threads share one address space, so a static table
 *     really would be shared there) -- but flock's overwhelming real-world use
 *     (a pidfile, a package-manager lock, a database's rollback
 *     journal) is keeping out a SEPARATE process, which an address-space-local
 *     table is structurally unable to do.
 *   - It gets even the same-process case backwards across fork(). This
 *     kernel's fork() is `vmm_clone_user`, an EAGER COPY of the address space
 *     (see the M18 note in CLAUDE.md) -- a forked child does not SHARE the
 *     table with its parent, it gets a frozen SNAPSHOT of it at the instant of
 *     fork. A lock the parent takes AFTER forking is invisible to the child
 *     and vice versa. Real flock() state is tied to the OPEN FILE DESCRIPTION,
 *     which fork() genuinely shares -- so a table with fork-eager-copy
 *     semantics would satisfy flock()'s signature while getting its single
 *     most common real pattern (parent locks, forks, child inherits the lock)
 *     exactly backwards.
 * That is precisely the "stub that returns 0 and the caller believes it holds
 * a lock" outcome the house rule (io.c's header comment; this file's task
 * description) forbids -- it would just take a hundred lines to arrive at the
 * same lie termios.c reaches in one, by not trying at all. So: ENOSYS,
 * honestly, exactly like every tcsetattr()-family call in termios.c when
 * there is no driver behind the state a caller is asking for. */
int flock(int fd, int operation)
{
    (void)operation;
    if (fd < 0) { errno = EBADF; return -1; }
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------------
 * ioctl() -- see <sys/ioctl.h> for what each request code does and why.
 * ------------------------------------------------------------------------ */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    /* Real ioctl() checks the fd BEFORE it ever looks at `request` -- fd
     * validity wins over both "unsupported request" and "bad arg pointer".
     * Checked empirically against a real Linux kernel (see git history for
     * this file): ioctl(-1, TIOCGWINSZ, ...), ioctl(-1, <bogus-request>,
     * ...) and ioctl(-1, FIONREAD, NULL) all report EBADF there, never
     * ENOTTY/EFAULT/ENOSYS. The first version of this function skipped this
     * check entirely and answered ENOTTY (or, for FIONREAD, ENOSYS) for a
     * completely invalid fd, which is a real behavioural bug, not just an
     * omitted nicety -- a caller cannot tell "this fd doesn't support the
     * request" from "this fd was never open" without it.
     *
     * lseek(fd,0,SEEK_CUR), not fstat(), is the probe used for this: it
     * returns a plain off_t, so on the host-diff build (where this
     * unimplemented-here call resolves to REAL glibc's lseek, same as
     * everywhere else in this file) there is no struct-layout ABI to get
     * wrong. fstat() would NOT be safe for this -- see the long comment
     * inside the FIONREAD case below, which explains why touching a
     * `struct stat` from code that can resolve to glibc's real fstat() is
     * dangerous, and is exactly why this general check deliberately avoids
     * it. A valid-but-unseekable fd (a pipe or the tty) legitimately fails
     * THIS probe with ESPIPE while still being genuinely open -- that must
     * not be mistaken for "no such fd", only a bare EBADF (or anything
     * else lseek might report) means that. The result is reused below for
     * FIONREAD instead of probing twice. */
    off_t fd_probe = lseek(fd, 0, SEEK_CUR);
    if (fd_probe < 0 && errno != ESPIPE) return -1;   /* no such fd: lseek's own errno stands */

    switch (request) {
    case TIOCGWINSZ:
    case TIOCGPGRP:
        /* Consistent with termios.c's not_a_pty(): no driver in this kernel
         * tracks a window size or a foreground process group for ANY fd, tty
         * or not. This is not even a divergence from Linux -- a non-tty fd
         * gets ENOTTY for these two there as well. */
        errno = ENOTTY;
        return -1;
    case FIONREAD: {
        if (!arg) { errno = EFAULT; return -1; }
        if (fd_probe < 0) {
            /* Only ESPIPE can have reached here (see the check above): the
             * fd genuinely exists but is a pipe or the tty, which is the
             * real "kernel doesn't expose a queued-byte count" gap this
             * file's header comment documents. ENOSYS, not a guess. */
            errno = ENOSYS;
            return -1;
        }
        /* ---- HOST-DIFF-UNSAFE PAST THIS POINT, on purpose documented in
         * detail rather than discovered by whoever adds the next test case.
         * fstat() is NOT implemented in this file (like read/write/lseek
         * elsewhere here, it is declared in <sys/stat.h> and resolves to
         * whatever is linked), which is exactly what makes read/write/lseek
         * safe to exercise against real glibc in tests/unit/libc_uio_test.c
         * -- but fstat() is different in kind, not just another instance of
         * the same trick: this codebase's OWN <sys/stat.h> declares `struct
         * stat` as 120 bytes (verified: `sizeof(struct stat)` under this
         * library's headers), while glibc's REAL on-disk-ABI `struct stat`
         * is 144 bytes on x86_64 (verified the same way against glibc's own
         * <sys/stat.h>). A host test binary that links THIS file (uio.c)
         * but not this library's OWN fstat() implementation (dirstat.c,
         * which is a DIFFERENT unit's file) makes any call to fstat() here
         * resolve to REAL glibc's fstat(), which writes ITS real 144-byte
         * struct into whatever address it is given -- overflowing a
         * 120-byte local variable declared under this library's own
         * headers by 24 bytes, silently, off the end of the stack. This
         * cannot be triggered through this codepath except by reaching the
         * line below with a fd that HAS a size to report (a valid, open,
         * seekable fd) -- so a host-diff test may exercise the fd-EBADF
         * path above and the `!arg`-EFAULT check just above freely (both
         * return before this point), but must NOT drive a valid regular fd
         * this far in any build that does not also link dirstat.c's real
         * fstat(). On the actual LogitOS target this is completely benign:
         * the target's own fstat() (dirstat.c, part of the full libc) is
         * sized consistently with this SAME header, so there is no
         * mismatch there at all -- this hazard is purely an artifact of
         * testing an unimplemented-here POSIX name against a host libc
         * whose ABI this header does not claim to match for every call,
         * only for the ones this file's own top comment says it does. */
        struct stat st;
        if (fstat(fd, &st) < 0) return -1;
        /* Deliberately UNCLAMPED. A cursor seeked past EOF is legal (POSIX;
         * a later write there creates a hole), and real Linux's generic
         * FIONREAD for a regular file is exactly `size - position` with no
         * floor at zero -- checked empirically against a real kernel
         * (outside this file's own headers, so the struct-size hazard just
         * above did not apply to that check): position 100 on a 10-byte
         * file reports -90 there, not 0. Clamping here would be exactly the
         * kind of plausible-looking invented number this file's own header
         * comment (and fsmisc.c's) argues against -- it would look like a
         * sane "nothing left to read" answer while being a value the real
         * call this is standing in for does not produce. */
        *(int *)arg = (int)(st.st_size - fd_probe);
        return 0;
    }
    default:
        errno = ENOTTY;   /* "Inappropriate ioctl for device" -- true of all of them here */
        return -1;
    }
}
