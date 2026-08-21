#ifndef _SCHED_H
#define _SCHED_H

/* WHY THIS FILE LIVES UNDER include/uonly/ AND NOT include/ DIRECTLY:
 * c/kernel/sched/sched.h is the kernel's OWN scheduler header, and 18 kernel
 * files reach it via a bare `#include "sched.h"`. INCDIRS (Makefile) is a
 * single sorted list of every directory under c/ and include/, shared by the
 * kernel and userland compiles alike -- so if this file sat directly in
 * c/apps/libc/include, it would sort ahead of c/kernel/sched alphabetically
 * and silently shadow the kernel's sched.h for every one of those 18 files
 * (undeclared `schedule`/`thread_create`/etc., a broken kernel build from a
 * userland header nobody meant to touch it). c/apps/libc/include/sys/wait.h
 * hit the identical failure mode once already (see the Makefile's INCDIRS
 * comment) and was fixed the same way: excluded from the flat scan and added
 * back ONLY to UCFLAGS (grep the Makefile for "uonly"). POSIX still requires
 * this header be reachable as bare `#include <sched.h>` from userland, which
 * -Ic/apps/libc/include/uonly (userland-only) provides without ever putting
 * it on the kernel's search path.
 *
 * LogitOS runs ONE scheduling POLICY (SCHED_OTHER) and, since 2026-08-20, a
 * WEIGHTED share within it: c/kernel/sched/sched.c charges each thread real
 * CPU time divided by a weight derived from its nice value and always
 * dispatches the thread furthest behind. So "every thread is equal", which
 * this paragraph said until that day, is no longer true -- but the correction
 * is narrower than it looks and the refusals below all stand:
 *
 *   - NICE is real and belongs to <sys/resource.h> (getpriority/setpriority)
 *     and <unistd.h> (nice). That is where POSIX puts it and where it is.
 *   - SCHED_FIFO / SCHED_RR / sched_param::sched_priority are still refused,
 *     and nice does not make them any more available: a nice-weighted share is
 *     not a realtime priority, it cannot preempt on release, and there is no
 *     bound on when a SCHED_FIFO thread would run. Accepting a priority this
 *     kernel cannot honour is how a program ends up believing its realtime
 *     thread is realtime, which is exactly what this file exists to prevent.
 *
 * sched_yield() is real (SYS_YIELD) and sched_getcpu() is real (SYS_CPU_INDEX,
 * the SMP-proof counter CLAUDE.md's M25 note describes). */

struct sched_param { int sched_priority; };

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

int sched_yield(void);
int sched_getcpu(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(int pid, struct sched_param *param);
int sched_setparam(int pid, const struct sched_param *param);
int sched_getscheduler(int pid);
int sched_setscheduler(int pid, int policy, const struct sched_param *param);

#endif /* _SCHED_H */
