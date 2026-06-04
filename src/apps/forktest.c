#include "aqua.h"

/* P1 validation: a ring-3 program that fork()s, both sides write distinguishing
 * lines to the serial log (fd 1), the parent waitpid()s the child. Proves the
 * whole fork -> child-returns-0 -> exit -> waitpid -> reap cycle end to end.
 * Headless: grep the serial output for the FORKTEST markers. */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void sp(const char *s) { sys_write(1, s, slen(s)); }
static void sn(int v)
{
    char t[12]; int i = 0;
    if (v < 0) { sys_write(1, "-", 1); v = -v; }
    if (!v) { sys_write(1, "0", 1); return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char o[12]; int k = 0; while (i) o[k++] = t[--i];
    sys_write(1, o, k);
}

void app_main(void)
{
    sp("FORKTEST: start pid="); sn(sys_getpid()); sp("\n");

    int pid = sys_fork();
    if (pid == 0) {
        sp("FORKTEST: CHILD fork()=0 pid="); sn(sys_getpid()); sp("\n");
        app_exit(7);
    } else if (pid > 0) {
        sp("FORKTEST: PARENT child="); sn(pid); sp("\n");
        int status = 0;
        int r = sys_waitpid(pid, &status);
        sp("FORKTEST: reaped="); sn(r); sp(" status="); sn(status); sp("\n");
        sp("FORKTEST: done\n");
        app_exit(0);
    } else {
        sp("FORKTEST: fork FAILED\n");
        app_exit(1);
    }
}
