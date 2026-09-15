#ifndef PLAYBACK_TEST_SCHED_H
#define PLAYBACK_TEST_SCHED_H
struct thread { unsigned unused; };
struct thread *sched_current_thread(void);
void thread_create(void (*entry)(void), const char *name);
#endif
