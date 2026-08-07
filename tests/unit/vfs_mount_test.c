/* Host unit tests for the VFS layer itself: the mount table, the permission
 * checks, and links.
 *
 * This links the REAL c/fs/vfs.c, c/fs/vfs_meta.c, c/fs/vfs_path.c and
 * c/fs/ramfs.c. Only the two things that genuinely need a kernel are stubbed:
 * the kdiag synthetic files and the credential of "the current process", which
 * here is a variable a test can set. So the code deciding every permission
 * below is the code that decides them on the machine -- the point of keeping
 * vfs.c free of kernel headers.
 *
 * The device tests (tests/boot/run-vfs-test.sh) then prove the same refusals
 * happen to a real ring-3 process. Both are needed and neither replaces the
 * other: this one can enumerate cases, that one can prove they are reached.
 */
#include <stdio.h>
#include <string.h>

#include "vfs.h"
#include "vfs_path.h"
#include "vfs_meta.h"
#include "ramfs.h"

static int checks, failures;
static void ok(int cond, const char *what)
{ checks++; if (!cond) { failures++; printf("  FAIL: %s\n", what); } }
static void eqi(int got, int want, const char *what)
{ checks++; if (got != want) { failures++; printf("  FAIL: %s (got %d, want %d)\n", what, got, want); } }
static void eqs(const char *got, const char *want, const char *what)
{ checks++; if (strcmp(got, want)) { failures++; printf("  FAIL: %s (got \"%s\", want \"%s\")\n", what, got, want); } }

/* --- the stubs ----------------------------------------------------------- */

static struct vcred g_cred = { 0, 0 };
void vfs_cred_current(struct vcred *c) { *c = g_cred; }
static void as_root(void)            { g_cred.uid = 0; g_cred.gid = 0; }
static void as_user(unsigned u, unsigned g) { g_cred.uid = u; g_cred.gid = g; }

/* kdiag and vfsctl: own nothing here. KDIAG_NOT_MINE is -2. */
int kdiag_size(const char *p) { (void)p; return -2; }
int kdiag_read(const char *p, void *b, int m) { (void)p; (void)b; (void)m; return -2; }
int kdiag_write(const char *p, const void *b, int n) { (void)p; (void)b; (void)n; return -2; }
int kdiag_dir_count(const char *d) { (void)d; return -2; }
const char *kdiag_dir_name(const char *d, int i) { (void)d; (void)i; return 0; }
int kdiag_dir_size(const char *d, int i) { (void)d; (void)i; return -2; }
int vfsctl_size(const char *p) { (void)p; return -2; }
int vfsctl_read(const char *p, void *b, int m) { (void)p; (void)b; (void)m; return -2; }
int vfsctl_write(const char *p, const void *b, int n) { (void)p; (void)b; (void)n; return -2; }
int vfsctl_dir_count(const char *d) { (void)d; return -2; }
const char *vfsctl_dir_name(const char *d, int i) { (void)d; (void)i; return 0; }
int vfsctl_dir_size(const char *d, int i) { (void)d; (void)i; return -2; }

/* --- fixture ------------------------------------------------------------- */

static struct filesystem *rootfs, *tmpfs;

/* Start from nothing. The tests run in one process, so the mount table, the
 * metadata store and the ramfs instance pool all persist between them -- and a
 * fixture that leaks instances quietly runs the later tests against a NULL
 * filesystem, where every operation fails for a reason that has nothing to do
 * with what is being tested. (It did, once. That is why this destroys.) */
static void fixture(void)
{
    as_root();                                  /* umount is not a user operation */
    while (vfs_mount_count()) {
        char all[512], deepest[128] = "";
        vfs_mounts_render(all, sizeof all);
        for (char *line = strtok(all, "\n"); line; line = strtok(NULL, "\n")) {
            char *s = strchr(line, ' ');
            if (s && strlen(s + 1) > strlen(deepest)) snprintf(deepest, sizeof deepest, "%s", s + 1);
        }
        if (!deepest[0] || vfs_umount(deepest) < 0) break;
    }
    if (rootfs) ramfs_destroy(rootfs);
    if (tmpfs)  ramfs_destroy(tmpfs);
    vmeta_reset();
    rootfs = ramfs_create("rootfs");
    tmpfs  = NULL;
    ok(rootfs != NULL, "fixture: a filesystem instance is available");
    vfs_register(rootfs);
    vfs_mount();
}

static void mkfile(const char *path, const char *body)
{ vfs_write(path, body, (int)strlen(body)); }

static const char *slurp(const char *path)
{
    static char b[256];
    int n = vfs_read(path, b, (int)sizeof b - 1);
    if (n < 0) { snprintf(b, sizeof b, "E%d", n); return b; }
    b[n] = 0;
    return b;
}

/* --------------------------------------------------------------------------
 * 1. The mount table
 * ------------------------------------------------------------------------ */
static void t_mount_table(void)
{
    printf("the mount table\n");
    fixture();
    eqi(vfs_mount_count(), 1, "the root is MOUNTED, not assumed");

    mkfile("/hello", "root filesystem");
    eqs(slurp("/hello"), "root filesystem", "a file on the root filesystem");

    eqi(vfs_mkdir("/mnt"), 0, "a mount point has to exist first");
    tmpfs = ramfs_create("tmpfs");
    ok(tmpfs != NULL, "a second ramfs instance is a DIFFERENT filesystem");
    eqi(vfs_mount_at("/mnt", tmpfs), 0, "mounting a second filesystem");
    eqi(vfs_mount_count(), 2, "two filesystems are mounted");

    mkfile("/mnt/hello", "second filesystem");
    eqs(slurp("/mnt/hello"), "second filesystem", "a file read from the SECOND filesystem");
    eqs(slurp("/hello"), "root filesystem", "and the first one still answers for its own");

    /* The dispatch is what is being tested: the backend must be handed the
     * path WITHOUT the mount point, or it would look for "/mnt/hello" in a
     * filesystem whose root is the mount point. */
    struct filesystem *fs = tmpfs;
    eqi(fs->iops->size(fs, "/hello"), 17, "the backend sees /hello, not /mnt/hello");
    eqi(fs->iops->size(fs, "/mnt/hello"), -1, "and does NOT see /mnt/hello");

    /* Mount-point boundaries. */
    mkfile("/mnttab", "not in the mount");
    eqs(slurp("/mnttab"), "not in the mount", "/mnttab is not swallowed by the mount at /mnt");

    /* Crossing out of a mount with "..". */
    eqs(slurp("/mnt/../hello"), "root filesystem", "\"..\" out of a mount reaches the parent fs");
    eqs(slurp("/mnt/../mnt/hello"), "second filesystem", "and back in again");

    eqi(vfs_mount_at("/mnt", rootfs), VFS_EBUSY, "mounting twice on one point is EBUSY");
    eqi(vfs_mount_at("/nosuch", rootfs), VFS_ENOENT, "a mount point that does not exist is ENOENT");
    eqi(vfs_mount_at("/mnttab", rootfs), VFS_ENOTDIR, "a mount point that is a FILE is ENOTDIR");
    eqi(vfs_delete("/mnt"), VFS_EBUSY, "a mount point cannot be unlinked while mounted");
    eqi(vfs_rename("/mnt/hello", "/hello2"), VFS_EXDEV, "rename across a mount is EXDEV");

    eqi(vfs_umount("/mnt"), 0, "unmounting");
    eqi(vfs_mount_count(), 1, "back to one");
    eqi(vfs_size("/mnt/hello"), -1, "and what was under it is gone from the namespace");
}

/* --------------------------------------------------------------------------
 * 2. Permissions -- refusals, each paired with the permitted case
 * ------------------------------------------------------------------------ */
static void t_permissions(void)
{
    printf("permission enforcement\n");
    fixture();
    mkfile("/secret", "classified");
    mkfile("/public", "readable");
    eqi(vfs_chmod("/secret", 0600), 0, "chmod 600 as root");

    as_root();
    eqs(slurp("/secret"), "classified", "root reads a 0600 root-owned file");

    as_user(1000, 1000);
    eqi(vfs_read("/secret", (char[8]){0}, 8), VFS_EACCES, "uid 1000 is REFUSED a 0600 root file");
    eqs(slurp("/public"), "readable", "and is allowed the 0644 one -- the check is not a blanket no");

    /* Writing. The default 0644 root:root already refuses a non-root write,
     * which is why the store only has to hold exceptions. */
    eqi(vfs_write("/public", "x", 1), VFS_EACCES, "uid 1000 cannot write a 0644 root file");
    as_root();
    eqi(vfs_write("/public", "y", 1), 1, "root can");

    /* Creating and deleting: the containing directory's mode decides. */
    as_user(1000, 1000);
    eqi(vfs_write("/newfile", "x", 1), VFS_EACCES, "uid 1000 cannot create in a 0755 root dir");
    eqi(vfs_delete("/public"), VFS_EACCES, "and cannot unlink from it");
    eqi(vfs_mkdir("/newdir"), VFS_EACCES, "and cannot mkdir in it");

    as_root();
    eqi(vfs_mkdir("/home"), 0, "root makes /home");
    eqi(vfs_chown("/home", 1000, 1000), 0, "and gives it to uid 1000");
    as_user(1000, 1000);
    eqi(vfs_write("/home/mine", "hi", 2), 2, "uid 1000 CAN create in a directory it owns");
    eqs(slurp("/home/mine"), "hi", "and read it back");
    eqi(vfs_delete("/home/mine"), 0, "and unlink it");

    /* Search permission on an intervening directory. */
    as_root();
    eqi(vfs_mkdir("/private"), 0, "root makes /private");
    mkfile("/private/data", "inside");
    eqi(vfs_chmod("/private", 0700), 0, "chmod 700 /private");
    as_user(1000, 1000);
    eqi(vfs_read("/private/data", (char[8]){0}, 8), VFS_EACCES,
        "uid 1000 is refused by the DIRECTORY, not the file");
    eqi(vfs_count("/private"), VFS_EACCES, "and cannot list it");
    as_root();
    eqs(slurp("/private/data"), "inside", "root walks through it");

    /* Group. */
    as_root();
    mkfile("/grouped", "team");
    eqi(vfs_chmod("/grouped", 0640), 0, "chmod 640");
    eqi(vfs_chown("/grouped", 0, 50), 0, "group 50");
    as_user(1000, 50);
    eqs(slurp("/grouped"), "team", "a member of the group may read");
    as_user(1000, 51);
    eqi(vfs_read("/grouped", (char[8]){0}, 8), VFS_EACCES, "a non-member may not");

    /* Who may change the mode. */
    as_user(1000, 1000);
    eqi(vfs_chmod("/grouped", 0777), VFS_EPERM, "a non-owner cannot chmod");
    eqi(vfs_chown("/grouped", 1000, 1000), VFS_EPERM, "and cannot chown -- root only");
    as_root();
    eqi(vfs_chown("/grouped", 1000, 1000), 0, "root can give it away");
    as_user(1000, 1000);
    eqi(vfs_chmod("/grouped", 0777), 0, "and then the new owner can chmod it");
}

/* --------------------------------------------------------------------------
 * 3. Links
 * ------------------------------------------------------------------------ */
static void t_hard_links(void)
{
    printf("hard links\n");
    fixture();
    mkfile("/a", "shared bytes");
    eqi(vfs_nlink("/a"), 1, "a fresh file has one link");

    eqi(vfs_link("/a", "/b"), 2, "link /a /b -> nlink 2");
    eqi(vfs_nlink("/a"), 2, "reported through the first name");
    eqi(vfs_nlink("/b"), 2, "and through the second");
    eqs(slurp("/b"), "shared bytes", "the second name reads the same bytes");

    /* One file, two names: a change through one is visible through the other.
     * This is the property that separates a hard link from a copy. */
    vfs_write("/b", "rewritten", 9);
    eqs(slurp("/a"), "rewritten", "a write through /b is visible through /a");

    /* And so is a chmod, because it is one file. */
    eqi(vfs_chmod("/b", 0600), 0, "chmod through /b");
    struct vattr at;
    eqi(vfs_stat("/a", &at), 0, "stat /a");
    eqi((int)at.mode, 0600, "the mode changed for /a too");

    eqi(vfs_link("/a", "/c"), 3, "a third name");
    eqi(vfs_nlink("/c"), 3, "nlink 3");

    /* Unlinking a name that is NOT where the bytes sit. */
    eqi(vfs_delete("/c"), 0, "unlink /c");
    eqi(vfs_nlink("/a"), 2, "nlink back to 2");
    eqs(slurp("/a"), "rewritten", "the bytes are untouched");

    /* Unlinking the name that IS where the bytes sit: the data must survive
     * under the surviving name. This is the case a naive implementation gets
     * wrong, and it deletes the file. */
    eqi(vfs_delete("/a"), 0, "unlink /a -- the name holding the bytes");
    eqi(vfs_size("/a"), -1, "the name is gone");
    eqi(vfs_nlink("/b"), 1, "nlink back to 1");
    eqs(slurp("/b"), "rewritten", "and the CONTENT SURVIVED under /b");

    eqi(vfs_delete("/b"), 0, "unlink the last name");
    eqi(vfs_size("/b"), -1, "now the file is really gone");

    /* Refusals. */
    mkfile("/x", "x");
    eqi(vfs_mkdir("/d"), 0, "a directory");
    eqi(vfs_link("/d", "/d2"), VFS_EPERM, "no hard links to directories");
    eqi(vfs_link("/x", "/x"), VFS_EEXIST, "linking onto an existing name is EEXIST");
    eqi(vfs_link("/nope", "/y"), VFS_ENOENT, "linking a file that does not exist");
}

static void t_sym_links(void)
{
    printf("symbolic links\n");
    fixture();
    mkfile("/target", "pointed at");
    eqi(vfs_symlink("/target", "/link"), 0, "symlink /link -> /target");
    eqs(slurp("/link"), "pointed at", "reading through the link");

    char t[128];
    eqi(vfs_readlink("/link", t, sizeof t), 7, "readlink returns the target length");
    eqs(t, "/target", "and the target");

    struct vattr a;
    eqi(vfs_lstat("/link", &a), 0, "lstat the link");
    eqi(a.type, VT_LNK, "lstat sees the link itself");
    eqi(vfs_stat("/link", &a), 0, "stat the link");
    eqi(a.type, VT_REG, "stat sees what it points at");

    /* A relative target, and a link through a directory. */
    eqi(vfs_mkdir("/dir"), 0, "a directory");
    mkfile("/dir/file", "in dir");
    eqi(vfs_symlink("dir", "/d"), 0, "a relative symlink to a directory");
    eqs(slurp("/d/file"), "in dir", "resolving through it");

    /* A dangling link is not an error until it is followed. */
    eqi(vfs_symlink("/nowhere", "/dangling"), 0, "a dangling symlink can be created");
    eqi(vfs_readlink("/dangling", t, sizeof t), 8, "and read");
    eqi(vfs_size("/dangling"), -1, "but resolves to nothing");

    /* A cycle is refused rather than followed. */
    eqi(vfs_symlink("/loop2", "/loop1"), 0, "loop1 -> loop2");
    eqi(vfs_symlink("/loop1", "/loop2"), 0, "loop2 -> loop1");
    eqi(vfs_size("/loop1"), VFS_ELOOP, "following the cycle is ELOOP, not a hang");
    eqi(vfs_read("/loop1", t, sizeof t), VFS_ELOOP, "and so is reading it");

    /* Unlinking a symlink removes the link, not the target. */
    eqi(vfs_delete("/link"), 0, "unlink the symlink");
    eqi(vfs_size("/link"), -1, "the link is gone");
    eqs(slurp("/target"), "pointed at", "the target is not");

    eqi(vfs_symlink("/target", "/target"), VFS_EEXIST, "a symlink cannot shadow an existing name");
}

/* --------------------------------------------------------------------------
 * 4. Symlinks and permissions together -- the case where a symlink is used to
 *    reach something the caller may not otherwise reach.
 * ------------------------------------------------------------------------ */
static void t_symlink_permission(void)
{
    printf("a symlink does not launder a permission\n");
    fixture();
    eqi(vfs_mkdir("/private"), 0, "root makes /private");
    mkfile("/private/data", "secret");
    eqi(vfs_chmod("/private", 0700), 0, "chmod 700");
    eqi(vfs_symlink("/private/data", "/shortcut"), 0, "a symlink pointing inside it");
    eqi(vfs_chmod("/private/data", 0666), 0, "and the file itself made world-readable");

    as_user(1000, 1000);
    eqi(vfs_read("/shortcut", (char[8]){0}, 8), VFS_EACCES,
        "uid 1000 is still refused: the check follows the RESOLVED path");
    as_root();
}

int main(void)
{
    t_mount_table();
    t_permissions();
    t_hard_links();
    t_sym_links();
    t_symlink_permission();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
