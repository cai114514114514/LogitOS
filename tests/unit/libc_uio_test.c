/* Feature-test macros for the GLIBC build only: plain `-std=c11` (no `-D_GNU_
 * SOURCE`) hides preadv/pwritev, IOV_MAX and SSIZE_MAX behind glibc's strict-
 * ISO-C11 feature gate. Our own headers declare all of these unconditionally
 * (there is no gate to satisfy), so defining these before any header is
 * included is a no-op for the "ours" build and exactly what the "glibc" build
 * needs -- this must come before every #include below. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1   /* glibc gates preadv/pwritev behind this (they predate their
                              * POSIX Issue 8 standardisation and are a glibc/BSD extension
                              * under plain -std=c11); the two macros above alone are not
                              * enough to see their prototypes. */

/* readv/writev/preadv/pwritev case list, compiled twice by tests/libc.mk (see
 * the header comment in tests/unit/libc_fnmatch_test.c for the two-build diff
 * strategy in full). UNLIKE fnmatch/regex/inet, this is not pure computation
 * over a string: every case here does REAL read()/write()/pipe()/open()
 * calls. That is still a valid diff, and for the reason the task description
 * for this unit spells out -- in the "ours" build, our uio.c's readv/writev/
 * preadv/pwritev call read()/write()/lseek(), which are declared in OUR
 * <unistd.h> but, since this is an ordinary dynamically-linked host binary
 * with no libc of our own linked in, resolve to GLIBC's read/write/lseek at
 * link time. So both the "ours" binary and the "glibc" reference binary
 * issue the exact same underlying Linux syscalls; what differs between them
 * is only the AGGREGATION logic on top -- the loop that decides when to stop
 * on a short transfer, and what to return after a partial error -- which is
 * exactly the part uio.c does not get from glibc. Linux's real readv/writev
 * syscalls are vectored natively but are specified (POSIX, and the kernel
 * follows it) to have the identical observable contract this loop
 * implements by hand, so a real diff between "one syscall did it" and "N
 * syscalls in a loop did it" is a legitimate proof that the loop is correct.
 *
 * Every case prints one or more lines of the form "<id>\t<value>"; id is a
 * running counter (not compared -- it just keeps the two outputs aligned for
 * a human reading a failed diff) and value is always a plain integer: a byte
 * count, or a 0/1 boolean built from a NAMED errno macro (EINVAL, EFAULT, ...)
 * so each build's own <errno.h> supplies the number being compared, per the
 * project's diff-testing rule. */
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int id;
static void chk(long long v) { printf("%d\t%lld\n", id++, v); }

/* REAL Linux open(2) flag values, deliberately NOT this library's <fcntl.h>
 * O_CREAT/O_TRUNC/O_WRONLY/O_RDONLY -- those are documented there as "must
 * match the kernel ABI" and they do, but the ABI they match is LogitOS's
 * SYS_OPEN, not Linux's. Neither build here links an open() implementation
 * of its own (only uio.c + this test), so `open()` in BOTH builds is
 * genuinely glibc's real host open(2) wrapper, and the flags handed to it
 * must be the numbers the REAL host kernel understands. This is scratch-file
 * setup plumbing for the test, not part of readv/writev/preadv/pwritev's
 * behaviour under comparison, so bypassing this library's <fcntl.h> for it
 * does not weaken the diff -- it is what makes the diff meaningful instead of
 * failing both builds' setup identically for an unrelated reason. (Found the
 * hard way: with the LogitOS O_CREAT value (0x100), the bit that gets set on
 * the real Linux syscall is O_NOCTTY, not O_CREAT (Linux's real O_CREAT is
 * 0x40) -- so the file was never created, and every case after the first
 * open() silently ran off a missing/bad fd in the "ours" run alone.) */
#define T_O_RDONLY 00
#define T_O_WRONLY 01
#define T_O_RDWR   02
#define T_O_CREAT  0100
#define T_O_TRUNC  01000

/* Real Linux fcntl(2) command value for F_SETPIPE_SZ (1031) -- used only by
 * the added cases below to pin a pipe's capacity to a known size so a
 * genuinely-full-pipe short write/EAGAIN is deterministic instead of
 * depending on the host's default pipe size. Same rationale as T_O_* above:
 * this is real host-kernel setup plumbing (fcntl() here is genuinely
 * glibc's real fcntl(2) wrapper in both builds, since neither links an
 * fcntl() implementation of its own), not part of this library's own
 * <fcntl.h>, which has no such concept for a kernel with no adjustable pipe
 * capacity. */
#define T_F_SETPIPE_SZ 1031

/* A scratch regular file. Every case that uses it opens with
 * T_O_CREAT|T_O_TRUNC (or just T_O_CREAT) so a stale leftover from a
 * previous, now-finished run never affects the outcome -- nothing here
 * depends on the file having existed before this process touched it.
 *
 * The path is PER-PID, not a fixed name, and that is load-bearing, not
 * cosmetic: tests/libc.mk's diff invocation is `diff <(./glibc) <(./ours)`,
 * and process substitution starts BOTH binaries running CONCURRENTLY, not
 * one after the other. A fixed shared path was tried first and is wrong --
 * found the hard way, empirically, while adding the cases below: with two
 * processes truncating/writing/reading the SAME file at the same time, one
 * process's T_O_TRUNC can land between the other's write and its very next
 * read, corrupting that read's result. The corruption is real but is a
 * property of the HARNESS racing itself against itself, not of readv/
 * writev/preadv/pwritev's own logic -- which is exactly why it must be
 * closed here rather than left as noise the diff can't distinguish from an
 * actual implementation bug. getpid() gives each side of the SAME diff run
 * its own file, eliminating the shared mutable state entirely. */
static char scratch_path[64];
#define SCRATCH scratch_path

int main(void)
{
    snprintf(scratch_path, sizeof scratch_path, "/tmp/logit_uio_difftest_%d.dat", (int)getpid());

    /* ---- 1. writev to a pipe, readv from a pipe: basic multi-iovec ---- */
    {
        int fd[2];
        pipe(fd);
        char a[] = "Hello", b[] = " ", c[] = "World!";
        struct iovec wv[3] = { { a, 5 }, { b, 1 }, { c, 6 } };
        chk(writev(fd[1], wv, 3));                       /* expect 12 */
        close(fd[1]);                                    /* EOF for the reader once drained */

        char b1[5], b2[1], b3[6];
        struct iovec rv[3] = { { b1, 5 }, { b2, 1 }, { b3, 6 } };
        chk(readv(fd[0], rv, 3));                         /* expect 12 */
        chk(memcmp(b1, "Hello", 5) == 0);
        chk(memcmp(b2, " ", 1) == 0);
        chk(memcmp(b3, "World!", 6) == 0);
        close(fd[0]);
    }

    /* ---- 2. zero-length iovecs mixed with data, both directions ---- */
    {
        int fd[2];
        pipe(fd);
        char a[] = "abc", c[] = "WXYZ";
        /* Zero-length entries carry a NULL base on purpose: a correct
         * implementation must never dereference iov_base when iov_len==0,
         * so a real base pointer here would hide a bug instead of catching
         * it. */
        struct iovec wv[5] = { { NULL, 0 }, { a, 3 }, { NULL, 0 }, { c, 4 }, { NULL, 0 } };
        chk(writev(fd[1], wv, 5));                        /* expect 7 */
        close(fd[1]);

        char b1[3], b2[4];
        struct iovec rv[5] = { { NULL, 0 }, { b1, 3 }, { NULL, 0 }, { b2, 4 }, { NULL, 0 } };
        chk(readv(fd[0], rv, 5));                          /* expect 7 */
        chk(memcmp(b1, "abc", 3) == 0 && memcmp(b2, "WXYZ", 4) == 0);
        close(fd[0]);
    }

    /* ---- 3. a single iovec (the degenerate case) ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_WRONLY, 0644);
        char msg[] = "solo";
        struct iovec wv[1] = { { msg, 4 } };
        chk(writev(fd, wv, 1));                            /* expect 4 */
        close(fd);

        fd = open(SCRATCH, T_O_RDONLY);
        char buf[4];
        struct iovec rv[1] = { { buf, 4 } };
        chk(readv(fd, rv, 1));                              /* expect 4 */
        chk(memcmp(buf, "solo", 4) == 0);
        close(fd);
    }

    /* ---- 4. 16 iovecs round trip through a pipe ---- */
    {
        int fd[2];
        pipe(fd);
        char src[16];
        for (int i = 0; i < 16; i++) src[i] = (char)('A' + i);
        struct iovec wv[16];
        for (int i = 0; i < 16; i++) { wv[i].iov_base = &src[i]; wv[i].iov_len = 1; }
        chk(writev(fd[1], wv, 16));                          /* expect 16 */
        close(fd[1]);

        char dst[16];
        struct iovec rv[16];
        for (int i = 0; i < 16; i++) { rv[i].iov_base = &dst[i]; rv[i].iov_len = 1; }
        chk(readv(fd[0], rv, 16));                            /* expect 16 */
        chk(memcmp(src, dst, 16) == 0);
        close(fd[0]);
    }

    /* ---- 5. short read at EOF on a pipe: must STOP, not roll into the
     * next iovec. Write end closes after 5 bytes, well short of the 20 the
     * reader's vector can hold. ---- */
    {
        int fd[2];
        pipe(fd);
        write(fd[1], "Hi!!!", 5);
        close(fd[1]);                                        /* EOF once the 5 bytes are drained */

        char b1[10], b2[10];
        memset(b1, (int)0xEE, sizeof b1);
        memset(b2, (int)0xEE, sizeof b2);
        struct iovec rv[2] = { { b1, 10 }, { b2, 10 } };
        chk(readv(fd[0], rv, 2));                            /* expect 5: short, stops in iov[0] */
        chk(memcmp(b1, "Hi!!!", 5) == 0);
        /* The untouched tail of b1 AND all of b2 must be exactly the
         * sentinel still -- proof the loop did not touch b2 at all after
         * the short transfer in b1 (a bug here would zero-fill or garble
         * these bytes instead of leaving the sentinel alone). */
        int untouched = 1;
        for (size_t i = 5; i < sizeof b1; i++) if ((unsigned char)b1[i] != 0xEE) untouched = 0;
        for (size_t i = 0; i < sizeof b2; i++) if ((unsigned char)b2[i] != 0xEE) untouched = 0;
        chk(untouched);
        close(fd[0]);
    }

    /* ---- 6. readv into a vector larger than the file (rule 1 on a
     * regular file, so it is deterministic without a close() race) ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_WRONLY, 0644);
        write(fd, "AB", 2);
        close(fd);
        fd = open(SCRATCH, T_O_RDONLY);
        char b1[4], b2[4];
        memset(b1, (int)0xEE, sizeof b1);
        memset(b2, (int)0xEE, sizeof b2);
        struct iovec rv[2] = { { b1, 4 }, { b2, 4 } };
        chk(readv(fd, rv, 2));                               /* expect 2: file is shorter than iov[0] alone */
        chk(memcmp(b1, "AB", 2) == 0 && (unsigned char)b1[2] == 0xEE);
        close(fd);
    }

    /* ---- 7. error after partial progress returns the COUNT, not -1
     * (readv): iov[0] is a full, in-bounds read; iov[1] points at NULL,
     * which read() faults on inside the kernel (EFAULT) without crashing
     * this process. (errno itself is NOT compared here -- see the long
     * comment above case 14 for why: a positive/partial return leaves errno
     * untouched by convention on both sides, so there is nothing meaningful
     * to diff at this specific spot even before that limitation applies.) */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_WRONLY, 0644);
        write(fd, "HELLOWORLD", 10);
        close(fd);
        fd = open(SCRATCH, T_O_RDONLY);
        char b1[5];
        struct iovec rv[2] = { { b1, 5 }, { NULL, 4 } };
        chk(readv(fd, rv, 2));                               /* expect 5, not -1 */
        chk(memcmp(b1, "HELLO", 5) == 0);
        close(fd);
    }

    /* ---- 8. same rule for writev: iov[0] lands for real, iov[1] faults ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_WRONLY, 0644);
        struct iovec wv[2] = { { "AB", 2 }, { NULL, 3 } };
        chk(writev(fd, wv, 2));                              /* expect 2, not -1 */
        close(fd);

        fd = open(SCRATCH, T_O_RDONLY);
        char b[8]; ssize_t n = read(fd, b, sizeof b);
        chk(n == 2 && memcmp(b, "AB", 2) == 0);              /* the partial write really landed */
        close(fd);
    }

    /* ---- 9. iovcnt == 0 still validates the fd, exactly like a nonempty
     * vector would -- an empty vector is not a free pass. Proven with TWO
     * fds, one valid for the direction asked and one not, so the return
     * value differs and the diff actually exercises the check. ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        chk(readv(fd, NULL, 0));                             /* expect 0: fd IS open for reading */
        chk(writev(fd, NULL, 0));                            /* expect -1: fd is NOT open for writing */
        close(fd);
    }

    /* ---- 10. iovcnt < 0: EINVAL, for both calls. Uses an O_RDWR fd
     * DELIBERATELY, not the read-only SCRATCH fd used elsewhere: on real
     * Linux, an fd opened in the wrong direction fails EBADF before the
     * count is ever inspected (case 9 just demonstrated exactly that), which
     * would make a real writev(read-only-fd, v, -1) fail for the WRONG
     * reason and defeat this specific case. With both directions valid, the
     * only thing left for either build to reject is the count itself. */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_RDWR, 0644);
        struct iovec v[1] = { { NULL, 0 } };
        errno = 0; chk(readv(fd, v, -1));   chk(errno == EINVAL);
        errno = 0; chk(writev(fd, v, -1));  chk(errno == EINVAL);
        close(fd);
    }

    /* ---- 11. iovcnt > IOV_MAX: EINVAL, checked before any I/O ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        static struct iovec v[IOV_MAX + 1];
        errno = 0; chk(readv(fd, v, IOV_MAX + 1));  chk(errno == EINVAL);
        close(fd);
    }

    /* ---- 12. iovcnt == IOV_MAX exactly (all zero-length): must succeed,
     * proving the boundary is > IOV_MAX refused, not >= IOV_MAX ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        static struct iovec v[IOV_MAX];
        for (int i = 0; i < IOV_MAX; i++) { v[i].iov_base = NULL; v[i].iov_len = 0; }
        chk(readv(fd, v, IOV_MAX));                          /* expect 0 */
        close(fd);
    }

    /* ---- 13. total length overflowing ssize_t: refused, for both calls,
     * using a NAMED limit (SSIZE_MAX) rather than a raw literal so each
     * build checks against its own <limits.h>. Only the RETURN VALUE (-1)
     * is compared, not errno -- checked empirically against this host's
     * real kernel (see the git history for this file's probe output): real
     * Linux does not reach a "the sum overflows" check for THIS
     * construction at all. Any two iov_len values that genuinely sum past
     * SSIZE_MAX are, individually, far larger than any real buffer that
     * could back them (SSIZE_MAX/IOV_MAX alone is ~9x10^15 bytes), so the
     * only way to build this case at all is a claimed length wildly past
     * the real backing allocation -- and the kernel's address-range check
     * (access_ok) rejects that with EFAULT before any length-sum logic
     * would matter, on a perfectly valid fd, every time. So real glibc's
     * errno here is EFAULT for this construction, not EINVAL, for a reason
     * that has nothing to do with the sum-overflow rule this library's
     * <sys/uio.h> documents and this file's vec_check() implements --
     * there is no way to construct a case that isolates ONE from the OTHER
     * with real memory. Both sides refusing the call (-1) is exactly what
     * IS comparable, and is compared. */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        char dummy;
        struct iovec v[2] = { { &dummy, (size_t)SSIZE_MAX }, { &dummy, (size_t)SSIZE_MAX } };
        chk(readv(fd, v, 2));    /* expect -1 */
        chk(writev(fd, v, 2));   /* expect -1 */
        close(fd);
    }

    /* ---- 14. a plainly bad fd: fails immediately, 0 bytes moved. Again,
     * only the RETURN VALUE is compared, and this is the general-case
     * explanation for every "errno not compared" note in this file above:
     * this fd is invalid, so the failure is detected by the REAL glibc
     * read()/write() this test's "ours" build links against (see this
     * file's top comment) -- and that real glibc sets ITS OWN internal
     * errno through a thread-local mechanism (__errno_location()) that
     * bypasses tests/unit/libc_host_errno_shim.c entirely. The shim's
     * plain, non-TLS `int errno;` is what THIS LIBRARY's own code (e.g.
     * vec_check()'s explicit `errno = EINVAL;` in cases 10-12 above) reads
     * and writes, and that round-trip is genuinely comparable because both
     * ends of it are this library's own code. It is NOT what glibc's
     * compiled-in read()/write() touch when THEY fail -- their TLS access
     * is a different machine instruction sequence entirely, resolved at
     * link time to glibc's own per-thread storage, never to a same-named
     * ordinary data symbol supplied by another object file. Confirmed
     * empirically (see git history): built alone against just uio.c and the
     * shim, readv(-1,...)/writev(-1,...) both fail exactly as tested below
     * (return -1) while errno silently reads back as whatever it was set to
     * beforehand -- proving the return value is real and the errno channel
     * for a REAL syscall's failure is simply not observable this way. */
    {
        struct iovec v[1] = { { &id, sizeof id } };
        chk(readv(-1, v, 1));    /* expect -1, 0 bytes moved */
        chk(writev(-1, v, 1));   /* expect -1, 0 bytes moved */
    }

    /* ---- 15. preadv does not disturb the fd's ordinary position ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_RDWR, 0644);
        write(fd, "0123456789", 10);
        close(fd);
        fd = open(SCRATCH, T_O_RDONLY);
        char head[4];
        read(fd, head, 4);                                    /* advances the cursor to 4 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 4 */

        char b[3];
        struct iovec rv[1] = { { b, 3 } };
        chk(preadv(fd, rv, 1, 0));                            /* expect 3, read from offset 0 */
        chk(memcmp(b, "012", 3) == 0);
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 4 still -- untouched by preadv */

        char next;
        read(fd, &next, 1);                                   /* must continue from 4, i.e. '4' */
        chk(next == '4');
        close(fd);
    }

    /* ---- 16. pwritev does not disturb the fd's ordinary position, and
     * really writes at the requested offset ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_RDWR, 0644);
        write(fd, "AAAAAAAAAA", 10);
        close(fd);
        fd = open(SCRATCH, T_O_RDWR);
        char head[3];
        read(fd, head, 3);                                    /* cursor -> 3 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 3 */

        struct iovec wv[1] = { { "ZZ", 2 } };
        chk(pwritev(fd, wv, 1, 5));                           /* expect 2, written at offset 5 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 3 still */

        char next;
        read(fd, &next, 1);                                   /* position 3 was never touched: still 'A' */
        chk(next == 'A');

        char tail[2];
        lseek(fd, 5, SEEK_SET);
        read(fd, tail, 2);
        chk(memcmp(tail, "ZZ", 2) == 0);                      /* the write landed where asked */
        close(fd);
    }

    /* ---- 17. preadv/pwritev on an unseekable fd (a pipe): refused, the
     * same way a real pread/pwrite is refused for the same fd type. errno
     * is not compared here either -- the failure is detected by the real
     * glibc lseek() this test links against (uio.c's at_offset() calls it
     * to save/seek/restore), so the same limitation case 14's comment
     * explains applies: real ESPIPE lands in glibc's own thread-local
     * errno, not in this test's plain shim. The return value alone proves
     * the refusal, which is the behaviour <sys/uio.h> documents. */
    {
        int fd[2];
        pipe(fd);
        char b[1];
        struct iovec rv[1] = { { b, 1 } };
        chk(preadv(fd[0], rv, 1, 0));    /* expect -1 */
        struct iovec wv[1] = { { "x", 1 } };
        chk(pwritev(fd[1], wv, 1, 0));   /* expect -1 */
        close(fd[0]); close(fd[1]);
    }

    /* ---- 18. iovcnt > IOV_MAX on preadv must fail WITHOUT seeking: the
     * fd's position after the refused call proves no lseek happened. ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        read(fd, &(char){0}, 1);                              /* cursor -> 1 */
        long before = lseek(fd, 0, SEEK_CUR);
        chk(before);                                          /* expect 1 */
        static struct iovec v[IOV_MAX + 1];
        errno = 0;
        chk(preadv(fd, v, IOV_MAX + 1, 0));                   /* expect -1 */
        chk(errno == EINVAL);
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 1 still: refused before any seek */
        close(fd);
    }

    /* ================================================================
     * ADVERSARIAL VERIFICATION ADDITIONS (independent second pass over
     * the same unit). Cases 19+ attack boundaries the original 18 cases
     * did not reach: negative preadv/pwritev offsets, a preadv/pwritev
     * iovcnt==0 probe through the offset save/seek/restore path (not
     * just the plain readv/writev path case 9 covers), a completely
     * invalid fd on preadv/pwritev, the "iovcnt==0 on the wrong-
     * direction fd" and "single explicit zero-length iovec on a bad fd"
     * shapes the implementer's own notes describe fixing but the
     * original suite never directly exercises, a REAL (not NULL-fault-
     * synthesized) short-write/EAGAIN pair on a genuinely full
     * nonblocking pipe, and ioctl()'s FIONREAD path. FIONREAD is exactly
     * as host-diffable as readv/writev's own aggregation logic, for the
     * identical reason given in this file's own top comment: our
     * ioctl(FIONREAD) computes its answer from real fstat()/lseek()
     * results, and real glibc's generic FIONREAD for a regular file
     * computes the identical formula in the kernel, so both sides are
     * checking the same arithmetic against the same real numbers.
     * ================================================================ */

    /* ---- 19. preadv/pwritev with a NEGATIVE offset: refused (the
     * seek-to-offset lseek fails), and the fd's ordinary position is left
     * exactly where it was. vec_check() already accepted the vector (it
     * is fine on its own), so this exercises at_offset()'s "the SECOND
     * lseek fails" branch, which case 18 never reaches (case 18 fails
     * inside vec_check(), before any lseek at all). Checked empirically
     * against real Linux (see git history for this file) that a negative
     * SEEK_SET target fails; only the RETURN VALUE is compared, not
     * errno, for the same reason case 17 does not compare it: the
     * failure is detected by the real lseek() this test links against,
     * whose errno is not observable through this test's plain shim (see
     * the long comment above case 14). ---- */
    {
        int fd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_RDWR, 0644);
        write(fd, "0123456789", 10);
        lseek(fd, 3, SEEK_SET);                               /* arbitrary non-zero starting position */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 3 */

        char b[4];
        struct iovec rv[1] = { { b, 4 } };
        chk(preadv(fd, rv, 1, -1));                           /* expect -1 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 3 still */

        struct iovec wv[1] = { { "Z", 1 } };
        chk(pwritev(fd, wv, 1, -1));                          /* expect -1 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 3 still */
        close(fd);
    }

    /* ---- 20. preadv/pwritev with iovcnt == 0: the same "an empty vector
     * still validates the fd" rule case 9 proves for plain readv/writev,
     * but through the offset save/seek/restore path -- proves at_offset()
     * issues its probe call AT the requested offset and still leaves the
     * ordinary position untouched afterward, for BOTH directions. ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        read(fd, &(char){0}, 1);                              /* cursor -> 1 */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 1 */
        chk(preadv(fd, NULL, 0, 5));                          /* expect 0: read probe, fd IS open for reading */
        chk(lseek(fd, 0, SEEK_CUR));                          /* expect 1 still: untouched by the probe */
        close(fd);

        int wfd = open(SCRATCH, T_O_WRONLY);
        chk(pwritev(wfd, NULL, 0, 5));                        /* expect 0: write probe, fd IS open for writing */
        close(wfd);

        int rfd2 = open(SCRATCH, T_O_RDONLY);
        chk(pwritev(rfd2, NULL, 0, 5));                       /* expect -1: write probe on a read-only fd */
        close(rfd2);
    }

    /* ---- 21. preadv/pwritev on a completely invalid fd: the FIRST lseek
     * (saving the current position) fails immediately, so at_offset()
     * must return -1 without ever attempting the offset seek or any I/O.
     * ---- */
    {
        struct iovec rv[1] = { { &id, sizeof id } };
        chk(preadv(-1, rv, 1, 0));                            /* expect -1 */
        struct iovec wv[1] = { { "x", 1 } };
        chk(pwritev(-1, wv, 1, 0));                           /* expect -1 */
    }

    /* ---- 22. iovcnt == 0 with a NON-NULL iov pointer, deliberately
     * "poisoned" (a bogus address, an enormous length): POSIX permits iov
     * to be anything when count is 0 because it is never examined. A real
     * pointer must behave identically to case 9's NULL, proving iovcnt==0
     * truly skips the array rather than reading iov[0] and getting lucky
     * that it happens to be harmless. ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        struct iovec dummy[1] = { { (void *)0x1, (size_t)-1 } };
        chk(readv(fd, dummy, 0));                             /* expect 0 */
        close(fd);
    }

    /* ---- 23. a SINGLE zero-length iovec (iovcnt==1, NOT 0) on a
     * completely bad fd: exactly the scenario the implementer's own notes
     * describe fixing (a zero-length individual entry must be CALLED
     * THROUGH, not skipped). If the loop instead skipped it, an invalid
     * fd with an all-zero-length vector would come back 0 (success)
     * instead of -1 -- confirmed empirically that a real zero-length
     * read()/write() on fd -1 genuinely fails EBADF (see git history), so
     * this is a real behavioural requirement, not a style nicety. ---- */
    {
        struct iovec rv[1] = { { NULL, 0 } };
        chk(readv(-1, rv, 1));                                /* expect -1, not 0 */
        struct iovec wv[1] = { { NULL, 0 } };
        chk(writev(-1, wv, 1));                               /* expect -1, not 0 */
    }

    /* ---- 24. case 9 only tried one direction each way (readv on a
     * readable fd, writev on a non-writable fd); complete the 2x2 with
     * iovcnt==0: readv on a WRITE-only fd (must fail) and writev on that
     * same WRITE-only fd (must succeed). Confirmed empirically (see git
     * history) that a zero-length read() on a write-only fd genuinely
     * fails EBADF, and a zero-length write() on it genuinely succeeds --
     * so both halves of this case are real kernel behaviour, not
     * assumption. ---- */
    {
        int wfd = open(SCRATCH, T_O_CREAT | T_O_TRUNC | T_O_WRONLY, 0644);
        chk(readv(wfd, NULL, 0));                             /* expect -1: fd is NOT open for reading */
        chk(writev(wfd, NULL, 0));                            /* expect 0: fd IS open for writing */
        close(wfd);
    }

    /* ---- 25. INTENTIONALLY NOT A TEST CASE -- documenting what was tried
     * and why it does not belong here. The original plan was a REAL
     * (kernel-detected, not NULL-fault-synthesized) rule-2 case: a
     * multi-iovec writev() to a nonblocking pipe with just enough room for
     * iov[0] but not iov[1], expecting writev_core's loop to land iov[0]
     * for real (46 bytes) and then fail on iov[1], returning 46 rather
     * than -1. Built and run against real glibc (see git history for this
     * file's probe output, both with F_SETPIPE_SZ=4096 and with a plain
     * default-size pipe): real writev() returns -1 with ZERO bytes moved
     * for that whole call, not 46 -- confirmed for a total request both
     * under AND over PIPE_BUF, so this is not the well-known "writes <=
     * PIPE_BUF are atomic" rule specifically, it is broader. A real
     * writev() is ONE kernel operation that evaluates room against the
     * WHOLE requested transfer before moving anything; uio.c's writev(),
     * built out of vec_check()'s reasons (see <sys/uio.h>: no vectored
     * SYS_WRITE exists) LOOPS separate write() calls, so an EARLIER iovec
     * that individually fits can land for real before a LATER one fails --
     * something a genuine single writev() syscall structurally cannot do.
     * This is a real, previously-undocumented divergence from real
     * writev()'s atomicity, not a bug with a fix available inside this
     * file (there is no vectored SYS_WRITE to make it atomic against), so
     * it is written up as trap #3 in this file's OWN top-of-file comment
     * instead of asserted here as if it were proven to hold. ---- */

    /* ---- 26. writev's rule 2, the OTHER branch: an error before ANY
     * progress (moved == 0) on a genuinely full pipe returns -1 -- the
     * same shape as case 14's bad-fd -1, but reached through a real
     * EAGAIN this time instead of an invalid fd. ---- */
    {
        int p[2];
        pipe(p);
        fcntl(p[1], T_F_SETPIPE_SZ, 4096);
        fcntl(p[1], F_SETFL, O_NONBLOCK);
        char fill[4096];
        memset(fill, 'F', sizeof fill);
        chk(write(p[1], fill, sizeof fill));                  /* expect 4096: pipe now completely full */

        struct iovec wv[1] = { { "x", 1 } };
        chk(writev(p[1], wv, 1));                              /* expect -1: zero room, zero progress */
        close(p[0]); close(p[1]);
    }

    /* ---- 27. ioctl(FIONREAD, NULL) on a valid, open regular fd: EFAULT,
     * and this specific sub-case IS safe and comparable through this
     * codepath -- the `!arg` check inside uio.c's FIONREAD case returns
     * before ever reaching fstat(), so it never touches the struct-size
     * hazard documented at length in uio.c right above the fstat() call
     * there (glibc's real `struct stat` is 144 bytes on x86_64; this
     * library's own <sys/stat.h> declares it as 120; a call that resolves
     * to real glibc's fstat() while writing into a local variable sized by
     * OUR header overflows the stack by 24 bytes -- verified directly with
     * `sizeof(struct stat)` compiled both ways). errno IS comparable here
     * (our own explicit assignment on both sides), same as case 30 below.
     *
     * DELIBERATELY NOT TESTED HERE, and this is the important part: the
     * actual FIONREAD byte-count success path (a valid fd with a non-NULL
     * arg) -- proving it would require reaching fstat() with a real,
     * currently-open regular fd, which is EXACTLY the hazard above. That
     * arithmetic was instead verified two other ways: (1) independently,
     * with a small standalone host program using ONLY glibc's own types
     * throughout (no exposure to this library's differently-sized
     * <sys/stat.h> at all), confirming real Linux's generic FIONREAD for a
     * regular file is exactly `size - position`, UNCLAMPED even past EOF
     * (position 100 on a 10-byte file reports -90 there, not 0 -- the
     * second real bug this review found and fixed in uio.c, which used to
     * clamp to 0); (2) by reading uio.c's FIONREAD case, which performs
     * that exact formula against real SYS_FSTAT/SYS_LSEEK numbers on the
     * actual target, where fstat() is this library's OWN implementation
     * (dirstat.c) and the struct-size question does not arise at all
     * (both sides of that call agree, because they are the same header). */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        errno = 0;
        chk(ioctl(fd, FIONREAD, NULL));                         /* expect -1 */
        chk(errno == EFAULT);
        close(fd);
    }

    /* ---- 28. ioctl() unrecognised request / TIOCGWINSZ / TIOCGPGRP on a
     * regular (non-tty) fd: ENOTTY on real Linux too (checked empirically,
     * see git history) -- not even a claimed divergence. errno is our own
     * explicit assignment on both sides, so directly comparable. ---- */
    {
        int fd = open(SCRATCH, T_O_RDONLY);
        int n;
        errno = 0; chk(ioctl(fd, TIOCGWINSZ, &n));  chk(errno == ENOTTY);
        errno = 0; chk(ioctl(fd, TIOCGPGRP, &n));   chk(errno == ENOTTY);
        errno = 0; chk(ioctl(fd, 0x9999UL, &n));    chk(errno == ENOTTY);
        close(fd);
    }

    /* ---- 29. ioctl() on a completely invalid fd: -1, for every request
     * shape, not just FIONREAD -- proves the fd-existence check runs
     * BEFORE request dispatch, matching real Linux (checked empirically,
     * see git history: ioctl(-1, ANY_REQUEST, ...) is EBADF there, never
     * whatever that specific request would otherwise answer -- e.g. a bad
     * fd with TIOCGWINSZ is EBADF, not ENOTTY). This regression-tests a
     * second real bug this review found and fixed in uio.c: the original
     * ioctl() never checked fd validity at all outside the FIONREAD case,
     * so a bad fd with TIOCGWINSZ/TIOCGPGRP/an unrecognised request used
     * to answer ENOTTY instead of EBADF. errno is not compared here: the
     * fd check is fstat(), which -- like read/write/lseek elsewhere in
     * this file -- is not one of this unit's own functions, so in the
     * "ours" host-diff build it resolves to REAL glibc's fstat() and sets
     * glibc's own internal TLS errno, invisible to this test's plain shim
     * (the same limitation documented at length above case 14). The
     * return value alone proves the check runs and rejects the fd. ---- */
    {
        int n;
        chk(ioctl(-1, FIONREAD, &n));                          /* expect -1 */
        chk(ioctl(-1, TIOCGWINSZ, &n));                         /* expect -1 */
        chk(ioctl(-1, 0x9999UL, &n));                           /* expect -1 */
    }

    /* ---- 30. FIONREAD with BOTH a bad fd and a NULL arg together: fd
     * validity wins (EBADF beats EFAULT) -- checked empirically against
     * real Linux (see git history) -- matching the fd-check-first order
     * case 29's fix establishes. Return value only, same errno limitation
     * as case 29. ---- */
    {
        chk(ioctl(-1, FIONREAD, NULL));                         /* expect -1 */
    }

    unlink(SCRATCH);
    return 0;
}
