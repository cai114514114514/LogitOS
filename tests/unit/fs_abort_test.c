/* A rejected LogitFS transaction must leave this mount usable, not merely
 * recover after a remount. The small, otherwise valid journal makes staging
 * fail after directory or inode data has already changed in RAM. */
#define LOGITFS_SIM_IDENTITIES
#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"

static const char *operation;
static int identities;

static void check(int valid, const char *what)
{
    fs_ok(valid, "%s v%d: %s", operation, identities ? 5 : 4, what);
}

static void require(int valid, const char *what)
{
    check(valid, what);
    if (!valid) exit(2); /* Fixture failure is not a rollback verdict. */
}

static int kept_is_old(void)
{
    char bytes[4] = {0};
    return logitfs.size("/kept") == 3 &&
           logitfs.read("/kept", bytes, sizeof bytes) == 3 &&
           memcmp(bytes, "old", 3) == 0;
}

static void check_attrs(const char *path, const struct vattr *saved)
{
    struct vattr now;
    int found = logitfs.getattr(path, &now) == 0;
    check(found && now.ino == saved->ino && now.mtime == saved->mtime &&
          now.ctime == saved->ctime, "refusal preserves inode and timestamps");
    if (identities)
        check(found && now.object_id == saved->object_id &&
              now.revision == saved->revision &&
              now.volume[0] == saved->volume[0] &&
              now.volume[1] == saved->volume[1],
              "refusal preserves object identity and revision");
}

static void run_case(const char *name, int with_id)
{
    operation = name;
    identities = with_id;
    sim_open_n(128);
    if (!with_id)
        ((struct lfs_super *)(void *)sim_media)->version = LFS_VERSION;
    require(logitfs.mount() == 0, "mount fixture");
    require(logitfs.write("/kept", "old", 3) == 3, "write existing file");
    require(logitfs_sync() == 0, "sync fixture");

    /* Re-mount with a legal journal too small for this operation. The data
     * area is unchanged; the failure occurs during log_add, before commit. */
    logitfs_unmount();
    struct lfs_super *super = (struct lfs_super *)(void *)sim_media;
    super->log_blocks = !strcmp(name, "overwrite") ? 2 : 4;
    require(logitfs.mount() == 0, "mount reduced journal");

    struct vattr root, kept;
    require(logitfs.getattr("/", &root) == 0, "capture root attributes");
    require(logitfs.getattr("/kept", &kept) == 0, "capture file attributes");
    fsstub_clock += 7;
    int rc;
    if (!strcmp(name, "overwrite")) rc = logitfs.write("/kept", "new", 3);
    else if (!strcmp(name, "create")) rc = logitfs.write("/new", "data", 4);
    else if (!strcmp(name, "mkdir")) rc = logitfs.mkdir("/newdir");
    else rc = logitfs.del("/kept");
    check(rc < 0, "staging refuses before commit");

    check(kept_is_old(), "same mount retains the old file bytes");
    check(logitfs.size("/new") < 0 && logitfs.count("/newdir") < 0,
          "same mount exposes no rejected entry");
    check(logitfs.count("/") == 1, "same mount retains root contents");
    check_attrs("/", &root);
    check_attrs("/kept", &kept);
    check(logitfs_sync() == 0, "aborted transaction leaves cache flushable");

    logitfs_unmount();
    require(logitfs.mount() == 0, "remount after refusal");
    check(kept_is_old(), "remount retains the old file bytes");
    check(logitfs_fsck(0) == 0, "durable filesystem remains clean");
    logitfs_unmount();
    sim_close();
}

int main(void)
{
    const char *ops[] = {"overwrite", "create", "mkdir", "delete"};
    for (int version = 0; version < 2; version++)
        for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
            run_case(ops[i], version);
    return fs_verdict("fs_abort_test");
}
