#include "clib.h"

/* ps -- the process table, read out of /proc.
 *
 * THE POINT OF THIS PROGRAM IS WHAT IT DOES NOT CONTAIN. There is no syscall
 * here that a `cat` does not also make: sys_open, sys_read, sys_close and the
 * directory enumeration `ls` already used. SYS_PROCS (95) exists, is the only
 * way this table could be reached before /proc, and is not called -- because a
 * ps that needs its own syscall is the thing /proc was built to stop being
 * necessary. If a future column cannot be got from a file, the fix is a file.
 *
 *   $ ps
 *     PID  PPID S FDS CMD
 *       1     0 R   3 sh
 *       4     1 R   3 ps
 *
 * -l adds the scheduler thread id and the GUI flag; -a is accepted and does
 * nothing, since there are no sessions on this machine to filter by and a flag
 * that silently filters nothing would be a lie about what it did.
 *
 * WHAT A ROW MEANS IF THE PROCESS EXITS WHILE THIS RUNS. /proc/<pid>/stat
 * fails to open, or reads -1, and the pid is SKIPPED rather than printed with
 * blanks -- which is correct and is the visible face of the lifetime rule in
 * c/fs/procfs.h: a process that is gone has no row, and never a row of stale
 * numbers. The listing is a sample, as every process listing on every system
 * is; what it is not is a mixture of two instants presented as one. */

static int slurp(const char *path, char *buf, int max)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    for (;;) {
        int r = sys_read(fd, buf + n, max - 1 - n);
        if (r <= 0) break;
        n += r;
        if (n >= max - 1) break;
    }
    sys_close(fd);
    buf[n] = 0;
    return n;
}

/* All digits and non-empty. /proc's root holds meminfo/uptime/version/self
 * beside the pids, and "self" would otherwise parse as pid 0. */
static int all_digits(const char *s)
{
    if (!s[0]) return 0;
    for (int i = 0; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static void pad(long v, int w)
{
    int d = 1; long t = v < 0 ? -v : v;
    if (v < 0) d++;
    while (t >= 10) { t /= 10; d++; }
    for (int i = d; i < w; i++) outc(' ');
    outn(v);
}

/* One /proc/<pid>/stat line: "<pid> (<comm>) <state> <ppid> <tid> <fds> <gui>".
 * The comm is in parentheses and is read by scanning to the LAST ')' -- the
 * same rule Linux's own parsers use, and for the same reason: a program named
 * "a b)c" would otherwise take three fields with it. Returns 0 on a malformed
 * line rather than printing half a row. */
struct row { int pid, ppid, tid, fds, gui; char st; char cmd[40]; };

static int parse_stat(const char *s, struct row *r)
{
    int i = 0;
    r->pid = 0;
    if (!(s[i] >= '0' && s[i] <= '9')) return 0;
    while (s[i] >= '0' && s[i] <= '9') r->pid = r->pid * 10 + (s[i++] - '0');
    while (s[i] == ' ') i++;
    if (s[i] != '(') return 0;
    int open_at = ++i, close_at = -1;
    for (int k = i; s[k]; k++) if (s[k] == ')') close_at = k;
    if (close_at < 0) return 0;
    int n = close_at - open_at;
    if (n > (int)sizeof r->cmd - 1) n = (int)sizeof r->cmd - 1;
    for (int k = 0; k < n; k++) r->cmd[k] = s[open_at + k];
    r->cmd[n] = 0;
    i = close_at + 1;
    while (s[i] == ' ') i++;
    if (!s[i]) return 0;
    r->st = s[i++];
    int *f[4] = { &r->ppid, &r->tid, &r->fds, &r->gui };
    for (int k = 0; k < 4; k++) {
        while (s[i] == ' ') i++;
        if (s[i] < '0' || s[i] > '9') return 0;
        int v = 0;
        while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
        *f[k] = v;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int lng = 0;
    for (int i = 1; i < argc; i++) {
        for (int k = 1; argv[i][k]; k++) {
            if (argv[i][k] == 'l') lng = 1;
            else if (argv[i][k] != 'a' && argv[i][k] != '-') {
                errs("ps: usage: ps [-l]\n");
                return 1;
            }
        }
    }

    int n = dir_count("/proc");
    if (n < 0) {
        /* Named, because "ps: no /proc" and "ps: /proc is empty" send a reader
         * to completely different places, and the first one is a kernel that
         * did not mount it (look for "[fs] /proc mount FAILED" on serial). */
        errs("ps: /proc is not mounted\n");
        return 1;
    }

    outs("  PID  PPID S FDS");
    if (lng) outs("  TID GUI");
    outs(" CMD\n");

    int shown = 0;
    for (int i = 0; i < n; i++) {
        char name[64];
        /* SYS_DIR_NAME RETURNS -2 FOR A DIRECTORY, and -1 is its only error.
         * That is not a quirk to route around, it is the ABI (see the case in
         * c/kernel/exec/syscall.c, and /bin/dir, which prints "dir" on exactly
         * this value) -- but `< 0 means it failed` is what anybody writes, and
         * it silently skips EVERY entry that is a directory. Every pid in
         * /proc is a directory, so this program printed its header and not one
         * row while `ls /proc` and `dir /proc` both listed the pids correctly
         * two lines earlier. Cost: one probe boot. */
        int kind = dir_name("/proc", i, name);
        if (kind == -1) continue;
        if (kind != -2) continue;                 /* a pid is a directory */
        if (!all_digits(name)) continue;

        char path[80];
        path_join(path, "/proc", name, sizeof path);
        char full[96];
        path_join(full, path, "stat", sizeof full);

        char buf[256];
        if (slurp(full, buf, sizeof buf) <= 0) continue;   /* it exited: no row */

        struct row r;
        if (!parse_stat(buf, &r)) continue;

        pad(r.pid, 5); pad(r.ppid, 6);
        outc(' '); outc(r.st);
        pad(r.fds, 4);
        if (lng) { pad(r.tid, 5); pad(r.gui, 4); }
        outc(' '); outs(r.cmd[0] ? r.cmd : "?");
        outc('\n');
        shown++;
    }

    /* Zero rows from a mounted /proc is not "no processes" -- this program is
     * one -- so it is an error, not an empty listing. */
    if (!shown) { errs("ps: /proc listed no process, not even this one\n"); return 1; }
    return 0;
}
