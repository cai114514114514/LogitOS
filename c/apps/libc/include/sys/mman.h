#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <stddef.h>

#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif

/* mmap() over SYS_MMAP/SYS_MUNMAP (include/abi/logit_abi.h).
 *
 * ANONYMOUS ONLY -- READ THIS BEFORE PORTING ANYTHING THAT MEMORY-MAPS A FILE.
 * The kernel syscall this wraps reserves address space and fills it with zero
 * pages on first touch; it has no file behind it at all. There is no
 * file-backed mapping ANYWHERE in this kernel: ELF images are read eagerly
 * into anonymous frames at load time and every fd's data lives in a kmalloc
 * buffer (c/kernel/exec/file.c), never mapped into a process's page tables.
 * So mmap() here honours exactly MAP_ANONYMOUS|MAP_PRIVATE with fd==-1 and
 * offset==0, which is the shape malloc.c's own arena reservation already uses
 * (see the long comment in c/apps/libc/src/malloc.c) -- and REFUSES a request
 * naming a real fd (MAP_FAILED/ENODEV) rather than silently degrading to an
 * anonymous mapping that reads back zeros where a caller expected file bytes.
 * A caller that gets MAP_FAILED here is getting the truth: this cannot be
 * done, not "this was done wrong."
 *
 * MAP_FIXED is refused (EINVAL) for the same reason: the kernel's SYS_MMAP
 * only ever picks the address (a `hint` is honoured only if free, never
 * forced), so a MAP_FIXED caller that got success back would be trusting a
 * placement nothing verified.
 *
 * mprotect(): there is no syscall to change a mapping's permissions after the
 * fact, so it always fails (ENOSYS) rather than pretending a protection
 * change took effect.
 * msync(): there is no file-backed mapping to flush (see above), so it always
 * fails (ENOSYS) rather than reporting a successful sync of nothing.
 * mlock()/munlock(): there is no page-pinning primitive, so they always fail
 * (ENOSYS) rather than claiming a guarantee this kernel cannot make. */

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      1
#define MS_SYNC       2
#define MS_INVALIDATE 4

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t len, int flags);
int   mlock(const void *addr, size_t len);
int   munlock(const void *addr, size_t len);

#endif /* _SYS_MMAN_H */
