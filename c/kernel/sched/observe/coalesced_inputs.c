/* Coalesced inputs: an immutable, ordered graph of which raw input events were
 * merged into each delivered event.
 *
 * Why a graph and not a set or a range.  Merged inputs need not be consecutive
 * (inputs 1, 3 and 8 may be merged while 2 and 4 to 7 went elsewhere), so a
 * "first to last" range would claim inputs that were never merged, and an
 * unordered set loses the order.  Every node is therefore published once and
 * never changed: a leaf holds one input id, a join holds its two children in
 * order, and a re-route holds one child.  Members are found by walking the
 * graph, never by guessing.
 *
 * Who owns what.  Every node has a reference count.  The caller that created or
 * acquired a node holds one reference; each parent node holds one on each of
 * its children; and the experiment archive holds one on every published node
 * until the INPUT and GROUPS streams have been exported, because the archived
 * graph is the offline evidence of which frame accounted for which input.
 * Releasing a node never recurses: when the count reaches zero the node is put
 * on a queue and destroyed later in bounded batches, so a device interrupt
 * never walks a long chain.  Nodes are never reused within a run, trading
 * capacity for a history that can be reproduced after STOP.
 *
 * No de-duplication happens in interrupt context.  The routing code must hand
 * each leaf over exactly once, and the offline checker verifies that.
 *
 * Design reference: sections 7.4.8 (appendix F10) and 10.4 of
 * docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md. */
#include "coalesced_inputs.h"
#include "input_ledger.h"
#include "observe_control.h"

/* A freshly published node starts with two references: one for the caller that
 * created it and one for the experiment archive. */
#define GROUP_INITIAL_REFERENCES 2u

/* The archive reference plus at least one live consumer.  A completion claim
 * needs the group to be in use by the commit being accounted, not merely
 * archived. */
#define GROUP_REFERENCES_ARCHIVE_ONLY 1u

/* A node has at most two children (a join has two, a re-route one, a leaf none). */
#define GROUP_MAX_CHILDREN 2u

/* Nodes destroyed or archive references dropped per call, which bounds the work
 * done in one timer tick or shutdown step. */
#define COALESCED_RELEASE_BATCH 32u

struct group_node {
    uint64_t reference_count;
    struct sched_observe_group_record record;
    uint32_t queued_for_release;
};

static struct observe_spinlock g_group_lock;
static struct group_node *g_nodes;
static uint32_t *g_release_queue;
static uint64_t g_capacity;
static uint64_t g_published_count;
static uint64_t g_live_count;
static uint64_t g_consumer_count;
static uint64_t g_queue_head;
static uint64_t g_queue_tail;
static uint64_t g_archive_cursor;
static uint32_t g_generation;

/* ---- hooks flipped by the negative-control tests ------------------------
 * Each hook behaves as production code unless the named macro is defined, and
 * the macro is defined only when the test named in its comment builds the
 * fault-injected variant. */

/* A full pool must be counted as a lost record, which invalidates the run.
 * Flipped by SCHED_NEGCTL_GROUP_POOL_DROP, which hides it (host test:
 * group_exhaustion_invalid). */
static void count_pool_exhaustion(void)
{
#ifndef SCHED_NEGCTL_GROUP_POOL_DROP
    observe_count_lost_record();
#endif
}

/* A published group must never change.  Flipped by SCHED_NEGCTL_GROUP_MUTATE,
 * which rewrites the left child after the join (host test: group_immutable). */
static void negctl_mutate_left_child(struct group_node *left_node,
                                     const struct coalesced_group_info *info)
{
#ifdef SCHED_NEGCTL_GROUP_MUTATE
    left_node->record.info.representative_input_id = info->representative_input_id;
#else
    (void)left_node;
    (void)info;
#endif
}

/* Releasing a reference must not free a node that others still hold.  Flipped by
 * SCHED_NEGCTL_GROUP_EARLY_FREE, which zeroes the count (host test:
 * group_live_ref). */
static void negctl_zero_references(struct group_node *node)
{
#ifdef SCHED_NEGCTL_GROUP_EARLY_FREE
    __atomic_store_n(&node->reference_count, 0, __ATOMIC_SEQ_CST);
#else
    (void)node;
#endif
}

/* Destroying a node must release its children.  Flipped by
 * SCHED_NEGCTL_GROUP_LEAK_CHILD, which keeps them alive (host test:
 * group_refs_drained). */
static int negctl_keep_children_alive(void)
{
#ifdef SCHED_NEGCTL_GROUP_LEAK_CHILD
    return 1;
#else
    return 0;
#endif
}

/* ---- handles and nodes -------------------------------------------------- */

/* A group id packs generation and slot; the id 0 is the only "no group". */
static uint64_t group_id_from_handle(struct coalesced_group_handle group)
{
    return ((uint64_t)group.generation << SCHED_OBSERVE_GROUP_ID_GENERATION_SHIFT) | group.slot;
}

/* Inverse of group_id_from_handle(). */
static struct coalesced_group_handle handle_from_group_id(uint64_t group_id)
{
    struct coalesced_group_handle group = {
        .slot = (uint32_t)group_id,
        .generation = (uint32_t)(group_id >> SCHED_OBSERVE_GROUP_ID_GENERATION_SHIFT),
    };
    return group;
}

/* Find a live node by handle.  Called with the group lock held.  A handle from
 * an earlier run (different generation) or past the published nodes is
 * rejected, as is a node whose references are all gone. */
static struct group_node *find_live_node(struct coalesced_group_handle group)
{
    if (!g_nodes || group.generation != g_generation) {
        return NULL;
    }
    if (!group.slot || group.slot > g_published_count) {
        return NULL;
    }
    struct group_node *node = &g_nodes[group.slot - 1];
    if (!__atomic_load_n(&node->reference_count, __ATOMIC_SEQ_CST)) {
        return NULL;
    }
    return node;
}

/* Add a reference, but only to a node that still has one.  A compare-and-swap
 * on a non-zero count means a node that reached zero is never brought back to
 * life.  The caller must already own a reference, or be protected by the queue
 * or receipt lock that carries one. */
static int acquire_node(struct group_node *node)
{
    uint64_t references = __atomic_load_n(&node->reference_count, __ATOMIC_SEQ_CST);
    while (references && references < UINT64_MAX) {
        if (__atomic_compare_exchange_n(&node->reference_count, &references, references + 1, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return 1;
        }
    }
    return 0;
}

static void release_node(struct group_node *node);

/* The last release of a node must only queue it for bounded destruction.
 * Flipped by SCHED_NEGCTL_GROUP_IRQ_RECURSE, which instead destroys the node
 * and, recursively, its children on the spot, as an unbounded walk inside an
 * interrupt (host test: group_irq_bounded).  Returns 1 if it destroyed the
 * node, so the caller must not queue it as well. */
static int negctl_destroy_in_caller(struct group_node *node)
{
#ifdef SCHED_NEGCTL_GROUP_IRQ_RECURSE
    OBSERVE_TEST_POINT(OBSERVE_POINT_GROUP_NODE_RELEASED);
    uint64_t child_ids[GROUP_MAX_CHILDREN] = {node->record.left_child_id,
                                              node->record.right_child_id};
    for (unsigned child = 0; child < GROUP_MAX_CHILDREN; child++) {
        if (child_ids[child]) {
            struct group_node *child_node = find_live_node(handle_from_group_id(child_ids[child]));
            if (child_node) {
                release_node(child_node);
            }
        }
    }
    g_live_count--;
    return 1;
#else
    (void)node;
    return 0;
#endif
}

/* Drop a reference.  If it was the last, put the node on the release queue
 * instead of destroying it here, so the caller (possibly a device interrupt)
 * never recurses down a long chain of parents.  Each node is queued once, so a
 * queue as large as the pool never overflows.  Called with the group lock held. */
static void release_node(struct group_node *node)
{
    uint64_t references_before = __atomic_fetch_sub(&node->reference_count, 1, __ATOMIC_SEQ_CST);
    if (references_before != 1) {
        return;
    }
    if (negctl_destroy_in_caller(node)) {
        return;
    }
    node->queued_for_release = 1;
    g_release_queue[g_queue_tail++ % g_capacity] = (uint32_t)(node - g_nodes);
}

/* ---- pool life cycle ---------------------------------------------------- */

/* Reserve the node pool and the release queue together before the run is
 * published.  The periodic timer drains the queue even between runs, so the
 * queue and counters change together with the pool under one lock hold; a
 * smaller new pool must not see the previous run's cursors. */
int coalesced_groups_allocate_pool(uint64_t capacity)
{
    if (g_generation == UINT32_MAX) {
        return SCHED_OBSERVE_EOVERFLOW;
    }
    struct group_node *nodes = observe_allocate_zeroed(capacity, sizeof *nodes);
    uint32_t *release_queue = observe_allocate_zeroed(capacity, sizeof *release_queue);
    if (!nodes || !release_queue) {
        observe_free(nodes);
        observe_free(release_queue);
        return SCHED_OBSERVE_ENOMEM;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    g_nodes = nodes;
    g_release_queue = release_queue;
    g_generation++;
    g_capacity = capacity;
    g_published_count = 0;
    g_live_count = 0;
    g_consumer_count = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
    g_archive_cursor = 0;
    observe_lock_release(&g_group_lock, saved_flags);
    return 0;
}

/* Free the pool.  Only the run controller calls this, after consumers, the
 * archive and the release queue are all empty.  A caller whose publish failed
 * because the pool was full keeps its own references; nothing is released on
 * its behalf. */
void coalesced_groups_free_pool(void)
{
    observe_free(g_nodes);
    observe_free(g_release_queue);
    g_nodes = NULL;
    g_release_queue = NULL;
    g_capacity = 0;
}

/* ---- publishing --------------------------------------------------------- */

/* Publish a new node, taking a reference on each child.  The node starts with a
 * caller reference and an archive reference.  member_count in the info is the
 * exact size of the group, not a guess from an input id interval.  Called with
 * the group lock held. */
static int publish_node(uint64_t input_id, struct group_node *left, struct group_node *right,
                   const struct coalesced_group_info *info, struct coalesced_group_handle *out)
{
    if (g_published_count == g_capacity) {
        count_pool_exhaustion();
        return SCHED_OBSERVE_ENOSPC;
    }
    struct group_node *node = &g_nodes[g_published_count];
    struct coalesced_group_handle handle = {
        .slot = (uint32_t)g_published_count + 1,
        .generation = g_generation,
    };
    if (left && !acquire_node(left)) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (right && !acquire_node(right)) {
        if (left) {
            release_node(left);
        }
        return SCHED_OBSERVE_EINVAL;
    }
    node->record = (struct sched_observe_group_record){
        .sequence = g_published_count + 1,
        .group_id = group_id_from_handle(handle),
        .left_child_id = left ? left->record.group_id : 0,
        .right_child_id = right ? right->record.group_id : 0,
        .input_id = input_id,
        .info = *info,
    };
    __atomic_store_n(&node->reference_count, GROUP_INITIAL_REFERENCES, __ATOMIC_SEQ_CST);
    g_published_count++;
    g_live_count++;
    g_consumer_count++;
    *out = handle;
    return 0;
}

/* Create the leaf for one raw input.  The leaf pins the input's ledger record,
 * and this pin is separate from the ledger's own reference, so the input stays
 * in the account even after the queue slot that carried it was evicted.
 *
 * The consumer count is raised before the ledger lock is taken, and kept until
 * the work is finished.  Otherwise STOP could see zero consumers in the gap
 * between pinning the input and publishing the node, and free the pool under
 * this call. */
int coalesced_group_create_leaf(uint64_t input_id, struct coalesced_group_handle *out)
{
    if (!out) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    if (!g_nodes || !observe_admitting_inputs()) {
        observe_lock_release(&g_group_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    g_consumer_count++;
    observe_lock_release(&g_group_lock, saved_flags);
    int input_pinned = input_ledger_acquire(input_id);
    saved_flags = observe_lock_acquire(&g_group_lock);
    int result = SCHED_OBSERVE_EINVAL;
    if (input_pinned && g_nodes && observe_admitting_inputs()) {
        const struct coalesced_group_info info = {
            .representative_input_id = input_id,
            .member_count = 1,
        };
        result = publish_node(input_id, NULL, NULL, &info, out);
    }
    if (!result) {
        /* The node was published and carries its own consumer count; drop the
         * construction pin taken above. */
        g_consumer_count--;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    if (result && input_pinned) {
        input_ledger_release(input_id);
    }
    if (result) {
        /* The construction pin is kept until the input pin has been given back,
         * so STOP cannot reclaim the ledger inside this lock-order gap. */
        saved_flags = observe_lock_acquire(&g_group_lock);
        g_consumer_count--;
        observe_lock_release(&g_group_lock, saved_flags);
    }
    return result;
}

/* Join two groups in order.  The new node keeps both children immutable.  A
 * duplicate member (the same input reaching the join twice) is a routing bug
 * that an independent offline check catches; detecting it here would need an
 * unbounded walk in interrupt context. */
int coalesced_group_join(struct coalesced_group_handle left, struct coalesced_group_handle right,
                         const struct coalesced_group_info *info,
                         struct coalesced_group_handle *out)
{
    if (!info || !out) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (!info->layer && info->reducer_id) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *left_node = find_live_node(left);
    struct group_node *right_node = find_live_node(right);
    int result = SCHED_OBSERVE_EINVAL;
    if (left_node && right_node) {
        uint32_t left_members = left_node->record.info.member_count;
        uint32_t right_members = right_node->record.info.member_count;
        int count_fits = left_members <= UINT32_MAX - right_members;
        int count_matches = count_fits && info->member_count == left_members + right_members;
        int representative_matches = info->representative_input_id ==
                                     right_node->record.info.representative_input_id;
        if (count_matches && representative_matches) {
            result = publish_node(0, left_node, right_node, info, out);
        }
    }
    if (!result) {
        negctl_mutate_left_child(left_node, info);
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return result;
}

/* Re-route a group.  Changing the old node in place would rewrite a group some
 * consumer already popped and make its completion evidence unverifiable, so a
 * new single-parent node is published instead. */
int coalesced_group_route(struct coalesced_group_handle source,
                          const struct coalesced_group_info *info,
                          struct coalesced_group_handle *out)
{
    if (!info || !out) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (!info->layer && info->reducer_id) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(source);
    int result = SCHED_OBSERVE_EINVAL;
    if (node) {
        /* A re-route changes where the group goes, never who is in it. */
        int same_members = info->member_count == node->record.info.member_count;
        int same_representative = info->representative_input_id ==
                                  node->record.info.representative_input_id;
        if (same_members && same_representative) {
            result = publish_node(0, node, NULL, info, out);
        }
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return result;
}

/* ---- consumers ---------------------------------------------------------- */

/* A new consumer takes a reference; consumers are counted separately from the
 * graph and archive references so STOP can wait for them to finish. */
int coalesced_group_acquire(struct coalesced_group_handle group)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    int valid = node && acquire_node(node);
    if (valid) {
        g_consumer_count++;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return valid;
}

/* A consumer is done.  This is constant work even for a long chain, because
 * destruction is queued.  Releasing a group nobody holds means a caller's
 * bookkeeping is wrong, which invalidates the run. */
void coalesced_group_release(struct coalesced_group_handle group)
{
    if (!group.slot && !group.generation) {
        return;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    if (node && g_consumer_count) {
        g_consumer_count--;
        release_node(node);
        negctl_zero_references(node);
    } else {
        observe_mark_invalid();
    }
    observe_lock_release(&g_group_lock, saved_flags);
}

/* Copy a node's description under the lock.  Never hand out a pointer into the
 * pool. */
int coalesced_group_describe(struct coalesced_group_handle group, struct coalesced_group_info *out)
{
    if (!out) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    if (node) {
        *out = node->record.info;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return node ? 0 : SCHED_OBSERVE_EINVAL;
}

/* The input id of member number 'index' (0-based, in arrival order) of a group,
 * or 0 if there is none.  Walks the graph iteratively, using each child's
 * member count to skip whole subtrees; the walk is bounded by the number of
 * published nodes in case the graph were ever damaged.  Export and test callers
 * may use it; the interrupt-time enqueue and release paths never do. */
static uint64_t member_input_id_at(struct group_node *node, uint32_t index)
{
    uint64_t steps_left = g_published_count;
    while (node && steps_left--) {
        if (node->record.input_id) {
            return index == 0 ? node->record.input_id : 0;
        }
        struct coalesced_group_handle left_handle = handle_from_group_id(node->record.left_child_id);
        struct group_node *left_child = find_live_node(left_handle);
        if (!left_child) {
            return 0;
        }
        if (index < left_child->record.info.member_count) {
            node = left_child;
        } else {
            index -= left_child->record.info.member_count;
            node = find_live_node(handle_from_group_id(node->record.right_child_id));
        }
    }
    return 0;
}

/* Member number 'first_member + offset' of a node.  Flipped by
 * SCHED_NEGCTL_GROUP_RANGE, which assumes the members are consecutive input ids
 * (host test: group_exact_leaves). */
static uint64_t member_for_copy(struct group_node *node, uint32_t first_member, uint32_t offset)
{
#ifdef SCHED_NEGCTL_GROUP_RANGE
    return member_input_id_at(node, 0) + first_member + offset;
#else
    return member_input_id_at(node, first_member + offset);
#endif
}

/* The exact members of a group, in order.  They survive the group being popped
 * from a queue and later joined into bigger groups; no interval is guessed. */
int coalesced_group_copy_members(struct coalesced_group_handle group, uint32_t first_member,
                                 uint64_t *input_ids, uint32_t count)
{
    if (!input_ids && count) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    if (!node || first_member > node->record.info.member_count) {
        observe_lock_release(&g_group_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    if (count > node->record.info.member_count - first_member) {
        count = node->record.info.member_count - first_member;
    }
    for (uint32_t offset = 0; offset < count; offset++) {
        input_ids[offset] = member_for_copy(node, first_member, offset);
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return count;
}

/* Completion needs a group that (a) is still held by a consumer beyond the
 * archive, (b) contains this input, and (c) carries exactly the route and merge
 * information the evidence claims.  The receipt code checks the window state and
 * surface before calling the ledger, which calls this. */
int coalesced_group_confirms_completion(uint64_t input_id,
                                        const struct input_completion_evidence *evidence)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(handle_from_group_id(evidence->group_id));
    int valid = 0;
    if (node) {
        const struct coalesced_group_info *info = &node->record.info;
        uint64_t references = __atomic_load_n(&node->reference_count, __ATOMIC_SEQ_CST);
        int held_by_consumer = references > GROUP_REFERENCES_ARCHIVE_ONLY;
        int metadata_matches = info->route_generation == evidence->route_generation &&
                               info->reducer_id == evidence->reducer_id &&
                               info->reducer_version == evidence->reducer_version &&
                               info->layer == evidence->layer;
        if (held_by_consumer && metadata_matches) {
            for (uint32_t member = 0; member < info->member_count; member++) {
                if (member_input_id_at(node, member) == input_id) {
                    valid = 1;
                    break;
                }
            }
        }
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return valid;
}

/* ---- deferred destruction and archive ----------------------------------- */

/* Destroy queued nodes bottom-up, at most one batch per call.  The ledger pin
 * a leaf holds is given back outside the group lock, so the group lock and the
 * ledger lock are never nested the wrong way round.  The live count drops only
 * after that final pin was returned: otherwise STOP could free and reopen the
 * ledger while this worker still owed a release to the same input id of the
 * previous run. */
void coalesced_groups_release_queued(void)
{
    for (unsigned destroyed = 0; destroyed < COALESCED_RELEASE_BATCH; destroyed++) {
        uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
        if (g_queue_head == g_queue_tail) {
            observe_lock_release(&g_group_lock, saved_flags);
            return;
        }
        struct group_node *node = &g_nodes[g_release_queue[g_queue_head++ % g_capacity]];
        OBSERVE_TEST_POINT(OBSERVE_POINT_GROUP_NODE_RELEASED);
        uint64_t input_id = node->record.input_id;
        uint64_t child_ids[GROUP_MAX_CHILDREN] = {node->record.left_child_id,
                                                  node->record.right_child_id};
        for (unsigned child = 0; child < GROUP_MAX_CHILDREN; child++) {
            if (!child_ids[child] || negctl_keep_children_alive()) {
                continue;
            }
            struct group_node *child_node = find_live_node(handle_from_group_id(child_ids[child]));
            if (child_node) {
                release_node(child_node);
            }
        }
        observe_lock_release(&g_group_lock, saved_flags);
        if (input_id) {
            input_ledger_release(input_id);
        }
        saved_flags = observe_lock_acquire(&g_group_lock);
        g_live_count--;
        observe_lock_release(&g_group_lock, saved_flags);
    }
}

/* Give back the archive's reference on a batch of nodes.  Called only after the
 * INPUT and GROUPS exports are confirmed.  Freeing groups earlier would let
 * their slots be reused and make the exported completion evidence point at
 * history that no longer exists. */
void coalesced_groups_drop_archive_references(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    for (unsigned dropped = 0; dropped < COALESCED_RELEASE_BATCH; dropped++) {
        if (g_archive_cursor >= g_published_count) {
            break;
        }
        release_node(&g_nodes[g_archive_cursor++]);
    }
    observe_lock_release(&g_group_lock, saved_flags);
}

/* Export one archive row as a private copy.  Reading never consumes a
 * reference. */
int coalesced_groups_read_archive_row(uint64_t sequence, struct sched_observe_group_record *out)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    int valid = 0;
    if (g_nodes && sequence && sequence <= g_published_count) {
        uint64_t references = __atomic_load_n(&g_nodes[sequence - 1].reference_count,
                                              __ATOMIC_SEQ_CST);
        valid = references != 0;
    }
    if (valid) {
        *out = g_nodes[sequence - 1].record;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return valid;
}

/* The last GROUPS cursor: every published node, not just the ones still live. */
uint64_t coalesced_groups_published_count(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    uint64_t count = g_published_count;
    observe_lock_release(&g_group_lock, saved_flags);
    return count;
}

/* Consumers still using groups.  STOP waits for zero here first; child and
 * archive references stay visible in the live count. */
uint64_t coalesced_groups_consumer_count(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    uint64_t count = g_consumer_count;
    observe_lock_release(&g_group_lock, saved_flags);
    return count;
}

/* Nodes not yet destroyed.  Zero proves the archive and every child reference
 * were released. */
uint64_t coalesced_groups_live_count(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    uint64_t count = g_live_count;
    observe_lock_release(&g_group_lock, saved_flags);
    return count;
}
