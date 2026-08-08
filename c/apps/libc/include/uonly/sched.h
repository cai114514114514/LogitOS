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
 * LogitOS runs one preemptive priority-less round-robin scheduler
 * (c/kernel/sched/sched.c) with no notion of a process's scheduling POLICY or
 * PRIORITY -- every thread is equal. sched_yield() is therefore real
 * (SYS_YIELD) and sched_getcpu() is real (SYS_CPU_INDEX, the SMP-proof
 * counter CLAUDE.md's M25 note describes); everything about setting a policy
 * or priority is refused rather than silently accepted, because accepting a
 * priority this kernel cannot honour is how a program ends up believing its
 * realtime thread is realtime. */

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
