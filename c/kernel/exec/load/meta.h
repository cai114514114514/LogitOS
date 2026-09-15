#ifndef LOGIT_META_H
#define LOGIT_META_H

/* The file-metadata syscalls: SYS_STAT / SYS_LSTAT / SYS_FSTAT / SYS_GETDENTS /
 * SYS_CHMOD / SYS_UMASK / SYS_SYMLINK / SYS_READLINK / SYS_LINK / SYS_CHOWN
 * (ABI numbers 120-129, include/abi/logit_abi.h).
 *
 * Forwarded whole from c/kernel/exec/syscall.c for the reason mm_syscall() and
 * uthread_syscall() give: which argument is a user pointer and what it means
 * are facts about this subsystem, and they belong beside the code that knows
 * them rather than in the dispatch table. It also keeps the shared syscall.c
 * hunk to one case group, which matters while a dozen lines are editing it.
 *
 * Returns the value for rax: 0 or a count on success, negative on failure. */
long meta_syscall(long num, long a, long b, long c);

#endif /* LOGIT_META_H */
