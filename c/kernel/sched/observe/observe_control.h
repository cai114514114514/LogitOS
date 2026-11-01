#ifndef LOGIT_OBSERVE_CONTROL_H
#define LOGIT_OBSERVE_CONTROL_H

#include "observe_platform.h"

/* Run control for the scheduler observation facility: the system call that
 * starts, stops, reads and reports a run, and the state shared by the flight
 * recorder, the input ledger and the coalesced groups (is a run active, is it
 * still valid, which inputs does the manifest expect).  See observe_control.c
 * and logit_sched_observe.h. */

/* Prepare the facility.  Called once at the end of sched_init().  It starts no
 * run and no benchmark; an ordinary image does nothing until a privileged tool
 * issues BEGIN. */
void observe_control_init(void);

/* The system call itself (SYS_SCHED_OBSERVE).  'command' is an
 * enum sched_observe_command, 'user_request' the user address of the request
 * struct and 'bytes' its size.  Returns a result or a negative error. */
long observe_syscall(long command, long user_request, long bytes);

/* Let an optional module (see enum sched_observe_provider) answer PROBE or
 * MODEL commands and/or add a line of text to the METADATA record.  Must be
 * called once, before the first run; registering during or after a run is
 * refused.  Returns 0 or a negative error. */
int observe_provider_register(unsigned kind, long (*command)(long, long, long),
                              int (*metadata)(char *, int));

/* Bookkeeping the other observer modules report into. */
void observe_mark_invalid(void);        /* necessary evidence was lost or disturbed */
void observe_count_lost_record(void);   /* a record had to be dropped (also invalidates) */
int observe_admitting_inputs(void);     /* 1 while new inputs may still be added */
uint64_t observe_run_id(void);          /* id of the current run, or 0 */

/* Move a stopping run forward as far as it can go without waiting: finish
 * draining and ask the CPUs to seal their rings.  Called from places that
 * already run regularly (the scheduler, every interrupt exit, the timer tick),
 * so a CPU that never gets there simply keeps the run from finishing instead of
 * being forced.  Returns after one load, taking no lock, unless a run is
 * draining or closing.  It never frees anything: the memory is given back by
 * the system call that makes the last export final (READ or STOP). */
void observe_advance_shutdown(void);

/* The scheduler's periodic tick calls this once for all the observer's
 * housekeeping (expire overdue inputs, destroy queued groups, advance a stopping
 * run).  With no run recording it reads no clock and takes no lock. */
void observe_periodic_tick(void);

/* A consistent copy of the counters, for STATUS and the HUD.  Takes the control
 * lock only to copy numbers; formatting happens afterwards. */
void observe_snapshot_status(struct sched_observe_status *out);

/* The preregistered manifest entry for an input's position in this run, or NULL
 * when the run has no manifest.  A position outside the manifest invalidates
 * the run. */
const struct sched_observe_manifest *observe_manifest_entry(uint64_t ordinal);

/* Development-machine harnesses that include scheduler code without the
 * observer get no-op versions, and the system call answers "not implemented". */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define observe_control_init() ((void)0)
#define observe_advance_shutdown() ((void)0)
#define observe_periodic_tick() ((void)0)
#define observe_syscall(command, user_request, bytes) ((long)SCHED_OBSERVE_ENOSYS)
#endif

#endif
