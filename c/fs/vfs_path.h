#ifndef LOGIT_VFS_PATH_H
#define LOGIT_VFS_PATH_H

/* ---------------------------------------------------------------------------
 * Path resolution, as a self-contained module with no kernel dependencies.
 *
 * This is deliberately its own translation unit rather than a helper inside
 * vfs.c, for one reason: path resolution is where filesystem bugs live. "."
 * and ".." at a root, a symlink that points at itself, a symlink to an
 * absolute path, a name that is exactly one byte too long for the caller's
 * buffer, the empty string -- every one of those is a real CVE in some real
 * kernel, and none of them needs a disk, a process or a boot to exercise. Kept
 * free of kernel headers, the whole walker compiles into a host unit test
 * (tests/unit/vfs_path_test.c) and the code under test is the code that ships.
 *
 * THE RULES, stated rather than implied:
 *
 *   - The result is always absolute, always begins with '/', never ends with
 *     '/' (except the root itself, which is exactly "/").
 *   - The empty path is ENOENT, not "the current directory". POSIX is explicit
 *     about this and the alternative silently turns unlink("") into a request
 *     to unlink the cwd.
 *   - ".." at the root is the root. A path cannot escape upward; there is no
 *     "above /" to escape to, and returning something outside the namespace is
 *     how a chroot becomes decorative.
 *   - ".." is applied to what has ALREADY been resolved, not to the text of the
 *     path. So /a/link/.. where link -> /x/y resolves to /x, not to /a. That is
 *     what Linux does and what every program that follows a symlink then walks
 *     back up expects.
 *   - A result that does not fit the caller's buffer is VFS_ENAMETOOLONG. It is
 *     never truncated. A truncated path is a valid path to a DIFFERENT file,
 *     which is worse than an error by a wide margin.
 *   - Symlink expansion is a bounded loop with a counter, not recursion: a
 *     cycle returns VFS_ELOOP instead of consuming the kernel stack.
 * ------------------------------------------------------------------------ */

/* Errors. Negative, and numerically the POSIX errno values so the eventual
 * errno-returning syscalls do not need a second translation table. */
#define VFS_EPERM         -1
#define VFS_ENOENT        -2
#define VFS_EIO           -5
#define VFS_EBADF         -9
#define VFS_EACCES       -13
#define VFS_EEXIST       -17
#define VFS_EXDEV        -18
#define VFS_ENOTDIR      -20
#define VFS_EISDIR       -21
#define VFS_EINVAL       -22
#define VFS_EMFILE       -24
#define VFS_ENOSPC       -28
#define VFS_EMLINK       -31
#define VFS_ENAMETOOLONG -36
#define VFS_ELOOP        -40
#define VFS_ENOTEMPTY    -39
#define VFS_EBUSY        -16
#define VFS_ENOSYS       -38

/* The longest path the walker will assemble internally. Callers hand in
 * buffers of their own size (128 is what the syscall layer uses); this bounds
 * the scratch space during expansion, which can legitimately be longer than
 * either the input or the result while a symlink target is spliced in. */
#define VFS_PATH_MAX  256
#define VFS_NAME_MAX   60      /* one component; matches the on-disk dirent */
#define VFS_SYMLOOP_MAX 8      /* Linux uses 40 nested; 8 is plenty here and the
                                * bound is what matters, not its size */

/* How the walker talks to the filesystem. Supplied by vfs.c; NULL for a purely
 * lexical normalisation.
 *
 * `visit` is called once for each component as the walk reaches it, with the
 * fully RESOLVED path of that component (`abs`) and whether it is the last one.
 * It answers two questions at once, deliberately: whether the component is a
 * symbolic link, and whether the walk is allowed to continue through it. They
 * are one callback because they happen at the same instant on the same
 * component, and splitting them would mean walking the path twice and checking
 * permission against a directory that is no longer the one that was traversed.
 *
 *   return 1 and fill `link` -> the component is a symlink pointing at `link`
 *          0                 -> an ordinary component; keep walking
 *         <0                 -> abort the whole walk with this error
 *                               (VFS_EACCES from a directory with no search
 *                               permission is the reason this return exists)
 *
 * A `1` on the final component is ignored when the caller asked not to follow
 * it, but the callback is still invoked -- so permission checks happen on
 * every component either way. */
struct vfs_path_env {
    int (*visit)(void *ctx, const char *abs, int is_final, char *link, int max);
    void *ctx;
    int   max_links;            /* 0 -> VFS_SYMLOOP_MAX */
};

/* Purely lexical: join `in` onto `cwd` if relative, collapse "." and "..",
 * squeeze repeated slashes, drop a trailing slash. No filesystem access, so no
 * symlink is followed. `cwd` may be NULL or "" (meaning "/").
 * Returns the length of the result, or a negative VFS_E*. */
int vfs_path_norm(const char *cwd, const char *in, char *out, int max);

/* The full walk: everything vfs_path_norm does, plus symlink expansion through
 * `env`. `follow_final` selects whether a symlink named by the LAST component
 * is followed -- 0 is the lstat/unlink/symlink behaviour (act on the link
 * itself), 1 is the open/stat behaviour (act on what it points at).
 * Returns the length of the result, or a negative VFS_E*. */
int vfs_path_resolve(const struct vfs_path_env *env, const char *cwd,
                     const char *in, char *out, int max, int follow_final);

/* Split a canonical absolute path into its parent directory and final
 * component. "/" has no final component -> VFS_EINVAL. Used by every operation
 * that must check write permission on the containing directory. */
int vfs_path_split(const char *abs, char *dir, int dirmax, char *name, int namemax);

/* 1 if `pfx` is `path` itself or a proper prefix of it at a component
 * boundary. "/mnt" is a prefix of "/mnt/x" but not of "/mnttab". This is the
 * mount-point test, and getting the boundary wrong is how a mount at /mnt
 * swallows /mnttab. */
int vfs_path_is_prefix(const char *pfx, const char *path);

#endif /* LOGIT_VFS_PATH_H */
