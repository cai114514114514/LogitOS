#ifndef _SYS_FILE_H
#define _SYS_FILE_H

/* flock(): whole-file advisory locking. See the long comment above flock()
 * in uio.c for why this is ENOSYS rather than a fabricated success or a
 * process-local lock table -- short version, it is the same rule as
 * termios.c's ENOTTY: an honest failure beats a program that believes it
 * holds a lock nothing on this machine enforces. */

#define LOCK_SH 1   /* shared (read) lock */
#define LOCK_EX 2   /* exclusive (write) lock */
#define LOCK_NB 4   /* OR with SH/EX/UN: do not block */
#define LOCK_UN 8   /* unlock */

int flock(int fd, int operation);

#endif /* _SYS_FILE_H */
