/* Run control for the scheduler observation facility.
 *
 * A privileged tool drives a run through the system call:
 *   BEGIN   validate everything, allocate every pool, then publish the run;
 *   STOP    close admission of new inputs, wait out the original deadlines of
 *           pending inputs, ask every CPU to seal its flight recorder ring, and
 *           report whether the run reached STOPPED;
 *   STATUS  copy the counters;
 *   READ    page records out of the four streams, each reader with its own
 *           cursor, and finally confirm that an export is complete.
 * Memory is freed only when all CPUs have sealed their rings, every stream has
 * been exported and confirmed, and no reader is inside a READ.
 *
 * Rules this file follows:
 *   - BEGIN either publishes a fully valid run or has no effect: lengths,
 *     pointers and allocation failures are all checked before anything is
 *     published.
 *   - STOP never waits while holding a lock and does not need an inter-
 *     processor interrupt.  The shutdown advances at points that already run
 *     (scheduler entry, interrupt exit, timer tick); a CPU that never reaches
 *     one keeps the run unfinished (reported as PENDING) and its memory is kept.
 *   - Nothing here makes a scheduling decision.  Optional later modules register
 *     as providers; a command nobody answers returns "not implemented", never a
 *     success-shaped stub.
 *
 * Design reference: section 10.4 (and appendix F11) of
 * docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md,
 * and task S0a of docs/superpowers/plans/2026-11-01-sched-s0-s1.md. */
#include "observe_control.h"
#include "flight_recorder.h"
#include "input_ledger.h"
#include "coalesced_inputs.h"
#include "../../diag/kprintf.h"

#ifdef SCHED_OBSERVE_GUEST
#include "sched.h"
#include "percpu.h"
static void observe_guest_main(void);
#endif

/* Streams are numbered from one.  A stream's records are numbered from one as
 * well, except INPUT, whose numbers are input ids (see logit_sched_observe.h).
 * The METADATA stream holds exactly one record. */
#define STREAM_FIRST_CURSOR 1u
#define METADATA_STREAM_LAST_RECORD 1u

/* Bit that marks a stream as exported in g_exported_stream_mask.  Flight
 * recorder exports are tracked per CPU instead (g_trace_exported_cpu_mask). */
#define STREAM_EXPORT_BIT(stream) (1u << ((stream) - 1))
#define NON_TRACE_STREAMS_EXPORTED_MASK                                   \
    (STREAM_EXPORT_BIT(SCHED_STREAM_INPUT) | STREAM_EXPORT_BIT(SCHED_STREAM_GROUPS) | \
     STREAM_EXPORT_BIT(SCHED_STREAM_METADATA))

struct provider_slot {
    long (*command)(long, long, long);
    int (*metadata)(char *, int);
};

static struct observe_spinlock g_control_lock;
static uint64_t g_run_id;
static uint64_t g_last_run_id;
static uint64_t g_lost_record_count;
static uint64_t g_run_invalid;
static uint64_t g_active_reads;
static uint64_t g_admitting_inputs;
static uint32_t g_state;
static uint32_t g_baseline;
static uint32_t g_owner_user_id;
static uint32_t g_owner_process_id;
static uint32_t g_producer_cpu_mask;
static uint32_t g_exported_stream_mask;
static uint32_t g_trace_exported_cpu_mask;
static int g_begin_in_progress;
static int g_control_initialized;
static struct sched_observe_manifest *g_manifest;
static uint64_t g_manifest_entry_count;
static uint64_t g_manifest_hash[SCHED_OBSERVE_MANIFEST_HASH_WORDS];
static struct provider_slot g_providers[SCHED_PROVIDER_COUNT];

/* Change the run state.  Always called with the control lock held; the store is
 * atomic because observe_advance_shutdown() reads the state without the lock to
 * skip the common "nothing to do" case cheaply. */
static void set_run_state(uint32_t new_state)
{
    __atomic_store_n(&g_state, new_state, __ATOMIC_RELEASE);
}

/* ---- hooks flipped by the negative-control tests ------------------------
 * Each hook behaves as production code unless the named macro is defined, and
 * the macro is defined only when the test named in its comment builds the
 * fault-injected variant. */

/* Memory may be reclaimed only when no READ is in progress.  Flipped by
 * SCHED_NEGCTL_TRACE_EARLY_FREE, which ignores readers (host test:
 * trace_session_live). */
static int no_read_blocks_reclaim(void)
{
#ifdef SCHED_NEGCTL_TRACE_EARLY_FREE
    return 1;
#else
    return !g_active_reads;
#endif
}

/* The group archive must survive until the exports are confirmed.  Flipped by
 * SCHED_NEGCTL_GROUP_ARCHIVE_EARLY_FREE, which frees it when STOP drains (host
 * and guest test: group_history_exportable). */
static void negctl_free_group_archive_early(void)
{
#ifdef SCHED_NEGCTL_GROUP_ARCHIVE_EARLY_FREE
    coalesced_groups_drop_archive_references();
    coalesced_groups_release_queued();
#endif
}

#ifdef SCHED_NEGCTL_TRACE_SHARED_CURSOR
static uint64_t g_shared_trace_cursor = 1;
#endif

/* Each reader has its own position in a trace ring.  Flipped by
 * SCHED_NEGCTL_TRACE_SHARED_CURSOR, which makes every reader continue where the
 * last one stopped (host and guest test: trace_reader_independent). */
static uint64_t trace_start_cursor(uint32_t stream, uint64_t requested_cursor)
{
#ifdef SCHED_NEGCTL_TRACE_SHARED_CURSOR
    if (stream == SCHED_STREAM_TRACE) {
        return g_shared_trace_cursor;
    }
#else
    (void)stream;
#endif
    return requested_cursor;
}

/* Part of the shared-cursor injection above: remember where this page ended. */
static void negctl_remember_trace_cursor(uint32_t stream, uint64_t next_cursor)
{
#ifdef SCHED_NEGCTL_TRACE_SHARED_CURSOR
    if (stream == SCHED_STREAM_TRACE) {
        g_shared_trace_cursor = next_cursor;
    }
#else
    (void)stream;
    (void)next_cursor;
#endif
}

/* The cursor after one record has been copied is the next sequence number.
 * Flipped by SCHED_NEGCTL_READ_SKIP_RECORD, which skips one (host and guest
 * test: read_pages_exact). */
static uint64_t cursor_after_record(uint64_t cursor)
{
#ifdef SCHED_NEGCTL_READ_SKIP_RECORD
    return cursor + 1 + 1;
#else
    return cursor + 1;
#endif
}

/* ---- life cycle --------------------------------------------------------- */

/* Mark the facility ready.  Called once, at the end of sched_init().  Nothing
 * else starts here: no run, no timer, no benchmark, and no module that belongs
 * to a later stage is linked in. */
void observe_control_init(void)
{
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    g_control_initialized = 1;
    observe_lock_release(&g_control_lock, saved_flags);
#ifdef SCHED_OBSERVE_GUEST
    /* Test images start the built-in diagnostic program as a kernel thread.
     * Default images neither contain it nor start it. */
    thread_create(observe_guest_main, "sched-observe-test");
#endif
}

/* Invalid is permanent for a run: losing evidence must never make a measurement
 * look better. */
void observe_mark_invalid(void)
{
    __atomic_store_n(&g_run_invalid, 1, __ATOMIC_SEQ_CST);
}

/* Count a record that could not be kept.  Done even with no active run (the
 * counter is a plain atomic, so there is no hidden allocation), and an active
 * run becomes invalid. */
void observe_count_lost_record(void)
{
    __atomic_fetch_add(&g_lost_record_count, 1, __ATOMIC_SEQ_CST);
    observe_mark_invalid();
}

/* Admission of new inputs closes first when STOP begins, before STOP examines
 * the pending count, and the check is made under the ledger lock. */
int observe_admitting_inputs(void)
{
    return __atomic_load_n(&g_admitting_inputs, __ATOMIC_SEQ_CST) != 0;
}

/* A run id is a number and so safe to keep in queues and receipts; a pointer to
 * the run would not be, because the run's memory is freed at the end. */
uint64_t observe_run_id(void)
{
    return __atomic_load_n(&g_run_id, __ATOMIC_SEQ_CST);
}

/* Look up the sealed manifest entry for an input's position.  The input ledger
 * calls this while holding its own lock, which pins the run.  BEGIN and STOP
 * never change the sealed manifest; it is freed only after the ledger, groups
 * and readers have drained. */
const struct sched_observe_manifest *observe_manifest_entry(uint64_t ordinal)
{
    if (!g_manifest_entry_count) {
        return NULL;
    }
    if (!ordinal || ordinal > g_manifest_entry_count) {
        observe_mark_invalid();
        return NULL;
    }
    return &g_manifest[ordinal - 1];
}

/* Register an optional module once, before the first run.  Registration and
 * BEGIN use the same lock, so they cannot interleave.  An unregistered kind
 * simply stays unsupported. */
int observe_provider_register(unsigned kind, long (*command)(long, long, long),
                              int (*metadata)(char *, int))
{
    if (!kind || kind >= SCHED_PROVIDER_COUNT || (!command && !metadata)) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    int result = 0;
    int run_ever_started = g_last_run_id != 0;
    run_ever_started = run_ever_started || g_begin_in_progress || g_state != SCHED_OBSERVE_IDLE;
    int kind_taken = g_providers[kind].command || g_providers[kind].metadata;
    if (run_ever_started || kind_taken) {
        result = SCHED_OBSERVE_EBUSY;
    } else {
        g_providers[kind] = (struct provider_slot){command, metadata};
    }
    observe_lock_release(&g_control_lock, saved_flags);
    return result;
}

/* ---- BEGIN -------------------------------------------------------------- */

/* Check every manifest entry before the run is published.  An empty manifest is
 * allowed only for the recorder-only condition (used by the S0a diagnostic
 * program); the two baselines must preregister their inputs. */
static int manifest_is_valid(const struct sched_observe_begin *request,
                             const struct sched_observe_manifest *manifest)
{
    if (!request->manifest_entry_count) {
        return request->baseline == SCHED_BASELINE_TRACE_ONLY;
    }
    for (uint64_t index = 0; index < request->manifest_entry_count; index++) {
        const struct sched_observe_manifest *entry = &manifest[index];
        if (entry->ordinal != index + 1) {
            return 0;
        }
        if (entry->input_class > INPUT_NO_DISPLAY || entry->source > SCHED_INPUT_IRQ) {
            return 0;
        }
        if (!entry->allowed_type_mask) {
            return 0;
        }
        /* A reducer id and its version are given together or not at all. */
        if (!!entry->reducer_id != !!entry->reducer_version) {
            return 0;
        }
    }
    return 1;
}

/* The request fields BEGIN refuses before touching anything.  Returns 0 if the
 * request is acceptable, else the error. */
static long begin_request_error(const struct sched_observe_begin *request)
{
    if (!request->header.run_id || request->flags) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (!request->input_capacity || request->input_capacity > SCHED_OBSERVE_MAX_CAPACITY) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (!request->group_capacity || request->group_capacity > SCHED_OBSERVE_MAX_CAPACITY) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (request->manifest_entry_count > request->input_capacity) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (request->baseline > SCHED_BASELINE_UNIFIED_WAITING) {
        return SCHED_OBSERVE_EINVAL;
    }
    return 0;
}

/* Start a run.  Lengths, pointers and allocation failures are checked before
 * anything is published, so a failed BEGIN leaves no trace.  The control lock is
 * held only to claim and to publish; allocation happens outside it. */
static long begin_run(const struct sched_observe_begin *request, uint32_t user_id,
                      uint32_t process_id)
{
    if (user_id) {
        return SCHED_OBSERVE_EPERM;
    }
    long request_error = begin_request_error(request);
    if (request_error) {
        return request_error;
    }
    struct sched_observe_manifest *manifest = NULL;
    if (request->manifest_entry_count) {
        manifest = observe_allocate_zeroed(request->manifest_entry_count, sizeof *manifest);
        if (!manifest) {
            return SCHED_OBSERVE_ENOMEM;
        }
        if (observe_copy_from_caller(manifest, request->manifest_address,
                                     request->manifest_entry_count * sizeof *manifest)) {
            observe_free(manifest);
            return SCHED_OBSERVE_EFAULT;
        }
    }
    if (!manifest_is_valid(request, manifest)) {
        observe_free(manifest);
        return SCHED_OBSERVE_EINVAL;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    int idle = g_control_initialized && g_state == SCHED_OBSERVE_IDLE;
    int run_id_is_new = request->header.run_id > g_last_run_id;
    if (!idle || g_begin_in_progress || !run_id_is_new) {
        observe_lock_release(&g_control_lock, saved_flags);
        observe_free(manifest);
        return SCHED_OBSERVE_EBUSY;
    }
    g_begin_in_progress = 1;
    observe_lock_release(&g_control_lock, saved_flags);

    uint32_t producers = observe_online_cpu_mask();
    long result = flight_recorder_allocate(producers);
    if (!result) {
        result = input_ledger_allocate_pool(request->input_capacity);
    }
    if (!result) {
        result = coalesced_groups_allocate_pool(request->group_capacity);
    }
    if (result) {
        flight_recorder_free();
        input_ledger_free_pool();
        coalesced_groups_free_pool();
        observe_free(manifest);
    }
    saved_flags = observe_lock_acquire(&g_control_lock);
    g_begin_in_progress = 0;
    if (!result) {
        g_manifest = manifest;
        g_manifest_entry_count = request->manifest_entry_count;
        for (unsigned word = 0; word < SCHED_OBSERVE_MANIFEST_HASH_WORDS; word++) {
            g_manifest_hash[word] = request->manifest_hash[word];
        }
        g_owner_user_id = user_id;
        g_owner_process_id = process_id;
        g_baseline = request->baseline;
        g_producer_cpu_mask = producers;
        g_exported_stream_mask = 0;
        g_trace_exported_cpu_mask = 0;
        g_active_reads = 0;
        __atomic_store_n(&g_lost_record_count, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_run_invalid, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_run_id, request->header.run_id, __ATOMIC_SEQ_CST);
        g_last_run_id = request->header.run_id;
        flight_recorder_start();
        set_run_state(SCHED_OBSERVE_ACTIVE);
        __atomic_store_n(&g_admitting_inputs, 1, __ATOMIC_SEQ_CST);
    }
    observe_lock_release(&g_control_lock, saved_flags);
    return result;
}

/* ---- STOP: shutdown state machine --------------------------------------- */

/* All exports are confirmed, nobody is reading, and every CPU sealed its ring:
 * the run's memory can go.  Called with the control lock held. */
static int exports_complete_and_unused(void)
{
    if (!no_read_blocks_reclaim()) {
        return 0;
    }
    uint32_t exported_streams = g_exported_stream_mask & NON_TRACE_STREAMS_EXPORTED_MASK;
    if (exported_streams != NON_TRACE_STREAMS_EXPORTED_MASK) {
        return 0;
    }
    return (g_trace_exported_cpu_mask & g_producer_cpu_mask) == g_producer_cpu_mask;
}

/* STOP's first phase is over: no input still waits for an outcome and no group
 * consumer remains.  Called with the control lock held. */
static int drain_finished(void)
{
    if (input_ledger_pending_count()) {
        return 0;
    }
    return coalesced_groups_consumer_count() == 0;
}

/* Advance a stopping run as far as it can go right now, without waiting.
 * DRAINING -> CLOSING once no input is pending and no group consumer remains;
 * CLOSING -> STOPPED once every producer CPU sealed its ring; STOPPED -> IDLE
 * once every export is confirmed and every reader is gone.  Called from places
 * that already run regularly, so a missing acknowledgement keeps all memory. */
void observe_advance_shutdown(void)
{
    /* Called on every scheduler entry and interrupt exit, so the common case
     * (no run, or a run that is still active) must not take a lock shared by
     * all CPUs.  A stale answer only delays the shutdown by one call. */
    uint32_t current_state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    if (current_state == SCHED_OBSERVE_IDLE || current_state == SCHED_OBSERVE_ACTIVE) {
        return;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    if (g_state == SCHED_OBSERVE_DRAINING && drain_finished()) {
        uint64_t manifest_entries = g_manifest_entry_count;
        if (manifest_entries && input_ledger_issued_count() != manifest_entries) {
            /* The run saw a different number of inputs than it preregistered. */
            observe_mark_invalid();
        }
        negctl_free_group_archive_early();
        set_run_state(SCHED_OBSERVE_CLOSING);
        flight_recorder_request_close();
    }
    if (g_state == SCHED_OBSERVE_CLOSING) {
        /* Recording one event makes this CPU seal its own ring.  It cannot seal
         * a peer's; those CPUs do the same when they next reach a hook. */
        flight_record_event(FLIGHT_EVENT_KERNEL_SECTION, 0, 0, 0, 0, 0);
        uint32_t acknowledged = flight_recorder_acknowledged_cpus() & g_producer_cpu_mask;
        if (acknowledged == g_producer_cpu_mask) {
            set_run_state(SCHED_OBSERVE_STOPPED);
        }
    }
    if (g_state == SCHED_OBSERVE_STOPPED && exports_complete_and_unused()) {
        coalesced_groups_drop_archive_references();
        coalesced_groups_release_queued();
        if (!coalesced_groups_live_count()) {
            coalesced_groups_free_pool();
            input_ledger_free_pool();
            flight_recorder_free();
            observe_free(g_manifest);
            g_manifest = NULL;
            g_manifest_entry_count = 0;
            set_run_state(SCHED_OBSERVE_IDLE);
        }
    }
    observe_lock_release(&g_control_lock, saved_flags);
}

/* ---- STATUS ------------------------------------------------------------- */

/* Copy all counters under the control lock, then let the caller format or draw
 * them with no lock held.  No module is called back while another module's lock
 * is held. */
void observe_snapshot_status(struct sched_observe_status *out)
{
    *out = (struct sched_observe_status){0};
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    out->header = (struct sched_observe_header){sizeof *out, SCHED_OBSERVE_VERSION, g_run_id};
    out->state = g_state;
    out->run_invalid = __atomic_load_n(&g_run_invalid, __ATOMIC_SEQ_CST);
    out->observation_lost_count = __atomic_load_n(&g_lost_record_count, __ATOMIC_SEQ_CST);
    out->essential_lost_count = out->observation_lost_count;
    out->producer_cpu_mask = g_producer_cpu_mask;
    out->active_read_count = g_active_reads;
    out->baseline = g_baseline;
    if (g_state != SCHED_OBSERVE_IDLE) {
        out->acknowledged_cpu_mask = flight_recorder_acknowledged_cpus();
        input_ledger_fill_status(out);
        flight_recorder_fill_status(out);
        out->groups_live_count = coalesced_groups_live_count();
        out->group_consumer_count = coalesced_groups_consumer_count();
        for (unsigned cpu = 0; cpu < SCHED_OBSERVE_MAX_CPUS; cpu++) {
            out->final_sequence[cpu] = flight_recorder_final_sequence(cpu);
        }
    }
    out->irq_disabled_coverage_partial = 1;
    observe_lock_release(&g_control_lock, saved_flags);
}

/* ---- READ --------------------------------------------------------------- */

/* The last record of a stream, which is the end of an export.  The caller holds
 * a reference as a reader, so the run cannot be freed meanwhile. */
static uint64_t final_sequence_of_stream(uint32_t stream, uint32_t cpu)
{
    if (stream == SCHED_STREAM_TRACE) {
        return flight_recorder_final_sequence(cpu);
    }
    if (stream == SCHED_STREAM_INPUT) {
        return input_ledger_first_input_id() - 1 + input_ledger_issued_count();
    }
    if (stream == SCHED_STREAM_GROUPS) {
        return coalesced_groups_published_count();
    }
    return METADATA_STREAM_LAST_RECORD;
}

/* Bytes of one record of a stream.  The metadata stream is text of fixed
 * maximum size. */
static size_t record_size_of_stream(uint32_t stream)
{
    if (stream == SCHED_STREAM_TRACE) {
        return sizeof(struct flight_event);
    }
    if (stream == SCHED_STREAM_INPUT) {
        return sizeof(struct input_record);
    }
    if (stream == SCHED_STREAM_GROUPS) {
        return sizeof(struct sched_observe_group_record);
    }
    return SCHED_OBSERVE_METADATA_RECORD_BYTES;
}

/* Write the single METADATA record: the run's identity and every registered
 * provider's description.  Providers format outside the control lock; the READ
 * reference taken by the caller keeps the run alive.  Returns the length, or a
 * negative error if a provider's text does not fit. */
static int format_metadata_record(char *out, int capacity)
{
    int used = ksnprintf(out, capacity,
        "run=%lu manifest=%016lx:%016lx:%016lx:%016lx entries=%lu "
        "coverage=partial tsc_offset=unknown uncertainty=unknown "
        "not verified on X79-VG4 hardware; not verified on i7-14700KF hardware\n",
        (unsigned long)g_run_id, (unsigned long)g_manifest_hash[0],
        (unsigned long)g_manifest_hash[1], (unsigned long)g_manifest_hash[2],
        (unsigned long)g_manifest_hash[3], (unsigned long)g_manifest_entry_count);
    for (unsigned kind = SCHED_PROVIDER_FRAME_METADATA;
         kind < SCHED_PROVIDER_COUNT && used < capacity - 1; kind++) {
        if (!g_providers[kind].metadata) {
            continue;
        }
        int added = g_providers[kind].metadata(out + used, capacity - used);
        if (added < 0 || added >= capacity - used - 1) {
            observe_mark_invalid();
            return SCHED_OBSERVE_EOVERFLOW;
        }
        used += added;
    }
    return used;
}

/* Does the READ request ask for something well-formed?  Returns 0 or the error. */
static long read_request_error(const struct sched_observe_read *request)
{
    if (request->stream < SCHED_STREAM_TRACE || request->stream > SCHED_STREAM_METADATA) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (request->cpu >= SCHED_OBSERVE_MAX_CPUS) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (request->record_capacity > SCHED_OBSERVE_MAX_READ_RECORDS || request->reserved) {
        return SCHED_OBSERVE_EINVAL;
    }
    if (request->flags & ~SCHED_READ_CONFIRM_EXPORT) {
        return SCHED_OBSERVE_EINVAL;
    }
    return 0;
}

/* Copy up to record_capacity records of one stream starting at the request's
 * cursor, into a private buffer, and then to the caller's memory.  The reader
 * pins the run under the control lock before touching any pool, and every
 * request carries its own cursor, so readers are independent.  Copying out
 * happens last, with no lock held, so a bad user pointer cannot stall anyone. */
static long read_stream_records(struct sched_observe_read *request, uint64_t request_address,
                                uint32_t user_id, uint32_t process_id)
{
    long request_error = read_request_error(request);
    if (request_error) {
        return request_error;
    }
    size_t record_size = record_size_of_stream(request->stream);
    size_t buffer_bytes = request->record_capacity * record_size;
    int buffer_writable = !request->record_capacity ||
                          observe_caller_range_writable(request->user_buffer_address, buffer_bytes);
    if (!buffer_writable || !observe_caller_range_writable(request_address, sizeof *request)) {
        return SCHED_OBSERVE_EFAULT;
    }
    void *buffer = buffer_bytes ? observe_allocate_zeroed(1, buffer_bytes) : NULL;
    if (buffer_bytes && !buffer) {
        return SCHED_OBSERVE_ENOMEM;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    long result = 0;
    if (g_state == SCHED_OBSERVE_IDLE || request->header.run_id != g_run_id) {
        result = SCHED_OBSERVE_EINVAL;
    } else if (user_id && (user_id != g_owner_user_id || process_id != g_owner_process_id)) {
        result = SCHED_OBSERVE_EPERM;
    } else if (request->stream == SCHED_STREAM_TRACE &&
               !(g_producer_cpu_mask & (1u << request->cpu))) {
        result = SCHED_OBSERVE_EINVAL;
    }
    if (result) {
        observe_lock_release(&g_control_lock, saved_flags);
        observe_free(buffer);
        return result;
    }
    g_active_reads++;
    int run_stopped = g_state == SCHED_OBSERVE_STOPPED;
    observe_lock_release(&g_control_lock, saved_flags);

    uint32_t requested_flags = request->flags;
    request->flags = 0;
    request->records_copied = 0;
    request->gap_first_sequence = 0;
    request->gap_last_sequence = 0;
    request->next_cursor = request->cursor ? request->cursor : STREAM_FIRST_CURSOR;
    if (!request->cursor && request->stream == SCHED_STREAM_INPUT) {
        request->next_cursor = input_ledger_first_input_id();
    }
    request->next_cursor = trace_start_cursor(request->stream, request->next_cursor);
    request->final_sequence = 0;
    if (run_stopped) {
        request->final_sequence = final_sequence_of_stream(request->stream, request->cpu);
    }

    for (uint32_t index = 0; index < request->record_capacity; index++) {
        uint64_t cursor = request->next_cursor;
        char *destination = (char *)buffer + request->records_copied * record_size;
        int copied;
        if (request->stream == SCHED_STREAM_TRACE) {
            request->cursor = cursor;
            copied = flight_recorder_read(request, (struct flight_event *)destination);
        } else if (request->stream == SCHED_STREAM_INPUT) {
            copied = input_ledger_copy_record(cursor, (struct input_record *)destination);
        } else if (request->stream == SCHED_STREAM_GROUPS) {
            copied = coalesced_groups_read_archive_row(
                cursor, (struct sched_observe_group_record *)destination);
        } else {
            copied = cursor == STREAM_FIRST_CURSOR ?
                     format_metadata_record(destination, record_size) : 0;
            if (copied > 0) {
                copied = 1;
            }
        }
        if (copied <= 0) {
            if (copied < 0) {
                result = copied;
            }
            break;
        }
        request->records_copied++;
        request->next_cursor = cursor_after_record(cursor);
    }
    if (run_stopped && request->next_cursor == request->final_sequence + 1) {
        request->flags |= SCHED_READ_FINAL;
    }
    negctl_remember_trace_cursor(request->stream, request->next_cursor);
    if (!result && request->records_copied) {
        size_t copied_bytes = request->records_copied * record_size;
        if (observe_copy_to_caller(request->user_buffer_address, buffer, copied_bytes)) {
            result = SCHED_OBSERVE_EFAULT;
        }
    }
    if (!result && observe_copy_to_caller(request_address, request, sizeof *request)) {
        result = SCHED_OBSERVE_EFAULT;
    }
    saved_flags = observe_lock_acquire(&g_control_lock);
    /* A confirmation is a separate, empty READ whose cursor is one past the last
     * record.  Only then is the stream counted as exported, because fetching
     * the final page does not prove the copy to the reader succeeded. */
    int asked_for_confirmation = requested_flags == SCHED_READ_CONFIRM_EXPORT;
    int cursor_is_past_end = request->cursor == request->final_sequence + 1;
    int is_confirmation = asked_for_confirmation && !request->records_copied && cursor_is_past_end;
    if (!result && run_stopped && is_confirmation) {
        if (request->stream == SCHED_STREAM_TRACE) {
            g_trace_exported_cpu_mask |= 1u << request->cpu;
        } else {
            g_exported_stream_mask |= STREAM_EXPORT_BIT(request->stream);
        }
    }
    g_active_reads--;
    observe_lock_release(&g_control_lock, saved_flags);
    observe_free(buffer);
    return result;
}

/* ---- the system call ---------------------------------------------------- */

/* Size of the request struct a command takes.  STOP, PROBE and MODEL take only
 * the common header. */
static size_t request_size_for(long command)
{
    if (command == SCHED_OBSERVE_BEGIN) {
        return sizeof(struct sched_observe_begin);
    }
    if (command == SCHED_OBSERVE_READ) {
        return sizeof(struct sched_observe_read);
    }
    if (command == SCHED_OBSERVE_STATUS) {
        return sizeof(struct sched_observe_status);
    }
    return sizeof(struct sched_observe_header);
}

/* STOP: close admission, then move the shutdown as far as it goes.  Root only.
 * Returns 0 once the run is STOPPED, or 1 while it is still draining (call
 * again).  Never waits. */
static long stop_run(const struct sched_observe_header *header, uint32_t user_id)
{
    if (user_id) {
        return SCHED_OBSERVE_EPERM;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    if (g_state == SCHED_OBSERVE_IDLE || header->run_id != g_run_id) {
        observe_lock_release(&g_control_lock, saved_flags);
        return SCHED_OBSERVE_EINVAL;
    }
    __atomic_store_n(&g_admitting_inputs, 0, __ATOMIC_SEQ_CST);
    if (g_state == SCHED_OBSERVE_ACTIVE) {
        set_run_state(SCHED_OBSERVE_DRAINING);
    }
    observe_lock_release(&g_control_lock, saved_flags);
    observe_advance_shutdown();
    saved_flags = observe_lock_acquire(&g_control_lock);
    long result = g_state == SCHED_OBSERVE_STOPPED ? 0 : 1;
    observe_lock_release(&g_control_lock, saved_flags);
    return result;
}

/* STATUS: copy the counters to the caller.  Allowed for root and for the run's
 * owner (same user and process). */
static long status_run(long user_request, size_t size, uint32_t user_id, uint32_t process_id)
{
    if (!observe_caller_range_writable(user_request, size)) {
        return SCHED_OBSERVE_EFAULT;
    }
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    int allowed = !user_id || (user_id == g_owner_user_id && process_id == g_owner_process_id);
    observe_lock_release(&g_control_lock, saved_flags);
    if (!allowed) {
        return SCHED_OBSERVE_EPERM;
    }
    struct sched_observe_status status;
    observe_snapshot_status(&status);
    return observe_copy_to_caller(user_request, &status, size) ? SCHED_OBSERVE_EFAULT : 0;
}

/* PROBE and MODEL belong to later stages.  Hand the request to the registered
 * provider, or report that nobody implements it.  MODEL requests go to the
 * capacity provider for now; stages S1a to S1c extend this dispatch with
 * sub-commands for topology, capacity and calibration. */
static long provider_command(long command, long user_request, long bytes)
{
    unsigned kind = command == SCHED_OBSERVE_PROBE ? SCHED_PROVIDER_PROBE : SCHED_PROVIDER_CAPACITY;
    uint64_t saved_flags = observe_lock_acquire(&g_control_lock);
    long (*provider)(long, long, long) = g_providers[kind].command;
    observe_lock_release(&g_control_lock, saved_flags);
    return provider ? provider(command, user_request, bytes) : SCHED_OBSERVE_ENOSYS;
}

/* Entry point of SYS_SCHED_OBSERVE.  The fixed size and version of the request
 * are checked before anything is dispatched.  Only this one diagnostic call
 * exists; number 225 (GUI receipt) continues through the window manager's
 * existing default handling. */
long observe_syscall(long command, long user_request, long bytes)
{
    size_t expected = request_size_for(command);
    if (bytes < 0 || (uint64_t)bytes != expected) {
        return SCHED_OBSERVE_EINVAL;
    }
    union {
        struct sched_observe_begin begin;
        struct sched_observe_read read;
        struct sched_observe_status status;
        struct sched_observe_header header;
    } request;
    if (observe_copy_from_caller(&request, user_request, expected)) {
        return SCHED_OBSERVE_EFAULT;
    }
    if (request.header.size != expected || request.header.version != SCHED_OBSERVE_VERSION) {
        return SCHED_OBSERVE_EINVAL;
    }
    uint32_t user_id;
    uint32_t process_id;
    observe_caller_identity(&user_id, &process_id);
    switch (command) {
    case SCHED_OBSERVE_BEGIN:
        return begin_run(&request.begin, user_id, process_id);
    case SCHED_OBSERVE_READ:
        return read_stream_records(&request.read, user_request, user_id, process_id);
    case SCHED_OBSERVE_PROBE:
    case SCHED_OBSERVE_MODEL:
        return provider_command(command, user_request, bytes);
    case SCHED_OBSERVE_STATUS:
        return status_run(user_request, expected, user_id, process_id);
    case SCHED_OBSERVE_STOP:
        return stop_run(&request.header, user_id);
    default:
        return SCHED_OBSERVE_ENOSYS;
    }
}

#ifdef SCHED_OBSERVE_GUEST
/* Test image only: the built-in diagnostic program (a kernel thread that drives
 * the real system call and checks the results).  It is part of the test tree,
 * not of the product. */
#include "../../../../tests/sched/observe/guest.c"
#endif
