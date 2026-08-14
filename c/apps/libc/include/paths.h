#ifndef _PATHS_H
#define _PATHS_H

/* BSD <paths.h>: well-known filesystem paths, as macros so a ported program
 * can `open(_PATH_DEVNULL, ...)` instead of a hardcoded string.
 *
 * UNLIKE sysexits.h, THESE VALUES ARE NOT PORTABLE CONSTANTS -- they are
 * facts about one filesystem, and copying Linux's paths.h (as glibc ships
 * it, with _PATH_STDPATH = "/usr/bin:/bin:/usr/sbin:/sbin", a /var tree,
 * /etc/shells, /etc/nologin, utmp/wtmp...) would make almost every macro
 * below name something that does not exist on LogitOS. Every value here was
 * checked against this tree instead:
 *   - _PATH_BSHELL: c/apps/coreutils/sh.c is built to /bin/sh (Makefile's
 *     CLI list); c/apps/libc/src/pwgrp.c's own fallback shell literal
 *     agrees ("/bin/sh").
 *   - _PATH_STDPATH / _PATH_DEFPATH: sh.c's run_external() does not consult
 *     a PATH variable at all -- it hardcodes "/bin/" + argv[0]
 *     (c/apps/coreutils/sh.c) and there is no /usr/bin, /sbin or /usr/sbin
 *     anywhere in the disk image the Makefile builds ($(DISK) recipe packs
 *     CLI programs to /bin and only /bin). So the search path on this
 *     machine really is the single directory "/bin", not BSD's four-element
 *     default; giving these macros the Linux value would be false here.
 *   - _PATH_TMP: c/apps/libc/src/stdio.c's tmpfile() does `mkdir("/tmp",
 *     0777)` before creating a temp file, i.e. this is the one temp
 *     directory this libc itself already depends on existing. Trailing
 *     slash matches BSD's own _PATH_TMP (these macros are meant to be
 *     concatenated straight onto a filename).
 *   - _PATH_DEV: where the synthetic control files actually live --
 *     c/fs/vfsctl.c serves /dev/{vfsctl,vfsmounts,vfsmeta,fsbench} and
 *     c/kernel/core/kdiag.c serves /dev/{kmsg,kstat,ktrigger,kprof}.
 *   - _PATH_PASSWD: c/apps/coreutils/login.c and c/apps/libc/src/pwgrp.c
 *     both hardcode "/etc/passwd" as the one account store on this system.
 *
 * _PATH_DEVNULL IS THE ONE VALUE HERE THAT IS **NOT** TRUE ON THIS MACHINE
 * YET, AND THAT IS DELIBERATE -- READ THIS BEFORE "FIXING" IT. There is no
 * null device: /dev/ on LogitOS serves exactly the eight synthetic names
 * listed above (grep c/fs/vfsctl.c's g_names[] and c/kernel/core/kdiag.c's
 * g_devnames[] -- "null" is in neither), so `open(_PATH_DEVNULL, O_WRONLY)`
 * fails with ENOENT today. The macro is still defined to the conventional
 * "/dev/null" rather than quietly pointed at some other real file (a ramfs
 * scratch file, say) that WOULD open successfully, because that would be
 * exactly the "lie that returns 0" this codebase's house style forbids: a
 * write to a fake null sink silently succeeds and silently accumulates
 * forever instead of discarding, which is worse than the honest ENOENT a
 * caller can actually detect and handle. A real null device belongs in
 * c/fs/vfsctl.c or c/kernel/core/kdiag.c (a third synthetic name whose
 * read always returns 0 bytes and whose write always reports success
 * without storing anything) -- not invented here as a path macro pointing
 * at nothing of the sort.
 */

#define _PATH_BSHELL   "/bin/sh"
#define _PATH_DEVNULL  "/dev/null"   /* see the note above: not yet backed by a real device */
#define _PATH_DEV      "/dev/"
#define _PATH_TMP      "/tmp/"
#define _PATH_STDPATH  "/bin"
#define _PATH_DEFPATH  "/bin"
#define _PATH_PASSWD   "/etc/passwd"

#endif /* _PATHS_H */
