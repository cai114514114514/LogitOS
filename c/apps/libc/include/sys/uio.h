#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#include <sys/types.h>

/* readv/writev/preadv/pwritev: scatter/gather I/O over an array of buffers.
 * The kernel (include/abi/logit_abi.h) has no vectored read/write syscall --
 * SYS_READ and SYS_WRITE each take exactly one (buf, len) -- so these are a
 * pure userland LOOP over read()/write(). See uio.c for the two rules a naive
 * loop gets wrong (short transfers, and errors after partial progress).
 *
 * IOV_MAX matches Linux's actual kernel limit (UIO_MAXIOV in
 * include/linux/uio.h / IOV_MAX in glibc's bits/xopen_lim.h), not the POSIX
 * floor of 16, so a program written against the real number behaves the same
 * here. It is defined HERE rather than in <limits.h> because this constant is
 * only meaningful next to <sys/uio.h>'s functions, and <limits.h> is owned by
 * another unit of this library. */
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
/* writev() is a LOOP over write() (no vectored SYS_WRITE exists -- see
 * above), which is exact for ordinary files but gives up one guarantee real
 * writev() has on a pipe: if the vector does not ENTIRELY fit the pipe's
 * available room, a real writev() fails the whole call atomically (nothing
 * moves); this loop can let an earlier iovec that individually fits land
 * for real before a later one fails, returning a partial count instead.
 * See the long comment in uio.c (found by an adversarial host-diff review,
 * empirically confirmed against real glibc) for the full story -- there is
 * no fix available without a real vectored write syscall. */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

/* preadv/pwritev take an explicit offset and must not disturb the fd's file
 * position. There is no kernel pread/pwrite (no syscall does I/O "at offset
 * X, leave the cursor alone"), so uio.c fakes it: save the current position
 * with lseek, seek to `offset`, run the ordinary readv/writev loop, then seek
 * back. That is honest about NOT being atomic -- a concurrent lseek on the
 * same fd from another thread (SYS_THREAD_CREATE threads share an fd table)
 * can interleave with it, which a real preadv(2) syscall would never allow.
 * See the comment on lseek_pread() in uio.c before relying on this across
 * threads sharing a fd. */
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

#endif /* _SYS_UIO_H */
