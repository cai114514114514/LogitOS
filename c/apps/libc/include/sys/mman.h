#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <stddef.h>

#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif

/* mmap() over SYS_MMAP / SYS_MMAP_FILE / SYS_MPROTECT / SYS_MUNMAP
 * (include/abi/logit_abi.h).
 *
 * THIS COMMENT USED TO OPEN "ANONYMOUS ONLY" AND SAY THERE WAS NO FILE-BACKED
 * MAPPING ANYWHERE IN THIS KERNEL. Both stopped being true, and the second one
 * had been false for a while: SYS_MMAP_FILE (162) exists, elf_load has been
 * mapping program text through the page cache with it, and the only thing
 * missing was a way for ring 3 to ASK. That is what this file is now.
 *
 * WHAT EACH REQUEST SHAPE GETS:
 *
 *   MAP_ANONYMOUS|MAP_PRIVATE, fd -1, offset 0
 *       address space reserved, zero-filled on first touch. The shape
 *       malloc.c's arena and pthread_create's stacks use. Reserving is nearly
 *       free -- the frames arrive on the fault, so a 64 MiB reservation that
 *       is 200 KiB used costs 200 KiB.
 *
 *   MAP_ANONYMOUS|MAP_SHARED
 *       REAL SHARED MEMORY, and PROT_WRITE is honoured. Backed by an unnamed
 *       segment (c/kernel/mm/shm.h) over SYS_SHM_OPEN+SYS_SHM_MAP; the memory
 *       survives fork as THE SAME MEMORY, not as a copy-on-write copy, and is
 *       freed when the last mapping goes.
 *
 *       THIS WAS ENOTSUP UNTIL SHARED MEMORY LANDED, and the reason is worth
 *       keeping: fork used to map every anonymous page copy-on-write, so a
 *       parent and child sharing a lock word this way would each get their own
 *       copy on the first write and neither would ever learn why. The refusal
 *       was the honest answer then. What makes it wrong now is one branch in
 *       vmm_clone_user, and tests/unit/mm_shm_test.c pins it by asserting the
 *       parent and child resolve to the SAME PHYSICAL FRAME.
 *
 *       TWO LIMITS, both from the kernel's segment table and both worth knowing
 *       before designing around this: a single mapping is at most 2 MiB, and
 *       there are 8 segments on the machine. Over either, ENOMEM.
 *
 *       AND IT CANNOT BE RECLAIMED. These pages are allocated up front and are
 *       structurally unevictable -- swapping a shared page breaks the sharing
 *       and there is no file behind it to re-read -- so this is memory spent
 *       until it is unmapped, unlike the MAP_PRIVATE reservation above.
 *
 *       WHAT YOU STILL CANNOT DO OVER IT IS BLOCK. SYS_FUTEX keys its wait
 *       queue on (address space, virtual address), so two processes waiting on
 *       the same word in one segment sit in two different queues and neither
 *       ever wakes the other. Cross-process synchronisation here must POLL --
 *       which is why sem_init(pshared != 0) is still ENOSYS rather than a
 *       semaphore that never wakes anybody.
 *
 *   a real fd, PROT_READ (and/or PROT_EXEC), any offset that is page aligned
 *       a FILE-BACKED mapping through c/kernel/mm/pcache.h. Pages are keyed on
 *       (dev, ino), so two processes mapping one file share the frames, and a
 *       page nobody is using can be dropped and re-read rather than swapped.
 *       MAP_SHARED and MAP_PRIVATE both succeed and both give the same thing,
 *       which is the honest answer while writes are impossible.
 *
 *   a real fd with PROT_WRITE
 *       REFUSED (ENOTSUP), never downgraded to a private copy. There is no
 *       dirty page, no writeback and no msync in this kernel, so the
 *       alternative is a program whose writes go nowhere and whose author has
 *       no way to find out.
 *
 * MAP_FIXED is refused (EINVAL): the kernel only ever PICKS the address (a
 * `hint` is honoured if it is free, never forced), so a MAP_FIXED caller that
 * got success back would be trusting a placement nothing verified.
 *
 * mprotect(): REAL, over SYS_MPROTECT. It changes the protection of a range
 * that is already reserved, and PROT_NONE is a real, grantable value -- which
 * mmap()'s prot is NOT (it floors PROT_NONE at PROT_READ, because a
 * reservation nobody may touch is indistinguishable from not making one). The
 * asymmetry is the guard page: mprotect(PROT_NONE) on a page of an existing
 * mapping is distinguishable from not reserving it, because the address stays
 * RESERVED and no later mmap can be placed there. munmap on that page would
 * work exactly once and then hand the hole to the next allocation.
 * Returns ENOMEM if any page of the range is unmapped (the range is left
 * untouched, never protected as far as it goes), ENOTSUP for PROT_WRITE over a
 * file mapping, EINVAL for a misaligned address or an unknown protection bit.
 *
 * msync(): still ENOSYS. Every mapping above is read-only, so there has never
 * been a dirty page to flush; returning 0 would teach a caller a habit this
 * kernel cannot keep.
 * mlock()/munlock(): there is no page-pinning primitive reachable from ring 3,
 * so they always fail (ENOSYS) rather than claiming a guarantee this kernel
 * cannot make. */

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
