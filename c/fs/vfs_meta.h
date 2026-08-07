#ifndef LOGIT_VFS_META_H
#define LOGIT_VFS_META_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * The metadata a filesystem is supposed to keep per file and this one does not
 * yet: owner, group, mode, link count, and a symlink's target.
 *
 * WHERE THIS LIVES, AND WHY IT IS NOT IN THE INODE
 * ------------------------------------------------|
 * It belongs in the inode. It is not there today because c/fs/logitfs.c is
 * being rewritten by the crash-consistency line at the same time as this, and
 * two lines editing one on-disk format is how a format ends up half-bumped. So
 * the VFS keeps the records itself, and `struct filesystem` grew two OPTIONAL
 * ops -- getattr/setattr (see vfs.h). A backend that implements them owns its
 * own metadata and this store is bypassed entirely; a backend that leaves them
 * NULL -- which is every backend today -- gets the VFS store. logitfs can
 * therefore take ownership later by filling in two function pointers, with no
 * other change anywhere, and until it does the semantics are identical except
 * that the records do not survive a reboot.
 *
 * That distinction is stated plainly rather than papered over: permissions
 * here are ENFORCED but not yet PERSISTENT. Enforcement is the part that is
 * worth nothing if it is faked, so it is the part that is real.
 *
 * DEFAULTS FOR A FILE WITH NO RECORD
 * ----------------------------------|
 * root:root, 0755 for directories and 0644 for regular files -- i.e. exactly
 * the mode a freshly unpacked system image has. This is not a fallback that
 * disables checking: an unprivileged process is refused a write to any
 * unrecorded file and refused create/unlink in any unrecorded directory,
 * because 0644/0755 root:root says so. The store therefore only has to hold
 * the EXCEPTIONS, which is why 256 records is a realistic size and not a toy
 * one.
 * ------------------------------------------------------------------------ */

#define VMETA_N       256
#define VMETA_PATH    112
#define VMETA_TARGET  112

#define VT_REG 0
#define VT_DIR 1
#define VT_LNK 2

/* Permission bits, POSIX numbering (so 0644 in a shell command means 0644). */
#define VM_IRUSR 0400
#define VM_IWUSR 0200
#define VM_IXUSR 0100
#define VM_IRGRP 0040
#define VM_IWGRP 0020
#define VM_IXGRP 0010
#define VM_IROTH 0004
#define VM_IWOTH 0002
#define VM_IXOTH 0001

#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4

struct vattr {
    uint32_t mode;      /* permission bits only, 0..0777 */
    uint32_t uid, gid;
    int      type;      /* VT_* */
    int      nlink;
};

struct vcred {
    uint32_t uid, gid;
};

/* --- the store ---------------------------------------------------------- */

void vmeta_reset(void);                                   /* tests */

/* Look up the record for a canonical absolute path. Returns 1 and fills `a`
 * when a record exists; 0 when it does not (the caller applies the defaults
 * via vmeta_attr). */
int  vmeta_lookup(const char *path, struct vattr *a);

/* The attributes to USE for `path`: the record if there is one, otherwise the
 * default for `is_dir`. Always succeeds. */
void vmeta_attr(const char *path, int is_dir, struct vattr *a);

int  vmeta_chmod(const char *path, int is_dir, uint32_t mode);
int  vmeta_chown(const char *path, int is_dir, uint32_t uid, uint32_t gid);

/* Symlinks. vmeta_readlink returns 1 + fills `out` when `path` is a symlink,
 * 0 when it is not. */
int  vmeta_symlink(const char *target, const char *path, const struct vcred *c);
int  vmeta_readlink(const char *path, char *out, int max);

/* Hard links. The backend is path-addressed and has no inode numbers to share,
 * so a "link group" is the VFS's stand-in for an inode: a set of names, one of
 * which (the canonical one) is where the bytes actually sit. Every name in the
 * group reports the same nlink and the same owner/mode, opening any of them
 * opens the canonical one, and unlinking the canonical name while others
 * remain promotes one of them and moves the data -- which is precisely the
 * observable behaviour of a real hard link on a real filesystem.
 *
 * vmeta_link:   register `newpath` as another name for `oldpath`.
 * vmeta_canon:  the name whose bytes to actually operate on (identity for a
 *               path with no group).
 * vmeta_unlink: drop `path`. Returns 1 and fills `promote` when the caller
 *               must MOVE the data from `path` to `promote` because other
 *               names in the group survive; 0 when the data may simply go. */
int  vmeta_link(const char *oldpath, const char *newpath, int is_dir);
const char *vmeta_canon(const char *path);
int  vmeta_nlink(const char *path);
int  vmeta_unlink(const char *path, char *promote, int max);

/* Keep the store consistent when names move or vanish underneath it. */
void vmeta_renamed(const char *from, const char *to);
void vmeta_forget_subtree(const char *prefix);

/* --- the check ---------------------------------------------------------- */

/* The whole permission decision, in one place so there is exactly one of it.
 * `want` is a mask of MAY_*. Returns 0 or VFS_EACCES. */
int  vmeta_permission(const struct vattr *a, const struct vcred *c, int want);

/* Rendered for /dev/vfsmeta. Returns bytes written. */
int  vmeta_render(char *buf, int max);

#endif /* LOGIT_VFS_META_H */
