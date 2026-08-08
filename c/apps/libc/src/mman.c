/* <sys/mman.h> over SYS_MMAP/SYS_MUNMAP. See the header for the anonymous-only
 * limitation and why mprotect/msync/mlock always fail. */
#include <sys/mman.h>
#include <errno.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    if (len == 0) { errno = EINVAL; return MAP_FAILED; }
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) { errno = EINVAL; return MAP_FAILED; }
    if (!(flags & MAP_ANONYMOUS) || fd != -1 || offset != 0) {
        /* Every real file/device in this kernel and every non-anonymous
         * request lands here: there is no file-backed mapping to give it. */
        errno = ENODEV;
        return MAP_FAILED;
    }
    if (flags & MAP_FIXED) { errno = EINVAL; return MAP_FAILED; }   /* can't be forced */
    if (!(flags & (MAP_SHARED | MAP_PRIVATE))) { errno = EINVAL; return MAP_FAILED; }

    int kprot = 0;
    if (prot & PROT_READ)  kprot |= 0x1;
    if (prot & PROT_WRITE) kprot |= 0x2;
    if (prot & PROT_EXEC)  kprot |= 0x4;
    if (kprot == 0) kprot = 0x1;   /* PROT_NONE: still needs a mapping to exist */

    long base = sys(SYS_MMAP, (long)len, kprot, (long)addr);
    if (base == 0) { errno = ENOMEM; return MAP_FAILED; }
    return (void *)base;
}

int munmap(void *addr, size_t len)
{
    if (!addr || len == 0) { errno = EINVAL; return -1; }
    long r = sys(SYS_MUNMAP, (long)addr, (long)len, 0);
    if (r != 0) { errno = EINVAL; return -1; }
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{ (void)addr; (void)len; (void)prot; errno = ENOSYS; return -1; }
int msync(void *addr, size_t len, int flags)
{ (void)addr; (void)len; (void)flags; errno = ENOSYS; return -1; }
int mlock(const void *addr, size_t len)
{ (void)addr; (void)len; errno = ENOSYS; return -1; }
int munlock(const void *addr, size_t len)
{ (void)addr; (void)len; errno = ENOSYS; return -1; }
