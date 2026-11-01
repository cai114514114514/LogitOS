#ifndef LOGIT_COALESCED_INPUTS_H
#define LOGIT_COALESCED_INPUTS_H

#include "observe_platform.h"

/* Coalesced inputs: the record of which raw input events the window manager
 * merged into one delivered event.
 *
 * When inputs arrive faster than the display can show them (a flood of mouse
 * moves), the input queues merge them.  A merged event stands for several raw
 * inputs, and each of those inputs still needs an outcome.  So the merge is
 * recorded as a "group": an immutable node that is either a leaf (one raw
 * input), a join of two groups in order, or a re-route of one group to another
 * window or layer.  Groups form a graph that is only ever added to, so an old
 * group seen by one consumer never changes under it.  See coalesced_inputs.c.
 * Design reference: sections 7.4.8 and 10.4. */

/* Most nodes a walk over a group's members visits while it holds the group
 * lock (with interrupts off) before it lets go and takes the lock again.  This
 * bounds how long copy_members and confirms_completion can delay an interrupt,
 * however long the group's chain of joins is. */
#define GROUP_WALK_STEPS_PER_LOCK_HOLD 256u

/* Create the leaf group for one raw input.  The leaf pins the input's ledger
 * record.  On success *out holds a handle the caller owns and must release.
 * Returns 0, or a negative error (no active run, no room, unknown input). */
int coalesced_group_create_leaf(uint64_t input_id, struct coalesced_group_handle *out);

/* Join two groups, left before right.  info must say exactly what the join
 * is: member_count is the sum of the two, and the representative is the right
 * group's.  Returns 0 or a negative error. */
int coalesced_group_join(struct coalesced_group_handle left, struct coalesced_group_handle right,
                         const struct coalesced_group_info *info,
                         struct coalesced_group_handle *out);

/* Re-route a group: make a new single-parent node with the same members but
 * different routing information, rather than editing the old node (which other
 * consumers may be reading).  Returns 0 or a negative error. */
int coalesced_group_route(struct coalesced_group_handle source,
                          const struct coalesced_group_info *info,
                          struct coalesced_group_handle *out);

/* Take another reference to a group (a new consumer), and give one back.
 * acquire returns 1 on success, 0 if the handle is stale or invalid.  The last
 * release does not free anything in the caller's context; destruction is queued
 * and done in bounded steps, so a device interrupt never walks a long chain. */
int coalesced_group_acquire(struct coalesced_group_handle group);
void coalesced_group_release(struct coalesced_group_handle group);

/* Copy a group's description.  Returns 0, or a negative error for a stale
 * handle. */
int coalesced_group_describe(struct coalesced_group_handle group, struct coalesced_group_info *out);

/* Copy up to 'count' member input ids, starting at member number 'first_member',
 * in the order the inputs arrived.  Returns how many were copied, or a negative
 * error.  Members need not be consecutive input ids.  The caller must hold a
 * reference to the group for the length of the call.  Process context: a long
 * group is walked in stretches of GROUP_WALK_STEPS_PER_LOCK_HOLD nodes with the
 * lock released in between. */
int coalesced_group_copy_members(struct coalesced_group_handle group, uint32_t first_member,
                                 uint64_t *input_ids, uint32_t count);

/* Does the evidence name a retained group that contains this input and whose
 * route and merge information match?  The group must still be held by a
 * consumer of its own, not only kept alive by the archive and by the groups
 * built from it.  Returns 1 if so.  The caller (the window manager's receipt
 * code) has already checked the window state and surface.  Walks the group in
 * bounded stretches, like copy_members. */
int coalesced_group_confirms_completion(uint64_t input_id,
                                        const struct input_completion_evidence *evidence);

/* Run life cycle, driven by observe_control.c.  allocate reserves the node pool
 * and the release queue; free_pool releases them once consumers, the archive
 * and the release queue are all empty. */
int coalesced_groups_allocate_pool(uint64_t capacity);
void coalesced_groups_free_pool(void);

/* Counters. */
uint64_t coalesced_groups_published_count(void);
uint64_t coalesced_groups_consumer_count(void);
uint64_t coalesced_groups_live_count(void);

/* Destroy queued nodes, a bounded number per call; called from the periodic
 * timer tick. */
void coalesced_groups_release_queued(void);

/* Drop the experiment's own archive reference on a bounded number of nodes per
 * call.  Done only after the exports are confirmed, because the archive is the
 * offline evidence of which input each shown frame accounted for. */
void coalesced_groups_drop_archive_references(void);

/* Give the whole archive back and destroy every node that becomes unreferenced,
 * in bounded batches with no lock held between them.  Process context only.
 * Returns how many nodes are still alive: zero unless a consumer still holds a
 * group. */
uint64_t coalesced_groups_drain_archive(void);

/* Copy archive row 'sequence' (1-based) for the GROUPS stream.  Returns 1 if the
 * row exists and is still referenced, else 0. */
int coalesced_groups_read_archive_row(uint64_t sequence, struct sched_observe_group_record *out);

/* Development-machine harnesses that include scheduler code without the
 * observer get a no-op release tick, so they neither link nor measure it. */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define coalesced_groups_release_queued() ((void)0)
#endif

#endif
