#ifndef LOGIT_INPUT_LEDGER_H
#define LOGIT_INPUT_LEDGER_H

#include "observe_platform.h"

/* Input ledger: one record for every user input event seen while a run is
 * active, with the deadlines it must meet and the single outcome it ended
 * with.  See input_ledger.c.  Design reference: sections 2.2 and 7.4.8. */

/* Add an input to the ledger when it arrives.  arrival_ns is the device
 * interrupt time (or the poll start; see flight_input_arrival_stamp()).
 * Returns the new input id (never 0), or 0 if the input could not be recorded
 * (no active run, ledger full, or invalid arguments), which also counts as a
 * lost record and invalidates the run. */
uint64_t input_ledger_record_arrival(uint64_t arrival_ns, uint64_t arrival_tsc,
                                     uint64_t frame_period_ns, uint64_t frame_generation,
                                     uint32_t source, uint32_t input_class,
                                     const struct input_payload *payload);

/* Record whether a scheduler token was minted for an input.  Stage S0 has no
 * tokens, so only INPUT_NOT_MINTED is accepted; anything else is refused
 * rather than recorded as a plausible success.  Returns 0, or a negative error. */
int input_ledger_set_allocation(uint64_t input_id, uint32_t allocation);

/* Give an input its final outcome.  Returns 1 if this call settled it, 0 if it
 * had already been settled (the first outcome stands), or a negative error if
 * the outcome is not allowed (missing evidence, wrong input class, or an
 * expiry before the deadline).  The caller never chooses a late outcome: a
 * settle after the outcome deadline always records EXPIRED. */
int input_ledger_settle(uint64_t input_id, uint32_t outcome, uint64_t now_ns,
                        const struct input_completion_evidence *evidence);

/* Expire inputs whose outcome deadline has passed.  Called from the periodic
 * timer tick; checks a bounded batch per call so the tick stays short. */
void input_ledger_expire_overdue(uint64_t now_ns);

/* Run life cycle, driven by observe_control.c: reserve the table before the run
 * is published, and free it after every group and reader is done with it. */
int input_ledger_allocate_pool(uint64_t capacity);
void input_ledger_free_pool(void);

/* Copy one record out under the lock; the live table is never exposed. */
int input_ledger_copy_record(uint64_t input_id, struct input_record *out);

/* Pin an input so its record outlives anything that refers to it, and unpin it.
 * acquire returns 1 on success, 0 if the id is not in the current run. */
int input_ledger_acquire(uint64_t input_id);
void input_ledger_release(uint64_t input_id);

/* Counters and cursors for status snapshots and exports. */
void input_ledger_fill_status(struct sched_observe_status *status);
uint64_t input_ledger_issued_count(void);
uint64_t input_ledger_pending_count(void);
uint64_t input_ledger_first_input_id(void);

/* The 1-based manifest position of an input in this run, or 0 if unknown. */
uint64_t input_ledger_manifest_ordinal(uint64_t input_id);

/* Development-machine harnesses that include scheduler code without the
 * observer get a no-op expiry tick, so they neither link nor measure it. */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define input_ledger_expire_overdue(now_ns) ((void)0)
#endif

#endif
