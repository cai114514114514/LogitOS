#include "clib.h"

/* A real Unix-ish shell: reads command lines from stdin (fd 0), runs builtins in
 * the shell itself, and external commands as fork+execve children. Supports
 * pipelines (cmd | cmd | ...), redirection (< > ), and background (&).
 * PATH is fixed to /bin (then the literal path). */

#define LINE   512
#define MAXTOK 64
#define MAXARG 32
#define MAXCMD 8

struct cmd { char *argv[MAXARG + 1]; int argc; char *infile, *outfile; };

/* Copy `in` to `out`, surrounding each of | < > & with spaces so a plain
 * whitespace tokenizer yields them as standalone tokens. */
static void spacify(const char *in, char *out, int max)
{
    int o = 0;
    for (const char *p = in; *p && o < max - 4; p++) {
        char c = *p;
        if (c == '|' || c == '<' || c == '>' || c == '&') { out[o++] = ' '; out[o++] = c; out[o++] = ' '; }
        else out[o++] = c;
    }
    out[o] = 0;
}

/* Split `s` on whitespace in place; fill tok[] with pointers. Returns count. */
static int tokenize(char *s, char *tok[], int maxtok)
{
    int n = 0;
    while (*s && n < maxtok) {
        while (*s == ' ' || *s == '\t') *s++ = 0;
        if (!*s) break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    return n;
}

static int is_op(const char *t, char c) { return t[0] == c && t[1] == 0; }

/* Resolve argv[0] against /bin then the literal path, and exec. Only returns on
 * failure (all execve attempts failed). */
static void run_external(char **argv)
{
    if (!argv[0]) app_exit(0);
    char buf[160];
    if (c_strlen(argv[0]) > (int)sizeof buf - 6) {   /* "/bin/" + name + NUL must fit */
        errs("sh: command name too long: "); errs(argv[0]); errs("\n");
        app_exit(127);
    }
    c_strcpy(buf, "/bin/", sizeof buf);
    int n = 5; for (int i = 0; argv[0][i] && n < (int)sizeof buf - 1; i++) buf[n++] = argv[0][i];
    buf[n] = 0;
    sys_execve(buf, argv, 0);        /* PATH = /bin */
    sys_execve(argv[0], argv, 0);    /* literal (absolute or cwd-relative) */
    errs("sh: command not found: "); errs(argv[0]); errs("\n");
    app_exit(127);
}

/* Run a parsed pipeline of `ncmd` stages. Returns after the foreground stages
 * finish (or immediately if background). */
static void run_pipeline(struct cmd *cmds, int ncmd, int background)
{
    int prev_read = -1, pids[MAXCMD], np = 0;
    for (int i = 0; i < ncmd; i++) {
        int pfd[2] = { -1, -1 };
        if (i < ncmd - 1 && sys_pipe(pfd) < 0) { errs("sh: pipe failed\n"); break; }

        int pid = sys_fork();
        if (pid < 0) {                                 /* fork failed: don't record a bogus pid */
            errs("sh: fork failed\n");
            if (pfd[0] >= 0) sys_close(pfd[0]);
            if (pfd[1] >= 0) sys_close(pfd[1]);
            break;
        }
        if (pid == 0) {                                  /* child */
            if (prev_read >= 0) sys_dup2(prev_read, 0);
            if (i < ncmd - 1) sys_dup2(pfd[1], 1);
            if (cmds[i].infile)  { int f = sys_open(cmds[i].infile, O_RDONLY);
                                   if (f < 0) { errs("sh: cannot open input\n"); app_exit(1); } sys_dup2(f, 0); sys_close(f); }
            if (cmds[i].outfile) { int f = sys_open(cmds[i].outfile, O_WRONLY | O_CREAT | O_TRUNC);
                                   if (f < 0) { errs("sh: cannot open output\n"); app_exit(1); } sys_dup2(f, 1); sys_close(f); }
            if (prev_read >= 0) sys_close(prev_read);
            if (pfd[0] >= 0) sys_close(pfd[0]);
            if (pfd[1] >= 0) sys_close(pfd[1]);
            run_external(cmds[i].argv);
            app_exit(127);
        }
        if (prev_read >= 0) sys_close(prev_read);
        if (pfd[1] >= 0) sys_close(pfd[1]);
        prev_read = pfd[0];
        if (np < MAXCMD) pids[np++] = pid;
    }
    if (prev_read >= 0) sys_close(prev_read);
    if (!background)
        for (int i = 0; i < np; i++) sys_waitpid(pids[i], 0);
}

/* A single-stage builtin that must run in the shell process. Returns 1 if it
 * handled the command, 0 otherwise. */
static int builtin(struct cmd *c)
{
    if (c->argc == 0) return 1;
    if (c_streq(c->argv[0], "exit")) app_exit(c->argc > 1 ? c_atoi(c->argv[1]) : 0);
    if (c_streq(c->argv[0], "cd")) {
        const char *d = c->argc > 1 ? c->argv[1] : "/";
        if (sys_chdir(d) < 0) { errs("cd: no such directory: "); errs(d); errs("\n"); }
        return 1;
    }
    if (c_streq(c->argv[0], "pwd")) { char b[128]; sys_getcwd(b, sizeof b); outs(b); outc('\n'); return 1; }
    if (c_streq(c->argv[0], "help")) {
        outs("builtins: cd <dir>, pwd, exit [n], help\n");
        outs("programs in /bin: ls cat echo wc head true false sleep mkdir rm touch clear uname\n");
        outs("pipes (a | b), redirection (> <), background (&)\n");
        return 1;
    }
    return 0;
}

static void exec_line(char *line)
{
    char spaced[LINE * 2];
    spacify(line, spaced, sizeof spaced);
    char *tok[MAXTOK];
    int nt = tokenize(spaced, tok, MAXTOK);
    if (nt == 0) return;

    struct cmd cmds[MAXCMD];
    int ncmd = 0, background = 0;
    cmds[0].argc = 0; cmds[0].infile = cmds[0].outfile = 0;
    for (int i = 0; i < nt; i++) {
        char *t = tok[i];
        if (is_op(t, '|')) {
            cmds[ncmd].argv[cmds[ncmd].argc] = 0;
            if (++ncmd >= MAXCMD) { errs("sh: pipeline too long\n"); return; }
            cmds[ncmd].argc = 0; cmds[ncmd].infile = cmds[ncmd].outfile = 0;
        } else if (is_op(t, '<')) { if (i + 1 < nt) cmds[ncmd].infile = tok[++i]; }
        else if (is_op(t, '>')) { if (i + 1 < nt) cmds[ncmd].outfile = tok[++i]; }
        else if (is_op(t, '&')) { background = 1; }
        else if (cmds[ncmd].argc < MAXARG) cmds[ncmd].argv[cmds[ncmd].argc++] = t;
    }
    cmds[ncmd].argv[cmds[ncmd].argc] = 0;
    ncmd++;

    if (ncmd == 1) {
        if (builtin(&cmds[0])) return;     /* cd/pwd/exit/help run in the shell */
    }
    run_pipeline(cmds, ncmd, background);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    outs("Aether shell -- type 'help'\n");
    char line[LINE];
    for (;;) {
        char cwd[128]; sys_getcwd(cwd, sizeof cwd);
        outs(cwd); outs(" $ ");
        int n = readline(0, line, sizeof line);
        if (n < 0) break;                  /* EOF on stdin */
        if (n == 0) continue;
        exec_line(line);
    }
    return 0;
}
