/* THE CRACK BETWEEN TWO NAME LIMITS, pinned from both sides.
 *
 * c/fs/vfs_path.h said a path component may be 60 bytes; c/fs/logitfs.c's
 * dirent holds 60 bytes INCLUDING the NUL and refuses 60. A name of exactly
 * 60 bytes therefore passed the VFS (the open was granted, the file existed
 * in RAM), and was refused by the filesystem at write-back, where nothing
 * reports a failure: on device `touch` returned 0 and `ls` never listed the
 * file. 59 created, 61 was refused out loud. Found 2026-08-20.
 *
 * The fix makes VFS_NAME_MAX DERIVE from LFS_NAME_MAX, and this test is the
 * proof that the two layers now agree at EVERY length, not just at the one
 * that was measured: for each candidate length it asks the VFS walker and the
 * real logitfs (c/fs/logitfs.c on the simulated device, the same build the
 * crash tests use) the same question and requires the same answer. The
 * negative control (-DVFS_NAME_MAX_LEGACY puts the typed 60 back) must fail
 * on exactly the length-60 checks and nothing else. */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"
#include "vfs_path.h"
#include <string.h>
#include <stdio.h>

/* Does the VFS walker accept a component of `len` bytes? */
static int vfs_accepts(int len)
{
    char in[2 + 128], out[VFS_PATH_MAX];
    in[0] = '/'; memset(in + 1, 'v', (size_t)len); in[len + 1] = 0;
    return vfs_path_norm("/", in, out, (int)sizeof out) >= 0;
}

/* Does logitfs create, LIST BYTE FOR BYTE, and delete a file of `len` bytes?
 * A refused name must leave the directory exactly as it was -- that is the
 * half of the bug that made it silent. */
static int lfs_accepts(int len, const char *op)
{
    char path[2 + 128];
    path[0] = '/'; memset(path + 1, 'f', (size_t)len); path[len + 1] = 0;
    int before = logitfs.count("/");
    int rc = !strcmp(op, "mkdir") ? logitfs.mkdir(path) : logitfs.write(path, "x", 1);
    int after = logitfs.count("/");
    if (rc < 0) {
        fs_ok(after == before, "%s of a refused %d-byte name must not change the directory (%d -> %d)",
              op, len, before, after);
        fs_ok(logitfs.size(path) < 0, "a refused %d-byte name must not resolve afterwards", len);
        return 0;
    }
    fs_ok(after == before + 1, "%s of a %d-byte name must add one entry (%d -> %d)", op, len, before, after);
    int found = 0;
    for (int i = 0; i < after; i++) {
        const char *nm = logitfs.ent_name("/", i);
        if (nm && !strcmp(nm, path + 1)) found = 1;
    }
    fs_ok(found, "%s: a %d-byte name must be listed back byte for byte", op, len);
    fs_ok(logitfs.del(path) == 0, "%s: a %d-byte name must be deletable by the same name", op, len);
    return 1;
}

int main(void)
{
    sim_open();
    fs_ok(logitfs.mount() == 0, "the simulated image must mount");

    /* 1. One definition. The header derives it; say the number out loud so a
     *    reader of the log sees what was compiled. */
    printf("VFS_NAME_MAX %d, LFS_NAME_MAX %d (dirent field, NUL included)\n", VFS_NAME_MAX, LFS_NAME_MAX);
    fs_ok(VFS_NAME_MAX == LFS_NAME_MAX - 1,
          "VFS_NAME_MAX (%d) must be the on-disk field minus its NUL (%d)", VFS_NAME_MAX, LFS_NAME_MAX - 1);

    /* 2. The measured cases, by name. */
    fs_ok(vfs_accepts(59),  "59 bytes: the VFS accepts");
    fs_ok(lfs_accepts(59, "write"), "59 bytes: logitfs creates and lists");
    fs_ok(!vfs_accepts(60), "60 bytes: the VFS refuses (THE CRACK: it used to accept)");
    fs_ok(!lfs_accepts(60, "write"), "60 bytes: logitfs refuses");
    fs_ok(!vfs_accepts(61), "61 bytes: the VFS refuses");
    fs_ok(!lfs_accepts(61, "write"), "61 bytes: logitfs refuses");
    fs_ok(!lfs_accepts(60, "mkdir"), "60 bytes: logitfs refuses a directory too");
    fs_ok(lfs_accepts(59, "mkdir"), "59 bytes: logitfs creates a directory");

    /* 3. Every length: the two layers must answer alike. The sweep is the
     *    point -- the bug was at a length nobody had typed. */
    static const int lens[] = { 1, 2, 7, 8, 31, 32, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 100, 127 };
    int disagree = 0;
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        int v = vfs_accepts(lens[i]);
        int f = lfs_accepts(lens[i], "write");
        if (v != f) { disagree++; printf("  length %d: vfs %s, logitfs %s\n", lens[i], v ? "accepts" : "refuses", f ? "accepts" : "refuses"); }
    }
    fs_ok(disagree == 0, "the VFS and logitfs must agree at every length: %d disagreement(s)", disagree);

    /* 4. Rename into an over-long name is refused, and the source survives. */
    fs_ok(logitfs.write("/short", "x", 1) == 1, "setup: /short");
    char longp[2 + 128]; longp[0] = '/'; memset(longp + 1, 'r', 60); longp[61] = 0;
    fs_ok(logitfs.rename("/short", longp) < 0, "rename to a 60-byte name is refused");
    fs_ok(logitfs.size("/short") == 1, "...and the source is untouched");
    longp[60] = 0;                                   /* 59 */
    fs_ok(logitfs.rename("/short", longp) == 0, "rename to a 59-byte name works");
    fs_ok(logitfs.size(longp) == 1 && logitfs.size("/short") < 0, "...and it moved");
    fs_ok(logitfs.del(longp) == 0, "cleanup");

    logitfs_unmount();
    sim_close();
    return fs_verdict("namemax");
}
