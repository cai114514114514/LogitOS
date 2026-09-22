/* A failed rename must restore the currently mounted filesystem, not merely
 * recover its old disk state after reboot. Exercise the real backend and cache
 * on a volatile simulated device, including allocation and log-space failures. */
#define LOGITFS_SIM_IDENTITIES
#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"

static int allocation_budget = -1;
static int live_allocations;
static char case_name[80];
static const char *source, *target;
static unsigned char *payload;
static int payload_size;
static const char *parents[2];
static int padded[2], parent_count;

struct identity_snapshot {
    const char *path;
    struct vattr attr;
};

void *kmalloc(size_t size)
{
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    void *pointer = malloc(size);
    if (pointer) live_allocations++;
    return pointer;
}

void kfree(void *pointer)
{
    if (pointer) live_allocations--;
    free(pointer);
}

static void check(int valid, const char *name)
{
    fs_ok(valid, "%s: %s", case_name, name);
}

static unsigned free_blocks(void)
{
    struct lfs_super super;
    sim_read_super(&super);
    const uint8_t *map = sim_media + super.bitmap_start * LFS_BS;
    unsigned count = 0;
    for (unsigned block = super.data_start; block < super.total_blocks; block++)
        if (!(map[block >> 3] & (1u << (block & 7)))) count++;
    return count;
}

static int bytes_equal(const char *path, const void *wanted, int size)
{
    unsigned char *actual = malloc((size_t)size + 1);
    if (!actual) exit(2);
    int got = logitfs.read(path, actual, size);
    int equal = got == size && !memcmp(actual, wanted, (size_t)size);
    free(actual);
    return equal;
}

static void require(int valid, const char *name)
{
    check(valid, name);
    if (!valid) exit(2);                    /* apparatus failure is not a control verdict */
}

static void set_log_capacity(unsigned blocks)
{
    logitfs_unmount();
    /* This is a legal smaller journal on the same image; the data area stays
     * put. Fixture creation used the normal capacity, then a short journal
     * forces flush_inode/flush_bitmap to refuse before log_commit starts. */
    struct lfs_super *super = (struct lfs_super *)(void *)sim_media;
    super->log_blocks = blocks;
    require(logitfs.mount() == 0, "mount requested journal capacity");
}

static void pad_path(char *path, int capacity, const char *parent, int index)
{
    snprintf(path, (size_t)capacity, "%s/pad%d", !strcmp(parent, "/") ? "" : parent, index);
}

static void check_padding(void)
{
    for (int parent = 0; parent < parent_count; parent++) {
        int intact = 1;
        for (int i = 0; i < padded[parent]; i++) {
            char path[96];
            pad_path(path, sizeof path, parents[parent], i);
            if (logitfs.size(path) != 0) intact = 0;
        }
        check(intact, "all padding entries retain their original names and sizes");
    }
}

static void expand_inode_table(void)
{
    /* The default simulator has fewer inodes than one block of live dirents.
     * Expand its EMPTY table before mounting; retain fs_sim's v5 root and volume
     * initialization, and move only the still-empty journal/data boundary. */
    struct lfs_super *super = (struct lfs_super *)(void *)sim_media;
    unsigned entries = LFS_BS / LFS_DIRENT_SZ;
    unsigned inodes = 2 * (entries + 1) + 8;
    unsigned blocks = (inodes + LFS_IPB - 1) / LFS_IPB;
    unsigned extra = blocks - super->inode_blocks;
    unsigned old_start = super->data_start;
    super->inode_count = blocks * LFS_IPB;
    super->inode_blocks = blocks;
    super->log_start += extra;
    super->data_start += extra;
    uint8_t *map = sim_media + super->bitmap_start * LFS_BS;
    for (unsigned block = old_start; block < super->data_start; block++)
        map[block >> 3] |= (uint8_t)(1u << (block & 7));
}

static void capture_identity(struct identity_snapshot *saved, const char *path)
{
    saved->path = path;
    require(logitfs.getattr(path, &saved->attr) == 0, "capture v5 identity");
    require((saved->attr.flags & VA_ID) && saved->attr.object_id && saved->attr.revision,
            "identity fixture advertises real nonzero v5 fields");
}

static void check_identity(const struct identity_snapshot *saved)
{
    struct vattr actual;
    int found = logitfs.getattr(saved->path, &actual) == 0;
    check(found && (actual.flags & VA_ID) && actual.volume[0] == saved->attr.volume[0] &&
          actual.volume[1] == saved->attr.volume[1] && actual.object_id == saved->attr.object_id,
          "refusal restores volume and object identity");
    check(found && actual.revision == saved->attr.revision, "refusal restores revision");
    check(found && actual.ctime == saved->attr.ctime, "refusal restores ctime");
    struct logit_file_id id = {{saved->attr.volume[0], saved->attr.volume[1]}, saved->attr.object_id};
    char path[96];
    check(logitfs.refpath(&id, path, sizeof path) == 0 && !strcmp(path, saved->path),
          "refusal preserves identity-to-path lookup");
}

static void fixture(int remaining, int cross, int identities, int wide)
{
    sim_open_n(128);
    if (!identities) ((struct lfs_super *)(void *)sim_media)->version = LFS_VERSION;
    if (wide) expand_inode_table();
    require(logitfs.mount() == 0, "mount fixture");
    source = cross ? "/left/source" : "/source";
    target = cross ? "/right/moved" : "/moved";
    if (cross) {
        require(logitfs.mkdir("/left") == 0, "make source directory");
        require(logitfs.mkdir("/right") == 0, "make destination directory");
    }
    require(logitfs.write(source, "kept", 4) == 4, "write source");
    unsigned char reserve[4 * LFS_BS];
    memset(reserve, 0x6b, sizeof reserve);
    require(logitfs.write("/reserve", reserve, sizeof reserve) == sizeof reserve,
            "write recovery reserve");
    require(logitfs.write("/empty", "", 0) == 0, "make empty file");
    parent_count = cross ? 2 : 1;
    parents[0] = cross ? "/left" : "/";
    parents[1] = "/right";
    padded[0] = padded[1] = 0;
    if (wide) {
        for (int parent = 0; parent < parent_count; parent++) {
            int existing = logitfs.count(parents[parent]);
            padded[parent] = LFS_BS / LFS_DIRENT_SZ + 1 - existing;
            for (int i = 0; i < padded[parent]; i++) {
                char path[96];
                pad_path(path, sizeof path, parents[parent], i);
                require(logitfs.write(path, "", 0) == 0, "pad parent beyond one data block");
            }
            struct vattr attr;
            require(logitfs.getattr(parents[parent], &attr) == 0, "stat padded parent");
            struct lfs_super *super = (struct lfs_super *)(void *)sim_media;
            const struct lfs_dinode *table = (const void *)(sim_media + super->inode_start * LFS_BS);
            require(table[attr.ino].size > LFS_BS, "parent really spans multiple data blocks");
        }
    }

    unsigned leave = remaining ? (unsigned)remaining : 1;
    unsigned blocks = free_blocks() - leave - 1; /* one single-indirect block */
    require(blocks > LFS_NDIRECT && blocks < LFS_NDIRECT + LFS_PPB,
            "filler has exactly one pointer block");
    payload_size = (int)(blocks * LFS_BS);
    payload = malloc((size_t)payload_size);
    if (!payload) exit(2);
    for (int i = 0; i < payload_size; i++) payload[i] = (uint8_t)(i * 17 + (i >> 8));
    require(logitfs.write("/filler", payload, payload_size) == payload_size, "write filler");
    /* Overwriting a pre-existing empty inode needs no parent-directory rewrite,
     * so its first byte can consume the last free block without forging a disk. */
    if (!remaining) require(logitfs.write("/empty", "z", 1) == 1, "consume last free block");
    require(free_blocks() == (unsigned)remaining, "exact free-block boundary");
    require(bytes_equal("/filler", payload, payload_size), "filler initially reads back");
}

static void run_case(int remaining, int cross, int fail_after, unsigned log_blocks,
                     int identities, int wide)
{
    snprintf(case_name, sizeof case_name, "%sspace=%d cross=%d heap=%d log=%u",
             identities ? (wide ? "v5-wide " : "v5 ") : "",
             remaining, cross, fail_after, log_blocks);
    fixture(remaining, cross, identities, wide);
    if (log_blocks != SIM_LOG_BLOCKS) set_log_capacity(log_blocks);
    struct identity_snapshot identity[3];
    if (identities) {
        capture_identity(&identity[0], source);
        for (int i = 0; i < parent_count; i++) capture_identity(&identity[i + 1], parents[i]);
    }
    require(logitfs_sync() == 0, "flush fixture before snapshot");
    size_t media_size = (size_t)sim_nblocks * LFS_BS;
    uint8_t *before = malloc(media_size);
    if (!before) exit(2);
    memcpy(before, sim_media, media_size);
    fsstub_clock += 11;                    /* a missing ctime rollback must be observable */
    int allocations_before = live_allocations;
    allocation_budget = fail_after;
    int rc = logitfs.rename(source, target);
    allocation_budget = -1;
    int refused = remaining < (wide ? 4 : 2) || fail_after >= 0 || log_blocks != SIM_LOG_BLOCKS;
    check(refused ? rc < 0 : rc == 0, "rename returns the expected space verdict");
    check(live_allocations == allocations_before, "rename releases temporary allocations");

    if (refused) {
        if (identities)
            for (int i = 0; i <= parent_count; i++) check_identity(&identity[i]);
        check_padding();
        check(bytes_equal(source, "kept", 4), "refused rename preserves source bytes");
        check(logitfs.size(target) < 0, "refused rename creates no destination");
        check(bytes_equal("/filler", payload, payload_size), "refused rename preserves bystander bytes");
        check(logitfs_sync() == 0, "refusal leaves cache flushable");
        check(!memcmp(before, sim_media, media_size), "refusal leaves durable bytes unchanged");
        /* Free space through an ordinary truncate, then retry on the same
         * mount. A leaked allocation bit can pass a remount-only test because
         * mounting reloads the clean bitmap and hides the broken RAM state. */
        check(logitfs.write("/reserve", "", 0) == 0, "other writes still work after refusal");
        check(bytes_equal("/filler", payload, payload_size), "later commit preserves bystander bytes");
        check(logitfs_fsck(0) == 0, "later commit has no leaked allocation bits");
        if (log_blocks != SIM_LOG_BLOCKS) set_log_capacity(SIM_LOG_BLOCKS);
        check(logitfs.rename(source, target) == 0, "rename succeeds after capacity recovers");
    }
    check(bytes_equal(target, "kept", 4), "successful rename retains source bytes");
    check(logitfs.size(source) < 0, "successful rename removes old name");
    check(bytes_equal("/filler", payload, payload_size), "successful rename retains bystander bytes");
    check_padding();
    if (identities) {
        struct vattr moved;
        int found = logitfs.getattr(target, &moved) == 0;
        check(found && moved.object_id == identity[0].attr.object_id &&
              moved.revision == identity[0].attr.revision, "successful move preserves source identity and content revision");
        check(found && moved.ctime > identity[0].attr.ctime, "successful move advances source ctime");
    }
    logitfs_unmount();
    require(logitfs.mount() == 0, "remount result");
    check(bytes_equal(target, "kept", 4), "renamed bytes survive remount");
    check(bytes_equal("/filler", payload, payload_size), "bystander bytes survive remount");
    check_padding();
    check(logitfs_fsck(0) == 0, "remounted filesystem is clean");
    logitfs_unmount();
    check(live_allocations == 0, "all filesystem allocations are released");
    free(before);
    free(payload);
    sim_close();
}

int main(void)
{
    for (int cross = 0; cross <= 1; cross++) {
        for (int space = 0; space <= 2; space++) run_case(space, cross, -1, SIM_LOG_BLOCKS, 0, 0);
        for (int failure = 0; failure < 5; failure++) run_case(8, cross, failure, SIM_LOG_BLOCKS, 0, 0);
        run_case(8, cross, -1, 4, 0, 0);     /* flush_inode runs out of log slots */
        run_case(8, cross, -1, 6, 0, 0);     /* flush_bitmap applies frees, then fails */

        run_case(1, cross, -1, SIM_LOG_BLOCKS, 1, 0);
        run_case(8, cross, -1, 7, 1, 0);     /* v5 also stages its root identity counter */
        run_case(3, cross, -1, SIM_LOG_BLOCKS, 1, 1);
        run_case(4, cross, -1, SIM_LOG_BLOCKS, 1, 1);
        for (int failure = 0; failure < 5; failure++) run_case(8, cross, failure, SIM_LOG_BLOCKS, 1, 1);
        run_case(8, cross, -1, 6, 1, 1);
        run_case(8, cross, -1, 9, 1, 1);
    }
    return fs_verdict("fs_rename_space_test");
}
