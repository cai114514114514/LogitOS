#include "aqua.h"

/* M18 validation harness (temporary, auto-launched to the serial log during
 * development). Exercises each new primitive as it lands: fork (P1), file
 * descriptors (P2), pipe + execve (P3). Grep the serial output for SYSTEST. */

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

static void test_fd(void)
{
    sp("SYSTEST: fd\n");
    int fd = sys_open("/systest.txt", O_WRONLY | O_CREAT | O_TRUNC);
    sp("  open(w)="); sn(fd); sp("\n");
    if (fd >= 0) { sys_write(fd, "hello-fd\n", 9); sys_close(fd); }

    int rf = sys_open("/systest.txt", O_RDONLY);
    char buf[64]; int n = rf >= 0 ? sys_read(rf, buf, sizeof buf) : -1;
    sp("  read("); sn(n); sp(")="); if (n > 0) sys_write(1, buf, n);
    if (rf >= 0) {
        sys_lseek(rf, 0, SEEK_SET);
        n = sys_read(rf, buf, 5);
        sp("  lseek0+read5="); if (n > 0) sys_write(1, buf, n); sp("\n");
        sys_close(rf);
    }
    char cwd[64]; sys_getcwd(cwd, sizeof cwd);
    sp("  cwd="); sp(cwd); sp("\n");
}

static void test_fork(void)
{
    sp("SYSTEST: fork pid="); sn(sys_getpid()); sp("\n");
    int pid = sys_fork();
    if (pid == 0) { sp("  CHILD fork()=0 pid="); sn(sys_getpid()); sp("\n"); app_exit(7); }
    else if (pid > 0) {
        sp("  PARENT child="); sn(pid); sp("\n");
        int st = 0; int rr = sys_waitpid(pid, &st);
        sp("  reaped="); sn(rr); sp(" status="); sn(st); sp("\n");
    } else sp("  fork FAILED\n");
}

void app_main(void)
{
    test_fd();
    test_fork();
    sp("SYSTEST: done\n");
    app_exit(0);
}
