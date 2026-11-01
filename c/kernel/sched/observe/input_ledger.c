/* Input ledger: the account of every user input event seen during a run.
 *
 * WHAT IT GUARANTEES.  Each input gets one record when it arrives and ends with
 * exactly one outcome (shown, merged into a later event, handled by the app
 * without drawing, or expired).  The record is never overwritten, even if the
 * input queue it travelled through drops or merges events, so the count of
 * inputs the run is judged on cannot shrink by accident.
 *
 * DEADLINES.  An input that arrives at time T, with display frame period P,
 * should show a response by T + 2P and must have an outcome by T + 3P.  The
 * deadlines are computed once from the original arrival and never restart.
 * Equal to the outcome deadline still counts as in time; later is EXPIRED.
 *
 * ONE LOCK DECIDES THE OUTCOME.  A single lock (g_ledger_lock), taken before no
 * other lock, serializes the app's receipt against the periodic timer's expiry,
 * so only one of them can win.  The completion evidence is copied in the same critical section as
 * the outcome.  No system call can choose an outcome directly.
 *
 * Design reference: sections 2.2, 7.4.8 and 10.4 of
 * docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md. */
#include "input_ledger.h"
#include "coalesced_inputs.h"
#include "flight_recorder.h"
#include "observe_control.h"

/* Response deadline = arrival + 2 frame periods; outcome deadline = arrival +
 * 3 frame periods (design section 2.2). */
#define RESPONSE_DEADLINE_FRAME_PERIODS 2u
#define OUTCOME_DEADLINE_FRAME_PERIODS 3u

/* The timer tick checks at most this many ledger rows for expiry, so a big
 * ledger cannot make one interrupt long.  A large run is drained over several
 * ticks, and STOP honestly reports "pending" until it is. */
#define INPUT_LEDGER_EXPIRY_BATCH 64u

/* Event types are used as bit positions in a 32-bit mask in the manifest. */
#define INPUT_TYPE_MASK_BITS 32u

/* Input ids are never reused within a boot.  This is the largest id handed out;
 * beyond it the ledger refuses new runs rather than wrap. */
#define INPUT_ID_LAST_ALLOWED (UINT64_MAX - 1)

struct ledger_entry {
    struct input_record record;
    uint64_t reference_count;   /* 1 for the ledger itself, plus one per pin */
};

static struct observe_spinlock g_ledger_lock;
static struct ledger_entry *g_entries;
static uint64_t g_capacity;
static uint64_t g_issued_count;
static uint64_t g_pending_count;
static uint64_t g_expiry_scan_cursor;

/* Input ids grow for the whole boot and are never reused.  Otherwise a caller
 * that was preempted holding an old id could pin an unrelated input of a later
 * run that happens to sit at the same table index.  The manifest ordinal and
 * the table index restart every run; the id does not. */
static uint64_t g_last_input_id;
static uint64_t g_first_input_id = 1;

static uint64_t g_outcome_counts[INPUT_OUTCOME_COUNT];

/* ---- hooks flipped by the negative-control tests ------------------------
 * Each hook behaves as production code unless the named macro is defined, and
 * the macro is defined only when the test named in its comment builds the
 * fault-injected variant. */

/* A new run must continue the input id numbering.  Flipped by
 * SCHED_NEGCTL_INPUT_REUSE_ID, which restarts it at 1 (host and guest test:
 * input_id_no_reuse).  Called with the ledger lock held. */
static void negctl_restart_input_ids(void)
{
#ifdef SCHED_NEGCTL_INPUT_REUSE_ID
    g_last_input_id = 0;
#endif
}

/* A full ledger must refuse new inputs and keep every old record.  Flipped by
 * SCHED_NEGCTL_INPUT_OVERWRITE, which damages the first record (host test:
 * input_first_retained).  Called with the ledger lock held. */
static void negctl_overwrite_first_record(void)
{
#ifdef SCHED_NEGCTL_INPUT_OVERWRITE
    g_entries[0].record.input_id = g_issued_count + 1;
#endif
}

/* The outcome deadline is fixed at arrival + 3 frame periods.  Flipped by
 * SCHED_NEGCTL_INPUT_RESET_DEADLINE, which pushes it one period later (host and
 * guest test: input_deadline_exact). */
static uint64_t outcome_deadline_for(uint64_t arrival_ns, uint64_t frame_period_ns)
{
    uint64_t deadline_ns = arrival_ns + OUTCOME_DEADLINE_FRAME_PERIODS * frame_period_ns;
#ifdef SCHED_NEGCTL_INPUT_RESET_DEADLINE
    deadline_ns += frame_period_ns;
#endif
    return deadline_ns;
}

/* DONE and COALESCED_DONE need valid evidence.  Flipped by
 * SCHED_NEGCTL_INPUT_DROP_PROOF, which skips the check (host and guest test:
 * input_completion_proof). */
static int negctl_skip_evidence_check(void)
{
#ifdef SCHED_NEGCTL_INPUT_DROP_PROOF
    return 1;
#else
    return 0;
#endif
}

/* APP_DONE means "the app handled it and correctly drew nothing", which is only
 * legal for inputs that need no display.  Flipped by SCHED_NEGCTL_APP_DONE_DISPLAY,
 * which allows it for any input (host and guest test: display_done_refused). */
static int app_done_refused(const struct input_record *record, uint32_t outcome)
{
#ifdef SCHED_NEGCTL_APP_DONE_DISPLAY
    (void)record;
    (void)outcome;
    return 0;
#else
    return outcome == INPUT_APP_DONE && record->input_class != INPUT_NO_DISPLAY;
#endif
}

/* The not-minted inputs of stage S0 must expire like any other.  Flipped by
 * SCHED_NEGCTL_INPUT_SKIP_NOT_MINTED, which exempts them (host and guest test:
 * unminted_expires). */
static int expiry_skips(const struct input_record *record)
{
#ifdef SCHED_NEGCTL_INPUT_SKIP_NOT_MINTED
    return record->allocation == INPUT_NOT_MINTED;
#else
    (void)record;
    return 0;
#endif
}

/* ---- pool life cycle ---------------------------------------------------- */

/* Reserve the ledger before the run is published, so the check and the
 * allocation happen at one place.  Publishing the table and resetting its
 * counters happen under the lock together: the periodic timer keeps running
 * between runs, and a smaller new table must never be scanned with the old
 * table's cursor or count. */
int input_ledger_allocate_pool(uint64_t capacity)
{
    struct ledger_entry *entries = observe_allocate_zeroed(capacity, sizeof *entries);
    if (!entries) {
        return SCHED_OBSERVE_ENOMEM;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    negctl_restart_input_ids();
    if (g_last_input_id >= INPUT_ID_LAST_ALLOWED) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        observe_free(entries);
        return SCHED_OBSERVE_EOVERFLOW;
    }
    g_entries = entries;
    g_first_input_id = g_last_input_id + 1;
    g_capacity = capacity;
    g_issued_count = 0;
    g_pending_count = 0;
    g_expiry_scan_cursor = 0;
    for (unsigned outcome = 0; outcome < INPUT_OUTCOME_COUNT; outcome++) {
        g_outcome_counts[outcome] = 0;
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
    return 0;
}

/* Free the ledger.  Called only after every coalesced group and reader that
 * refers to it has finished and the exports were confirmed. */
void input_ledger_free_pool(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    struct ledger_entry *entries = g_entries;
    g_entries = NULL;
    g_capacity = 0;
    g_issued_count = 0;
    g_expiry_scan_cursor = 0;
    g_pending_count = 0;
    observe_lock_release(&g_ledger_lock, saved_flags);
    observe_free(entries);
}

/* ---- arrival ------------------------------------------------------------ */

/* FNV-1a hash of the raw payload bytes, as written in the manifest.  It hashes
 * the original sample, never a value a queue later replaces with a newer one. */
static uint64_t input_payload_hash(const struct input_payload *payload)
{
    uint64_t hash = SCHED_OBSERVE_FNV1A_OFFSET_BASIS;
    const unsigned char *bytes = (const unsigned char *)payload;
    for (size_t i = 0; i < sizeof *payload; i++) {
        hash ^= bytes[i];
        hash *= SCHED_OBSERVE_FNV1A_PRIME;
    }
    return hash;
}

/* Does the arrival match what the experiment preregistered for this position?
 * A mismatch does not remove the input from the ledger (it stays in the count),
 * but it makes the run invalid.  Returns 1 if there is no manifest to compare
 * against.  The class and source are compared as given, never rewritten. */
static int arrival_matches_manifest(uint64_t ordinal, uint32_t source, uint32_t input_class,
                                    const struct input_payload *payload)
{
    const struct sched_observe_manifest *entry = observe_manifest_entry(ordinal);
    if (!entry) {
        return 1;
    }
    if (entry->ordinal != ordinal) {
        return 0;
    }
    if (entry->input_class != input_class || entry->source != source) {
        return 0;
    }
    if (entry->payload_hash != input_payload_hash(payload)) {
        return 0;
    }
    if (payload->event_type >= INPUT_TYPE_MASK_BITS) {
        return 0;
    }
    if (!(entry->allowed_type_mask & (1u << payload->event_type))) {
        return 0;
    }
    if (entry->left_button != payload->left_button) {
        return 0;
    }
    if (entry->right_button != payload->right_button) {
        return 0;
    }
    if (entry->middle_button != payload->middle_button) {
        return 0;
    }
    return entry->modifiers == payload->modifiers;
}

/* Add an input to the ledger.  The deadlines come from the original arrival
 * time.  If the arithmetic would overflow, the run is invalid, rather than a
 * wrapped deadline that a late receipt could appear to beat. */
uint64_t input_ledger_record_arrival(uint64_t arrival_ns, uint64_t arrival_tsc,
                                     uint64_t frame_period_ns, uint64_t frame_generation,
                                     uint32_t source, uint32_t input_class,
                                     const struct input_payload *payload)
{
    if (!payload || !frame_period_ns) {
        observe_mark_invalid();
        return 0;
    }
    if (input_class > INPUT_NO_DISPLAY || source > SCHED_INPUT_IRQ) {
        observe_mark_invalid();
        return 0;
    }
    /* Both deadlines must fit in 64 bits, or they would wrap to a tiny value. */
    if (frame_period_ns > (UINT64_MAX - arrival_ns) / OUTCOME_DEADLINE_FRAME_PERIODS) {
        observe_mark_invalid();
        return 0;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    if (!g_entries || !observe_admitting_inputs()) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        observe_count_lost_record();
        return 0;
    }
    if (g_issued_count == g_capacity) {
        negctl_overwrite_first_record();
        observe_lock_release(&g_ledger_lock, saved_flags);
        observe_count_lost_record();
        return 0;
    }
    if (g_last_input_id >= INPUT_ID_LAST_ALLOWED) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        observe_mark_invalid();
        return 0;
    }
    uint64_t input_id = ++g_last_input_id;
    struct ledger_entry *entry = &g_entries[g_issued_count++];
    entry->record = (struct input_record){
        .input_id = input_id,
        .irq_timestamp_ns = arrival_ns,
        .irq_timestamp_tsc = arrival_tsc,
        .frame_period_ns = frame_period_ns,
        .frame_generation = frame_generation,
        .response_deadline_ns = arrival_ns + RESPONSE_DEADLINE_FRAME_PERIODS * frame_period_ns,
        .outcome_deadline_ns = outcome_deadline_for(arrival_ns, frame_period_ns),
        .source = source,
        .input_class = input_class,
        .allocation = INPUT_NOT_MINTED,
        .outcome = INPUT_PENDING,
        .payload = *payload,
    };
    entry->reference_count = 1;
    g_pending_count++;
    if (!arrival_matches_manifest(g_issued_count, source, input_class, payload)) {
        observe_mark_invalid();
    }
    /* Record the INPUT event while still holding the lock, which keeps
     * admission pinned through its publication.  A thread that polled a device
     * could otherwise be suspended after unlocking and resume inside a later
     * run. */
    flight_record_event(FLIGHT_EVENT_INPUT, 0, 0, source, input_id, arrival_ns);
    observe_lock_release(&g_ledger_lock, saved_flags);
    return input_id;
}

/* Is this id one of the inputs recorded in the current run?  Called with the
 * lock held.  Ids of earlier runs are rejected because ids are never reused. */
static int input_in_current_run(uint64_t input_id)
{
    if (!g_entries) {
        return 0;
    }
    if (input_id < g_first_input_id) {
        return 0;
    }
    return input_id - g_first_input_id < g_issued_count;
}

/* Stage S0 mints no tokens, so claiming MINTED or MERGED here would present an
 * unrealistic success and contaminate the baseline measurements.  A later stage
 * that issues tokens must implement issuing before enabling this. */
int input_ledger_set_allocation(uint64_t input_id, uint32_t allocation)
{
    if (allocation != INPUT_NOT_MINTED) {
        return SCHED_OBSERVE_ENOSYS;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    int valid = input_in_current_run(input_id);
    observe_lock_release(&g_ledger_lock, saved_flags);
    return valid ? 0 : SCHED_OBSERVE_EINVAL;
}

/* ---- outcome ------------------------------------------------------------ */

/* Record the one final outcome of an input.  The caller holds the ledger lock.
 * Returns 1 if this call settled the input and 0 if it was already settled.
 * This is the only place an outcome is written.  An outcome time equal to the
 * deadline is still in time; a later one turns whatever was asked into EXPIRED
 * and drops the evidence. */
static int settle_outcome_locked(struct input_record *record, uint32_t outcome, uint64_t now_ns,
                                 const struct input_completion_evidence *evidence)
{
    if (record->outcome != INPUT_PENDING) {
        return 0;
    }
    OBSERVE_TEST_POINT(OBSERVE_POINT_OUTCOME_BEFORE_DEADLINE_CHECK);
    if (now_ns > record->outcome_deadline_ns) {
        outcome = INPUT_EXPIRED;
        evidence = NULL;
    }
    record->completed_at_ns = now_ns;
    if (evidence) {
        record->completion_evidence = *evidence;
    }
    record->outcome = outcome;
    g_pending_count--;
    g_outcome_counts[outcome]++;
    flight_record_event(FLIGHT_EVENT_OUTCOME, 0, 0, outcome, record->input_id, now_ns);
    return 1;
}

/* Settle an input and release the lock.  The outcome must be written before the
 * lock is released.  Flipped by SCHED_NEGCTL_OUTCOME_UNLOCKED, which releases it
 * first so the timer can race in (host test: input_single_outcome). */
static int settle_and_unlock(struct input_record *record, uint32_t outcome, uint64_t now_ns,
                             const struct input_completion_evidence *evidence,
                             uint64_t saved_flags)
{
#ifdef SCHED_NEGCTL_OUTCOME_UNLOCKED
    observe_lock_release(&g_ledger_lock, saved_flags);
    return settle_outcome_locked(record, outcome, now_ns, evidence);
#else
    int changed = settle_outcome_locked(record, outcome, now_ns, evidence);
    observe_lock_release(&g_ledger_lock, saved_flags);
    return changed;
#endif
}

/* Check that the evidence offered for DONE or COALESCED_DONE really shows that
 * a retained group covers this input.  Runs before the ledger lock is taken:
 * the group lock must never be held while taking the ledger lock, so the
 * checks that need the group module are done first, and the group archive keeps
 * the evidence alive until it has been exported.  Returns 1 if acceptable.
 * The window-manager side (stage S0b) verifies the window state and surface
 * before calling here; this only checks what the ledger can see. */
static int completion_evidence_acceptable(uint64_t input_id, uint32_t outcome,
                                          const struct input_completion_evidence *evidence)
{
    if (!evidence) {
        return 0;
    }
    if (!evidence->frame_id || !evidence->state_version || !evidence->surface_version) {
        return 0;
    }
    if (!coalesced_group_confirms_completion(input_id, evidence)) {
        return 0;
    }
    struct coalesced_group_handle group = {
        .slot = (uint32_t)evidence->group_id,
        .generation = (uint32_t)(evidence->group_id >> SCHED_OBSERVE_GROUP_ID_GENERATION_SHIFT),
    };
    struct coalesced_group_info info;
    if (coalesced_group_describe(group, &info)) {
        return 0;
    }
    if (outcome == INPUT_DONE) {
        /* The input's own delivery was shown, so the group must be represented
         * by this very input. */
        return info.representative_input_id == input_id;
    }
    /* COALESCED_DONE: a later event stands for this input.  Merely carrying the
     * raw input along is not a merge; the group must come from a preregistered
     * semantic merge (non-zero layer and reducer) matching the manifest's
     * route, capture and window versions. */
    const struct sched_observe_manifest *entry =
        observe_manifest_entry(input_ledger_manifest_ordinal(input_id));
    if (!entry) {
        return 0;
    }
    if (!info.layer || !info.reducer_id || !info.reducer_version) {
        return 0;
    }
    if (info.representative_input_id == input_id) {
        return 0;
    }
    if (entry->reducer_id != info.reducer_id || entry->reducer_version != info.reducer_version) {
        return 0;
    }
    if (entry->route_generation != info.route_generation) {
        return 0;
    }
    if (entry->capture_generation != info.capture_generation) {
        return 0;
    }
    if (entry->window_generation != info.window_generation) {
        return 0;
    }
    return entry->window == info.window;
}

/* The public way to settle an input (called by the receipt code of stage S0b,
 * and by tests).  Refuses outcomes the evidence or class does not support, so a
 * user program can never pick its own result. */
int input_ledger_settle(uint64_t input_id, uint32_t outcome, uint64_t now_ns,
                        const struct input_completion_evidence *evidence)
{
    if (!input_id || outcome <= INPUT_PENDING || outcome >= INPUT_OUTCOME_COUNT) {
        return SCHED_OBSERVE_EINVAL;
    }
    int needs_display_evidence = outcome == INPUT_DONE || outcome == INPUT_COALESCED_DONE;
    if (needs_display_evidence && !negctl_skip_evidence_check()) {
        if (!completion_evidence_acceptable(input_id, outcome, evidence)) {
            return SCHED_OBSERVE_EINVAL;
        }
    }
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    if (!input_in_current_run(input_id)) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    struct input_record *record = &g_entries[input_id - g_first_input_id].record;
    if (app_done_refused(record, outcome)) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    int settles_before_arrival = now_ns < record->irq_timestamp_ns;
    int expires_early = outcome == INPUT_EXPIRED && now_ns <= record->outcome_deadline_ns;
    if (settles_before_arrival || expires_early) {
        observe_lock_release(&g_ledger_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    return settle_and_unlock(record, outcome, now_ns, evidence, saved_flags);
}

/* The periodic timer expires overdue inputs, independently of any scheduler
 * token or group lifetime.  It examines a bounded batch per tick, continuing
 * where the previous tick stopped. */
void input_ledger_expire_overdue(uint64_t now_ns)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    uint64_t batch = g_issued_count < INPUT_LEDGER_EXPIRY_BATCH ? g_issued_count
                                                                 : INPUT_LEDGER_EXPIRY_BATCH;
    for (uint64_t examined = 0; g_entries && examined < batch; examined++) {
        if (g_expiry_scan_cursor >= g_issued_count) {
            g_expiry_scan_cursor = 0;
        }
        struct input_record *record = &g_entries[g_expiry_scan_cursor++].record;
        if (expiry_skips(record)) {
            continue;
        }
        if (now_ns > record->outcome_deadline_ns) {
            settle_outcome_locked(record, INPUT_EXPIRED, now_ns, NULL);
        }
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
}

/* ---- reading and pinning ------------------------------------------------ */

/* Copy one record as it is at this instant.  The live table is never exposed,
 * so a reader cannot see a half-updated record. */
int input_ledger_copy_record(uint64_t input_id, struct input_record *out)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    int valid = input_in_current_run(input_id);
    if (valid) {
        *out = g_entries[input_id - g_first_input_id].record;
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
    return valid;
}

/* Pin an input so that a coalesced group's leaf keeps its record alive after
 * the queue slot that carried the event was popped. */
int input_ledger_acquire(uint64_t input_id)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    int valid = input_in_current_run(input_id) &&
                g_entries[input_id - g_first_input_id].reference_count < UINT64_MAX;
    if (valid) {
        g_entries[input_id - g_first_input_id].reference_count++;
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
    return valid;
}

/* Drop a pin.  The ledger's own reference (the first) is never dropped here; it
 * ends when the whole table is freed.  Dropping a pin that is not held means a
 * caller's bookkeeping is wrong, which invalidates the run. */
void input_ledger_release(uint64_t input_id)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    int pin_is_held = 0;
    if (input_in_current_run(input_id)) {
        pin_is_held = g_entries[input_id - g_first_input_id].reference_count > 1;
    }
    if (pin_is_held) {
        g_entries[input_id - g_first_input_id].reference_count--;
    } else {
        observe_mark_invalid();
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
}

/* ---- counters ----------------------------------------------------------- */

/* All outcome counts are read under one lock hold, so the HUD and STATUS see a
 * set that adds up. */
void input_ledger_fill_status(struct sched_observe_status *status)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    status->inputs_issued = g_issued_count;
    status->inputs_pending = g_pending_count;
    status->inputs_done = g_outcome_counts[INPUT_DONE];
    status->inputs_coalesced_done = g_outcome_counts[INPUT_COALESCED_DONE];
    status->inputs_app_done = g_outcome_counts[INPUT_APP_DONE];
    status->inputs_expired = g_outcome_counts[INPUT_EXPIRED];
    observe_lock_release(&g_ledger_lock, saved_flags);
}

/* How many inputs this run has recorded; it defines the end of the INPUT stream. */
uint64_t input_ledger_issued_count(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    uint64_t count = g_issued_count;
    observe_lock_release(&g_ledger_lock, saved_flags);
    return count;
}

/* Inputs still waiting for an outcome.  STOP waits for this to reach zero
 * without holding any lock while it waits. */
uint64_t input_ledger_pending_count(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    uint64_t count = g_pending_count;
    observe_lock_release(&g_ledger_lock, saved_flags);
    return count;
}

/* A READ with cursor 0 starts at this run's first input id. */
uint64_t input_ledger_first_input_id(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    uint64_t first = g_first_input_id;
    observe_lock_release(&g_ledger_lock, saved_flags);
    return first;
}

/* Turn an input id into its 1-based position in this run, which is the key of
 * the sealed manifest. */
uint64_t input_ledger_manifest_ordinal(uint64_t input_id)
{
    uint64_t saved_flags = observe_lock_acquire(&g_ledger_lock);
    uint64_t ordinal = 0;
    if (input_in_current_run(input_id)) {
        ordinal = input_id - g_first_input_id + 1;
    }
    observe_lock_release(&g_ledger_lock, saved_flags);
    return ordinal;
}
