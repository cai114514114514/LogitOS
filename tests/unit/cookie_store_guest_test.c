/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "logit_abi.h"
#include "cookie_persistence.h"
static struct logit_stat entries[4];
static int present[4], current_mask, mode_failure, durable_failure, read_failure;
static int writes, reads, write_result, checks, failures;
static const char *paths[4] = { BROWSER_DIR, COOKIE_STORE_DIR, COOKIE_STORE_SLOT0, COOKIE_STORE_SLOT1 };
static int index_of(const char *path)
{ for (int i = 0; i < 4; i++) if (!strcmp(path, paths[i])) return i; return -1; }
static int sys_geteuid(void) { return 1000; }
static void create(int i, unsigned type, unsigned mode)
{
    present[i] = 1; memset(&entries[i], 0, sizeof entries[i]);
    entries[i].len = sizeof entries[i]; entries[i].version = LOGIT_STAT_VERSION;
    entries[i].mode = type | mode; entries[i].uid = 1000; entries[i].nlink = 1;
    entries[i].attr = LSTA_MODE_STORED | LSTA_MODE_DURABLE;
}
static long _sys(long op, long a, long b, long c)
{
    (void)c;
    if (op == SYS_UMASK) { int old = current_mask; current_mask = (int)a; return old; }
    int i = index_of((const char *)a); if (i < 0 || !present[i]) return -ENOENT;
    if (op == SYS_LSTAT) { memcpy((void *)b, &entries[i], sizeof entries[i]); return 0; }
    if (op == SYS_CHMOD) {
        if (mode_failure) return -EPERM;
        entries[i].mode = (entries[i].mode & LST_IFMT) | (unsigned)b;
        entries[i].attr = LSTA_MODE_STORED | (durable_failure ? 0 : LSTA_MODE_DURABLE); return 0;
    }
    return -EINVAL;
}
static int make_dir(const char *path)
{ int i = index_of(path); if (i < 0 || present[i]) return -1; create(i, LST_IFDIR, 0777 & ~current_mask); return 0; }
static int read_file(const char *path, void *out, int max)
{ (void)path; (void)out; (void)max; reads++; return read_failure ? -1 : 3; }
static int write_file(const char *path, const void *bytes, int n)
{
    (void)bytes; writes++; int i = index_of(path);
    if (!present[i]) create(i, LST_IFREG, 0666 & ~current_mask);
    entries[i].size = n; return write_result ? write_result : n;
}
#include "cookie_store_guest.inc"
static void ck(int ok, const char *label)
{ checks++; if (!ok) failures++; printf("%s: %s\n", ok ? "ok" : "FAIL", label); }
static void fresh(void)
{ memset(present, 0, sizeof present); current_mask = 0022; writes = reads = mode_failure = durable_failure = read_failure = write_result = 0; }
int main(void)
{
    char out[20]; fresh();
    ck(cookie_os_store.mkdir(COOKIE_STORE_DIR) == 0 && (entries[1].mode & 0777) == 0700, "guest adapter creates private cookie directory");
    ck(current_mask == 0022, "directory creation restores caller umask");
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) == 3 && (entries[2].mode & 0777) == 0600, "guest adapter creates private snapshot from first write");
    ck(current_mask == 0022, "snapshot write restores caller umask");
    ck(cookie_os_store.read(COOKIE_STORE_SLOT0, out, sizeof out) == 3, "private snapshot read succeeds");
    ck(cookie_os_store.read(COOKIE_STORE_SLOT1, out, sizeof out) == -1, "missing snapshot is distinct from I/O failure");
    ck(cookie_os_store.write("/browser/elsewhere", "abc", 3) < 0 && writes == 1, "adapter refuses paths outside its two snapshot slots");
    entries[2].mode = LST_IFREG | 0644;
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) == 3 && (entries[2].mode & 0777) == 0600, "existing own file tightened before rewrite");
    entries[2].mode = LST_IFLNK | 0777; int before = reads;
    ck(cookie_os_store.read(COOKIE_STORE_SLOT0, out, sizeof out) == -2 && reads == before, "symlink snapshot is rejected before content read");
    entries[2].mode = LST_IFREG | 0600; entries[2].nlink = 2; before = writes;
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) < 0 && writes == before, "multiply linked snapshot is rejected before write");
    entries[2].nlink = 1; entries[2].uid = 2000;
    ck(cookie_os_store.read(COOKIE_STORE_SLOT0, out, sizeof out) == -2, "different owner is rejected");
    entries[2].uid = 1000; entries[1].mode = LST_IFDIR | 0755; mode_failure = 1;
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) < 0 && writes == before, "failed private directory chmod blocks write");
    mode_failure = 0; durable_failure = 1;
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) < 0 && writes == before, "RAM-only permission metadata blocks credentials write");
    durable_failure = 0; entries[0].mode = LST_IFDIR | 0777;
    ck(cookie_os_store.mkdir(COOKIE_STORE_DIR) < 0, "writable parent directory is rejected");
    entries[0].mode = LST_IFLNK | 0755;
    ck(cookie_os_store.mkdir(COOKIE_STORE_DIR) < 0, "symlink parent directory is rejected");
    fresh(); cookie_os_store.mkdir(COOKIE_STORE_DIR); write_result = 2;
    ck(cookie_os_store.write(COOKIE_STORE_SLOT0, "abc", 3) < 0 && current_mask == 0022, "short guest write fails and restores umask");
    write_result = 0; read_failure = 1;
    ck(cookie_os_store.read(COOKIE_STORE_SLOT0, out, sizeof out) == -2, "existing-file read error is explicit");
    printf("cookie-store-guest-adapter: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
