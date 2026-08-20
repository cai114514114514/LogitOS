#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H

#include <stdint.h>

/* eventfd(2). A counter with a wait queue and a descriptor -- what makes a
 * poll() loop closable by another thread. See the SYS_EVENTFD block in
 * include/abi/logit_abi.h for the counter's exact semantics.
 *
 * EFD_CLOEXEC IS ABSENT ON PURPOSE, not forgotten. execve does not close file
 * descriptors on this machine (see O_CLOEXEC in <fcntl.h>, which is accepted
 * and ignored), so a flag named "close on exec" would be accepted and would do
 * nothing -- and a program that relies on it to keep a private wakeup channel
 * out of a child would be wrong with no error to look at. A name that does not
 * exist is a compile error, which is the failure you want. */
typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 0x0001
#define EFD_NONBLOCK  0x0800     /* == O_NONBLOCK, deliberately the same bit */

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#endif /* _SYS_EVENTFD_H */
