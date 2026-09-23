#include "proc.h"
#include "shm.h"

_Static_assert(NSHM <= 8 * sizeof(unsigned), "SHM handle mask must cover every slot");

/* A global segment slot is easy to enumerate (there are only eight). A raw
 * slot used as a syscall handle let another process map mode-0600 memory and
 * decrement the owner's reference with SHM_CLOSE without ever opening it.
 * Keep the table beside fd ownership, under the same per-process lock. The
 * transient reference in acquire lets CLOSE on a sibling run while MAP builds
 * its VMA, without freeing or recycling the segment in that window. */

void proc_shm_reset(struct proc *p)
{
    for (int i = 0; i < NSHM; i++) {
        p->shm[i].segment = -1;
        p->shm[i].writable = 0;
    }
    p->shm_mask = 0;
}

int proc_shm_install(struct proc *p, int segment, int writable)
{
    if (!p || segment < 0) return -1;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    int handle = -1;
    if (!__atomic_load_n(&p->teardown, __ATOMIC_ACQUIRE))
        for (int i = 0; i < NSHM; i++)
            if (p->shm[i].segment < 0) {
                p->shm[i].segment = segment;
                p->shm[i].writable = !!writable;
                __atomic_fetch_or(&p->shm_mask, 1u << i, __ATOMIC_RELEASE);
                handle = i;
                break;
            }
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return handle;
}

int proc_shm_acquire(struct proc *p, int handle, int write, int *writable)
{
    if (!p || handle < 0 || handle >= NSHM) return -1;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct proc_shm_handle *h = &p->shm[handle];
    int segment = -1;
#ifdef SHMFD_NEGCTL_GLOBAL
    /* Control: the old syscall interpreted a number as the global slot. */
    (void)write;
    segment = handle;
    if (shm_ref(segment) < 0) segment = -1;
    if (segment >= 0 && writable) *writable = 1;
#else
    if (h->segment >= 0 && (!write || h->writable)) {
        segment = h->segment;
        if (shm_ref(segment) < 0) segment = -1;
        if (segment >= 0 && writable) *writable = h->writable;
    }
#endif
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return segment;
}

int proc_shm_close(struct proc *p, int handle)
{
    if (!p || handle < 0 || handle >= NSHM) return -1;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    int segment = p->shm[handle].segment;
#ifdef SHMFD_NEGCTL_GLOBAL
    segment = handle;
#else
    p->shm[handle].segment = -1;
    p->shm[handle].writable = 0;
    __atomic_fetch_and(&p->shm_mask, ~(1u << handle), __ATOMIC_RELEASE);
#endif
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (segment < 0) return -1;
    shm_put(segment);
    return 0;
}

int proc_shm_clone(struct proc *dst, struct proc *src)
{
    if (!dst || !src) return -1;
    /* dst is BUILDING and private. Every source entry owns a reference, so
     * shm_ref cannot normally fail; the caller aborts a partial clone if the
     * invariant ever breaks. */
    uint64_t fl = spin_lock_irqsave(&src->fd_lock);
    for (int i = 0; i < NSHM; i++) {
        int segment = src->shm[i].segment;
        if (segment < 0) continue;
        if (shm_ref(segment) < 0) {
            spin_unlock_irqrestore(&src->fd_lock, fl);
            return -1;
        }
        dst->shm[i] = src->shm[i];
        dst->shm_mask |= 1u << i;
    }
    spin_unlock_irqrestore(&src->fd_lock, fl);
    return 0;
}

void proc_shm_close_all(struct proc *p)
{
    if (!p) return;
    /* The caller owns an unpublished proc or is its elected final thread;
     * teardown has stopped new installs. Most processes never open SHM, so
     * avoid sixteen empty lock acquisitions on every exit (zero instead).
     * With handles, detach all under one lock, then put references outside it. */
    if (!__atomic_load_n(&p->shm_mask, __ATOMIC_ACQUIRE)) return;
    int drop[NSHM], n = 0;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    for (int i = 0; i < NSHM; i++) {
        if (p->shm[i].segment < 0) continue;
        drop[n++] = p->shm[i].segment;
        p->shm[i].segment = -1;
        p->shm[i].writable = 0;
    }
    __atomic_store_n(&p->shm_mask, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    for (int i = 0; i < n; i++) shm_put(drop[i]);
}
