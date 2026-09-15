#ifndef AUDIO_TEST_SCHED_H
#define AUDIO_TEST_SCHED_H
struct thread { unsigned unused; };
struct thread *sched_current_thread(void);
struct thread *thread_create(void (*entry)(void), const char *name);
#endif
