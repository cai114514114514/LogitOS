/* <sys/mman.h> over SYS_MMAP / SYS_MMAP_FILE / SYS_MPROTECT / SYS_MUNMAP.
 * See the header for what each request shape does and why the ones that are
 * refused are refused. */
#include <sys/mman.h>
#include <errno.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* MMAP_PROT_* and PROT_* happen to have the same three values, but they are two
 * different ABIs (one POSIX, one this kernel's) and nothing keeps them equal --
 * so the conversion is written out rather than assumed. */
static int kprot_of(int prot)
{
    int k = 0;
    if (prot & PROT_READ)  k |= MMAP_PROT_READ;
    if (prot & PROT_WRITE) k |= MMAP_PROT_WRITE;
    if (prot & PROT_EXEC)  k |= MMAP_PROT_EXEC;
    return k;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    if (len == 0) { errno = EINVAL; return MAP_FAILED; }
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) { errno = EINVAL; return MAP_FAILED; }
    if (flags & MAP_FIXED) { errno = EINVAL; return MAP_FAILED; }   /* can't be forced */
    if (!(flags & (MAP_SHARED | MAP_PRIVATE))) { errno = EINVAL; return MAP_FAILED; }

    if (flags & MAP_ANONYMOUS) {
        if (fd != -1 || offset != 0) { errno = EINVAL; return MAP_FAILED; }

        /* MAP_SHARED ON AN ANONYMOUS MAPPING. THIS USED TO BE ENOTSUP, and the
         * comment here argued at length that it had to be: fork mapped every
         * anonymous page copy-on-write, so a parent and child sharing a ring
         * buffer through such a mapping would each get their own copy on the
         * first write and neither would learn why. That argument was correct
         * and is now spent -- c/kernel/mm/shm.c gives a segment its own frames
         * and vmm_clone_user hands a VMM_PTE_SHM page to the child WITHOUT
         * downgrading it, which is the exact hazard the refusal was written
         * against. tests/unit/mm_shm_test.c asserts frame identity across the
         * fork, and -DSHM_FORK_COPY is the old behaviour on a switch, required
         * to fail.
         *
         * TWO SYSCALLS, NOT ONE, and the middle one is why this is not simply
         * SYS_MMAP with a flag: the segment is the object and the mapping is a
         * view of it (logit_abi.h). Create it unnamed -- nothing can look it up,
         * so it is reachable only through this mapping and whatever inherits it
         * across fork, which is exactly MAP_ANONYMOUS's promise.
         *
         * THEN CLOSE THE HANDLE IMMEDIATELY, which is the whole lifetime
         * argument in one line. The MAPPING holds its own reference
         * (vma_reserve_shm took one), so after the close the region is kept
         * alive by precisely the thing POSIX says keeps it alive -- the mapping
         * -- and munmap() frees it. Holding the handle instead would leak a
         * segment table slot per mmap, and there are eight. */
        if (flags & MAP_SHARED) {
            /* PROT_NONE floors to READ as the private branch does, but the
             * WRITE bit is passed through: a segment's frames ARE the storage,
             * so unlike a file mapping there is nothing to write back to. */
            int sprot = kprot_of(prot);
            if (sprot == 0) sprot = MMAP_PROT_READ;

            /* Rounded up here so the page count handed to the kernel covers the
             * bytes the caller asked for. A partial page still costs a whole
             * frame -- there is no sub-page sharing -- and truncating instead
             * would hand back a mapping one page short of the request, whose
             * last bytes fault forever. */
            /* The round-up is guarded BEFORE it happens. `len + 0xFFF` on a
             * length near SIZE_MAX wraps to a small number, which would ask the
             * kernel for a handful of pages and hand the caller a mapping
             * astronomically smaller than it requested. The kernel refuses that
             * request too (vma_reserve_shm re-derives the rounded length and
             * rejects it when it comes out below the original), but relying on
             * that would make this function's correctness depend on a bounds
             * check in another file -- and the value it would pass is already
             * wrong by then. */
            if ((unsigned long)len > (unsigned long)-1 - 0xFFF) { errno = ENOMEM; return MAP_FAILED; }
            unsigned long pages = ((unsigned long)len + 0xFFF) >> 12;
            if (pages == 0 || pages > 0xFFFFFFFFul) { errno = ENOMEM; return MAP_FAILED; }

            long h = sys(SYS_SHM_OPEN, 0, (long)pages, 0);
            if (h < 0) {
                /* SHM_E_NOMEM covers both "no table slot" (8 segments) and "not
                 * enough frames"; SHM_E_INVAL here means the request was larger
                 * than SHM_PAGEMAX, i.e. over 2 MiB. Both are ENOMEM to the
                 * caller, because both mean "ask for less", which is the only
                 * response either admits. The 2 MiB ceiling is stated in
                 * <logit_shm.h> so it is a documented limit rather than a
                 * surprise at a size nobody wrote down. */
                errno = ENOMEM;
                return MAP_FAILED;
            }
            long base = sys(SYS_SHM_MAP, h, (long)len,
                            (long)((unsigned long long)(unsigned)sprot << 32));
            sys(SYS_SHM_CLOSE, h, 0, 0);       /* unconditional: on the failure
                                                * path this is what frees the
                                                * segment, since no mapping took
                                                * a reference */
            if (base == 0) { errno = ENOMEM; return MAP_FAILED; }
            return (void *)base;
        }

        int kprot = kprot_of(prot);
        if (kprot == 0) kprot = MMAP_PROT_READ;   /* PROT_NONE: SYS_MMAP floors prot at
                                                   * READ on purpose (logit_abi.h). Reach
                                                   * a genuinely inaccessible page through
                                                   * mprotect() below, which CAN express
                                                   * it -- that asymmetry is deliberate
                                                   * and argued in the header. */
        long base = sys(SYS_MMAP, (long)len, kprot, (long)addr);
        if (base == 0) { errno = ENOMEM; return MAP_FAILED; }
        return (void *)base;
    }

    /* ---- a real file ----------------------------------------------------
     * THIS BRANCH IS WHY THE HEADER'S "ANONYMOUS ONLY" NOTE IS GONE. The
     * kernel grew SYS_MMAP_FILE (162) against the page cache in
     * c/kernel/mm/pcache.h, elf_load has been using it to map program text
     * since, and until now ring 3 could not reach it through mmap() at all --
     * this function returned ENODEV to every request naming an fd.
     *
     * A SEPARATE SYSCALL, NOT A FLAG, and the libc side does not undo that:
     * the argument in include/abi/logit_abi.h is that fd + 64-bit offset +
     * length + hint do not fit the three-register convention, so the file form
     * takes a struct pointer. Packing them back into SYS_MMAP's three
     * registers here would mean inventing a second, narrower encoding of the
     * same call and would put a 32-bit ceiling on `offset` for no gain. */
    if (fd < 0) { errno = EBADF; return MAP_FAILED; }
    if (offset < 0 || ((unsigned long)offset & 0xFFF)) { errno = EINVAL; return MAP_FAILED; }

    /* WRITABLE FILE MAPPINGS ARE REFUSED HERE TOO, before the syscall, and the
     * duplication is on purpose. The kernel refuses them (mmsys.c returns
     * LOGIT_MMAP_FILE_E_WRITE) because there is no dirty page, no writeback
     * and no msync anywhere in this kernel; refusing them here as well means a
     * caller gets the same answer whether or not the kernel it is running on
     * has that check, and means the reason can be stated in the errno the
     * caller will actually look at. NOT DOWNGRADED to a private copy: a
     * program whose writes go nowhere and whose author has no way to find out
     * is the exact failure this whole file is written against.
     *
     * MAP_SHARED is ACCEPTED on a read-only file mapping, and that is not a
     * courtesy either -- it is the truth. pcache keys its pages on (dev, ino),
     * so two processes mapping the same file really are looking at the same
     * physical frames; a read-only MAP_SHARED is precisely what this kernel
     * gives. What MAP_SHARED additionally promises for WRITES is unreachable,
     * and writes are refused on the line above. */
    if (prot & PROT_WRITE) { errno = ENOTSUP; return MAP_FAILED; }

    struct logit_mmap_file_req req;
    req.hint = (unsigned long long)(unsigned long)addr;
    req.len  = (unsigned long long)len;
    req.off  = (unsigned long long)offset;
    req.fd   = fd;
    req.prot = kprot_of(prot);
    if (req.prot == 0) req.prot = MMAP_PROT_READ;    /* PROT_NONE floor, as above */

    long base = sys(SYS_MMAP_FILE, (long)&req, 0, 0);
    if (base == LOGIT_MMAP_FILE_E_WRITE) { errno = ENOTSUP; return MAP_FAILED; }
    if (base == 0) {
        /* The kernel's one generic failure: a bad or non-regular fd, no VMA
         * slot, or a file the page cache cannot open (not a LogitFS regular
         * file). They are not distinguishable at the ABI and inventing a
         * distinction here would be guessing. ENODEV keeps the answer this
         * function has always given for "there is no file mapping to be had
         * for this thing", so a caller's existing fallback to read() still
         * fires. */
        errno = ENODEV;
        return MAP_FAILED;
    }
    return (void *)base;
}

int munmap(void *addr, size_t len)
{
    if (!addr || len == 0) { errno = EINVAL; return -1; }
    long r = sys(SYS_MUNMAP, (long)addr, (long)len, 0);
    if (r != 0) { errno = EINVAL; return -1; }
    return 0;
}

/* mprotect. Was ENOSYS unconditionally; SYS_MPROTECT (166) is what it now
 * reaches. PROT_NONE is a REAL value here and not floored to PROT_READ the way
 * mmap()'s is -- see <sys/mman.h> for the argument, which is the whole reason
 * pthread_create can now put a guard page under a thread stack. */
int mprotect(void *addr, size_t len, int prot)
{
    if (len == 0) { errno = EINVAL; return -1; }
    if ((unsigned long)addr & 0xFFF) { errno = EINVAL; return -1; }   /* POSIX: the
                                                                       * address must be
                                                                       * page aligned */
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) { errno = EINVAL; return -1; }

    long r = sys(SYS_MPROTECT, (long)addr, (long)len, kprot_of(prot));
    if (r == 0) return 0;
    switch (r) {
    case LOGIT_MPROTECT_E_NOMEM: errno = ENOMEM;  break;   /* part of the range is not mapped */
    case LOGIT_MPROTECT_E_ACCES: errno = ENOTSUP; break;   /* PROT_WRITE on a file mapping:
                                                            * never, not "not right now" */
    default:                     errno = EINVAL;  break;
    }
    return -1;
}

/* msync() STAYS ENOSYS even though file-backed mappings now exist, because the
 * thing it exists to do still does not: every mapping this kernel can hand out
 * is READ-ONLY (mmap() above refuses PROT_WRITE on a file, and the kernel
 * refuses it again), so there has never been a dirty page to write back. A
 * caller that reached msync is either flushing nothing -- in which case 0 and
 * ENOSYS are equally true and ENOSYS is the one that does not teach it a habit
 * this kernel cannot keep -- or expecting a writeback that does not exist. */
int msync(void *addr, size_t len, int flags)
{ (void)addr; (void)len; (void)flags; errno = ENOSYS; return -1; }
int mlock(const void *addr, size_t len)
{ (void)addr; (void)len; errno = ENOSYS; return -1; }
int munlock(const void *addr, size_t len)
{ (void)addr; (void)len; errno = ENOSYS; return -1; }
