#ifndef LOGIT_KDIAG_H
#define LOGIT_KDIAG_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * kdiag -- the runtime diagnostic surface.
 *
 * WHY FILES AND NOT A SYSCALL
 * ---------------------------|
 * The log has to be readable from inside a running system, and there were two
 * plausible shapes: a new syscall, or a synthetic file. This is a file, for
 * three reasons that are about the rest of the tree rather than about elegance:
 *
 *   1. It needs no new ABI number. include/abi/logit_abi.h is the single most
 *      contended header in this repo -- a dozen lines are adding syscalls at
 *      once -- and a diagnostic facility that collides with the thing you are
 *      trying to diagnose is a bad trade.
 *   2. Every reader already exists. `cat /dev/kmsg`, `cat /dev/kmsg > log.txt`,
 *      opening it in TextEdit from the Finder, `wc -l`, a pipe into anything --
 *      all of it works today, on a laptop with no serial port, with no new
 *      userland program to write and no shell builtin to add.
 *   3. Writes give us a trigger channel for free. `echo bt > /dev/ktrigger` is
 *      this kernel's /proc/sysrq-trigger: it is how the panic path, the
 *      assertion path and the interrupt-context log test are exercised on a
 *      real machine without a debugger attached.
 *
 * The cost is one hook in c/fs/vfs.c (six lines: consult kdiag before the disk
 * filesystem). That is the only shared file this facility touches.
 * ------------------------------------------------------------------------ */

#define KDIAG_NOT_MINE (-2)      /* returned by every op for a path we do not own */

void kdiag_init(void);           /* install the diagnostic IPI vectors */

int  kdiag_size(const char *path);
int  kdiag_read(const char *path, void *buf, int max);
int  kdiag_write(const char *path, const void *buf, int len);

/* Directory enumeration for "/dev" so the files can be FOUND, not just known. */
int         kdiag_dir_count(const char *dir);
const char *kdiag_dir_name(const char *dir, int i);
int         kdiag_dir_size(const char *dir, int i);

/* Park every other core with interrupts disabled. The panic path calls this so
 * the report is not shredded by a core that is still running. No-op before the
 * LAPIC is up or on a single-core boot. */
void kdiag_stop_other_cpus(void);

#endif /* LOGIT_KDIAG_H */
