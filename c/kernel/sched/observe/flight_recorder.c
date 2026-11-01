/* Flight recorder: a fixed-size ring of scheduler events per CPU, used to
 * measure the scheduler without changing it.
 *
 * HOW A RING STAYS CONSISTENT WITHOUT LOCKS
 *   - Each CPU is the only writer of its own ring, and it writes with
 *     interrupts off, so no interrupt handler can start a second write in the
 *     middle of the first.  The hot path takes no lock.
 *   - A reader (the READ system call) may run on another CPU at the same time
 *     as the writer.  Every word that both sides can touch is therefore read
 *     and written with a sequentially consistent 64-bit atomic.  Plain loads or
 *     memcpy on a slot would be a data race in C even if it happened to work.
 *   - Each slot has a version word.  The writer sets it to an odd number
 *     (2 * sequence - 1) before touching the payload and to the even number
 *     (2 * sequence) when the payload is complete.  A reader accepts a slot
 *     only if the version is the expected even number both before and after it
 *     copied the payload.  A copy that overlapped a write is thrown away and
 *     reported as a gap or a retry; it is never handed to the caller.
 *   - NMI and machine-check handlers can interrupt a write at any instruction,
 *     so they never write the ring.  They are only counted ("uncovered").
 *
 * The recorder is never allowed to disturb the thing it measures: it takes no
 * lock that the measured code can take, allocates nothing, and prints nothing.
 *
 * Design reference: section 10.4 (and appendix F11) of
 * docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md.
 * Evidence for this file comes from host tests and QEMU only; it is not
 * verified on X79-VG4 hardware and not verified on i7-14700KF hardware. */
#include "flight_recorder.h"
#include "observe_control.h"
#include "../../../drivers/core/irq.h"

/* Slots per CPU ring.  The host test uses a tiny ring so that wrapping happens
 * after a handful of events.  On the real kernel the size comes from
 * measurement: an injected 200-input QEMU run reached sequence 4859 on CPU 0
 * before the tail was exported, so a 1024-slot ring lost [1, 3835]; another
 * replay reached 10315 and overflowed 8192 as well.  65536 slots (4.5 MiB per
 * ring, allocated only up to the highest online CPU) is still a bound, not a
 * guarantee against unlimited GUI traffic: a later gap invalidates the run. */
#ifdef SCHED_OBSERVE_HOST
#define FLIGHT_RING_SLOTS 8u
#else
#define FLIGHT_RING_SLOTS 65536u
#endif

/* How many times a reader retries one slot before giving up on it.  A busy
 * writer can keep a slot changing; four attempts bound the reader's work, and
 * running out marks the run invalid instead of returning a doubtful record. */
#define FLIGHT_READ_ATTEMPTS 4u

/* Deepest interrupt nesting the recorder tracks per CPU. */
#define FLIGHT_MAX_INTERRUPT_NESTING 16u

/* Largest sequence number a ring may reach.  The slot version is twice the
 * sequence, so stopping at half of 2^64 keeps it from wrapping into a value
 * that looks like an old record.  Reaching it invalidates the run. */
#define FLIGHT_SEQUENCE_LIMIT (UINT64_MAX / 2)

/* The payload of one slot is eight 64-bit words.  These are their meanings. */
enum slot_word {
    SLOT_WORD_SEQUENCE,             /* written last, so an old value marks a half-written slot */
    SLOT_WORD_TIMESTAMP_TSC,
    SLOT_WORD_MONOTONIC_NS,
    SLOT_WORD_THREAD_IDS,           /* thread_id in the low half, peer_thread_id in the high half */
    SLOT_WORD_CPU_KIND_REASON,      /* cpu in bits 0-15, kind in 16-31, reason in 32-63 */
    SLOT_WORD_FIRST_DETAIL,
    SLOT_WORD_SECOND_DETAIL,
    SLOT_WORD_IS_ESSENTIAL,
    SLOT_WORD_COUNT
};

/* The writer pauses here in the host test: about half of the words are in. */
#define SLOT_WORD_HALFWAY SLOT_WORD_CPU_KIND_REASON

/* Bit 0 of a slot version: set while the writer is filling the slot. */
#define SLOT_VERSION_BUSY_BIT 1u

#define SLOT_KIND_SHIFT 16
#define SLOT_REASON_SHIFT 32
#define SLOT_PEER_THREAD_SHIFT 32

struct ring_slot {
    uint64_t version;
    uint64_t payload_words[SLOT_WORD_COUNT];
};

struct cpu_ring {
    struct ring_slot slots[FLIGHT_RING_SLOTS];
    uint64_t last_started_sequence;     /* highest sequence whose write has begun */
    uint64_t published_sequence;        /* highest sequence whose write is complete */
    uint64_t overwritten_count;         /* events that replaced an older, unread one */
    uint64_t final_sequence;            /* published_sequence at the moment of sealing */
    uint64_t acknowledged;              /* 1 once this CPU sealed the ring */
};

/* Where, when and by which vector one interrupt entered this CPU. */
struct interrupt_origin {
    uint64_t arrival_tsc;
    uint64_t arrival_ns;
    uint32_t vector;
};

/* Mutable per-CPU state, touched only by the owning CPU with interrupts off,
 * except for the few fields that status snapshots read atomically. */
struct cpu_state {
    uint64_t session_epoch;             /* run this state was last reset for */
    struct interrupt_origin interrupt_stack[FLIGHT_MAX_INTERRUPT_NESTING];
    unsigned interrupt_depth;
    unsigned interrupt_overflow;        /* entries beyond the tracked depth */
    unsigned irq_disabled_depth;
    uint64_t irq_disabled_start_tsc;
    uint64_t irq_disabled_start_observer_tsc;   /* observer_cost_tsc when the interval began */
    uint64_t observer_cost_tsc;         /* cycles spent inside the recorder on this CPU */
    uint64_t irq_disabled_max_tsc;      /* longest interval, with the recorder's own cost removed */
    uintptr_t irq_disabled_caller;
    uint64_t switch_sequence;           /* sequence of the last SWITCH event written here */
    uint64_t last_published_sequence;
    uint32_t outgoing_thread_id;
    uint32_t incoming_thread_id;
    uint32_t switch_reason;
};

static struct cpu_ring *g_cpu_rings;
static struct cpu_state g_cpu_state[SCHED_OBSERVE_MAX_CPUS];
static uint32_t g_producer_cpu_mask;
static uint64_t g_session_running;
static uint64_t g_close_requested;
static uint64_t g_enabled_cpu_mask;
static uint64_t g_session_epoch;
static uint64_t g_console_byte_count;
static uint64_t g_uart_byte_count;
static uint64_t g_uncovered_interrupts;

/* The version a slot shows while record 'sequence' is being written (odd)... */
static uint64_t version_being_written(uint64_t sequence)
{
    return 2 * sequence - 1;
}

/* ...and once that record is complete (even).  Both encodings are what readers
 * compare against, so they are defined in one place. */
static uint64_t version_complete(uint64_t sequence)
{
    return 2 * sequence;
}

/* ---- hooks flipped by the negative-control tests ------------------------
 * Each hook below behaves as production code unless the named macro is
 * defined, and the macro is defined only when the test named in its comment
 * builds the fault-injected variant. */

/* Interrupts must be masked while a slot is written.  Flipped by
 * SCHED_NEGCTL_TRACE_NO_IRQSAVE (host test: trace_nested_commit). */
static uint64_t commit_irq_save(void)
{
#ifdef SCHED_NEGCTL_TRACE_NO_IRQSAVE
    return 0;
#else
    return observe_irq_save();
#endif
}

/* The interrupt flag must go back as it was found.  Flipped by
 * SCHED_NEGCTL_TRACE_NO_IRQSAVE (trace_nested_commit) and by
 * SCHED_NEGCTL_TRACE_FORCE_STI, which wrongly re-enables interrupts
 * (host test: trace_restores_if). */
static void commit_irq_restore(uint64_t saved_flags)
{
#ifdef SCHED_NEGCTL_TRACE_NO_IRQSAVE
    (void)saved_flags;
#else
#ifdef SCHED_NEGCTL_TRACE_FORCE_STI
    saved_flags |= OBSERVE_EFLAGS_INTERRUPT_ENABLE;
#endif
    observe_irq_restore(saved_flags);
#endif
}

/* The paired image with recording switched off, used to measure what the
 * recorder costs.  Set by SCHED_OBSERVE_TRACE_OFF (an image build option, not
 * a test failure).  Run control and sealing still work in that image. */
static int recording_switched_off(void)
{
#ifdef SCHED_OBSERVE_TRACE_OFF
    return 1;
#else
    return 0;
#endif
}

/* A sequence at the limit must stop and invalidate the run.  Flipped by
 * SCHED_NEGCTL_TRACE_WRAP, which lets it wrap (host test: trace_sequence_no_wrap). */
static int sequence_exhausted(uint64_t sequence)
{
#ifdef SCHED_NEGCTL_TRACE_WRAP
    (void)sequence;
    return 0;
#else
    return sequence >= FLIGHT_SEQUENCE_LIMIT;
#endif
}

/* The odd version tells readers the slot is being written.  Flipped by
 * SCHED_NEGCTL_TRACE_NO_ODD, which skips it (host test: trace_snapshot_not_torn). */
static void mark_slot_being_written(struct ring_slot *slot, uint64_t sequence)
{
    uint64_t busy_version = version_being_written(sequence);
#ifndef SCHED_NEGCTL_TRACE_NO_ODD
    __atomic_store_n(&slot->version, busy_version, __ATOMIC_SEQ_CST);
#else
    (void)slot;
    (void)busy_version;
#endif
}

/* Load one payload word.  Flipped by SCHED_NEGCTL_TRACE_PLAIN_COPY, which reads
 * it without an atomic (checked by tests/sched/observe/atomic_audit.py, which
 * must report trace_atomic_access_only). */
static uint64_t load_payload_word(const struct ring_slot *slot, unsigned index)
{
#ifdef SCHED_NEGCTL_TRACE_PLAIN_COPY
    return slot->payload_words[index];
#else
    return __atomic_load_n(&slot->payload_words[index], __ATOMIC_SEQ_CST);
#endif
}

/* After copying a slot, the reader must see the same complete version again.
 * Flipped by SCHED_NEGCTL_TRACE_NO_RECHECK, which skips the second look
 * (host test: trace_snapshot_not_torn). */
static int version_unchanged_after_copy(const struct ring_slot *slot,
                                        uint64_t version_before, uint64_t sequence)
{
    uint64_t version_after = __atomic_load_n(&slot->version, __ATOMIC_SEQ_CST);
#ifdef SCHED_NEGCTL_TRACE_NO_RECHECK
    (void)version_after;
    (void)version_before;
    (void)sequence;
    return 1;
#else
    int unchanged = version_after == version_before;
    int complete_for_sequence = version_after == version_complete(sequence);
    int not_being_written = !(version_after & SLOT_VERSION_BUSY_BIT);
    return unchanged && complete_for_sequence && not_being_written;
#endif
}

/* The interrupts-off timing core must never record an event itself, because
 * recording runs with interrupts off and would measure its own work.  Flipped by
 * SCHED_NEGCTL_IRQOFF_RECURSE, which records one from inside the timing core
 * (host test: irqoff_observer_excluded). */
static void negctl_record_event_while_timing(void)
{
#ifdef SCHED_NEGCTL_IRQOFF_RECURSE
    flight_record_event(FLIGHT_EVENT_IRQ_DISABLED, 0, 0, 0, 0, 0);
#endif
}

/* ---- per-CPU state ------------------------------------------------------ */

/* The calling CPU's state, reset for the current run, or NULL when this CPU
 * must not record (no run, or this CPU already sealed its ring).  Only the
 * owning CPU initializes its state, with interrupts off.  A remote reset at
 * BEGIN could race with an old hook still finishing on that CPU; resetting
 * lazily when the run's epoch number changes avoids that.  Fields that status
 * snapshots read from other CPUs are always written atomically. */
static struct cpu_state *cpu_state_if_recording(unsigned cpu)
{
    if (cpu >= SCHED_OBSERVE_MAX_CPUS) {
        return NULL;
    }
    if (!__atomic_load_n(&g_session_running, __ATOMIC_SEQ_CST)) {
        return NULL;
    }
    uint64_t enabled_cpus = __atomic_load_n(&g_enabled_cpu_mask, __ATOMIC_SEQ_CST);
    if (!(enabled_cpus & (1ull << cpu))) {
        return NULL;
    }
    struct cpu_state *state = &g_cpu_state[cpu];
    uint64_t epoch = __atomic_load_n(&g_session_epoch, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&state->session_epoch, __ATOMIC_SEQ_CST) != epoch) {
        state->interrupt_depth = 0;
        state->interrupt_overflow = 0;
        state->irq_disabled_depth = 0;
        state->irq_disabled_start_tsc = 0;
        state->irq_disabled_start_observer_tsc = 0;
        state->irq_disabled_caller = 0;
        state->switch_sequence = 0;
        state->last_published_sequence = 0;
        state->outgoing_thread_id = 0;
        state->incoming_thread_id = 0;
        state->switch_reason = 0;
        __atomic_store_n(&state->observer_cost_tsc, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->irq_disabled_max_tsc, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&state->session_epoch, epoch, __ATOMIC_SEQ_CST);
    }
    return state;
}

/* True while this CPU is still part of the run that started at 'epoch'.  A
 * recording call that is in progress when the run closes may make a second
 * nested call; this stops that second call from silently joining a newly
 * started run.  It never touches ring memory, which may already be freed. */
static int still_in_session(unsigned cpu, uint64_t epoch)
{
    if (__atomic_load_n(&g_session_epoch, __ATOMIC_SEQ_CST) != epoch) {
        return 0;
    }
    uint64_t enabled_cpus = __atomic_load_n(&g_enabled_cpu_mask, __ATOMIC_SEQ_CST);
    return (enabled_cpus & (1ull << cpu)) != 0;
}

#ifdef SCHED_OBSERVE_HOST
/* Host test only: jump this CPU's ring to a chosen sequence, to exercise the
 * limit without executing 2^63 writes.  It does not replace any part of the
 * write or read algorithm. */
void flight_recorder_set_last_sequence_for_test(uint64_t sequence)
{
    __atomic_store_n(&g_cpu_rings[observe_current_cpu()].last_started_sequence,
                     sequence, __ATOMIC_SEQ_CST);
}
#endif

/* Bit position of the upper 32-bit value in a packed word. */
#define PACKED_HIGH_SHIFT 32

/* Pack two 32-bit values into one atomic word.  Only integers go into the
 * ring: an address stored there could outlive the object it points to. */
static uint64_t pack_two_u32(uint32_t low, uint32_t high)
{
    return (uint64_t)low | ((uint64_t)high << PACKED_HIGH_SHIFT);
}

/* If a close was requested, seal this CPU's ring: publish its final sequence,
 * stop this CPU from recording, then acknowledge.  Returns 1 if the ring is (or
 * just became) sealed.  The CPU seals itself, with interrupts off and after its
 * last possible write, so the acknowledgement really means "no more writes from
 * here".  No inter-processor interrupt is invented for this stage: a CPU that
 * never reaches an observation point simply stays unacknowledged. */
static int seal_ring_if_close_requested(unsigned cpu)
{
    if (!__atomic_load_n(&g_close_requested, __ATOMIC_SEQ_CST)) {
        return 0;
    }
    struct cpu_ring *ring = &g_cpu_rings[cpu];
    uint64_t final_sequence = __atomic_load_n(&ring->published_sequence, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ring->final_sequence, final_sequence, __ATOMIC_SEQ_CST);
    /* Stop this CPU recording before acknowledging.  Another CPU frees the ring
     * as soon as everyone acknowledged; an acknowledged CPU keeps taking
     * interrupts and must never look at the freed ring again.  Testing the
     * ring's own 'acknowledged' flag for that would itself touch freed memory,
     * so the gate is this static mask. */
    __atomic_fetch_and(&g_enabled_cpu_mask, ~(1ull << cpu), __ATOMIC_SEQ_CST);
    __atomic_store_n(&ring->acknowledged, 1, __ATOMIC_RELEASE);
    return 1;
}

/* ---- run life cycle ----------------------------------------------------- */

/* Reserve the rings before the run is published, so a failed allocation leaves
 * nothing half-started.  Only CPUs up to the highest producer get a ring. */
int flight_recorder_allocate(uint32_t producer_cpu_mask)
{
    unsigned ring_count = 0;
    for (unsigned cpu = 0; cpu < SCHED_OBSERVE_MAX_CPUS; cpu++) {
        if (producer_cpu_mask & (1u << cpu)) {
            ring_count = cpu + 1;
        }
    }
    g_cpu_rings = observe_allocate_zeroed(ring_count, sizeof *g_cpu_rings);
    if (!g_cpu_rings) {
        return SCHED_OBSERVE_ENOMEM;
    }
    g_producer_cpu_mask = producer_cpu_mask;
    return 0;
}

/* Begin recording.  Called last in BEGIN, after every pool and the manifest are
 * ready, never part-way through it. */
void flight_recorder_start(void)
{
    __atomic_store_n(&g_console_byte_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_uart_byte_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_uncovered_interrupts, 0, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_session_epoch, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_close_requested, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_enabled_cpu_mask, g_producer_cpu_mask, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_session_running, 1, __ATOMIC_SEQ_CST);
}

/* Ask every CPU to seal its ring.  This only requests; acknowledgements come
 * from the CPUs themselves and are never forged on their behalf. */
void flight_recorder_request_close(void)
{
    __atomic_store_n(&g_close_requested, 1, __ATOMIC_SEQ_CST);
}

/* Free the rings.  The run controller calls this only after every CPU
 * acknowledged, the tail was exported and no reader is active. */
void flight_recorder_free(void)
{
    __atomic_store_n(&g_session_running, 0, __ATOMIC_SEQ_CST);
    observe_free(g_cpu_rings);
    g_cpu_rings = NULL;
}

/* ---- recording ---------------------------------------------------------- */

/* Record one event.  The check that this CPU may record and the write itself
 * sit in one interrupt-off section; restoring the caller's original interrupt
 * flag (rather than re-enabling) keeps a caller that already had interrupts off
 * from becoming preemptible just because tracing is on
 * (Intel SDM 325384-093US, Vol. 2, CLI / STI / POPF). */
void flight_record_event(uint16_t kind, uint32_t thread_id, uint32_t peer_thread_id,
                         uint32_t reason, uint64_t first_detail, uint64_t second_detail)
{
    if (!__atomic_load_n(&g_session_running, __ATOMIC_SEQ_CST)) {
        return;
    }
    uint64_t saved_flags = commit_irq_save();
    unsigned cpu = observe_current_cpu();
    struct cpu_state *state = cpu_state_if_recording(cpu);
    if (!state) {
        commit_irq_restore(saved_flags);
        return;
    }
    if (seal_ring_if_close_requested(cpu)) {
        commit_irq_restore(saved_flags);
        return;
    }
    uint64_t start_tsc = observe_read_tsc();
    if (recording_switched_off()) {
        commit_irq_restore(saved_flags);
        return;
    }
    struct cpu_ring *ring = &g_cpu_rings[cpu];
    uint64_t sequence = __atomic_load_n(&ring->last_started_sequence, __ATOMIC_SEQ_CST) + 1;
    if (sequence_exhausted(sequence)) {
        observe_mark_invalid();
        commit_irq_restore(saved_flags);
        return;
    }
    struct ring_slot *slot = &ring->slots[(sequence - 1) % FLIGHT_RING_SLOTS];
    __atomic_store_n(&ring->last_started_sequence, sequence, __ATOMIC_SEQ_CST);
    mark_slot_being_written(slot, sequence);
    OBSERVE_TEST_POINT(OBSERVE_POINT_WRITER_SLOT_MARKED_BUSY);

    uint64_t words[SLOT_WORD_COUNT];
    words[SLOT_WORD_TIMESTAMP_TSC] = start_tsc;
    words[SLOT_WORD_MONOTONIC_NS] = observe_monotonic_ns();
    words[SLOT_WORD_THREAD_IDS] = pack_two_u32(thread_id, peer_thread_id);
    words[SLOT_WORD_CPU_KIND_REASON] =
        pack_two_u32(cpu | ((uint32_t)kind << SLOT_KIND_SHIFT), reason);
    words[SLOT_WORD_FIRST_DETAIL] = first_detail;
    words[SLOT_WORD_SECOND_DETAIL] = second_detail;
    words[SLOT_WORD_IS_ESSENTIAL] = kind == FLIGHT_EVENT_INPUT || kind == FLIGHT_EVENT_OUTCOME;
    for (unsigned index = SLOT_WORD_TIMESTAMP_TSC; index < SLOT_WORD_COUNT; index++) {
        __atomic_store_n(&slot->payload_words[index], words[index], __ATOMIC_SEQ_CST);
        if (index == SLOT_WORD_HALFWAY) {
            OBSERVE_TEST_POINT(OBSERVE_POINT_WRITER_HALF_WRITTEN);
        }
    }
    /* The sequence word goes in last on purpose: while the version is odd, a
     * slot may hold the old sequence next to new payload words, and the tests
     * pause exactly here so that the sequence alone cannot hide torn data. */
    __atomic_store_n(&slot->payload_words[SLOT_WORD_SEQUENCE], sequence, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slot->version, version_complete(sequence), __ATOMIC_SEQ_CST);
    __atomic_store_n(&ring->published_sequence, sequence, __ATOMIC_SEQ_CST);
    state->last_published_sequence = sequence;
    if (sequence > FLIGHT_RING_SLOTS) {
        __atomic_fetch_add(&ring->overwritten_count, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_fetch_add(&state->observer_cost_tsc, observe_read_tsc() - start_tsc,
                       __ATOMIC_SEQ_CST);
    commit_irq_restore(saved_flags);
}

/* ---- reading ------------------------------------------------------------ */

/* Copy the event with the cursor's sequence into *out, if it is still in the
 * ring and was not being written.  Returns 1 if copied, 0 if not (see the
 * request flags: gap means the events were overwritten, retry means the slot
 * kept changing), or a negative error.  Only a version-stable private copy is
 * ever returned.  Four attempts bound the work under a busy writer; ambiguity
 * after that invalidates the run. */
int flight_recorder_read(struct sched_observe_read *request, struct flight_event *out)
{
    if (request->cpu >= SCHED_OBSERVE_MAX_CPUS || !(g_producer_cpu_mask & (1u << request->cpu))) {
        return SCHED_OBSERVE_EINVAL;
    }
    struct cpu_ring *ring = &g_cpu_rings[request->cpu];
    uint64_t sequence = request->cursor ? request->cursor : 1;
    request->next_cursor = sequence;
    for (unsigned attempt = 0; attempt < FLIGHT_READ_ATTEMPTS; attempt++) {
        uint64_t published = __atomic_load_n(&ring->published_sequence, __ATOMIC_SEQ_CST);
        uint64_t oldest_kept = 1;
        if (published >= FLIGHT_RING_SLOTS) {
            oldest_kept = published - FLIGHT_RING_SLOTS + 1;
        }
        if (sequence < oldest_kept) {
            request->gap_first_sequence = sequence;
            request->gap_last_sequence = oldest_kept - 1;
            request->next_cursor = oldest_kept;
            request->flags |= SCHED_READ_GAP;
            observe_mark_invalid();
            return 0;
        }
        if (sequence > published) {
            return 0;
        }
        struct ring_slot *slot = &ring->slots[(sequence - 1) % FLIGHT_RING_SLOTS];
        uint64_t version_before = __atomic_load_n(&slot->version, __ATOMIC_SEQ_CST);
        if (version_before != version_complete(sequence)) {
            continue;
        }
        OBSERVE_TEST_POINT(OBSERVE_POINT_READER_VERSION_CHECKED);
        uint64_t words[SLOT_WORD_COUNT];
        for (unsigned index = 0; index < SLOT_WORD_COUNT; index++) {
            words[index] = load_payload_word(slot, index);
        }
        OBSERVE_TEST_POINT(OBSERVE_POINT_READER_PAYLOAD_COPIED);
        if (!version_unchanged_after_copy(slot, version_before, sequence)) {
            continue;
        }
        uint64_t cpu_kind_reason = words[SLOT_WORD_CPU_KIND_REASON];
        if (words[SLOT_WORD_SEQUENCE] != sequence || (uint16_t)cpu_kind_reason != request->cpu) {
            continue;
        }
        *out = (struct flight_event){
            .sequence = words[SLOT_WORD_SEQUENCE],
            .timestamp_tsc = words[SLOT_WORD_TIMESTAMP_TSC],
            .monotonic_ns = words[SLOT_WORD_MONOTONIC_NS],
            .thread_id = (uint32_t)words[SLOT_WORD_THREAD_IDS],
            .peer_thread_id = (uint32_t)(words[SLOT_WORD_THREAD_IDS] >> SLOT_PEER_THREAD_SHIFT),
            .cpu = (uint16_t)cpu_kind_reason,
            .kind = (uint16_t)(cpu_kind_reason >> SLOT_KIND_SHIFT),
            .reason = (uint32_t)(cpu_kind_reason >> SLOT_REASON_SHIFT),
            .first_detail = words[SLOT_WORD_FIRST_DETAIL],
            .second_detail = words[SLOT_WORD_SECOND_DETAIL],
            .is_essential = words[SLOT_WORD_IS_ESSENTIAL],
        };
        request->next_cursor = sequence + 1;
        return 1;
    }
    request->flags |= SCHED_READ_RETRY;
    observe_mark_invalid();
    return 0;
}

/* Bit mask of the producer CPUs that have sealed their ring.  Reads each CPU's
 * own acknowledgement, not merely the request to close. */
uint32_t flight_recorder_acknowledged_cpus(void)
{
    uint32_t mask = 0;
    for (unsigned cpu = 0; cpu < SCHED_OBSERVE_MAX_CPUS; cpu++) {
        if (!(g_producer_cpu_mask & (1u << cpu))) {
            continue;
        }
        if (__atomic_load_n(&g_cpu_rings[cpu].acknowledged, __ATOMIC_ACQUIRE)) {
            mask |= 1u << cpu;
        }
    }
    return mask;
}

/* The last sequence of a CPU's ring.  Stable only after that CPU sealed it. */
uint64_t flight_recorder_final_sequence(unsigned cpu)
{
    if (cpu >= SCHED_OBSERVE_MAX_CPUS || !(g_producer_cpu_mask & (1u << cpu))) {
        return 0;
    }
    return __atomic_load_n(&g_cpu_rings[cpu].final_sequence, __ATOMIC_SEQ_CST);
}

/* How many events overwrote an older one on a CPU.  This is a coverage
 * counter; it is different from the loss a particular reader suffers, which
 * depends on how far that reader had already read. */
uint64_t flight_overwritten_count(unsigned cpu)
{
    if (cpu >= SCHED_OBSERVE_MAX_CPUS || !(g_producer_cpu_mask & (1u << cpu))) {
        return 0;
    }
    return __atomic_load_n(&g_cpu_rings[cpu].overwritten_count, __ATOMIC_SEQ_CST);
}

/* ---- interrupt tracking ------------------------------------------------- */

/* Remember which interrupt is being handled and when it arrived, then record
 * it.  The nesting stack keeps the real source: PIT and APIC timer ticks,
 * system calls, faults and IPIs are never mistaken for an input device's
 * interrupt.  (An earlier note called USB input a "softirq"; that was wrong.
 * usb_core.c decodes HID reports in the caller's stack, including from timer
 * polls.  A future deferred decoder must carry its own arrival stamp.) */
void flight_interrupt_enter(uint32_t vector, uint64_t arrival_tsc)
{
    if (vector == FLIGHT_VECTOR_NMI || vector == FLIGHT_VECTOR_MACHINE_CHECK) {
        __atomic_fetch_add(&g_uncovered_interrupts, 1, __ATOMIC_SEQ_CST);
        return;
    }
    unsigned cpu = observe_current_cpu();
    struct cpu_state *state = cpu_state_if_recording(cpu);
    if (!state) {
        return;
    }
    if (__atomic_load_n(&g_close_requested, __ATOMIC_SEQ_CST)) {
        /* Even a spurious interrupt is a point where this CPU can seal its
         * ring; the recording call below sees the close request, seals and
         * adds no event. */
        flight_record_event(FLIGHT_EVENT_IRQ, 0, 0, vector, 0, 0);
        return;
    }
    if (state->interrupt_depth >= FLIGHT_MAX_INTERRUPT_NESTING) {
        state->interrupt_overflow++;
        observe_mark_invalid();
        return;
    }
    state->interrupt_stack[state->interrupt_depth++] = (struct interrupt_origin){
        .arrival_tsc = arrival_tsc,
        .arrival_ns = observe_monotonic_ns(),
        .vector = vector,
    };
    flight_record_event(FLIGHT_EVENT_IRQ, 0, 0, vector, arrival_tsc, state->interrupt_depth);
}

/* The interrupt is finished: pop it and record how long this stretch of kernel
 * execution lasted.  Done before the scheduler can move the stack to another
 * CPU.  A context switch also clears the stack, so a suspended system call
 * never labels another thread's input. */
void flight_interrupt_leave(void)
{
    struct cpu_state *state = cpu_state_if_recording(observe_current_cpu());
    if (!state) {
        return;
    }
    if (state->interrupt_overflow) {
        state->interrupt_overflow--;
        return;
    }
    if (state->interrupt_depth) {
        const struct interrupt_origin *origin =
            &state->interrupt_stack[state->interrupt_depth - 1];
        flight_record_event(FLIGHT_EVENT_KERNEL_SECTION, 0, 0, origin->vector,
                            origin->arrival_ns, observe_monotonic_ns());
        state->interrupt_depth--;
    }
}

/* Whether this vector belongs to a real input device: a legacy PIC device
 * vector (PS/2 keyboard and mouse, Ethernet) or a dynamically assigned
 * device vector (see irq.h).  The system-call vector is deliberately not one. */
static int vector_is_input_device(uint32_t vector)
{
    if (vector >= FLIGHT_VECTOR_LEGACY_DEVICE_FIRST && vector <= FLIGHT_VECTOR_LEGACY_DEVICE_LAST) {
        return 1;
    }
    return vector >= IRQ_VEC_BASE && vector < IRQ_VEC_BASE + IRQ_NVEC;
}

/* The time an input counts as having arrived.  Only the innermost interrupt, if
 * it is a real input device's, supplies a hardware arrival time.  Anything else
 * (a timer tick, or a thread polling the device) is stamped with the time of
 * the call and marked POLL, so a timer entry is never passed off as a device
 * interrupt (design section 2.2). */
uint64_t flight_input_arrival_stamp(uint32_t *source, uint64_t *arrival_tsc)
{
    uint64_t saved_flags = observe_irq_save();
    struct cpu_state *state = cpu_state_if_recording(observe_current_cpu());
    if (state && state->interrupt_depth && !state->interrupt_overflow) {
        const struct interrupt_origin *origin =
            &state->interrupt_stack[state->interrupt_depth - 1];
        if (vector_is_input_device(origin->vector)) {
            *source = SCHED_INPUT_IRQ;
            *arrival_tsc = origin->arrival_tsc;
            uint64_t arrival_ns = origin->arrival_ns;
            observe_irq_restore(saved_flags);
            return arrival_ns;
        }
    }
    *source = SCHED_INPUT_POLL;
    *arrival_tsc = observe_read_tsc();
    uint64_t now_ns = observe_monotonic_ns();
    observe_irq_restore(saved_flags);
    return now_ns;
}

/* ---- interrupts-off timing ---------------------------------------------- */

/* Start timing an interrupts-off interval.  This is a bare timing core: it
 * takes no lock, records nothing and prints nothing, so it cannot recurse into
 * the code it is measuring.  Only the outermost begin starts the clock.  The
 * caller's address is kept so the longest interval can be attributed. */
void flight_irq_disabled_begin(uint64_t flags_before_disable, uintptr_t caller_address)
{
    (void)flags_before_disable;
    struct cpu_state *state = cpu_state_if_recording(observe_current_cpu());
    if (!state) {
        return;
    }
    if (!state->irq_disabled_depth++) {
        state->irq_disabled_start_tsc = observe_read_tsc();
        state->irq_disabled_start_observer_tsc =
            __atomic_load_n(&state->observer_cost_tsc, __ATOMIC_SEQ_CST);
        state->irq_disabled_caller = caller_address;
    }
    OBSERVE_TEST_POINT(OBSERVE_POINT_IRQ_DISABLED_BEGUN);
    negctl_record_event_while_timing();
}

/* Finish an interrupts-off interval and record it.  The time spent inside the
 * recorder during the interval is subtracted, so the figure is the measured
 * code's, not the recorder's.  The recorded caller is stored as an offset from
 * flight_irq_disabled_begin so that no raw kernel address is exported; the
 * offline tool resolves it against the kernel's symbol table.  Bare cli/sti
 * instructions and assembly code are not covered, so coverage is always
 * partial and the maximum is never reported as a whole-kernel maximum. */
void flight_irq_disabled_end(uint64_t flags_before_disable)
{
    struct cpu_state *state = cpu_state_if_recording(observe_current_cpu());
    if (!state || !state->irq_disabled_depth) {
        return;
    }
    state->irq_disabled_depth--;
    if (state->irq_disabled_depth) {
        /* This ended an inner, nested interval; the outermost end records it. */
        return;
    }
    uint64_t elapsed_tsc = observe_read_tsc() - state->irq_disabled_start_tsc;
    uint64_t observer_cost_tsc =
        __atomic_load_n(&state->observer_cost_tsc, __ATOMIC_SEQ_CST) -
        state->irq_disabled_start_observer_tsc;
    elapsed_tsc = elapsed_tsc > observer_cost_tsc ? elapsed_tsc - observer_cost_tsc : 0;
    if (elapsed_tsc > __atomic_load_n(&state->irq_disabled_max_tsc, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&state->irq_disabled_max_tsc, elapsed_tsc, __ATOMIC_SEQ_CST);
    }
    uint64_t caller_offset = 0;
    if (state->irq_disabled_caller) {
        caller_offset = state->irq_disabled_caller - (uintptr_t)&flight_irq_disabled_begin;
    }
    int interrupts_were_enabled = (flags_before_disable & OBSERVE_EFLAGS_INTERRUPT_ENABLE) != 0;
    flight_record_event(FLIGHT_EVENT_IRQ_DISABLED, 0, 0, interrupts_were_enabled,
                        elapsed_tsc, caller_offset);
}

/* ---- context switches --------------------------------------------------- */

/* Called by the outgoing thread when a switch is committed, on this CPU.  The
 * old scheduler hands its lock to the incoming stack, so the interrupts-off
 * interval and the interrupt nesting of the outgoing stack are closed here, on
 * this CPU; the resumed stack must never close an interval that the old stack
 * opened. */
void flight_record_switch(uint32_t outgoing_thread_id, uint32_t incoming_thread_id, uint32_t reason)
{
    unsigned cpu = observe_current_cpu();
    struct cpu_state *state = cpu_state_if_recording(cpu);
    if (!state) {
        return;
    }
    uint64_t epoch = __atomic_load_n(&state->session_epoch, __ATOMIC_SEQ_CST);
    flight_record_event(FLIGHT_EVENT_KERNEL_SECTION, outgoing_thread_id, incoming_thread_id,
                        reason, state->interrupt_depth, 0);
    if (!still_in_session(cpu, epoch)) {
        return;
    }
    state->interrupt_depth = 0;
    state->interrupt_overflow = 0;
    if (state->irq_disabled_depth) {
        state->irq_disabled_depth = 1;
        flight_irq_disabled_end(0);
    }
    if (!still_in_session(cpu, epoch)) {
        return;
    }
    flight_record_event(FLIGHT_EVENT_SWITCH, outgoing_thread_id, incoming_thread_id, reason, 0, 0);
    state->outgoing_thread_id = outgoing_thread_id;
    state->incoming_thread_id = incoming_thread_id;
    state->switch_reason = reason;
    state->switch_sequence = state->last_published_sequence;
}

/* Called by a thread that has just started running on its own stack.  The
 * second event of the switch pair is written here, not at the pick, so what is
 * recorded is the thread that really runs, and the READY event carries how long
 * it waited from becoming runnable (ready_since_ns) to this instant. */
void flight_record_run_start(uint32_t thread_id, uint64_t ready_since_ns)
{
    uint64_t now_ns = observe_monotonic_ns();
    unsigned cpu = observe_current_cpu();
    struct cpu_state *state = cpu_state_if_recording(cpu);
    if (!state) {
        return;
    }
    if (state->incoming_thread_id == thread_id) {
        uint64_t epoch = __atomic_load_n(&state->session_epoch, __ATOMIC_SEQ_CST);
        flight_record_event(FLIGHT_EVENT_SWITCH, state->outgoing_thread_id, thread_id,
                            state->switch_reason | SCHED_SWITCH_INCOMING,
                            state->switch_sequence, ready_since_ns);
        if (!still_in_session(cpu, epoch)) {
            return;
        }
    }
    uint64_t waited_ns = 0;
    if (ready_since_ns && now_ns >= ready_since_ns) {
        waited_ns = now_ns - ready_since_ns;
    }
    flight_record_event(FLIGHT_EVENT_READY, thread_id, 0, 0, ready_since_ns, waited_ns);
    state->switch_sequence = 0;
}

/* ---- console and UART interference -------------------------------------- */

/* A byte reached the console.  Counted at the real output point, never at a
 * format attempt.  Atomic only: this runs inside the printing code, so it must
 * not take a lock or format anything.  Any byte during a run makes it invalid. */
void flight_count_console_byte(void)
{
    if (__atomic_load_n(&g_session_running, __ATOMIC_SEQ_CST) &&
        !__atomic_load_n(&g_close_requested, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_add(&g_console_byte_count, 1, __ATOMIC_SEQ_CST);
        observe_mark_invalid();
    }
}

/* A byte was written to the UART transmit register.  Counted at that write, so
 * it also covers code that bypasses kprintf and bytes drained later from the
 * serial queue. */
void flight_count_uart_byte(void)
{
    if (__atomic_load_n(&g_session_running, __ATOMIC_SEQ_CST) &&
        !__atomic_load_n(&g_close_requested, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_add(&g_uart_byte_count, 1, __ATOMIC_SEQ_CST);
        observe_mark_invalid();
    }
}

/* Lets tests tell a threshold-dependent console write from a ring-only log. */
uint64_t flight_console_byte_count(void)
{
    return __atomic_load_n(&g_console_byte_count, __ATOMIC_SEQ_CST);
}

/* Nesting of the interrupts-off timer on the calling CPU, for tests. */
unsigned flight_irq_disabled_depth(void)
{
    return g_cpu_state[observe_current_cpu()].irq_disabled_depth;
}

/* Add the recorder's counters to a status snapshot.  Each CPU only writes its
 * own accounting; this reads it with atomic loads.  The sums are diagnostic,
 * not a claim of synchronized time across CPUs. */
void flight_recorder_fill_status(struct sched_observe_status *status)
{
    status->console_bytes = flight_console_byte_count();
    status->uart_bytes = __atomic_load_n(&g_uart_byte_count, __ATOMIC_SEQ_CST);
    status->irq_disabled_coverage_partial = 1;
    status->uncovered_interrupt_count = __atomic_load_n(&g_uncovered_interrupts, __ATOMIC_SEQ_CST);
    uint64_t epoch = __atomic_load_n(&g_session_epoch, __ATOMIC_SEQ_CST);
    for (unsigned cpu = 0; cpu < SCHED_OBSERVE_MAX_CPUS; cpu++) {
        if (__atomic_load_n(&g_cpu_state[cpu].session_epoch, __ATOMIC_SEQ_CST) != epoch) {
            continue;
        }
        uint64_t longest_tsc = __atomic_load_n(&g_cpu_state[cpu].irq_disabled_max_tsc,
                                               __ATOMIC_SEQ_CST);
        if (longest_tsc > status->irq_disabled_max_tsc) {
            status->irq_disabled_max_tsc = longest_tsc;
        }
        status->observer_cost_tsc +=
            __atomic_load_n(&g_cpu_state[cpu].observer_cost_tsc, __ATOMIC_SEQ_CST);
    }
}
