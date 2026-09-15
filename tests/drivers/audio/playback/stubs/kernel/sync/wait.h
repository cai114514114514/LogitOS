#ifndef PLAYBACK_TEST_WAIT_H
#define PLAYBACK_TEST_WAIT_H
struct waitq { unsigned wakes; };
struct semaphore { unsigned tokens; };
void waitq_init(struct waitq *queue);
void waitq_wake_all(struct waitq *queue);
void semaphore_init(struct semaphore *sem, int value);
void sem_post(struct semaphore *sem);
void sem_wait(struct semaphore *sem);
void playback_test_wait(void);
/* Deterministic interleaving after a syscall drops its mixer lock. The test
 * never claims this substitutes for scheduling/preemption in a real guest. */
#define wait_event_timeout(queue, condition, milliseconds, result) \
    do { (void)(queue); (void)(milliseconds); playback_test_wait(); \
         (result) = !!(condition); } while (0)
#endif
