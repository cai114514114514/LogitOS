#ifndef LOGIT_KLOG_H
#define LOGIT_KLOG_H

#include <stdint.h>
#include <stdarg.h>

/* ---------------------------------------------------------------------------
 * klog -- the kernel log ring.
 *
 * Until this existed the entire diagnostic apparatus was kprintf(), which fans
 * characters at VGA and COM1 and keeps nothing. That is a channel you have to
 * be WATCHING at the moment of the failure, on a machine that has a serial
 * port. Neither holds on the hardware the driver lines are aiming at.
 *
 * So every character kprintf emits also lands here, in a fixed ring of records
 * that survives the event that wrote it and can be read back afterwards --
 * from userland (`cat /dev/kmsg`), or dumped by the panic path.
 *
 * WHY THIS IS SAFE IN INTERRUPT AND FAULT CONTEXT
 * -----------------------------------------------
 * kprintf is called from IRQ handlers, from the fault path, and from code
 * holding the BKL. A logging path that can allocate, sleep, or invert a lock
 * order turns a diagnosable failure into a hang. The rules this file keeps:
 *
 *  1. NO ALLOCATION, EVER. The ring, the per-CPU line buffers and the render
 *     snapshot are all .bss. kmalloc is never called, so klog cannot recurse
 *     into the heap (whose lock sits above ours) and cannot fail.
 *
 *  2. NO SLEEPING, NO BLOCKING. Nothing here yields, parks, or waits on a
 *     device. A full ring OVERWRITES its oldest record and counts the loss; it
 *     never applies back-pressure to the writer. That is the defined behaviour,
 *     not an accident -- a logger that blocks when full deadlocks the machine
 *     precisely when it has the most to say.
 *
 *  3. THE RING LOCK IS A LEAF. g_klog_lock is taken by nothing but this file
 *     and this file calls nothing that takes another lock while holding it. A
 *     lock that acquires no other lock cannot be one end of an inversion, so
 *     there is no ordering to get wrong against the BKL, g_sched_lock,
 *     g_file_lock or the heap lock. Callers may hold any of them.
 *
 *  4. IT IS TAKEN WITH IF=0 (spin_lock_irqsave). A core inside the critical
 *     section cannot be interrupted into code that re-takes the lock, so the
 *     same-core self-deadlock does not exist. The section is a <=112-byte copy
 *     with no I/O in it -- microseconds, not the milliseconds a UART would add.
 *
 *  5. THE SLOW PART HOLDS NOTHING. Line assembly happens in a PER-CPU buffer,
 *     and the console write (serial_putc busy-waits on the UART: ~87us per
 *     character on real hardware) happens with no lock held at all -- exactly
 *     as it did before. Only the finished line's memcpy into the ring is
 *     serialised. Console byte order is therefore unchanged.
 *
 *  6. PANIC BREAKS THE LOCK. If the machine died inside the critical section,
 *     waiting for that lock is waiting forever. klog_panic_takeover() forcibly
 *     re-initialises it (Linux's bust_spinlocks), because a panic report that
 *     hangs is worth less than one that risks a garbled line.
 * ------------------------------------------------------------------------ */

/* Severity. Lower is more severe; the numbering is deliberately the same shape
 * as syslog's so it reads the same way to anyone who has seen dmesg. */
#define KL_PANIC 0
#define KL_ERR   1
#define KL_WARN  2
#define KL_INFO  3      /* what plain kprintf() records at */
#define KL_DEBUG 4      /* ring only by default: a driver can trace without
                         * drowning the boot output every harness greps */
#define KL_NLEVELS 5

#define KLOG_SLOTS 512            /* records retained (power of two) */
#define KLOG_TEXT  112            /* bytes of text per record; longer lines are
                                   * TRUNCATED and the loss counted, never split
                                   * and never dropped whole */

struct klog_rec {
    uint64_t seq;                 /* 1-based; 0 = slot never written / torn */
    uint64_t ms;                  /* timer_ms() at commit */
    uint32_t dropped;             /* characters truncated off this line */
    uint16_t len;                 /* bytes used in text[] */
    uint8_t  level;
    uint8_t  cpu;
    char     text[KLOG_TEXT];
};

struct klog_stats {
    uint64_t records;             /* lines committed since boot */
    uint64_t overwritten;         /* lines aged out of the ring unread */
    uint64_t truncated_lines;     /* lines that lost characters */
    uint64_t truncated_chars;
    uint64_t by_level[KL_NLEVELS];
    uint32_t slots, text_max;
};

/* --- producers ------------------------------------------------------------ */

/* Feed one console character into the ring's line assembler. kprintf calls this
 * for every character it emits, so the ring sees exactly the console stream. */
void klog_putc(int level, char c);

/* Formatted logging at a level. Levels at or below the console threshold also
 * go to VGA+serial; the rest are ring-only. Same conversions as kprintf. */
void klog(int level, const char *fmt, ...);
void klogv(int level, const char *fmt, va_list ap);

#define kerr(...)   klog(KL_ERR,   __VA_ARGS__)
#define kwarn(...)  klog(KL_WARN,  __VA_ARGS__)
#define kinfo(...)  klog(KL_INFO,  __VA_ARGS__)
#define kdebug(...) klog(KL_DEBUG, __VA_ARGS__)

int  klog_console_level(void);
void klog_set_console_level(int level);   /* clamped to [KL_PANIC, KL_DEBUG] */

/* Commit whatever partial (newline-less) line each CPU is holding. The panic
 * path calls this so the last thing printed before the crash is not lost. */
void klog_flush_partial(void);

/* --- consumers ------------------------------------------------------------ */

uint64_t klog_next_seq(void);          /* seq the next record will get */
uint64_t klog_first_seq(void);         /* oldest seq still in the ring */
int      klog_get(uint64_t seq, struct klog_rec *out);   /* 1 = copied, 0 = gone */
void     klog_get_stats(struct klog_stats *out);

/* Render one record as a dmesg line (no trailing NUL guarantee beyond `max`).
 * Returns bytes written. */
int klog_format_rec(const struct klog_rec *r, char *buf, int max);

/* Render the whole ring, oldest first, into `buf`. Returns bytes written. */
int klog_render(char *buf, int max);

/* Write the tail of the ring straight to the console. The panic path uses this:
 * it takes no lock and tolerates a concurrently-wedged writer. Its own output
 * is suppressed from the ring, so it cannot chase its own tail. */
void klog_dump_tail(int lines);
/* Same, but ending at `before_seq` -- the panic path marks the sequence before
 * it starts printing so the dump shows the run-up to the crash and not the
 * crash report it is in the middle of writing. */
void klog_dump_tail_before(uint64_t before_seq, int lines);

/* Panic entry: break the ring lock and switch the ring to lock-free mode. */
void klog_panic_takeover(void);

#endif /* LOGIT_KLOG_H */
