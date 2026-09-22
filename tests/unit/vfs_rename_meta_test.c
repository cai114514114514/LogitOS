/* Real VFS + RAMFS operations: moving bytes is not enough if their permissions
 * and link identities stay under the old name. Only the current credential and
 * synthetic control files are host stubs; no filesystem operation is replaced. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfs.h"
#include "ramfs.h"

static int checks, failures;
static struct filesystem *rootfs, *nested;
static struct vcred caller;

void vfs_cred_current(struct vcred *cred) { *cred = caller; }
int vfsctl_size(const char *path) { (void)path; return -2; }
int vfsctl_read(const char *path, void *buf, int max)
{ (void)path; (void)buf; (void)max; return -2; }
int vfsctl_write(const char *path, const void *buf, int size)
{ (void)path; (void)buf; (void)size; return -2; }
int vfsctl_dir_count(const char *path) { (void)path; return -2; }
const char *vfsctl_dir_name(const char *path, int index)
{ (void)path; (void)index; return NULL; }
int vfsctl_dir_size(const char *path, int index)
{ (void)path; (void)index; return -2; }

static void expect(int valid, const char *name)
{
    checks++;
    if (!valid) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static void number(int actual, int wanted, const char *name)
{
    checks++;
    if (actual != wanted) {
        failures++;
        printf("FAIL: %s (got %d, want %d)\n", name, actual, wanted);
    }
}

static void content(const char *path, const char *wanted, const char *name)
{
    char bytes[64] = {0};
    int size = vfs_read(path, bytes, sizeof bytes - 1);
    expect(size == (int)strlen(wanted) && !strcmp(bytes, wanted), name);
}

static void attributes(const char *path, unsigned mode, unsigned uid, unsigned gid,
                       int links, const char *name)
{
    struct vattr attr;
    int rc = vfs_stat(path, &attr);
    expect(rc == 0 && attr.mode == mode && attr.uid == uid && attr.gid == gid &&
           attr.nlink == links && (attr.flags & VA_STORED), name);
}

static void cleanup(void)
{
    caller = (struct vcred){0, 0};
    if (nested) {
        number(vfs_umount("/tree/mounted"), 0, "unmount nested fixture");
        ramfs_destroy(nested);
        nested = NULL;
    }
    if (rootfs) {
        number(vfs_umount("/"), 0, "unmount root fixture");
        ramfs_destroy(rootfs);
        rootfs = NULL;
    }
}

static void fixture(void)
{
    cleanup();
    vmeta_reset();
    rootfs = ramfs_create("rename-meta");
    if (!rootfs) exit(2);
    vfs_register(rootfs);
    if (vfs_mount() < 0) exit(2);
}

static void descendants(void)
{
    fixture();
    number(vfs_mkdir("/tree"), 0, "make source directory without a metadata record");
    number(vfs_mkdir("/tree/branch"), 0, "make nested directory");
    number(vfs_write("/tree/branch/leaf", "secret", 6), 6, "write nested file");
    number(vfs_chmod("/tree/branch", 0710), 0, "set nested directory mode");
    number(vfs_chown("/tree/branch", 1000, 2000), 0, "set nested directory owner");
    number(vfs_chmod("/tree/branch/leaf", 0600), 0, "set nested file mode");
    number(vfs_chown("/tree/branch/leaf", 1000, 2000), 0, "set nested file owner");
    number(vfs_mkdir("/treeish"), 0, "make prefix sibling");
    number(vfs_chmod("/treeish", 0701), 0, "set prefix sibling mode");
    number(vfs_rename("/tree", "/longer-tree"), 0, "rename metadata-bearing descendants");
    attributes("/longer-tree/branch", 0710, 1000, 2000, 1, "descendant directory metadata follows rename");
    attributes("/longer-tree/branch/leaf", 0600, 1000, 2000, 1, "descendant file metadata follows rename");
    attributes("/treeish", 0701, 0, 0, 1, "prefix sibling metadata is unchanged");
    caller = (struct vcred){1000, 2000};
    content("/longer-tree/branch/leaf", "secret", "original owner still reads moved private file");
    caller = (struct vcred){3000, 3000};
    char buf[8];
    number(vfs_read("/longer-tree/branch/leaf", buf, sizeof buf), VFS_EACCES,
           "rename does not expose private descendants");
    caller = (struct vcred){0, 0};
    number(vfs_rename("/longer-tree", "/t"), 0, "rename metadata to a shorter prefix");
    attributes("/t/branch/leaf", 0600, 1000, 2000, 1, "shorter rename keeps suffix and attributes");
    number(vfs_mkdir("/tree"), 0, "reuse old directory name");
    number(vfs_mkdir("/tree/branch"), 0, "reuse old child name");
    number(vfs_write("/tree/branch/leaf", "new", 3), 3, "reuse old leaf name");
    struct vattr attr;
    number(vfs_stat("/tree/branch/leaf", &attr), 0, "stat reused leaf");
    expect(attr.uid == 0 && attr.gid == 0 && attr.mode == 0644 && !(attr.flags & VA_STORED),
           "old names do not inherit moved metadata");
}

static void links(void)
{
    fixture();
    number(vfs_mkdir("/tree"), 0, "make link directory");
    number(vfs_write("/tree/file", "inside", 6), 6, "write canonical file inside tree");
    expect(vfs_link("/tree/file", "/outside-alias") > 0, "hard link outside the moved tree");
    expect(vfs_link("/tree/file", "/tree/alias") > 0, "hard link inside the moved tree");
    number(vfs_chmod("/tree/file", 0600), 0, "set linked file mode");
    number(vfs_chown("/tree/file", 1200, 1300), 0, "set linked file owner");
    number(vfs_write("/outside-file", "outside", 7), 7, "write canonical file outside tree");
    expect(vfs_link("/outside-file", "/tree/external") > 0, "hard link into the moved tree");
    number(vfs_symlink("file", "/tree/relative"), 0, "relative symlink inside tree");
    number(vfs_symlink("/tree/file", "/tree/absolute"), 0, "absolute symlink inside tree");
    number(vfs_symlink("/tree/file", "/outside-symbolic"), 0, "external symbolic reference");
    number(vfs_rename("/tree", "/moved"), 0, "rename tree containing both kinds of links");
    content("/outside-alias", "inside", "external hard link follows moved canonical record");
    content("/moved/alias", "inside", "internal hard link follows moved canonical record");
    content("/moved/external", "outside", "moved hard link retains outside canonical record");
    attributes("/moved/file", 0600, 1200, 1300, 3, "hard link attributes and count survive rename");
    content("/moved/relative", "inside", "moved relative symlink resolves against new parent");
    char target[VMETA_TARGET] = {0};
    number(vfs_readlink("/moved/relative", target, sizeof target), 4, "read moved relative symlink");
    expect(!strcmp(target, "file"), "relative symlink target remains literal");
    number(vfs_readlink("/moved/absolute", target, sizeof target), 10, "read moved absolute symlink");
    expect(!strcmp(target, "/tree/file"), "absolute symlink target remains literal");
    expect(vfs_size("/moved/absolute") < 0, "absolute symlink becomes dangling after directory move");
    expect(vfs_size("/outside-symbolic") < 0, "outside symbolic target is not rewritten");
    number(vfs_readlink("/tree/relative", target, sizeof target), VFS_EINVAL,
           "old symbolic name is gone");
    number(vfs_delete("/moved/file"), 0, "unlink moved hard link canonical name");
    content("/outside-alias", "inside", "canonical promotion keeps external alias bytes");
    content("/moved/alias", "inside", "canonical promotion keeps internal alias bytes");
    number(vfs_chmod("/moved/alias", 0640), 0, "chmod moved hard link alias");
    attributes("/outside-alias", 0640, 1200, 1300, 2, "hard link group still shares metadata");
}

static void length_limits(void)
{
    fixture();
    number(vfs_mkdir("/a"), 0, "make length fixture");
    number(vfs_write("/a/file", "kept", 4), 4, "write length fixture file");
    number(vfs_chmod("/a/file", 0600), 0, "record length fixture attributes");
    char leaf[VFS_NAME_MAX + 1];
    memset(leaf, 's', sizeof leaf - 1);
    leaf[sizeof leaf - 1] = 0;
    char source[VFS_PATH_MAX], destination[VFS_PATH_MAX], moved[VFS_PATH_MAX];
    snprintf(source, sizeof source, "/a/%s", leaf);
    number(vfs_symlink("file", source), 0, "make long metadata-only descendant");
    destination[0] = '/';
    memset(destination + 1, 'd', VFS_NAME_MAX);
    destination[VFS_NAME_MAX + 1] = 0;
    /* RAMFS's physical descendants fit. Only the VFS-owned symlink exceeds
     * VMETA_PATH, so a backend refusal cannot accidentally make this pass. */
    number(vfs_rename("/a", destination), VFS_ENAMETOOLONG,
           "metadata path overflow is refused before backend rename");
    content("/a/file", "kept", "metadata length refusal leaves original bytes");
    attributes("/a/file", 0600, 0, 0, 1, "metadata length refusal leaves attributes");
    char target[VMETA_TARGET] = {0};
    number(vfs_readlink(source, target, sizeof target), 4, "metadata length refusal leaves symlink");
    expect(vfs_count(destination) < 0, "metadata length refusal creates no destination");

    int prefix_length = VMETA_PATH - 2 - VFS_NAME_MAX;
    destination[prefix_length] = 0;
    number(vfs_rename("/a", destination), 0, "metadata path exactly at capacity is accepted");
    snprintf(moved, sizeof moved, "%s/%s", destination, leaf);
    number((int)strlen(moved), VMETA_PATH - 1, "fixture reaches the exact metadata path limit");
    number(vfs_readlink(moved, target, sizeof target), 4, "maximum length moved symlink survives");
    content(moved, "kept", "maximum length moved symlink resolves");
}

static void full_store(void)
{
    fixture();
    number(vfs_mkdir("/tree"), 0, "make full-store source");
    number(vfs_write("/tree/file", "full", 4), 4, "write full-store source");
    number(vfs_chmod("/tree/file", 0600), 0, "record full-store source mode");
    number(vfs_mkdir("/pool"), 0, "make metadata-only link pool");
    int added = 0, result = 0;
    for (; added <= VMETA_N; added++) {
        char path[64];
        snprintf(path, sizeof path, "/pool/link%d", added);
        result = vfs_symlink("/missing", path);
        if (result < 0) break;
    }
    number(result, VFS_ENOSPC, "fixture fills metadata capacity");
    number(added, VMETA_N - 1, "only the source record used a slot before filling");
    number(vfs_rename("/tree", "/moved"), 0, "full store rename needs no extra records");
    attributes("/moved/file", 0600, 0, 0, 1, "full store retains descendant attributes");
    content("/moved/file", "full", "full store retains descendant bytes");
}

static void refusals(void)
{
    fixture();
    number(vfs_mkdir("/tree"), 0, "make refusal source");
    number(vfs_write("/tree/file", "source", 6), 6, "write refusal source");
    number(vfs_chmod("/tree/file", 0600), 0, "record refusal source mode");
    number(vfs_mkdir("/occupied"), 0, "make occupied destination");
    expect(vfs_rename("/tree", "/occupied") < 0, "backend refusal is preserved");
    attributes("/tree/file", 0600, 0, 0, 1, "backend refusal leaves source metadata");
    number(vfs_symlink("/missing", "/symbolic"), 0, "make metadata-only destination");
    number(vfs_rename("/tree", "/symbolic"), VFS_EEXIST,
           "metadata-only destination cannot be clobbered");
    content("/tree/file", "source", "metadata collision leaves source bytes");
    attributes("/tree/file", 0600, 0, 0, 1, "metadata collision leaves source attributes");

    fixture();
    number(vfs_mkdir("/tree"), 0, "make mounted-subtree source");
    number(vfs_mkdir("/tree/mounted"), 0, "make nested mountpoint");
    nested = ramfs_create("nested");
    if (!nested) exit(2);
    number(vfs_mount_at("/tree/mounted", nested), 0, "mount separate subtree");
    number(vfs_write("/tree/mounted/file", "nested", 6), 6, "write mounted file");
    number(vfs_chmod("/tree/mounted/file", 0600), 0, "record mounted file mode");
    number(vfs_rename("/tree", "/other"), VFS_EBUSY,
           "rename cannot relocate another mount's metadata");
    content("/tree/mounted/file", "nested", "mounted descendant remains reachable");
    attributes("/tree/mounted/file", 0600, 0, 0, 1, "mounted descendant retains metadata");
}

int main(void)
{
    descendants();
    links();
    length_limits();
    full_store();
    refusals();
    cleanup();
    printf("vfs-rename-meta: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
