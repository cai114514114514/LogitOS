#ifndef LOGIT_SCHED_OBSERVE_ABI_H
#define LOGIT_SCHED_OBSERVE_ABI_H

#include <stdint.h>

/* ABI of the scheduler observation facility: one diagnostic system call
 * (SYS_SCHED_OBSERVE, whose number lives only in logit_abi.h).
 *
 * WHAT IT IS.  A measurement tool.  While a "run" is active the kernel keeps
 *   - a flight recorder: a fixed-size ring per CPU of scheduler events (wake,
 *     context switch, interrupt entry, time spent with interrupts off, ...);
 *   - an input ledger: one record per user input event (mouse, key, ...) with
 *     its deadlines and how it ended (shown, merged into a later event,
 *     handled by the app without drawing, or expired);
 *   - an archive of "coalesced groups": which raw inputs the window manager
 *     merged into one delivered event.
 * A privileged tool starts a run with BEGIN, ends it with STOP and pages the
 * records out with READ.  Nothing here changes a scheduling decision.
 *
 * REQUEST RULES.  Every request starts with struct sched_observe_header and uses
 * fixed-width fields.  User buffers are passed as 64-bit addresses, never as
 * native pointers.  The kernel checks size and version first, copies the
 * request in, and only then acts, so a bad pointer or length has no side effect.
 *
 * CURSORS.  Each reader supplies its own cursor, so two readers never move each
 * other's position.  Cursor 0 means "the first record"; any other cursor is the
 * first sequence number this reader has not yet accepted.  INPUT records are
 * numbered by an input id that grows for the whole boot (it is not an index
 * into the per-run pool), so start with cursor 0 and follow next_cursor.  The
 * manifest ordinal below is a separate 1-based number that restarts every run.
 *
 * Design reference: docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md
 * section 10.4 (recorder and export protocol) and sections 2.2 and 7.4.8
 * (input deadlines and merged-input accounting). */

#define SCHED_OBSERVE_VERSION 1u

/* Most input records, and most coalesced-group nodes, one run may ask for. */
#define SCHED_OBSERVE_MAX_CAPACITY 1048576u

/* Most CPUs the recorder can follow; one bit per CPU in the masks below. */
#define SCHED_OBSERVE_MAX_CPUS 32u

/* A group id packs the node's generation above its slot number:
 * (generation << SCHED_OBSERVE_GROUP_ID_GENERATION_SHIFT) | slot. */
#define SCHED_OBSERVE_GROUP_ID_GENERATION_SHIFT 32

/* Words in the opaque manifest identity hash carried by BEGIN. */
#define SCHED_OBSERVE_MANIFEST_HASH_WORDS 4u

/* Most records one READ request may return, which also bounds the private
 * kernel buffer a READ allocates. */
#define SCHED_OBSERVE_MAX_READ_RECORDS 64u

/* Size of one METADATA record: a single text line of run identity and the
 * descriptions registered by the optional providers. */
#define SCHED_OBSERVE_METADATA_RECORD_BYTES 1024u

/* FNV-1a 64-bit hash parameters (the public constants of that hash).  The
 * manifest's payload hash and the test tools' export hashes both use them, so
 * they are defined once here. */
#define SCHED_OBSERVE_FNV1A_OFFSET_BASIS 14695981039346656037ull
#define SCHED_OBSERVE_FNV1A_PRIME 1099511628211ull

/* Negative error numbers returned by the system call.  Their values equal
 * minus the libc errno of the same name (c/apps/libc/include/errno.h).  The
 * kernel has no shared errno header, so each subsystem names the few it uses
 * (the VFS does the same with its VFS_E* names). */
#define SCHED_OBSERVE_EPERM (-1)
#define SCHED_OBSERVE_EAGAIN (-11)
#define SCHED_OBSERVE_ENOMEM (-12)
#define SCHED_OBSERVE_EFAULT (-14)
#define SCHED_OBSERVE_EBUSY (-16)
#define SCHED_OBSERVE_EINVAL (-22)
#define SCHED_OBSERVE_ENOSPC (-28)
#define SCHED_OBSERVE_ENOSYS (-38)
#define SCHED_OBSERVE_EOVERFLOW (-75)

enum sched_observe_command {
    SCHED_OBSERVE_BEGIN = 1,
    SCHED_OBSERVE_STOP,
    SCHED_OBSERVE_STATUS,
    SCHED_OBSERVE_READ,
    SCHED_OBSERVE_PROBE,    /* hardware probe; answered by a later stage's provider */
    SCHED_OBSERVE_MODEL     /* topology and capacity model; answered by a later provider */
};

/* Which record stream a READ request pages through. */
enum sched_observe_stream {
    SCHED_STREAM_TRACE = 1,     /* flight recorder events, one ring per CPU */
    SCHED_STREAM_INPUT,         /* the input ledger */
    SCHED_STREAM_GROUPS,        /* the coalesced-group archive */
    SCHED_STREAM_METADATA       /* one text record describing the run */
};

/* The experimental condition a run is part of.  The names in brackets are the
 * design document's names for the two measured baselines. */
enum sched_observe_baseline {
    SCHED_BASELINE_TRACE_ONLY,          /* recorder only; no baseline claim */
    SCHED_BASELINE_ORIGINAL_WAITING,    /* [B-HEAD] GUI threads wait as they did before this work */
    SCHED_BASELINE_UNIFIED_WAITING      /* [B-input] GUI threads wait on one combined source */
};

/* Optional modules (added by later stages) that can answer PROBE or MODEL
 * commands or add a line to the METADATA record.  A kind nobody registered
 * answers "not implemented"; there is no success-shaped stub. */
enum sched_observe_provider {
    SCHED_PROVIDER_FRAME_METADATA = 1,
    SCHED_PROVIDER_PROBE,
    SCHED_PROVIDER_TOPOLOGY,
    SCHED_PROVIDER_CAPACITY,
    SCHED_PROVIDER_CALIBRATION,
    SCHED_PROVIDER_SOUND_METADATA,
    SCHED_PROVIDER_COUNT        /* one past the last kind; sizes the registry */
};

/* What a flight recorder event is about.  Stage S0 records only events that
 * already exist in the scheduler.  Later stages may add a delivery token (a
 * ticket the scheduler hands to one input so the thread that handles it can be
 * found) and a lease (a time-limited reservation of a CPU core for one app);
 * events for those are not defined yet.  The values are fixed by the stage
 * plan. */
enum flight_event_kind {
    FLIGHT_EVENT_IRQ = 1,           /* an interrupt entered the kernel */
    FLIGHT_EVENT_WAKE,              /* a thread was made runnable (or the wake did nothing) */
    FLIGHT_EVENT_SWITCH,            /* the CPU changed from one thread to another */
    FLIGHT_EVENT_READY,             /* a runnable thread started running; carries its wait */
    FLIGHT_EVENT_MIGRATE,           /* reserved: thread moved CPUs */
    FLIGHT_EVENT_INPUT,             /* an input was added to the ledger */
    FLIGHT_EVENT_OUTCOME,           /* an input reached its final outcome */
    FLIGHT_EVENT_CAPACITY,          /* reserved: CPU capacity changed */
    FLIGHT_EVENT_TIMER,             /* a timer wake fired */
    FLIGHT_EVENT_IRQ_DISABLED,      /* one interval with interrupts off ended */
    FLIGHT_EVENT_KERNEL_SECTION     /* one stretch of kernel execution ended */
};

/* Why a WAKE event happened. */
enum sched_wake_reason {
    SCHED_WAKE_OBJECT = 1,      /* a synchronization object released a waiter */
    SCHED_WAKE_ASYNC_ID,        /* woken by thread id from another context */
    SCHED_WAKE_TIMEOUT,         /* a sleep or wait deadline passed */
    SCHED_WAKE_NEW              /* a newly created thread became runnable */
};

/* Why a SWITCH event happened.  These are the existing scheduler paths; this
 * work adds no new way to switch. */
enum sched_switch_reason {
    SCHED_SWITCH_SCHEDULE = 1,  /* schedule() chose another thread */
    SCHED_SWITCH_EXIT,          /* the running thread exited */
    SCHED_SWITCH_BLOCK          /* the running thread blocked */
};

/* Set in the reason of the second SWITCH event of a pair.  The first event is
 * written by the outgoing thread when the switch is committed; the second is
 * written by the incoming thread once it really runs on its own stack, and
 * carries the first event's sequence number in first_detail. */
#define SCHED_SWITCH_INCOMING (1u << 31)

/* Where an input's timestamp came from.  Only a real device interrupt gives a
 * hardware arrival time; a timer or thread that polls a device only knows when
 * it looked. */
enum sched_input_source {
    SCHED_INPUT_POLL,
    SCHED_INPUT_IRQ
};

/* Whether the scheduler issued ("minted") a delivery token for an input, that
 * is the ticket a later stage will hand to the thread that handles the input.
 * Stage S0 has no tokens, so every real input is NOT_MINTED and must expire like
 * any other; MINTED and MERGED (the token was shared with a later input) exist
 * for later stages and for fault-injection tests. */
enum input_allocation {
    INPUT_NOT_MINTED,
    INPUT_MINTED,
    INPUT_MERGED
};

/* How an input ended.  Exactly one of the four final outcomes is recorded per
 * input; the sum of the four counts always equals the number of inputs issued
 * once none is pending. */
enum input_outcome {
    INPUT_PENDING,
    INPUT_DONE,             /* its own result was shown on screen in time */
    INPUT_COALESCED_DONE,   /* merged into a later event whose result was shown */
    INPUT_APP_DONE,         /* the app handled it and correctly drew nothing */
    INPUT_EXPIRED,          /* no outcome arrived before the deadline */
    INPUT_OUTCOME_COUNT     /* one past the last outcome; sizes counters */
};

/* Whether an input must produce a visible change.  APP_DONE is only legal for
 * NO_DISPLAY inputs. */
enum input_class {
    INPUT_DISPLAY_REQUIRED,
    INPUT_NO_DISPLAY
};

/* One flight recorder event as READ returns it: a private, internally
 * consistent copy.  Inside the ring the same data is stored as a version word
 * plus eight atomic 64-bit words, so this struct is only the export format.
 * The meaning of first_detail and second_detail depends on kind. */
struct flight_event {
    uint64_t sequence;          /* 1-based position in this CPU's ring */
    uint64_t timestamp_tsc;     /* raw TSC, serialized, at the moment of recording */
    uint64_t monotonic_ns;      /* kernel monotonic clock at recording */
    uint32_t thread_id;
    uint32_t peer_thread_id;    /* the other thread of a wake or switch; 0 if none */
    uint16_t cpu;
    uint16_t kind;              /* enum flight_event_kind */
    uint32_t reason;
    uint64_t first_detail;
    uint64_t second_detail;
    uint64_t is_essential;      /* 1 if losing this event invalidates the run, else 0 */
};

/* Names one node in the coalesced-group archive.  slot is 1-based and
 * generation changes every run, so a stale handle can never name a node of a
 * later run.  {0, 0} means "no group". */
struct coalesced_group_handle {
    uint32_t slot;
    uint32_t generation;
};

/* The raw content of one input event, copied when it arrived.  The ledger
 * never changes it afterwards.  The field names follow struct logit_event. */
struct input_payload {
    uint32_t event_type;        /* an EV_* value from logit_abi.h */
    uint32_t event_flags;
    int32_t x;
    int32_t y;
    int32_t left_button;
    int32_t right_button;
    int32_t middle_button;
    int32_t wheel_delta;
    int32_t modifiers;          /* EV_MOD_* bits held when the event was generated */
};

/* Describes how a coalesced group was built.  A group is immutable once
 * published.  A group is either a leaf (one raw input), a join of two groups
 * in order, or a re-route of one group to another window or layer. */
struct coalesced_group_info {
    uint64_t representative_input_id;   /* the input whose delivery stands for the whole group */
    uint64_t route_generation;          /* version of the routing decision for this node */
    uint64_t capture_generation;        /* version of the pointer-capture state it was merged in */
    uint64_t window_generation;         /* version of the destination window it was merged for */
    uint32_t window;
    uint32_t member_count;              /* exact number of raw inputs in the group */
    uint32_t layer;                     /* 0 = raw transport only; above 0 = a semantic merge */
    uint32_t reducer_id;                /* which merge rule was applied (0 when layer is 0) */
    uint32_t reducer_version;
};

/* Evidence, copied into an input record when it reaches DONE or COALESCED_DONE,
 * that a specific shown frame accounts for the input.  An offline checker uses
 * it to re-verify the claim from the exported records alone. */
struct input_completion_evidence {
    uint64_t group_id;          /* generation << 32 | slot of the covering group */
    uint64_t route_generation;
    uint64_t state_version;     /* window state the frame was drawn from */
    uint64_t surface_version;   /* the surface the frame was presented on */
    uint64_t frame_id;
    uint32_t reducer_id;
    uint32_t reducer_version;
    uint32_t layer;
};

/* One row of the input ledger as READ returns it.  Deadlines are fixed from the
 * original arrival and never restart (design section 2.2).  The design document
 * calls response_deadline_ns "d_resp" and outcome_deadline_ns "d_outcome". */
struct input_record {
    uint64_t input_id;                  /* grows for the whole boot; never reused */
    uint64_t irq_timestamp_ns;          /* arrival: device interrupt time (poll start for POLL) */
    uint64_t irq_timestamp_tsc;         /* the same instant on the raw TSC */
    uint64_t frame_period_ns;           /* display frame period P in force at arrival */
    uint64_t frame_generation;          /* version of the frame clock settings at arrival */
    uint64_t response_deadline_ns;      /* arrival + 2 frame periods */
    uint64_t outcome_deadline_ns;       /* arrival + 3 frame periods; settling later expires it */
    uint64_t completed_at_ns;           /* when the final outcome was recorded; 0 while pending */
    uint32_t source;                    /* enum sched_input_source */
    uint32_t input_class;               /* enum input_class */
    uint32_t allocation;                /* enum input_allocation */
    uint32_t outcome;                   /* enum input_outcome */
    struct input_payload payload;
    struct input_completion_evidence completion_evidence;   /* written once with the outcome */
};

/* First field of every request. */
struct sched_observe_header {
    uint32_t size;      /* sizeof the whole request; must match the command */
    uint32_t version;   /* SCHED_OBSERVE_VERSION */
    uint64_t run_id;    /* ids increase; BEGIN must use a larger id than any earlier run */
};

/* One preregistered expected input.  The experiment declares its inputs before
 * the run so that a missing, extra or altered input is detected rather than
 * silently shifting the statistics.  payload_hash is the FNV-1a hash of the 36
 * raw bytes of struct input_payload (little endian).  ordinal starts at one.
 * allowed_type_mask uses the event type as a bit index (types 0 to 31).  The
 * whole manifest is copied and sealed at BEGIN. */
struct sched_observe_manifest {
    uint64_t ordinal;
    uint64_t payload_hash;
    uint64_t route_generation;
    uint64_t capture_generation;
    uint64_t window_generation;
    uint32_t input_class;
    uint32_t source;
    uint32_t reducer_id;
    uint32_t reducer_version;
    uint32_t window;
    int32_t left_button;
    int32_t right_button;
    int32_t middle_button;
    int32_t modifiers;
    uint32_t allowed_type_mask;
};

/* BEGIN: start a run.  manifest_hash is an opaque identity of the preregistered
 * plan, kept for the report; it is not a security signature. */
struct sched_observe_begin {
    struct sched_observe_header header;
    uint64_t input_capacity;        /* ledger rows to reserve (1 to SCHED_OBSERVE_MAX_CAPACITY) */
    uint64_t group_capacity;        /* coalesced-group nodes to reserve */
    uint64_t manifest_hash[SCHED_OBSERVE_MANIFEST_HASH_WORDS];
    uint64_t manifest_address;      /* user address of manifest_entry_count entries, or 0 */
    uint64_t manifest_entry_count;
    uint32_t baseline;              /* enum sched_observe_baseline */
    uint32_t flags;                 /* must be 0 */
};

/* READ flags.  GAP, RETRY and FINAL are results; CONFIRM_EXPORT is a request
 * flag.  Exporting the last page is not enough to release the run's memory,
 * because the copy to the reader may still fail; the reader confirms with a
 * separate READ that has this flag and cursor = final_sequence + 1.  That is
 * legal only after STOP. */
#define SCHED_READ_GAP 1u               /* records were lost between gap_first and gap_last */
#define SCHED_READ_RETRY 2u             /* a slot kept changing; try the same cursor again */
#define SCHED_READ_FINAL 4u             /* the page reaches the end of a stopped stream */
#define SCHED_READ_CONFIRM_EXPORT 8u

struct sched_observe_read {
    struct sched_observe_header header;
    uint32_t stream;                /* enum sched_observe_stream */
    uint32_t cpu;                   /* which CPU's ring (TRACE stream only) */
    uint64_t cursor;
    uint64_t user_buffer_address;
    uint32_t record_capacity;       /* records the buffer holds (at most MAX_READ_RECORDS) */
    uint32_t records_copied;        /* result */
    uint64_t next_cursor;           /* result: where to continue */
    uint64_t gap_first_sequence;    /* result: first lost record when SCHED_READ_GAP is set */
    uint64_t gap_last_sequence;
    uint64_t final_sequence;        /* result: last record of the stream once the run is stopped */
    uint32_t flags;
    uint32_t reserved;              /* must be 0 */
};

/* Run life cycle.  BEGIN makes ACTIVE.  STOP closes admission of new inputs
 * and moves to DRAINING while the original deadlines of pending inputs run
 * out; then CLOSING while each CPU seals its own ring; then STOPPED, when all
 * records are final and can be exported.  After the exports are confirmed the
 * memory is freed and the state returns to IDLE; that happens inside the READ
 * that makes the last export final (or the last reader to leave, or a repeated
 * STOP), never in an interrupt. */
enum sched_observe_state {
    SCHED_OBSERVE_IDLE,
    SCHED_OBSERVE_ACTIVE,
    SCHED_OBSERVE_DRAINING,
    SCHED_OBSERVE_CLOSING,
    SCHED_OBSERVE_STOPPED
};

/* STATUS: a snapshot of the counters.  The four input outcome counts plus
 * inputs_pending always add up to inputs_issued.
 *
 * Two loss counters, and what separates them.  essential_lost_count counts
 * records the run cannot do without and that are gone for good: an input
 * refused because the ledger was full, a group node refused because the pool
 * was full.  Any one of them makes the run invalid.  observation_lost_count is
 * that number plus every flight event that replaced an older one in a ring,
 * whether or not a reader had already copied the old one; a ring overwrite
 * alone does not invalidate a run, because a reader that kept up lost nothing. */
struct sched_observe_status {
    struct sched_observe_header header;
    uint64_t inputs_issued;
    uint64_t inputs_pending;
    uint64_t inputs_done;
    uint64_t inputs_coalesced_done;
    uint64_t inputs_app_done;
    uint64_t inputs_expired;
    uint64_t observation_lost_count;    /* essential losses plus ring overwrites */
    uint64_t essential_lost_count;      /* refused input and group records */
    uint64_t console_bytes;             /* bytes printed on the console during the run */
    uint64_t uart_bytes;                /* bytes sent to the serial port during the run */
    uint64_t irq_disabled_max_tsc;      /* longest measured interrupts-off interval (partial) */
    uint64_t observer_cost_tsc;         /* TSC cycles spent inside the recorder itself */
    uint64_t groups_live_count;
    uint64_t group_consumer_count;
    uint64_t uncovered_interrupt_count; /* NMI and machine-check entries, which are not recorded */
    uint64_t final_sequence[SCHED_OBSERVE_MAX_CPUS];
    uint32_t state;                     /* enum sched_observe_state */
    uint32_t run_invalid;               /* 1 once any necessary evidence was lost */
    uint32_t producer_cpu_mask;         /* CPUs that write a ring this run */
    uint32_t acknowledged_cpu_mask;     /* CPUs that have sealed their ring */
    uint32_t active_read_count;
    uint32_t baseline;
    uint32_t irq_disabled_coverage_partial; /* always 1: bare cli/sti is not seen */
    uint32_t unobserved_cpu_count;      /* CPUs beyond MAX_CPUS: running, but not recorded */
};

/* One row of the coalesced-group archive as READ returns it.  The archive keeps
 * the whole ordered graph, including child ids and the original input id of
 * each leaf, because exporting only the representative inputs could not
 * rebuild the evidence that an input was covered by a shown frame. */
struct sched_observe_group_record {
    uint64_t sequence;          /* 1-based row number */
    uint64_t group_id;          /* generation << 32 | slot */
    uint64_t left_child_id;     /* 0 for a leaf */
    uint64_t right_child_id;    /* 0 for a leaf or a re-route */
    uint64_t input_id;          /* the raw input of a leaf; 0 for other nodes */
    struct coalesced_group_info info;
};

#if !defined(__cplusplus)
/* The guest TCC (0.9.27) does not implement C11 _Static_assert, and this header is
 * published into the guest sysroot, where TCC must still parse it.  A negative array
 * bound is also a compile error, so the wire-size checks stay active for TCC under
 * that spelling.  The spelling is undone at the end of this block so includers do not
 * inherit an assertion macro from this protocol header (logit_boot.h does the same). */
#ifdef __TINYC__
#define LOGIT_SCHED_OBSERVE_JOIN_(left, right) left##right
#define LOGIT_SCHED_OBSERVE_JOIN(left, right) LOGIT_SCHED_OBSERVE_JOIN_(left, right)
#define _Static_assert(condition, message) \
    typedef char LOGIT_SCHED_OBSERVE_JOIN(sched_observe_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/* Freeze the wire sizes before the receipt and probe stages consume this ABI.
 * Ring storage has its own atomic layout and is deliberately not one of these
 * types. */
_Static_assert(sizeof(struct flight_event) == 64, "flight event wire size");
_Static_assert(sizeof(struct input_record) == 176, "input record wire size");
_Static_assert(sizeof(struct sched_observe_group_record) == 96, "group record wire size");
_Static_assert(sizeof(struct sched_observe_manifest) == 80, "manifest wire size");
_Static_assert(sizeof(struct sched_observe_begin) == 88, "begin wire size");
_Static_assert(sizeof(struct sched_observe_read) == 88, "read wire size");
_Static_assert(sizeof(struct sched_observe_status) == 424, "status wire size");

#ifdef __TINYC__
#undef _Static_assert
#undef LOGIT_SCHED_OBSERVE_JOIN
#undef LOGIT_SCHED_OBSERVE_JOIN_
#endif
#endif

#endif
