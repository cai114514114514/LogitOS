#ifndef _LOGIT_SHM_H
#define _LOGIT_SHM_H
#include <stddef.h>

/* NAMED SHARED MEMORY, over SYS_SHM_OPEN/MAP/UNLINK/CLOSE (176-179).
 *
 * ===========================================================================
 * WHY THESE ARE NOT CALLED shm_open() AND shm_unlink()
 *
 * They are the POSIX operations and they are deliberately NOT given the POSIX
 * names, because the POSIX ones promise something this kernel does not give and
 * a caller would only find out by corrupting its own data.
 *
 * POSIX shm_open() returns a FILE DESCRIPTOR. That is not a detail; it is the
 * whole interface, because the next two lines a program writes are always:
 *
 *     fd = shm_open("/thing", O_CREAT | O_RDWR, 0600);
 *     ftruncate(fd, size);                                  <-- sets the size
 *     p  = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
 *
 * Every one of those steps needs the handle to BE an fd: ftruncate() takes one,
 * mmap() takes one, fstat() reports the size through one, and close() releases
 * it. A `shm_open` here that returned a segment handle instead would sail
 * through the compiler and then hand `ftruncate` and `mmap` an integer that
 * means something else entirely in the fd table -- most likely a valid fd
 * belonging to something else. That is not a missing feature, it is a loaded
 * gun, and it is exactly the "silently degraded" outcome <sys/mman.h> refuses
 * a file mapping for.
 *
 * Making them real POSIX would mean putting segments in the process fd table
 * (c/kernel/exec/proc.c and file.c), which is another line's file. Until then
 * the honest thing is a name that promises only what it does. When the fd
 * integration lands, `shm_open` can be added beside these and mean what it
 * says.
 *
 * WHAT IS FULLY POSIX AND NEEDS NOTHING FROM THIS HEADER:
 *
 *     mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0)
 *
 * That call requires no fd, so it has no such problem, and <sys/mman.h> now
 * implements it exactly: memory shared with every child across fork(), released
 * when the last mapping goes. A program that only needs to share with its own
 * children should use that and never include this header.
 *
 * ===========================================================================
 * WHAT A SEGMENT COSTS. Its pages are allocated when it is created and CANNOT
 * be reclaimed while it exists -- there is nowhere to evict them to, since
 * swapping a shared page would break the sharing and there is no file behind it
 * to re-read. A segment is memory spent until it is unlinked and unmapped. The
 * kernel's ceiling is 8 segments of 2 MiB each.
 *
 * WHAT YOU CANNOT DO OVER ONE, and it is the first thing most callers reach
 * for: BLOCK. SYS_FUTEX keys its wait queue on (address space, virtual
 * address), so two processes waiting on the same word in a shared segment are
 * in two different queues and neither will ever wake the other. Cross-process
 * synchronisation over a segment must POLL today. sem_init(pshared != 0) and
 * sem_open() still return ENOSYS for that reason rather than handing back a
 * semaphore that never wakes anybody -- see <semaphore.h>.
 * ======================================================================== */

/* Flags for lshm_open(). */
#define LSHM_CREAT  0x1     /* create it if it does not exist */
#define LSHM_EXCL   0x2     /* with LSHM_CREAT: fail if it already exists */
#define LSHM_WRITE  0x4     /* the caller intends to write; needs the w bit */

/* The longest name, in bytes, NOT counting the NUL -- the kernel's table holds
 * 32 bytes per name (SHM_NAMEMAX in c/kernel/mm/shm.h) and one of them is the
 * terminator. Exposed rather than left to be discovered because a name that is
 * one byte too long is refused, not truncated, and a caller building a name
 * from a pid or a path needs the budget before it builds it. */
#define LSHM_NAMEMAX 31

/* Open, and optionally create, a named segment. `pages` and `mode` are used
 * only when creating -- an open of an existing segment gets the size it already
 * has, never the size this caller guessed.
 *
 * `name` is at most 31 bytes and MAY NOT CONTAIN '/': this is a kernel table,
 * not a filesystem, and a name that looked like a path would invite the belief
 * that the two namespaces agree.
 *
 * Returns a handle >= 0, or -1 with errno set (ENOENT, EEXIST, EACCES, ENOMEM,
 * EINVAL). */
int lshm_open(const char *name, unsigned pages, unsigned mode, unsigned flags);

/* Map `len` bytes of a segment starting at page `page_off`. `prot` is the
 * PROT_* set from <sys/mman.h>, and PROT_WRITE is honoured -- unlike a file
 * mapping, a segment's frames ARE the storage, so a write has nothing to be
 * flushed back to and is simply visible to everyone else mapping it.
 *
 * Returns the base address, or MAP_FAILED. Unmap it with munmap(). */
void *lshm_map(int handle, size_t len, unsigned page_off, int prot);

/* Remove the NAME. The memory lives until the last handle and the last mapping
 * are gone, so "create, map, unlink" is the safe idiom: everyone who already
 * has it keeps it, and no latecomer can join. Needs write permission. */
int lshm_unlink(const char *name);

/* Drop this handle. Does NOT unmap anything -- a mapping holds its own
 * reference -- which is what makes open/map/close/unlink complete. */
int lshm_close(int handle);

#endif /* _LOGIT_SHM_H */
