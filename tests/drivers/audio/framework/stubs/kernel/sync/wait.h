#ifndef AUDIO_TEST_WAIT_H
#define AUDIO_TEST_WAIT_H
struct waitq { unsigned wakes; };
struct semaphore { unsigned tokens; };
void waitq_init(struct waitq *queue);
void waitq_wake_all(struct waitq *queue);
void semaphore_init(struct semaphore *sem, int value);
void sem_post(struct semaphore *sem);
void sem_wait(struct semaphore *sem);
/* This fixture schedules the real worker explicitly; it does not model a
 * sleeping syscall or pretend to verify the production scheduler. */
#define wait_event_timeout(queue, condition, milliseconds, result) \
    do { (void)(queue); (void)(milliseconds); (result) = !!(condition); } while (0)
#endif
