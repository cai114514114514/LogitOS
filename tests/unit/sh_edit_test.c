/* Host test for /bin/sh: the tokenizer's quoting rules, the interactive line
 * editor + history + completion, the environment filter that keeps protocol
 * bytes out of redirected output, and the job-control state machine.
 *
 * It compiles the REAL c/apps/coreutils/sh.c (see sh_hoststub.h) against a
 * pipe model with honest reader/writer refcounts, so the ^C path is exercised
 * through the same code the OS runs, not through a re-implementation.
 */

#include "sh_hoststub.h"

#define main sh_main
#include "sh.c"
#undef main

#ifdef SH_NEGATIVE_CONTROL
/* The negative control, and it lives HERE rather than as a hook in sh.c.
 *
 * It reinstates the naive design this protocol exists to avoid: hand the rich
 * channel to every child, the way an in-band escape scheme unavoidably does,
 * because the escape bytes go wherever stdout goes. env_build()'s filter is the
 * ONLY thing keeping protocol bytes out of a redirected file, so with it
 * bypassed the compatibility assertions in t_env() MUST fail. If they still
 * pass, they were not testing the mechanism. */
#define env_build(rich) env_build(1)
#endif

#include <stdio.h>

static int fails;
#define CHK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                           printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- wiring: fd 1 = stdout, fd 2 = stderr, fd 3 = rich, fd 4 = control ---- */
static int fd_out_r, fd_err_r, fd_rich_r, fd_ctl_w;

static void wire_up(void)
{
    /* Bind the fixed fd numbers FIRST: sys_pipe() hands out the lowest free fd,
     * so allocating before binding would have it return 1 and the bind would
     * then overwrite the pipe end it had just created. */
    int po = stub_pipe_alloc(), pe = stub_pipe_alloc();
    int pr = stub_pipe_alloc(), pc = stub_pipe_alloc();
    stub_fd_bind(1, po, 1);
    stub_fd_bind(2, pe, 1);
    stub_fd_bind(3, pr, 1);
    stub_fd_bind(4, pc, 0);
    fd_out_r  = stub_fd_alloc(po, 0);
    fd_err_r  = stub_fd_alloc(pe, 0);
    fd_rich_r = stub_fd_alloc(pr, 0);
    fd_ctl_w  = stub_fd_alloc(pc, 1);
}

/* Push one control frame at the shell, the way the terminal would. */
static struct rt_enc TE;
static void ctl_frame(int type)
{
    unsigned char h[RT_HDR];
    rt_hdr(h, type, 0, (unsigned)TE.n);
    sys_write(fd_ctl_w, h, RT_HDR);
    if (TE.n) sys_write(fd_ctl_w, TE.b, TE.n);
    rt_reset(&TE);
}
static void ctl_text(const char *s) { rt_reset(&TE); rt_str(&TE, s); ctl_frame(RT_C_TEXT); }
static void ctl_key(int k)          { rt_reset(&TE); rt_u32(&TE, (unsigned)k); ctl_frame(RT_C_KEY); }
static void ctl_plain(int t)        { rt_reset(&TE); ctl_frame(t); }

static char capbuf[8192];
static int cap(int fd, char *out, int max)
{
    int n = 0, r;
    while (n < max - 1 && (r = sys_read(fd, out + n, max - 1 - n)) > 0) n += r;
    out[n] = 0;
    return n;
}

/* Read back the LAST RT_T_INPUT the shell published. */
static int last_input(char *prompt, int pmax, char *buf, int bmax, int *curs)
{
    static struct rt_parser p;
    rt_parser_init(&p);
    int n = cap(fd_rich_r, capbuf, sizeof capbuf);
    int off = 0, found = 0;
    while (off < n) {
        int took = rt_parser_feed(&p, capbuf + off, n - off);
        if (!took) break;
        off += took;
        struct rt_frame f;
        while (rt_parser_next(&p, &f)) {
            if (f.type == RT_T_INPUT) {
                struct rt_rd r; rt_rd_init(&r, &f);
                *curs = rt_rd_u16(&r);
                rt_rd_str(&r, prompt, pmax);
                rt_rd_str(&r, buf, bmax);
                found = 1;
            }
            rt_parser_done(&p, &f);
        }
    }
    return found;
}

/* ------------------------------------------------------------ tokenizer --- */

static void t_tokens(void)
{
    static char store[2048];
    struct tok tk[MAXTOK];

    int n = tokenize("echo hello world", store, sizeof store, tk, MAXTOK);
    CHK(n == 3, "plain: %d tokens", n);
    CHK(!strcmp(tk[0].s, "echo") && !strcmp(tk[2].s, "world"), "plain token text");

    n = tokenize("echo \"a|b\" 'c d'", store, sizeof store, tk, MAXTOK);
    CHK(n == 3, "quoted: %d tokens (a quoted pipe must not split)", n);
    CHK(n == 3 && !strcmp(tk[1].s, "a|b"), "quoted pipe -> '%s'", n == 3 ? tk[1].s : "?");
    CHK(n == 3 && !strcmp(tk[2].s, "c d"), "single-quoted space -> '%s'", n == 3 ? tk[2].s : "?");
    CHK(n == 3 && tk[1].op == 0, "quoted | reported as an operator");

    n = tokenize("a|b > f", store, sizeof store, tk, MAXTOK);
    CHK(n == 5 && tk[1].op == '|' && tk[3].op == '>', "operators: n=%d", n);

    n = tokenize("echo a\\ b", store, sizeof store, tk, MAXTOK);
    CHK(n == 2 && !strcmp(tk[1].s, "a b"), "backslash escape -> '%s'", n > 1 ? tk[1].s : "?");

    env_set("GREET", "hi");
    last_status = 7;
    n = tokenize("echo $GREET $? '$GREET'", store, sizeof store, tk, MAXTOK);
    CHK(n == 4, "expansion: %d tokens", n);
    CHK(n == 4 && !strcmp(tk[1].s, "hi"), "$VAR -> '%s'", n > 1 ? tk[1].s : "?");
    CHK(n == 4 && !strcmp(tk[2].s, "7"), "$? -> '%s'", n > 2 ? tk[2].s : "?");
    CHK(n == 4 && !strcmp(tk[3].s, "$GREET"), "single quotes must not expand -> '%s'", n > 3 ? tk[3].s : "?");

    n = tokenize("ls *.txt", store, sizeof store, tk, MAXTOK);
    CHK(n == 2 && tk[1].glob == 1, "glob flag not set");
    n = tokenize("ls '*.txt'", store, sizeof store, tk, MAXTOK);
    CHK(n == 2 && tk[1].glob == 0, "quoted glob must stay literal");

    CHK(glob_match("*.txt", "notes.txt"), "glob *.txt vs notes.txt");
    CHK(!glob_match("*.txt", "notes.md"), "glob *.txt vs notes.md");
    CHK(glob_match("note?.txt", "note2.txt"), "glob note?.txt");
    CHK(!glob_match("note?.txt", "notes12.txt"), "glob ? matched two chars");
    CHK(glob_match("*", "anything"), "glob *");
}

/* --------------------------------------------------------------- env ------ */

static void t_env(void)
{
    nenv = 0;
    c_strcpy(envstore[nenv++], "LOGIT_TERM=logit-rich-1", ENVLEN);
    c_strcpy(envstore[nenv++], "LOGIT_RICH=3", ENVLEN);
    c_strcpy(envstore[nenv++], "LOGIT_CTL=4", ENVLEN);
    c_strcpy(envstore[nenv++], "LOGIT_COLS=80", ENVLEN);

    char **e = env_build(1);
    int seen_rich = 0, seen_ctl = 0, n = 0;
    for (; e[n]; n++) {
        if (!strncmp(e[n], "LOGIT_RICH=", 11)) seen_rich = 1;
        if (!strncmp(e[n], "LOGIT_CTL=", 10)) seen_ctl = 1;
    }
    CHK(seen_rich && seen_ctl && n == 4, "rich child env: n=%d rich=%d ctl=%d", n, seen_rich, seen_ctl);

    e = env_build(0);
    seen_rich = seen_ctl = 0; n = 0;
    int seen_term = 0, seen_cols = 0;
    for (; e[n]; n++) {
        if (!strncmp(e[n], "LOGIT_RICH=", 11)) seen_rich = 1;
        if (!strncmp(e[n], "LOGIT_CTL=", 10)) seen_ctl = 1;
        if (!strncmp(e[n], "LOGIT_TERM=", 11)) seen_term = 1;
        if (!strncmp(e[n], "LOGIT_COLS=", 11)) seen_cols = 1;
    }
    /* THE compatibility mechanism: a stage whose stdout is not the terminal is
     * not given the channel, so it cannot emit protocol bytes into a file. */
    CHK(!seen_rich, "LOGIT_RICH leaked to a non-terminal stage");
    CHK(!seen_ctl, "LOGIT_CTL leaked to a non-terminal stage");
    CHK(seen_term && seen_cols, "the harmless env vars were dropped too (term=%d cols=%d)", seen_term, seen_cols);
    CHK(n == 2, "non-rich env: n=%d", n);
}

/* ------------------------------------------------------------- editing ---- */

static void begin_edit_session(void)
{
    interactive = 1;
    ctl_fd = 4;
    rt_attach(3);
    rt_parser_init(&cparse);
    rt_reset(&enc);
    pend_intr = pend_eof = 0;
    pend_text_n = 0; have_rerun = 0;
    hcount = 0; hpos = 0;
    c_strcpy(stub_cwd, "/home", sizeof stub_cwd);
}

static void t_editing(void)
{
    char prompt[96], shown[512];
    int curs = -1;

    begin_edit_session();

    /* typing + Enter */
    ctl_text("echo hi\n");
    CHK(edit_line() == EDIT_LINE, "Enter did not end the line");
    CHK(!strcmp(lbuf, "echo hi"), "typed line = '%s'", lbuf);

    /* backspace and ^U */
    ctl_text("abcd\b\b");
    ctl_text("Z\n");
    edit_line();
    CHK(!strcmp(lbuf, "abZ"), "backspace: '%s'", lbuf);

    ctl_text("junk");
    { char u[2] = { 21, 0 }; ctl_text(u); }          /* ^U */
    ctl_text("ok\n");
    edit_line();
    CHK(!strcmp(lbuf, "ok"), "^U did not clear: '%s'", lbuf);

    /* cursor movement: Left Left then insert */
    ctl_text("abc");
    ctl_key(KEY_LEFT); ctl_key(KEY_LEFT);
    ctl_text("X\n");
    edit_line();
    CHK(!strcmp(lbuf, "aXbc"), "insert at cursor: '%s'", lbuf);

    /* HOME/END */
    ctl_text("tail");
    ctl_key(KEY_HOME);
    ctl_text("head-");
    ctl_key(KEY_END);
    ctl_text("!\n");
    edit_line();
    CHK(!strcmp(lbuf, "head-tail!"), "home/end: '%s'", lbuf);

    /* history: three commands, then Up Up */
    hcount = 0;
    hist_add("one"); hist_add("two"); hist_add("three");
    ctl_key(KEY_UP); ctl_key(KEY_UP);
    ctl_text("\n");
    edit_line();
    CHK(!strcmp(lbuf, "two"), "history up twice: '%s'", lbuf);

    ctl_key(KEY_UP); ctl_key(KEY_UP); ctl_key(KEY_DOWN);
    ctl_text("\n");
    edit_line();
    CHK(!strcmp(lbuf, "three"), "history up-up-down: '%s'", lbuf);

    /* history does not record consecutive duplicates */
    hcount = 0;
    hist_add("dup"); hist_add("dup");
    CHK(hcount == 1, "duplicate history entry recorded (%d)", hcount);

    /* ^C clears the line but does not end it */
    ctl_text("garbage");
    ctl_plain(RT_C_INTR);
    ctl_text("clean\n");
    edit_line();
    CHK(!strcmp(lbuf, "clean"), "^C at the prompt: '%s'", lbuf);

    /* ^D on an EMPTY line is EOF; on a non-empty line it is ignored */
    ctl_text("keepme");
    ctl_plain(RT_C_EOF);
    ctl_text("\n");
    CHK(edit_line() == EDIT_LINE, "^D on a non-empty line ended the shell");
    CHK(!strcmp(lbuf, "keepme"), "^D ate the line: '%s'", lbuf);
    ctl_plain(RT_C_EOF);
    CHK(edit_line() == EDIT_EOF, "^D on an empty line did not end the shell");

    /* a pasted multi-line block runs line by line */
    ctl_text("first\nsecond\n");
    edit_line();
    CHK(!strcmp(lbuf, "first"), "paste line 1: '%s'", lbuf);
    edit_line();
    CHK(!strcmp(lbuf, "second"), "paste line 2: '%s'", lbuf);

    /* re-run, delivered as a control frame by a click in the terminal */
    rt_reset(&TE); rt_str(&TE, "echo rerun"); ctl_frame(RT_C_RERUN);
    edit_line();
    CHK(!strcmp(lbuf, "echo rerun"), "rerun: '%s'", lbuf);

    /* the shell publishes what it is editing, including the cursor column */
    cap(fd_rich_r, capbuf, sizeof capbuf);           /* drop history */
    ctl_text("abcdef");
    ctl_key(KEY_LEFT);
    ctl_text("\n");
    edit_line();
    CHK(last_input(prompt, sizeof prompt, shown, sizeof shown, &curs), "no RT_T_INPUT published");
    CHK(!strcmp(prompt, "/home $ "), "prompt = '%s'", prompt);
    CHK(!strcmp(shown, "abcdef"), "published buffer = '%s'", shown);
    CHK(curs == 5, "published cursor = %d (want 5)", curs);
}

/* ---------------------------------------------------------- completion ---- */

static void set_line(const char *s)
{ c_strcpy(lbuf, s, LINE); llen = c_strlen(lbuf); lcur = llen; }

static void t_completion(void)
{
    c_strcpy(stub_cwd, "/home", sizeof stub_cwd);

    set_line("ca");            do_complete();
    CHK(!strcmp(lbuf, "cat "), "unique command completion -> '%s'", lbuf);

    set_line("ch");            do_complete();
    CHK(!strcmp(lbuf, "ch"), "ambiguous chart/chmodx should not extend -> '%s'", lbuf);

    set_line("sh");            do_complete();
    CHK(!strcmp(lbuf, "show "), "command 'sh' -> '%s'", lbuf);

    set_line("s");             do_complete();
    CHK(!strcmp(lbuf, "s"), "ambiguous set/show must not extend -> '%s'", lbuf);

    set_line("cat note");      do_complete();
    CHK(!strcmp(lbuf, "cat note"), "ambiguous file prefix -> '%s'", lbuf);

    set_line("cat notes");     do_complete();
    CHK(!strcmp(lbuf, "cat notes.txt "), "unique file completion -> '%s'", lbuf);

    set_line("cat /bin/sh");   do_complete();
    CHK(!strcmp(lbuf, "cat /bin/show "), "path completion -> '%s'", lbuf);

    set_line("zzz");           do_complete();
    CHK(!strcmp(lbuf, "zzz"), "no match must change nothing -> '%s'", lbuf);
}

/* ------------------------------------------------------------ job control -- */

static int pump_pipe, pump_seen;

/* Drives the shell's own wait loop: type on nap 1, sample what reached the
 * job's stdin on nap 3, then let the phantom child exit. */
static void pump_hook(int n)
{
    if (n == 1) ctl_text("fed\n");
    if (n == 3) { pump_seen = stub_pipes[pump_pipe].count; stub_child_exit_all(); }
}

static void t_jobs(void)
{
    begin_edit_session();
    for (int i = 0; i < MAXJOBS; i++) jobs[i].used = 0;

    struct cmd c;
    c.argc = 1; c.argv[0] = (char *)"sleeper"; c.argv[1] = 0;
    c.infile = c.outfile = 0; c.append = 0;

    /* 1. a job that finishes on its own: the liveness pipe reports EOF and the
     *    child's status comes back. */
    stub_child_status = 3;
    struct job *j = start_pipeline(&c, 1, 0, "sleeper");
    CHK(j != 0, "start_pipeline returned nothing");
    if (j) {
        CHK(job_running(j), "a live job reported as finished");
        stub_child_exit_all();
        CHK(!job_running(j), "an exited job reported as running");
        int st = wait_foreground(j);
        CHK(st == 3, "status = %d, want 3", st);
    }

    /* 2. ^C while the job is still alive: the shell gets its prompt back, the
     *    job is marked interrupted, and the status is 130. */
    stub_child_status = 0;
    j = start_pipeline(&c, 1, 0, "sleeper");
    CHK(j != 0, "second start_pipeline failed");
    if (j) {
        ctl_plain(RT_C_INTR);
        int st = wait_foreground(j);
        CHK(st == 130, "interrupt status = %d, want 130", st);
        CHK(j->interrupted == 1, "job not marked interrupted");
        CHK(job_running(j), "the job should still be RUNNING -- there is no kill(2)");
        stub_child_exit_all();
    }

    /* 3. ^D during a job closes that job's stdin, so a child blocked on read
     *    sees EOF. That is what the control channel buys over stdin. */
    j = start_pipeline(&c, 1, 0, "reader");
    CHK(j != 0 && j->stdin_fd >= 0, "no per-job stdin pipe");
    if (j) {
        int sp = j->stdin_fd;
        int pipe_idx = stub_fds[sp].pipe;
        ctl_plain(RT_C_EOF);
        stub_child_exit_all();
        wait_foreground(j);
        CHK(stub_pipes[pipe_idx].writers == 0, "job stdin still has a writer after ^D");
    }

    /* 4. typed text during a job is pumped into the job's stdin by the real
     *    wait loop -- driven here through the nap hook so the timing is the
     *    shell's own, not the test's. */
    stub_child_status = 0;
    j = start_pipeline(&c, 1, 0, "reader2");
    if (j) {
        pump_pipe = stub_fds[j->stdin_fd].pipe;
        pump_seen = -1;
        stub_naps = 0;
        stub_nap_hook = pump_hook;
        wait_foreground(j);
        stub_nap_hook = 0;
        CHK(pump_seen == 4, "stdin pump delivered %d bytes, want 4", pump_seen);
    }
}

int main(void)
{
    wire_up();
    t_tokens();
    t_env();
    t_editing();
    t_completion();
    t_jobs();
    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails != 0;
}
