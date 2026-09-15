/* SPDX-License-Identifier: MIT */
/* Cookie snapshots contain credentials. The shared tabs/localStorage adapter
 * creates 0644 files, so it cannot be reused unchanged. Keep this adapter on
 * logit.h's real syscalls: libc fchmod is ENOSYS, and a successful chmod alone
 * does not prove its mode survives a reboot (LSTA_MODE_DURABLE does).
 *
 * Only the browser-owned directory and two fixed regular, single-link files
 * are accepted. lstat rejects symlinks before following any snapshot path.
 * umask protects a NEW inode from its first instant; chmod tightens older
 * inodes before credential bytes are written. Save/restore of the process
 * umask is safe here because browser storage calls run on its event loop.
 */
#include <errno.h>
#include "cookie_persistence.h"
static int cookie_os_stat(const char *path, struct logit_stat *st)
{ return (int)_sys(SYS_LSTAT, (long)path, (long)st, sizeof *st); }
static int cookie_os_mode(const char *path, unsigned kind, unsigned mode)
{
    struct logit_stat st;
    if (cookie_os_stat(path, &st) < 0 || st.version != LOGIT_STAT_VERSION ||
        st.len < sizeof st || (st.mode & LST_IFMT) != kind ||
        st.uid != (unsigned)sys_geteuid() || (kind == LST_IFREG && st.nlink != 1)) return -1;
    unsigned durable = LSTA_MODE_STORED | LSTA_MODE_DURABLE;
    if ((st.attr & durable) == durable && (st.mode & 07777) == mode) return 0;
    if (_sys(SYS_CHMOD, (long)path, mode, 0) < 0 || cookie_os_stat(path, &st) < 0) return -1;
    return (st.attr & durable) == durable && (st.mode & 07777) == mode ? 0 : -1;
}
static int cookie_os_directory(const char *path, int private)
{
    struct logit_stat st;
    int rc = cookie_os_stat(path, &st);
    if (rc == -ENOENT) {
        long old = _sys(SYS_UMASK, 0077, 0, 0);
        if (old < 0) return -1;
        int made = make_dir(path);
        int restored = (int)_sys(SYS_UMASK, old, 0, 0);
        if (made < 0 || restored < 0) return -1;
    } else if (rc < 0) return -1;
    if (private) return cookie_os_mode(path, LST_IFDIR, 0700);
    /* /browser also owns session/history. Verify its ownership and traversal
     * boundary without changing permissions on unrelated browser state. */
    return cookie_os_stat(path, &st) == 0 && (st.mode & LST_IFMT) == LST_IFDIR &&
        st.uid == (unsigned)sys_geteuid() && !(st.mode & 0022) ? 0 : -1;
}
static int cookie_os_mkdir(const char *path)
{
    if (strcmp(path, COOKIE_STORE_DIR)) return -1;
    return cookie_os_directory(BROWSER_DIR, 0) < 0 ? -1 : cookie_os_directory(path, 1);
}
static int cookie_os_path(const char *path)
{ return !strcmp(path, COOKIE_STORE_SLOT0) || !strcmp(path, COOKIE_STORE_SLOT1); }
static int cookie_os_read(const char *path, void *buf, int max)
{
    if (!cookie_os_path(path) || cookie_os_mkdir(COOKIE_STORE_DIR) < 0) return -2;
    struct logit_stat st;
    int rc = cookie_os_stat(path, &st);
    if (rc == -ENOENT) return -1;
    if (rc < 0 || cookie_os_mode(path, LST_IFREG, 0600) < 0 || st.size > (unsigned)max) return -2;
    int got = read_file(path, buf, max);
    return got < 0 ? -2 : got;
}
static int cookie_os_write(const char *path, const void *buf, int len)
{
    if (!cookie_os_path(path) || cookie_os_mkdir(COOKIE_STORE_DIR) < 0) return -1;
    struct logit_stat st;
    int rc = cookie_os_stat(path, &st);
    if ((rc < 0 && rc != -ENOENT) || (rc == 0 && cookie_os_mode(path, LST_IFREG, 0600) < 0)) return -1;
    long old = _sys(SYS_UMASK, 0077, 0, 0);
    if (old < 0) return -1;
    int wrote = write_file(path, buf, len);
    int restored = (int)_sys(SYS_UMASK, old, 0, 0);
    if (restored < 0 || (wrote != 0 && wrote != len) || cookie_os_mode(path, LST_IFREG, 0600) < 0) return -1;
    return wrote;
}
static const struct bstore_ops cookie_os_store = { cookie_os_read, cookie_os_write, cookie_os_mkdir };
