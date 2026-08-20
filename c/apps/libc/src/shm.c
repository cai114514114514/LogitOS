/* Named shared memory, over SYS_SHM_OPEN/MAP/UNLINK/CLOSE (176-179).
 *
 * The interface and the argument for its NAMES are in <logit_shm.h>; this file
 * is only the translation. It exists as a separate TU from mman.c for one
 * reason worth stating: <sys/mman.h> is POSIX and every declaration in it means
 * what POSIX says it means, while these four are this kernel's own and are
 * named so a reader cannot mistake them for the POSIX ones. Putting them in
 * mman.c would have blurred exactly the line the header spends a page drawing.
 *
 * AS_LIBC is a wildcard over c/apps/libc/src (Makefile:718), so this file
 * needed no build-system change.
 *
 * THE ERRNO MAPPING IS THE WHOLE OF THE WORK HERE, and it is written out one
 * case at a time rather than folded into "anything negative is EINVAL". The
 * kernel already went to the trouble of distinguishing "no such segment" from
 * "not yours" from "out of memory" (shm.h argues why: a caller that cannot tell
 * them apart cannot report any of them, and they send an operator to three
 * different places). Collapsing them here would throw that away at the last
 * step, in the one layer the application actually reads. */
#include <logit_shm.h>
#include <sys/mman.h>
#include <errno.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* One place, so every entry point below answers the same way. Returns -1 with
 * errno set, so a caller can `return shm_errno(r);` and be done. */
static int shm_errno(long r)
{
    switch (r) {
    case SHM_E_NOENT: errno = ENOENT; break;
    case SHM_E_EXIST: errno = EEXIST; break;
    case SHM_E_ACCES: errno = EACCES; break;
    case SHM_E_NOMEM: errno = ENOMEM; break;
    case SHM_E_INVAL: errno = EINVAL; break;
    /* An unrecognised negative is NOT quietly turned into EINVAL: EINVAL claims
     * the caller passed nonsense and would send the author to re-read their own
     * arguments, which is the wrong place when the kernel has returned a code
     * this libc has not been taught. */
    default:          errno = ENOTSUP; break;
    }
    return -1;
}

/* The name is validated HERE as well as in the kernel, and the duplication is
 * deliberate for the reason mman.c gives about writable file mappings: the
 * caller gets the same answer whichever side checks first, and the reason can
 * be stated in the errno it will actually look at. ENAMETOOLONG and EINVAL are
 * two different mistakes and the kernel's single SHM_E_INVAL cannot say which.
 *
 * The '/' refusal matters most. A caller that writes "/frames" out of POSIX
 * habit gets a refusal at the call instead of a segment whose name silently
 * differs from every other process's idea of it. */
static int name_bad(const char *name)
{
    if (!name || !name[0]) { errno = EINVAL; return 1; }
    int i = 0;
    for (; name[i]; i++) {
        if (i >= LSHM_NAMEMAX) { errno = ENAMETOOLONG; return 1; }
        if (name[i] == '/')    { errno = EINVAL; return 1; }
    }
    return 0;
}

int lshm_open(const char *name, unsigned pages, unsigned mode, unsigned flags)
{
    if (name_bad(name)) return -1;
    /* LSHM_* and SHM_O_* happen to share their three values, but they are two
     * ABIs -- one this header's, one the kernel's -- and nothing keeps them
     * equal, so the conversion is written out rather than assumed. mman.c makes
     * the identical move between PROT_* and MMAP_PROT_*. */
    unsigned kf = 0;
    if (flags & LSHM_CREAT) kf |= SHM_O_CREAT;
    if (flags & LSHM_EXCL)  kf |= SHM_O_EXCL;
    if (flags & LSHM_WRITE) kf |= SHM_O_WRITE;
    if (flags & ~(unsigned)(LSHM_CREAT | LSHM_EXCL | LSHM_WRITE)) {
        errno = EINVAL;                 /* an unknown flag is refused rather than
                                         * dropped: a caller that asked for
                                         * something this libc does not know about
                                         * must not be told it got it */
        return -1;
    }
    long r = sys(SYS_SHM_OPEN, (long)name, (long)pages,
                 (long)(((mode & 0777u) << 4) | kf));
    if (r < 0) return shm_errno(r);
    return (int)r;
}

void *lshm_map(int handle, size_t len, unsigned page_off, int prot)
{
    if (handle < 0 || len == 0) { errno = EINVAL; return MAP_FAILED; }
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) { errno = EINVAL; return MAP_FAILED; }

    int kprot = 0;
    if (prot & PROT_READ)  kprot |= MMAP_PROT_READ;
    if (prot & PROT_WRITE) kprot |= MMAP_PROT_WRITE;
    if (prot & PROT_EXEC)  kprot |= MMAP_PROT_EXEC;
    if (kprot == 0) kprot = MMAP_PROT_READ;   /* PROT_NONE floor, as SYS_MMAP's is */

    /* The page offset is checked against the 32 bits the ABI gives it before it
     * is shifted, not after. Letting it wrap would turn "map from page 2^32"
     * into "map from page 0" -- a request for the far end of a segment quietly
     * served from the near end, which is the wrong-index bug mm_shm_test.c
     * exists to catch, reintroduced above the syscall where that test cannot
     * see it. */
    if (page_off > 0xFFFFFFFFu) { errno = EINVAL; return MAP_FAILED; }

    long r = sys(SYS_SHM_MAP, (long)handle, (long)len,
                 (long)(((unsigned long long)(unsigned)kprot << 32) | page_off));
    if (r == 0) {
        /* SYS_SHM_MAP has ONE failure value and several causes: a dead handle,
         * a range past the end of the segment, or no VMA slot. They are not
         * distinguishable at the ABI, and inventing a distinction here would be
         * guessing at which one it was. ENOMEM is the one a caller has a
         * sensible response to (map less, or map in pieces). */
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)r;
}

int lshm_unlink(const char *name)
{
    if (name_bad(name)) return -1;
    long r = sys(SYS_SHM_UNLINK, (long)name, 0, 0);
    if (r < 0) return shm_errno(r);
    return 0;
}

int lshm_close(int handle)
{
    if (handle < 0) { errno = EINVAL; return -1; }
    sys(SYS_SHM_CLOSE, (long)handle, 0, 0);
    /* SYS_SHM_CLOSE cannot fail -- it drops a handle, and a handle that was
     * never live is a no-op in the kernel. Returning int anyway so that a
     * per-process handle table (mmsys.c names it as the missing piece) can
     * start reporting EBADF here without changing every caller's shape. */
    return 0;
}
