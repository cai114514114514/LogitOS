/* kill [-s SIGNAL | -SIGNAL] PID...    kill -l
 *
 * Signals by name (with or without SIG) or number. A thin wrapper over kill(2)
 * (SYS_KILL); the default is TERM, as POSIX says. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static const struct { const char *n; int v; } sigs[] = {
    {"HUP",1},{"INT",2},{"QUIT",3},{"ILL",4},{"TRAP",5},{"ABRT",6},{"BUS",7},{"FPE",8},
    {"KILL",9},{"USR1",10},{"SEGV",11},{"USR2",12},{"PIPE",13},{"ALRM",14},{"TERM",15},
    {"CHLD",17},{"CONT",18},{"STOP",19},{"TSTP",20},{"TTIN",21},{"TTOU",22},
};
#define NSIGS ((int)(sizeof sigs / sizeof sigs[0]))

static int signum(const char *s)
{
    if (*s >= '0' && *s <= '9') return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 0; i < NSIGS; i++) if (!strcmp(s, sigs[i].n)) return sigs[i].v;
    return -1;
}

int main(int argc, char **argv)
{
    int sig = 15, i = 1;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (int k = 0; k < NSIGS; k++) printf("%s%c", sigs[k].n, k + 1 < NSIGS ? ' ' : '\n');
        return 0;
    }
    if (i < argc && !strcmp(argv[i], "-s")) {
        if (i + 1 >= argc || (sig = signum(argv[i + 1])) < 0) { fputs("kill: invalid signal\n", stderr); return 1; }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
        if ((sig = signum(argv[i] + 1)) < 0) { fprintf(stderr, "kill: invalid signal: %s\n", argv[i] + 1); return 1; }
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) { fputs("usage: kill [-s SIGNAL | -SIGNAL] PID...\n", stderr); return 1; }
    int rc = 0;
    for (; i < argc; i++) {
        char *e;
        long pid = strtol(argv[i], &e, 10);
        if (*e || e == argv[i]) { fprintf(stderr, "kill: %s: arguments must be process IDs\n", argv[i]); rc = 1; continue; }
        if (kill((int)pid, sig) < 0) { fprintf(stderr, "kill: (%ld): No such process\n", pid); rc = 1; }
    }
    return rc;
}
