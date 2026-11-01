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
 * acquired a node holds one reference (a "consumer", counted per node as well,
 * so that a group kept alive only by its archive entry and its parents is not
 * mistaken for one that somebody is using); each parent node holds one on each
 * of its children; and the experiment archive holds one on every published node
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
 * created it (its first consumer) and one for the experiment archive. */
#define GROUP_INITIAL_REFERENCES 2u

/* A group's fully expanded tree has at most this many nodes per member: the
 * member leaves plus the joins above them, which number one fewer than the
 * leaves.  Re-route nodes are budgeted separately, one per published node. */
#define GROUP_WALK_NODES_PER_MEMBER 2ull

/* Words in the heavier-sibling stack of a member walk.  A walk goes into the
 * lighter child first and leaves the heavier one waiting, so every waiting entry
 * stands for a subtree at least as big as the one being walked.  The member
 * count is 32 bits and each stacked entry at least halves what remains to walk,
 * so 32 entries are enough for any intact graph; more means a damaged one. */
#define GROUP_WALK_STACK_DEPTH 32u

/* A node has at most two children (a join has two, a re-route one, a leaf none). */
#define GROUP_MAX_CHILDREN 2u

/* Nodes destroyed or archive references dropped per call, which bounds the work
 * done in one timer tick or shutdown step. */
#define COALESCED_RELEASE_BATCH 32u

struct group_node {
    uint64_t reference_count;   /* consumers, parents and the archive together */
    uint64_t consumer_count;    /* only the callers that hold the group for use */
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
static void negative_control_mutate_left_child(struct group_node *left_node,
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
static void negative_control_zero_references(struct group_node *node)
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
static int negative_control_keep_children_alive(void)
{
#ifdef SCHED_NEGCTL_GROUP_LEAK_CHILD
    return 1;
#else
    return 0;
#endif
}

/* A completion claim needs a group that a consumer holds for use, not one kept
 * alive only by the archive and by parent groups.  Flipped by
 * SCHED_NEGCTL_GROUP_COUNT_ANY_REFERENCE, which counts every reference as a
 * consumer, as the first version did (host test: group_consumer_required). */
static int held_by_consumer(const struct group_node *node)
{
#ifdef SCHED_NEGCTL_GROUP_COUNT_ANY_REFERENCE
    return __atomic_load_n(&node->reference_count, __ATOMIC_SEQ_CST) > 1;
#else
    return node->consumer_count > 0;
#endif
}

/* How many nodes a member walk may visit before it gives up the group lock.
 * Flipped by SCHED_NEGCTL_GROUP_WALK_UNBOUNDED, which never gives it up, so a
 * long chain keeps interrupts off for the whole walk (host test:
 * group_walk_bounded). */
static unsigned walk_steps_per_lock_hold(void)
{
#ifdef SCHED_NEGCTL_GROUP_WALK_UNBOUNDED
    return UINT32_MAX;
#else
    return GROUP_WALK_STEPS_PER_LOCK_HOLD;
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
static int negative_control_destroy_in_caller(struct group_node *node)
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
    if (negative_control_destroy_in_caller(node)) {
        return;
    }
    node->queued_for_release = 1;
    g_release_queue[g_queue_tail % g_capacity] = (uint32_t)(node - g_nodes);
    g_queue_tail = g_queue_tail + 1;
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
 * its behalf.  The pointers are taken out under the lock and the memory is
 * freed after it is dropped, so a timer tick on another CPU that is destroying a
 * queued node never meets freed memory, and the free itself runs with the lock
 * released. */
void coalesced_groups_free_pool(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *nodes = g_nodes;
    uint32_t *release_queue = g_release_queue;
    g_nodes = NULL;
    g_release_queue = NULL;
    g_capacity = 0;
    observe_lock_release(&g_group_lock, saved_flags);
    observe_free(nodes);
    observe_free(release_queue);
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
    node->consumer_count = 1;
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
    int pool_open = g_nodes != NULL && observe_admitting_inputs();
    if (input_pinned && pool_open) {
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
        negative_control_mutate_left_child(left_node, info);
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
        node->consumer_count = node->consumer_count + 1;
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
    /* Releasing what this node's consumers never held would take the archive's
     * or a parent's reference, so it is a bookkeeping error, not a release. */
    int holds_consumer = node != NULL && node->consumer_count != 0;
    if (holds_consumer && g_consumer_count) {
        node->consumer_count = node->consumer_count - 1;
        g_consumer_count--;
        release_node(node);
        negative_control_zero_references(node);
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

/* ---- walking the members of a group ------------------------------------ */

/* Where a walk over one group's leaves stands.  The walk visits every leaf once
 * and tells the visitor the leaf's position among the group's members (0 is the
 * first to arrive), so the order of the visits does not matter.  It goes into
 * the lighter child of each join and leaves the heavier waiting on a small
 * fixed stack, which bounds the stack by the bit width of a member count even
 * for a chain of ten thousand joins.  A walk is resumable: it can give up the
 * group lock between two stretches, because published nodes never change and
 * the caller's reference keeps them alive. */
struct member_walk {
    uint32_t generation;                /* the pool the slots below belong to */
    uint32_t current_slot;              /* node being walked; 0 when none */
    uint64_t current_first_member;      /* position of its first member */
    uint64_t window_first_member;       /* only positions from here... */
    uint64_t window_end_member;         /* ...up to (not including) here are wanted */
    uint32_t waiting_slot[GROUP_WALK_STACK_DEPTH];
    uint64_t waiting_first_member[GROUP_WALK_STACK_DEPTH];
    unsigned waiting_count;
    unsigned steps_in_this_hold;        /* nodes visited since the lock was taken */
    uint64_t steps_left;                /* total budget; running out means damage */
};

/* Called for each leaf; returns non-zero to end the walk early. */
typedef int (*member_visitor)(void *context, uint64_t input_id, uint64_t position);

/* Start a walk at 'root'.  Called with the group lock held and the root known to
 * be live.  Only members in [window_first, window_end) are wanted; subtrees
 * outside that window are skipped without being walked.  The step budget is the
 * size of the group's fully expanded tree plus every route node, so an intact
 * graph never runs out of it. */
static void member_walk_begin(struct member_walk *walk, const struct group_node *root,
                              uint64_t window_first, uint64_t window_end)
{
    *walk = (struct member_walk){0};
    walk->generation = g_generation;
    walk->current_slot = (uint32_t)root->record.sequence;
    walk->window_first_member = window_first;
    walk->window_end_member = window_end;
    walk->steps_left = GROUP_WALK_NODES_PER_MEMBER * root->record.info.member_count +
                       g_published_count;
}

/* Put the heavier child of a join on the waiting stack.  Returns 0 or -EINVAL
 * when the stack is full, which no intact graph can cause. */
static int member_walk_wait_for(struct member_walk *walk, uint32_t slot, uint64_t first_member)
{
    if (walk->waiting_count == GROUP_WALK_STACK_DEPTH) {
        return SCHED_OBSERVE_EINVAL;
    }
    walk->waiting_slot[walk->waiting_count] = slot;
    walk->waiting_first_member[walk->waiting_count] = first_member;
    walk->waiting_count = walk->waiting_count + 1;
    return 0;
}

/* Descend from the current node of a join or re-route, one step.  Called with
 * the group lock held.  Returns 0 or a negative error for a damaged graph. */
static int member_walk_descend(struct member_walk *walk, const struct group_node *node)
{
    struct coalesced_group_handle left_handle = handle_from_group_id(node->record.left_child_id);
    const struct group_node *left = find_live_node(left_handle);
    if (!left) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t first_member = walk->current_first_member;
    if (!node->record.right_child_id) {
        /* A re-route: the same members, one child. */
        walk->current_slot = left_handle.slot;
        return 0;
    }
    struct coalesced_group_handle right_handle = handle_from_group_id(node->record.right_child_id);
    const struct group_node *right = find_live_node(right_handle);
    if (!right) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t right_first_member = first_member + left->record.info.member_count;
    int left_is_lighter = left->record.info.member_count <= right->record.info.member_count;
    if (left_is_lighter) {
        walk->current_slot = left_handle.slot;
        return member_walk_wait_for(walk, right_handle.slot, right_first_member);
    }
    walk->current_slot = right_handle.slot;
    walk->current_first_member = right_first_member;
    return member_walk_wait_for(walk, left_handle.slot, first_member);
}

/* Advance a walk to its next leaf.  Called with the group lock held.  Returns 1
 * with the leaf's input id and position, 0 when the walk is finished, -EAGAIN
 * when this lock hold has used up its steps (give the lock up and call again),
 * or -EINVAL for a damaged or stale graph. */
static int member_walk_next_leaf(struct member_walk *walk, uint64_t *input_id, uint64_t *position)
{
    for (;;) {
        if (!walk->current_slot) {
            if (!walk->waiting_count) {
                return 0;
            }
            walk->waiting_count = walk->waiting_count - 1;
            walk->current_slot = walk->waiting_slot[walk->waiting_count];
            walk->current_first_member = walk->waiting_first_member[walk->waiting_count];
        }
        if (walk->steps_in_this_hold >= walk_steps_per_lock_hold()) {
            return SCHED_OBSERVE_EAGAIN;
        }
        if (!walk->steps_left) {
            return SCHED_OBSERVE_EINVAL;
        }
        walk->steps_in_this_hold = walk->steps_in_this_hold + 1;
        walk->steps_left = walk->steps_left - 1;
        struct coalesced_group_handle handle = {
            .slot = walk->current_slot,
            .generation = walk->generation,
        };
        const struct group_node *node = find_live_node(handle);
        if (!node) {
            return SCHED_OBSERVE_EINVAL;
        }
        uint64_t first_member = walk->current_first_member;
        uint64_t end_member = first_member + node->record.info.member_count;
        int outside_window = end_member <= walk->window_first_member ||
                             first_member >= walk->window_end_member;
        if (outside_window) {
            walk->current_slot = 0;
            continue;
        }
        if (node->record.input_id) {
            walk->current_slot = 0;
            *input_id = node->record.input_id;
            *position = first_member;
            return 1;
        }
        int descended = member_walk_descend(walk, node);
        if (descended) {
            return descended;
        }
    }
}

/* Run a walk to its end, one bounded stretch at a time.  Each stretch holds the
 * group lock for at most walk_steps_per_lock_hold() nodes and then lets go, so
 * a long chain never keeps interrupts off for the whole walk.  The visitor runs
 * with the lock held and must be short.  Returns 1 if the visitor ended the walk,
 * 0 if every leaf was visited, or a negative error for a damaged graph. */
static int member_walk_run(struct member_walk *walk, member_visitor visit, void *context)
{
    for (;;) {
        uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
        walk->steps_in_this_hold = 0;
        int result = 0;
        int visitor_ended_walk = 0;
        while (!visitor_ended_walk) {
            uint64_t input_id = 0;
            uint64_t position = 0;
            result = member_walk_next_leaf(walk, &input_id, &position);
            if (result != 1) {
                break;
            }
            visitor_ended_walk = visit(context, input_id, position) != 0;
        }
        observe_lock_release(&g_group_lock, saved_flags);
        if (visitor_ended_walk) {
            return 1;
        }
        if (result != SCHED_OBSERVE_EAGAIN) {
            return result;
        }
        OBSERVE_TEST_POINT(OBSERVE_POINT_GROUP_WALK_LOCK_RELEASED);
    }
}

/* What copy_members collects: where the wanted window starts, where each id goes
 * and how many have arrived. */
struct member_collection {
    uint64_t *input_ids;
    uint64_t first_member;
    uint32_t wanted;
    uint32_t collected;
    int duplicate_position;
};

/* Store one member at its position.  Two leaves claiming one position would mean
 * the graph's member counts lie, so the walk is ended and the copy refused. */
static int collect_member(void *context, uint64_t input_id, uint64_t position)
{
    struct member_collection *collection = context;
    uint64_t index = position - collection->first_member;
    if (collection->input_ids[index]) {
        collection->duplicate_position = 1;
        return 1;
    }
    collection->input_ids[index] = input_id;
    collection->collected = collection->collected + 1;
    return collection->collected == collection->wanted;
}

/* The ids must be the group's real leaves, not a range guessed from the first
 * one.  Flipped by SCHED_NEGCTL_GROUP_RANGE, which fills in consecutive numbers
 * after the first (host test: group_exact_leaves). */
static void negative_control_assume_consecutive(uint64_t *input_ids, uint32_t count)
{
#ifdef SCHED_NEGCTL_GROUP_RANGE
    for (uint32_t index = 1; index < count; index++) {
        input_ids[index] = input_ids[0] + index;
    }
#else
    (void)input_ids;
    (void)count;
#endif
}

/* The exact members of a group, in order.  They survive the group being popped
 * from a queue and later joined into bigger groups; no interval is guessed.  A
 * long group is walked in bounded stretches (see member_walk_run), so the cost is
 * linear in the group's size, not in size times depth. */
int coalesced_group_copy_members(struct coalesced_group_handle group, uint32_t first_member,
                                 uint64_t *input_ids, uint32_t count)
{
    if (!input_ids && count) {
        return SCHED_OBSERVE_EINVAL;
    }
    struct member_walk walk;
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    if (!node || first_member > node->record.info.member_count) {
        observe_lock_release(&g_group_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    if (count > node->record.info.member_count - first_member) {
        count = node->record.info.member_count - first_member;
    }
    member_walk_begin(&walk, node, first_member, (uint64_t)first_member + count);
    observe_lock_release(&g_group_lock, saved_flags);
    if (!count) {
        return 0;
    }
    for (uint32_t index = 0; index < count; index++) {
        input_ids[index] = 0;
    }
    struct member_collection collection = {
        .input_ids = input_ids, .first_member = first_member, .wanted = count,
    };
    int walked = member_walk_run(&walk, collect_member, &collection);
    /* Every wanted position must be filled exactly once; anything else means the
     * graph's member counts do not match its leaves. */
    int copy_complete = collection.collected == count && !collection.duplicate_position;
    if (walked < 0 || !copy_complete) {
        return SCHED_OBSERVE_EINVAL;
    }
    negative_control_assume_consecutive(input_ids, count);
    return count;
}

/* What the membership test remembers: the input it looks for. */
struct member_search {
    uint64_t input_id;
};

/* End the walk as soon as the wanted input shows up as a leaf. */
static int find_member(void *context, uint64_t input_id, uint64_t position)
{
    const struct member_search *search = context;
    (void)position;
    return input_id == search->input_id;
}

/* Does the group contain this input?  A full walk in bounded stretches. */
static int group_contains_input(struct coalesced_group_handle group, uint64_t input_id)
{
    struct member_walk walk;
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    if (!node) {
        observe_lock_release(&g_group_lock, saved_flags);
        return 0;
    }
    member_walk_begin(&walk, node, 0, UINT64_MAX);
    observe_lock_release(&g_group_lock, saved_flags);
    struct member_search search = {.input_id = input_id};
    return member_walk_run(&walk, find_member, &search) == 1;
}

/* Completion needs a group that (a) is still held by a consumer, not merely kept
 * by the archive and its parents, (b) contains this input, and (c) carries
 * exactly the route and merge information the evidence claims.  The receipt code
 * checks the window state and surface before calling the ledger, which calls
 * this. */
int coalesced_group_confirms_completion(uint64_t input_id,
                                        const struct input_completion_evidence *evidence)
{
    struct coalesced_group_handle group = handle_from_group_id(evidence->group_id);
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    struct group_node *node = find_live_node(group);
    int plausible = 0;
    if (node) {
        const struct coalesced_group_info *info = &node->record.info;
        /* The route the evidence names, and the merge rule and its version. */
        int route_matches = info->route_generation == evidence->route_generation;
        int rule_matches = info->reducer_id == evidence->reducer_id;
        rule_matches = rule_matches && info->reducer_version == evidence->reducer_version;
        int layer_matches = info->layer == evidence->layer;
        int metadata_matches = route_matches && rule_matches;
        metadata_matches = metadata_matches && layer_matches;
        plausible = held_by_consumer(node) && metadata_matches;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    if (!plausible) {
        return 0;
    }
    return group_contains_input(group, input_id);
}

/* ---- deferred destruction and archive ----------------------------------- */

/* Destroy queued nodes bottom-up, at most one batch per call, and say how many
 * were destroyed.  The ledger pin a leaf holds is given back outside the group
 * lock, so the group lock and the ledger lock are never nested the wrong way
 * round.  The live count drops only after that final pin was returned:
 * otherwise STOP could free and reopen the ledger while this worker still owed a
 * release to the same input id of the previous run. */
static unsigned destroy_queued_batch(void)
{
    unsigned destroyed = 0;
    while (destroyed < COALESCED_RELEASE_BATCH) {
        uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
        if (!g_nodes || g_queue_head == g_queue_tail) {
            observe_lock_release(&g_group_lock, saved_flags);
            break;
        }
        uint32_t queued_index = g_release_queue[g_queue_head % g_capacity];
        g_queue_head = g_queue_head + 1;
        struct group_node *node = &g_nodes[queued_index];
        OBSERVE_TEST_POINT(OBSERVE_POINT_GROUP_NODE_RELEASED);
        uint64_t input_id = node->record.input_id;
        uint64_t child_ids[GROUP_MAX_CHILDREN] = {node->record.left_child_id,
                                                  node->record.right_child_id};
        for (unsigned child = 0; child < GROUP_MAX_CHILDREN; child++) {
            if (!child_ids[child] || negative_control_keep_children_alive()) {
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
        destroyed = destroyed + 1;
    }
    return destroyed;
}

/* Destroy queued nodes, a bounded number per call; called from the periodic
 * timer tick. */
void coalesced_groups_release_queued(void)
{
    (void)destroy_queued_batch();
}

/* Give back the archive's reference on a batch of nodes, and say how many.
 * Called only after the INPUT and GROUPS exports are confirmed.  Freeing groups
 * earlier would let their slots be reused and make the exported completion
 * evidence point at history that no longer exists. */
static unsigned drop_archive_batch(void)
{
    unsigned dropped = 0;
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    if (!g_nodes) {
        observe_lock_release(&g_group_lock, saved_flags);
        return 0;
    }
    while (dropped < COALESCED_RELEASE_BATCH && g_archive_cursor < g_published_count) {
        struct group_node *node = &g_nodes[g_archive_cursor];
        g_archive_cursor = g_archive_cursor + 1;
        release_node(node);
        dropped = dropped + 1;
    }
    observe_lock_release(&g_group_lock, saved_flags);
    return dropped;
}

/* Drop the experiment's own archive reference on a bounded batch of nodes. */
void coalesced_groups_drop_archive_references(void)
{
    (void)drop_archive_batch();
}

/* Give the whole archive back and destroy everything that becomes unreferenced,
 * in bounded batches with no lock held between them.  Process context only:
 * with a million groups this is a long loop.  Returns how many nodes are still
 * alive, which is zero unless a consumer still holds a group. */
uint64_t coalesced_groups_drain_archive(void)
{
    unsigned progress;
    do {
        progress = drop_archive_batch();
        progress = progress + destroy_queued_batch();
    } while (progress);
    return coalesced_groups_live_count();
}

/* Export one archive row as a private copy.  Reading never consumes a
 * reference. */
int coalesced_groups_read_archive_row(uint64_t sequence, struct sched_observe_group_record *out)
{
    uint64_t saved_flags = observe_lock_acquire(&g_group_lock);
    int valid = 0;
    int row_in_range = sequence != 0 && sequence <= g_published_count;
    if (g_nodes && row_in_range) {
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
