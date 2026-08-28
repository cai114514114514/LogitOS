/* ===========================================================================
 * THE NETWORK STACK'S MUTUAL EXCLUSION -- step 4a of the BKL removal.
 *
 * WHAT WAS HERE BEFORE, and why it was never a lock:
 *
 *     static inline uint64_t net_lock(void)
 *     { uint64_t f; __asm__ volatile ("pushfq\n\tpop %0\n\tcli"
 *                                     : "=r"(f) :: "memory"); return f; }
 *
 * with the justification "Everything net runs on the BSP, and the NIC IRQ is
 * routed to the BSP, so masking interrupts is enough."
 *
 * The SECOND clause is true by routing (smp.c points the NIC's I/O APIC entry
 * at the BSP). The FIRST is true only because of the big kernel lock: mainline
 * net code -- tcp_send, tcp_recv, tcp_connect, udp_send, arp_resolve -- is
 * reached from syscalls, and a syscall runs on whatever core made it. It is the
 * global lock, not any property of this subsystem, that keeps two of them out of
 * tcp_send at once. `cli` on core 0 says nothing at all about core 1.
 *
 * So this file makes net_lock() a REAL lock. The scope is stated in net.h and
 * repeated in the report; it is deliberately EXACTLY the set net_lock() already
 * covered plus the transmit funnel (see eth_send), not a widening.
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS RECURSIVE, and this is not a convenience -- it is structural.
 *
 * The receive path is ENTERED with the lock held and re-takes it three levels
 * down. Derived from the call graph, not from the prose:
 *
 *   e1000_rx_drain()          takes net_lock  (c/drivers/net/e1000.c:357)
 *     -> cb() == eth_input()
 *          -> arp_input()     takes net_lock  (c/net/link/arp.c:462)
 *          -> ip_input() -> tcp_input() -> tcp_output() -> ip_output()
 *               -> arp_output()   takes net_lock  (arp.c:642)
 *                    -> arp_resolve() takes net_lock  (arp.c:575)
 *
 * and the same shape on the timer side: tcp_poll() takes it (tcp.c:1587) and
 * reaches send_seg -> ip_output -> arp_output. virtio-net, rtl8139 and rtl8169
 * hold it across their drains too. A NON-recursive lock self-deadlocks on the
 * first ARP frame this machine receives.
 *
 * Flattening the nesting into `_locked` variants would be ~90 call sites across
 * six files for no behavioural gain: a recursive acquisition by the SAME core
 * creates no interleaving that `cli` did not already allow, so recursion here
 * preserves today's semantics exactly rather than inventing new ones.
 *
 * ---------------------------------------------------------------------------
 * LOCK ORDER, and why it cannot invert.
 *
 *     net_lock  ->  waitq->lock  ->  g_sched_lock
 *
 * The only locks a net_lock holder ever takes are waitq locks -- tcp.c's
 * backlog_push() (l->wq.lock) and waitq_wake_all(&rx_wq) -- and wait.h already
 * fixes wq->lock -> g_sched_lock. A net_lock holder takes NOTHING else: grepped,
 * there is no spinlock, mutex or waitq anywhere in c/drivers/net, in
 * virtio_net.c or in kprintf, and no net_lock critical section calls kmalloc,
 * the VFS or a block device. In particular it never takes an mm lock, which is
 * the inversion c/kernel/sched/uthread.c warns about (mm sits below everything).
 *
 * Nothing goes the other way: c/kernel/core/wait.c and sched.c contain no call
 * into c/net. The ONE place that took a waitq lock and then net_lock was
 * tcp_wait_readable()'s predicate (`wait_event_timeout(&rx_wq,
 * tcp_available(id) != 0, ...)`, where tcp_available takes net_lock under
 * rx_wq.lock while tcp_input holds net_lock and calls waitq_wake_all(&rx_wq)) --
 * a textbook AB-BA that `cli` made invisible because it never waits. It is fixed
 * in tcp.c in the same change, and it is the reason this file's header says
 * "derive the list from the declarations": nothing in the prose named it.
 *
 * ---------------------------------------------------------------------------
 * WHAT A HOLDER MAY NOT DO, asserted rather than assumed:
 *
 *  - block. Every net_lock section is IF=0 and non-preemptible; a holder that
 *    parked would strand every other core. Nothing does today: the blocking
 *    entry points (tcp_wait_readable/writable, tcp_accept_wait, lsock's
 *    sched_sleep_ms, tcp_send's retry loop, net_idle's sti;hlt) all park with
 *    the lock RELEASED, and after the tcp.c fix above the predicates do too.
 *  - allocate. kmalloc under this lock would put kheap_lock below it and give
 *    the reclaim path a way in. c/net/core/unix.c is the only c/net file that
 *    kmallocs and it does not use net_lock at all.
 *
 * THE ONE LONG HOLD, named because it is the cost: e1000_tx_frame() spins up to
 * 1,000,000 times for a free TX descriptor while holding this. That is not new
 * serialisation -- every one of those paths holds the BKL today -- but after the
 * BKL it is this lock a second core waits on instead, so it is stated here
 * rather than discovered.
 * ========================================================================== */
#include <stdint.h>
#include "net.h"
#include "spinlock.h"
#include "percpu.h"
#include "serial.h"

static spinlock_t    g_net_lk    = SPINLOCK_INIT;
static volatile int  g_net_owner = -1;   /* cpu index holding it, -1 = free */
static int           g_net_depth;        /* recursion depth; owner-only field */

/* The measurement. A lock that cannot be counted is a lock nobody can argue
 * about later -- the whole BKL line exists because g_bkl counts itself. */
static unsigned long st_acq;       /* outermost acquisitions */
static unsigned long st_recur;     /* recursive re-entries (depth > 1) */
static unsigned long st_maxdepth;  /* deepest nesting ever reached */
static unsigned long st_viol;      /* contract violations -- see net_lock_assert_held */

/* Report without taking a lock. This can run INSIDE the very critical section
 * whose discipline it is complaining about, and a printer that took a lock could
 * be the second half of the deadlock it is reporting -- the same argument
 * spinlock.c's bkl_puts() makes, and the same primitive. */
static void nl_puts(const char *m) { while (m && *m) serial_putc(*m++); }

static void nl_bug(const char *what, const char *who)
{
    static volatile int said;
    st_viol++;
    if (said) return;                 /* one line: the first one is the cause */
    said = 1;
    nl_puts("[netlock] BUG: ");
    nl_puts(what);
    if (who) { nl_puts(" at "); nl_puts(who); }
    nl_puts(" -- net_lock() is a real lock now; this was covered by the BKL\r\n");
}

uint64_t net_lock(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(f) :: "memory");
    /* Read the owner only with interrupts already off. The comparison is safe
     * against every concurrent writer: the only core that may store `me` here is
     * this one, and a release by another core can only move the field further
     * away from `me`. */
    int me = this_cpu()->index;
    if (g_net_owner == me) {
        if ((unsigned long)++g_net_depth > st_maxdepth)
            st_maxdepth = (unsigned long)g_net_depth;
        st_recur++;
        return f;
    }
    spin_lock(&g_net_lk);
    g_net_owner = me;
    g_net_depth = 1;
    st_acq++;
    if (st_maxdepth < 1) st_maxdepth = 1;
    return f;
}

void net_unlock(uint64_t f)
{
    /* A ticket lock released by a core that does not hold it advances `serving`
     * past a ticket nobody has, and every later acquirer then spins forever on a
     * lock that reads as free -- spinlock.c documents that failure at length.
     * An unbalanced net_unlock() was harmless when this was `sti`; it is not
     * harmless now, so it is refused rather than performed. */
    if (g_net_owner != this_cpu()->index) {
        nl_bug("net_unlock() by a core that does not hold it", 0);
    } else if (--g_net_depth == 0) {
        g_net_owner = -1;
        spin_unlock(&g_net_lk);
    }
    if (f & 0x200) __asm__ volatile ("sti");
}

int net_lock_held(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(f) :: "memory");
    int held = (g_net_owner == this_cpu()->index);
    if (f & 0x200) __asm__ volatile ("sti");
    return held;
}

/* ENFORCEMENT, not observation. c/net/link/eth.h has said for a long time
 *
 *     CONTRACT: callers hold net_lock() for the duration, which all four NIC
 *     drivers do. eth_input keeps a de-tagging buffer that relies on it.
 *
 * and that sentence is exactly the shape the BKL-removal spec warns about: true,
 * unqualified, and checkable by nobody. This makes it checkable. It prints once
 * and counts thereafter, because the interesting number is "did it ever happen",
 * and a line per frame would be its own denial of service. */
void net_lock_assert_held(const char *who)
{
    if (!net_lock_held()) nl_bug("entered without net_lock", who);
}

void net_lock_stats(unsigned long *acq, unsigned long *recur,
                    unsigned long *maxdepth, unsigned long *viol)
{
    if (acq)      *acq      = st_acq;
    if (recur)    *recur    = st_recur;
    if (maxdepth) *maxdepth = st_maxdepth;
    if (viol)     *viol     = st_viol;
}
