#ifndef LOGIT_FLIGHT_RECORDER_H
#define LOGIT_FLIGHT_RECORDER_H

#include "observe_platform.h"

/* Flight recorder: a fixed-size ring of scheduler events per CPU, written by
 * hooks in the scheduler, the interrupt entry paths and the spin lock API.
 * See flight_recorder.c for how the ring stays consistent without locks.
 * Design reference: section 10.4 of the heterogeneous-scheduler design. */

/* x86 interrupt vectors the recorder treats specially.  NMI and machine check
 * can arrive while a ring is half written, so they are counted but never
 * recorded.  The legacy PIC device vectors are 33 to 47 (IRQ 1 to 15, after the
 * 32 CPU exception vectors); vector 32 is the PIT timer, which is not a device
 * that produces input. */
#define FLIGHT_VECTOR_NMI 2
#define FLIGHT_VECTOR_MACHINE_CHECK 18
#define FLIGHT_VECTOR_LEGACY_DEVICE_FIRST 33
#define FLIGHT_VECTOR_LEGACY_DEVICE_LAST 47

/* Record one event on the calling CPU.  A no-op unless a run is active.  Safe
 * to call with interrupts on or off, and from interrupt context. */
void flight_record_event(uint16_t kind, uint32_t thread_id, uint32_t peer_thread_id,
                         uint32_t reason, uint64_t first_detail, uint64_t second_detail);

/* An interrupt entered the kernel: remember its arrival time on this CPU's
 * nesting stack and record it.  Pair every call with flight_interrupt_leave(). */
void flight_interrupt_enter(uint32_t vector, uint64_t arrival_tsc);
void flight_interrupt_leave(void);

/* The arrival time an input event should be stamped with.  If the innermost
 * interrupt is a real input device's, that interrupt's arrival time; otherwise
 * (a timer or a thread polling the device) the current time with source POLL. */
uint64_t flight_input_arrival_stamp(uint32_t *source, uint64_t *arrival_tsc);

/* Time one interval with interrupts off, from the lock API that disables them
 * (spin_lock_irqsave) to the one that restores them.  Nested intervals count
 * once, from the outermost begin to the matching end. */
void flight_irq_disabled_begin(uint64_t flags_before_disable, uintptr_t caller_address);
void flight_irq_disabled_end(uint64_t flags_before_disable);

/* Context switch hooks.  flight_record_switch() is called by the outgoing
 * thread when the switch is committed; flight_record_run_start() by the
 * incoming thread once it is really running on its own stack, which is the
 * moment its wait for a CPU ends. */
void flight_record_switch(uint32_t outgoing_thread_id, uint32_t incoming_thread_id, uint32_t reason);
void flight_record_run_start(uint32_t thread_id, uint64_t ready_since_ns);

/* Count bytes that reached the console or the serial port while a run is
 * active.  Output during a measurement disturbs it, so any such byte makes the
 * run invalid.  These only touch atomic counters, so they can be called from
 * inside the printing code without recursing into it. */
void flight_count_console_byte(void);
void flight_count_uart_byte(void);
uint64_t flight_console_byte_count(void);

/* Records lost to wrapping on one CPU's ring.  Overwriting old events is
 * allowed, but it is counted. */
uint64_t flight_overwritten_count(unsigned cpu);

/* Nesting depth of the interrupts-off timer on this CPU (for tests). */
unsigned flight_irq_disabled_depth(void);

/* Run life cycle, driven by observe_control.c.
 *   allocate  reserve the rings before the run is published;
 *   start     begin recording;
 *   request_close  ask every CPU to seal its ring (each CPU does so itself,
 *             the next time it reaches an observation point);
 *   acknowledged_cpus  which CPUs have sealed;
 *   free      release the rings, after every CPU sealed and every reader left. */
int flight_recorder_allocate(uint32_t producer_cpu_mask);
void flight_recorder_start(void);
void flight_recorder_request_close(void);
uint32_t flight_recorder_acknowledged_cpus(void);
uint64_t flight_recorder_final_sequence(unsigned cpu);
void flight_recorder_free(void);

/* Read the record at request->cursor of request->cpu's ring.  Returns 1 and
 * fills *out when a record was copied, 0 when there is none (the request's
 * flags then say whether that is a gap or a retry), or a negative error. */
int flight_recorder_read(struct sched_observe_read *request, struct flight_event *out);

/* Add the recorder's counters to a status snapshot. */
void flight_recorder_fill_status(struct sched_observe_status *status);

#ifdef SCHED_OBSERVE_HOST
/* Host test only: jump a ring's sequence counter to a value near the limit,
 * so the exhaustion check can be exercised without 2^63 writes. */
void flight_recorder_set_last_sequence_for_test(uint64_t sequence);
#endif

/* Development-machine harnesses that include scheduler code without the
 * observer get no-op hooks: they collect nothing and link nothing. */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define flight_record_event(...) ((void)0)
#define flight_interrupt_enter(...) ((void)0)
#define flight_interrupt_leave() ((void)0)
#define flight_irq_disabled_begin(...) ((void)0)
#define flight_irq_disabled_end(...) ((void)0)
#define flight_record_switch(...) ((void)0)
#define flight_record_run_start(...) ((void)0)
#define flight_count_console_byte() ((void)0)
#define flight_count_uart_byte() ((void)0)
#endif

#endif
