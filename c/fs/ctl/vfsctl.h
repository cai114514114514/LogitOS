#ifndef LOGIT_VFSCTL_H
#define LOGIT_VFSCTL_H

/* ---------------------------------------------------------------------------
 * The VFS's control and introspection surface, as three synthetic files:
 *
 *   /dev/vfsctl     write a command; read back the result of the last one
 *   /dev/vfsmounts  the mount table
 *   /dev/vfsmeta    the metadata store and the credential table
 *
 * FILES RATHER THAN SYSCALLS, and this time it is not only about contention.
 * include/abi/logit_abi.h has a dozen lines appending to it simultaneously, so
 * spending six numbers on chmod/chown/link/symlink/mount/setuid is a poor
 * trade -- kdiag made that argument first and it still holds. But there is a
 * second reason that is specific to this facility: every one of these commands
 * has to be reachable from an UNPRIVILEGED process for the refusal to be worth
 * anything, and a synthetic file is reachable through the shell that already
 * exists. `echo 'chmod 600 /secret' > /dev/vfsctl` is a real ring-3 process
 * making a real request through the real permission check. A syscall would
 * have needed a new coreutil to call it, in a directory this line does not own.
 *
 * The write goes through vfs_write, so the node's own mode is checked first:
 * an unprivileged process cannot write to a 0644 root-owned /dev/vfsctl, which
 * is the same reason it cannot write to any other root-owned file. That is
 * deliberate. The one command whose whole purpose is to LOWER privilege is
 * therefore issued while still privileged, exactly like su(1).
 *
 * COMMANDS
 *   chmod <octal> <path>
 *   chown <uid> <gid> <path>
 *   ln <old> <new>              hard link
 *   ln -s <target> <link>       symbolic link
 *   stat <path>                 mode, uid, gid, nlink, type, size
 *   readlink <path>
 *   fdtest <path>               exercise the open-file-description layer in THIS
 *                               process's fd table: dup shares an offset, and a
 *                               close through one descriptor leaves the other
 *                               usable. See cmd_fdtest in vfsctl.c.
 *   mount ramfs <label> <at>    an in-memory filesystem
 *   mount lfsro <blkdev> <at>   a LogitFS image on another block device, read-only
 *   umount <at>
 *   id                          the calling process's credential, the login
 *                               SESSION, and the supplementary groups. The
 *                               pair (uid, session) is the diagnosis: equal
 *                               numbers mean the process inherited the
 *                               session, which is what every desktop app
 *                               does; a difference means somebody called
 *                               setuid. Since M32 these are real -- see
 *                               SYS_GETUID..SYS_GETSESSION (150-159) and
 *                               /bin/login, which is what sets them.
 *   setuid <uid> [gid]          change THIS process's credential (root only)
 *   su <uid> [gid]              change the PARENT's -- i.e. the shell's. See
 *                               the note in vfsctl.c on why that is the useful
 *                               one and why it is not a hole.
 *
 * The result of the last command is global rather than per-process, which is
 * what makes `echo ... > /dev/vfsctl; cat /dev/vfsctl` work at all: the shell
 * runs each of those in a separate forked child.
 * ------------------------------------------------------------------------ */

/* The kdiag protocol, verbatim: KDIAG_NOT_MINE for a path we do not own, so an
 * unrelated path falls through to the mounted filesystem. */
int         vfsctl_size(const char *path);
int         vfsctl_read(const char *path, void *buf, int max);
int         vfsctl_write(const char *path, const void *buf, int len);
int         vfsctl_dir_count(const char *dir);
const char *vfsctl_dir_name(const char *dir, int i);
int         vfsctl_dir_size(const char *dir, int i);

#endif /* LOGIT_VFSCTL_H */
