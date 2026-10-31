/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hwreport_log.h"
#include "vfs.h"
#include "kprintf.h"
#include <stdint.h>

/* No new on-disk counter: its update could succeed while the log's failed (or
 * vice versa). The largest canonical filename is authoritative. With retention
 * this survives ordinary reboots and gaps; deleting ALL boot logs deliberately
 * starts a new sequence at 1. LogitFS vfs_create_file/vfs_write commit before
 * returning (c/fs/logitfs/logitfs.h); do not issue a second speculative sync.
 * Model/QEMU evidence only: not verified on X79-VG4 hardware. */
enum { KEEP_LOGS = 20 };
static char boot_path[64];
static int disabled;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint64_t log_number(const char *name)
{
    const char *prefix = "boot-";
    while (*prefix) if (*name++ != *prefix++) return 0;
    /* Canonical spelling makes the number/path relation one-to-one. In
     * particular boot-01.log is somebody else's file, not boot-1.log. */
    if (*name < '1' || *name > '9') return 0;
    uint64_t n = 0;
    while (*name >= '0' && *name <= '9') {
        unsigned digit = (unsigned)(*name++ - '0');
        if (n > (UINT64_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    return same(name, ".log") ? n : 0;
}

static int directory(const char *path)
{
    struct vattr attr;
    int rc = vfs_lstat(path, &attr);
    if (rc == 0) return attr.type == VT_DIR ? 0 : -1;
    if (rc != VFS_ENOENT || vfs_mkdir(path) < 0) return -1;
    return 0;
}

static uint64_t entry_number(int i, char path[64])
{
    char name[64];
    if (vfs_ent_name_copy("/var/log", i, name, sizeof name) < 0) return 0;
    uint64_t n = log_number(name);
    if (!n) return 0;
    ksnprintf(path, 64, "/var/log/%s", name);
    struct vattr attr;
    /* Do not follow a numbered symlink, overwrite it, or prune a directory
     * that happens to be named boot-7.log. */
    if (vfs_lstat(path, &attr) < 0 || attr.type != VT_REG) return 0;
    return n;
}

static int create_log(const char *text, int length)
{
    if (directory("/var") < 0 || directory("/var/log") < 0) return -1;
    int count = vfs_count("/var/log");
    if (count < 0) return -1;
    uint64_t latest[KEEP_LOGS] = {0};
    char path[64];
    for (int i = 0; i < count; i++) {
        uint64_t n = entry_number(i, path);
        for (int j = 0; n && j < KEEP_LOGS; j++) {
            if (n > latest[j]) {
                uint64_t old = latest[j];
                latest[j] = n;
                n = old;
            }
        }
    }
    if (latest[0] == UINT64_MAX) return -1; /* never wrap and overwrite boot-1 */
    ksnprintf(boot_path, sizeof boot_path, "/var/log/boot-%llu.log",
              (unsigned long long)(latest[0] + 1));
    if (vfs_create_file(boot_path, text, length) != length) {
        boot_path[0] = 0;
        return -1;
    }
    /* Write first, prune second: a full/read-only disk must not cost an old
     * report just because today's report could not be saved. Each operation
     * is journaled, but the pair is not atomic: power loss between them can
     * leave 21 logs; the next successful boot prunes back to 20. */
    uint64_t cutoff = latest[KEEP_LOGS - 2];
    if (!cutoff) return 0;
    count = vfs_count("/var/log");
    if (count < 0) return -1;
    /* Reverse iteration avoids skipping entries shifted down by deletion. */
    for (int i = count - 1; i >= 0; i--) {
        uint64_t n = entry_number(i, path);
        if (n && n < cutoff && vfs_delete(path) < 0) return -1;
    }
    return 0;
}

int hwreport_save(const char *text, int length)
{
    if (disabled) return -1;
    struct vstatfs info;
    /* LogitFS currently reports flags=0 even when its transport refuses writes.
     * Honor explicit read-only filesystems, then treat the FIRST mutation
     * failure as terminal. Never probe by raw-writing a sector: that risks a
     * different partition and cannot establish filesystem writability. The
     * synchronous transport owns its own completion deadline; this reporter
     * cannot cancel a wedged driver from inside a VFS call. */
    if (vfs_statfs("/", &info) < 0) { disabled = 1; return -1; }
    if (info.flags & VSTATFS_RDONLY) { disabled = 1; return -1; }
    int rc = boot_path[0] ? (vfs_write(boot_path, text, length) == length ? 0 : -1)
                          : create_log(text, length);
    if (rc < 0) disabled = 1;
    return rc;
}
