#ifndef LOGIT_BOOT_SERVICES_H
#define LOGIT_BOOT_SERVICES_H
/* Explicit opt-in survives reboot. A fresh product image has no enable files,
 * accounts or host keys. Start before console authentication: after login the
 * user cannot read the host key or authenticate a different user's account.
 * Each child pins root credentials before the desktop session can change and
 * uses its own log/stdin, so a daemon never consumes the login prompt. */
static void boot_service(const char *flag, const char *program, const char *logpath)
{
    struct logit_stat st;
    if (sys_getuid() != 0 || st_lstat(flag, &st) < 0 ||
        (st.mode & LST_IFMT) != LST_IFREG || st.uid != 0 || (st.mode & 022)) return;
    int pid = sys_fork();
    if (pid != 0) {
        if (pid < 0) errs("login: could not launch enabled service\n");
        return;
    }
    if (sys_setgid(0) < 0 || sys_setuid(0) < 0) app_exit(126);
    make_dir("/var"); make_dir("/var/log");
    /* This OS has no /dev/null. A pipe with its writer closed is an actual
     * EOF stream; using a familiar nonexistent device silently aborted boot. */
    int eof[2];
    if (sys_pipe(eof) < 0) { errs("login: service stdin pipe failed\n"); app_exit(126); }
    sys_close(eof[1]);
    int input = eof[0];
    int output = sys_open(logpath, O_WRONLY | O_CREAT | O_TRUNC);
    if (output < 0) { errs("login: service log unavailable\n"); app_exit(126); }
    sys_dup2(input, 0); sys_dup2(output, 1); sys_dup2(output, 2);
    for (int fd = 3; fd < LOGIT_POLL_MAX; fd++) sys_close(fd);
    char *argv[] = { (char *)program, 0 };
    sys_execve(program, argv, 0);
    errs("login: enabled service exec failed\n");
    app_exit(127);
}
static void boot_services(void)
{
    boot_service("/etc/sshd.enabled", "/bin/sshd", "/var/log/sshd.log");
    boot_service("/etc/httpd.enabled", "/bin/httpd", "/var/log/httpd.log");
    boot_service("/etc/httpsd.enabled", "/bin/httpsd", "/var/log/httpsd.log");
}
#endif
