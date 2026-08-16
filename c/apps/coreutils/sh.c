#include "clib.h"
#include "logit_rich.h"

/* /bin/sh -- LogitOS shell.
 *
 * Two personalities, chosen by the environment, from one binary:
 *
 *   NON-INTERACTIVE (the serial console, `sh < script`): byte-for-byte the old
 *   behaviour -- prompt on stdout, one line read from fd 0, no echo, no frames.
 *   Nothing that consumes this shell over a pipe can tell the rest of this file
 *   exists, which is what keeps tests/boot/run-shell-test.sh honest.
 *
 *   INTERACTIVE (LOGIT_TERM + LOGIT_RICH + LOGIT_CTL are set, i.e. the GUI
 *   Terminal started us): the shell owns the line. It edits, keeps history and
 *   completes, and publishes the live buffer to the terminal as an RT_T_INPUT
 *   frame -- because the process that knows PATH, the cwd and the builtin list
 *   is the shell, not the window. It also reports command boundaries and exit
 *   status (RT_T_CMD_BEGIN / RT_T_CMD_END), which is what lets the terminal
 *   attribute every line of scrollback to the command that produced it.
 *
 * WHERE INTERACTIVE INPUT COMES FROM, AND WHY IT IS NOT STDIN
 * ----------------------------------------------------------
 * In interactive mode the terminal sends keystrokes down the CONTROL channel
 * (fd 4), not stdin, and the shell pumps them into the running job's own stdin
 * pipe. Three things fall out of that, none of which a stdin-only design can
 * have here:
 *   - ^C reaches the shell even while a job is running. On stdin it would be
 *     read by the job instead, which is precisely when you need it.
 *   - ^D closes the job's stdin for real, so a child blocked on read() sees EOF.
 *     There is no line discipline in this kernel to do that for us.
 *   - stdin stays a plain byte pipe with no terminal-private codes in it, so a
 *     program reading stdin gets exactly what was typed.
 * This is the tty line discipline, implemented in the process that should own
 * it. It costs the shell a per-job stdin pipe.
 *
 * KNOWN GAP: there is no kill(2) in this kernel. ^C therefore ABANDONS the
 * foreground job (you get your prompt back, the job is marked interrupted and
 * listed by `jobs`) but cannot terminate it. That needs one kernel primitive
 * and is called out in the report rather than faked here.
 */

#define LINE   512
#define MAXTOK 64
#define MAXARG 32
#define MAXCMD 8
#define HISTN  64

/* ------------------------------------------------------------ environment -- */

#define MAXENV 24
#define ENVLEN 160
static char envstore[MAXENV][ENVLEN];
static int  nenv;
static char *envbuild[MAXENV + 1];

static int env_find(const char *k)
{
    int kl = c_strlen(k);
    for (int i = 0; i < nenv; i++) {
        int j = 0;
        while (j < kl && envstore[i][j] == k[j]) j++;
        if (j == kl && envstore[i][j] == '=') return i;
    }
    return -1;
}

static const char *env_get(const char *k)
{ int i = env_find(k); return i < 0 ? 0 : envstore[i] + c_strlen(k) + 1; }

static void env_set(const char *k, const char *v)
{
    int i = env_find(k);
    if (i < 0) { if (nenv >= MAXENV) return; i = nenv++; }
    int n = 0;
    for (int j = 0; k[j] && n < ENVLEN - 2; j++) envstore[i][n++] = k[j];
    envstore[i][n++] = '=';
    for (int j = 0; v[j] && n < ENVLEN - 1; j++) envstore[i][n++] = v[j];
    envstore[i][n] = 0;
}

static void env_unset(const char *k)
{
    int i = env_find(k);
    if (i < 0) return;
    for (int j = i; j < nenv - 1; j++) c_strcpy(envstore[j], envstore[j + 1], ENVLEN);
    nenv--;
}

/* Build an envp for execve. `rich` says whether this child may talk to the
 * terminal: withholding LOGIT_RICH is the ENTIRE mechanism that keeps protocol
 * bytes out of a redirected file, and it is a decision only the shell can make
 * because only the shell knows the pipeline's shape. */
static char **env_build(int rich)
{
    int n = 0;
    for (int i = 0; i < nenv && n < MAXENV; i++) {
        if (!rich && (c_strncmp(envstore[i], "LOGIT_RICH=", 11) == 0 ||
                      c_strncmp(envstore[i], "LOGIT_CTL=", 10) == 0))
            continue;
        envbuild[n++] = envstore[i];
    }
    envbuild[n] = 0;
    return envbuild;
}

/* ------------------------------------------------------------- shell state -- */

static int  interactive;            /* the terminal is driving us */
static int  ctl_fd = -1;
static int  last_status;
static unsigned cmdid_next = 1;

static struct rt_parser cparse;
static struct rt_enc    enc;

/* Control-channel state.
 *
 * The channel is ORDERED, and the shell has to honour that: "abc", Left, "X"
 * must produce abXc. An earlier version of this file latched each frame type
 * into its own pending flag and applied them in a fixed order at the end of the
 * batch, which silently reordered every mixed sequence -- and collapsed two
 * Up-arrows into one. So frames are DISPATCHED AS THEY ARE PARSED, and the mode
 * below is what a frame's meaning depends on: at the prompt a keystroke edits
 * the line, during a job it belongs to the job's stdin. */
#define CTL_EDIT 0
#define CTL_JOB  1
static int  ctl_mode = CTL_EDIT;

static int  pend_intr, pend_eof;      /* only meaningful in CTL_JOB */
static char pend_text[1024];          /* CTL_JOB: bytes bound for the job's stdin.
                                       * CTL_EDIT: the tail of a pasted block that
                                       * belongs to the NEXT line. */
static int  pend_text_n;
static char pend_rerun[LINE];
static int  have_rerun;
static int  ed_done, ed_eof, ed_dirty;

static void nap(void) { if (sys_sleep_ms(3) < 0) sys_yield(); }

/* c_atoi's inverse; clib has outn() but no string form. */
static void outn_str(char *o, int v)
{
    char t[16]; int i = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    if (!v) { o[0] = '0'; o[1] = 0; return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int k = 0; while (i) o[k++] = t[--i]; o[k] = 0;
}

/* forward declarations: the control dispatcher drives the editor below. */
static void publish_input(void);
static void apply_key(int k);
static int  feed_edit_text(const char *s, int n);

/* ------------------------------------------------------------------ jobs --- */

struct job {
    int used, id, bg, done, interrupted, status;
    int npid, pids[MAXCMD];
    int live_fd;                 /* EOF here == every stage has exited        */
    int stdin_fd;                /* write end of the job's stdin, or -1       */
    char cmd[96];
};
#define MAXJOBS 8
static struct job jobs[MAXJOBS];
static int job_next = 1;

static struct job *job_alloc(void)
{
    for (int i = 0; i < MAXJOBS; i++) if (!jobs[i].used) { jobs[i].used = 1; jobs[i].id = job_next++; return &jobs[i]; }
    return 0;
}

static void job_release(struct job *j)
{
    if (j->live_fd >= 0) { sys_close(j->live_fd); j->live_fd = -1; }
    if (j->stdin_fd >= 0) { sys_close(j->stdin_fd); j->stdin_fd = -1; }
    j->used = 0;
}

/* Alive test without blocking: the job holds the only write ends of live_fd, so
 * a read that returns 0 (rather than EAGAIN) means every stage is gone. There is
 * no WNOHANG in this kernel's waitpid, and blocking in waitpid would make ^C
 * unreachable -- this is the standard pipe-as-a-process-handle trick, and it is
 * exact rather than a poll of a guessed condition. */
static int job_running(struct job *j)
{
    if (j->live_fd < 0) return 0;
    char c;
    int n = sys_read(j->live_fd, &c, 1);
    if (n == 0) return 0;
    return 1;                                    /* EAGAIN_RC, or a stray byte */
}

static void job_reap(struct job *j)
{
    for (int i = 0; i < j->npid; i++) {
        int st = 0;
        if (sys_waitpid(j->pids[i], &st) >= 0) j->status = st;
    }
}

/* ------------------------------------------------------------- line editor -- */

static char lbuf[LINE];
static int  llen, lcur;
static char hist[HISTN][LINE];
static int  hcount, hpos;
static char hstash[LINE];

static void hist_add(const char *s)
{
    if (!s[0]) return;
    if (hcount > 0 && c_streq(hist[(hcount - 1) % HISTN], s)) return;
    c_strcpy(hist[hcount % HISTN], s, LINE);
    hcount++;
}
static const char *hist_at(int back)        /* back = 1 -> most recent */
{
    if (back <= 0 || back > hcount || back > HISTN) return 0;
    return hist[(hcount - back) % HISTN];
}

static void prompt_text(char *out, int max)
{
    char cwd[128];
    sys_getcwd(cwd, sizeof cwd);
    int n = 0;
    for (int i = 0; cwd[i] && n < max - 4; i++) out[n++] = cwd[i];
    out[n++] = ' '; out[n++] = '$'; out[n++] = ' ';
    out[n] = 0;
}

static void publish_input(void)
{
    if (!interactive) return;
    char p[96];
    prompt_text(p, sizeof p);
    rt_reset(&enc);
    rt_u16(&enc, lcur);
    rt_str(&enc, p);
    rt_strn(&enc, lbuf, llen);
    rt_send(RT_T_INPUT, &enc);
}

/* ------------------------------------------------------------- completion -- */

static const char *BUILTINS[] = { "cd", "pwd", "exit", "help", "export", "unset",
                                  "jobs", "fg", "wait", "history", "set", 0 };

/* Extend `pfx` to the longest common prefix of everything that starts with it.
 * Returns the number of matches and leaves the completion in `out`. */
struct compl_st { char out[128]; int n; char first[128]; };

static void compl_offer(struct compl_st *cs, const char *cand)
{
    if (cs->n == 0) { c_strcpy(cs->out, cand, sizeof cs->out); c_strcpy(cs->first, cand, sizeof cs->first); }
    else {
        int i = 0;
        while (cs->out[i] && cs->out[i] == cand[i]) i++;
        cs->out[i] = 0;
    }
    cs->n++;
}

static int has_prefix(const char *s, const char *p)
{ int i = 0; while (p[i]) { if (s[i] != p[i]) return 0; i++; } return 1; }

/* Split the word under the cursor into (dir, leaf) for path completion. */
static void split_path(const char *word, char *dir, int dmax, char *leaf, int lmax)
{
    int last = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') last = i;
    if (last < 0) { c_strcpy(dir, ".", dmax); c_strcpy(leaf, word, lmax); return; }
    int n = 0;
    for (int i = 0; i <= last && n < dmax - 1; i++) dir[n++] = word[i];
    if (n > 1 && dir[n - 1] == '/') n--;
    if (n == 0) dir[n++] = '/';
    dir[n] = 0;
    c_strcpy(leaf, word + last + 1, lmax);
}

static void do_complete(void)
{
    /* the word under the cursor */
    int ws = lcur;
    while (ws > 0 && lbuf[ws - 1] != ' ') ws--;
    char word[128];
    int wl = lcur - ws;
    if (wl > (int)sizeof word - 1) return;
    for (int i = 0; i < wl; i++) word[i] = lbuf[ws + i];
    word[wl] = 0;

    /* is it the command position? */
    int first = 1;
    for (int i = 0; i < ws; i++) if (lbuf[i] != ' ') { first = 0; break; }

    struct compl_st cs; cs.n = 0; cs.out[0] = 0; cs.first[0] = 0;
    char dir[128], leaf[128];

    if (first && !has_prefix(word, "/") && !has_prefix(word, "./")) {
        for (int i = 0; BUILTINS[i]; i++) if (has_prefix(BUILTINS[i], word)) compl_offer(&cs, BUILTINS[i]);
        int n = dir_count("/bin");
        for (int i = 0; i < n; i++) {
            char nm[64];
            if (dir_name("/bin", i, nm) == -1) continue;
            if (has_prefix(nm, word)) compl_offer(&cs, nm);
        }
        c_strcpy(dir, "", sizeof dir);
        c_strcpy(leaf, word, sizeof leaf);
    } else {
        split_path(word, dir, sizeof dir, leaf, sizeof leaf);
        const char *d = dir[0] ? dir : ".";
        int n = dir_count(d);
        for (int i = 0; i < n; i++) {
            char nm[64];
            if (dir_name(d, i, nm) == -1) continue;
            if (has_prefix(nm, leaf)) compl_offer(&cs, nm);
        }
    }
    if (cs.n == 0) return;

    /* replace the leaf with the completion */
    int keep = ws + (int)(c_strlen(word) - c_strlen(leaf));
    int add = c_strlen(cs.out);
    int tail = llen - lcur;
    if (keep + add + tail >= LINE - 2) return;
    for (int i = tail - 1; i >= 0; i--) lbuf[keep + add + i] = lbuf[lcur + i];
    for (int i = 0; i < add; i++) lbuf[keep + i] = cs.out[i];
    llen = keep + add + tail;
    lcur = keep + add;
    if (cs.n == 1 && llen < LINE - 2) {
        /* exactly one answer: a trailing space is the confirmation */
        for (int i = tail - 1; i >= 0; i--) lbuf[lcur + 1 + i] = lbuf[lcur + i];
        lbuf[lcur++] = ' ';
        llen++;
    }
    lbuf[llen] = 0;
}

/* ----------------------------------------------------------- input, edited -- */

#define EDIT_LINE 0
#define EDIT_EOF  1

static void ins_char(char c)
{
    if (llen >= LINE - 2) return;
    for (int i = llen; i > lcur; i--) lbuf[i] = lbuf[i - 1];
    lbuf[lcur++] = c;
    llen++;
    lbuf[llen] = 0;
}

static void apply_key(int k)
{
    switch (k) {
    case KEY_LEFT:  if (lcur > 0) lcur--; break;
    case KEY_RIGHT: if (lcur < llen) lcur++; break;
    case KEY_HOME:  lcur = 0; break;
    case KEY_END:   lcur = llen; break;
    case KEY_UP:
        if (hpos == 0) c_strcpy(hstash, lbuf, LINE);
        if (hpos < hcount && hpos < HISTN) {
            hpos++;
            const char *h = hist_at(hpos);
            if (h) { c_strcpy(lbuf, h, LINE); llen = c_strlen(lbuf); lcur = llen; }
        }
        break;
    case KEY_DOWN:
        if (hpos > 0) {
            hpos--;
            const char *h = hpos ? hist_at(hpos) : hstash;
            c_strcpy(lbuf, h ? h : "", LINE); llen = c_strlen(lbuf); lcur = llen;
        }
        break;
    default: break;
    }
}

/* Apply typed bytes to the line. Returns the index of the byte that ENDED the
 * line (a newline), or -1 if the whole run was consumed. */
static int feed_edit_text(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        ed_dirty = 1;
        if (c == '\n' || c == '\r') { lbuf[llen] = 0; return i; }
        if (c == '\b' || c == 127) {
            if (lcur > 0) {
                for (int k = lcur - 1; k < llen - 1; k++) lbuf[k] = lbuf[k + 1];
                lcur--; llen--; lbuf[llen] = 0;
            }
            continue;
        }
        if (c == '\t') { do_complete(); continue; }
        if (c == 21) { llen = 0; lcur = 0; lbuf[0] = 0; continue; }   /* ^U kill line  */
        if (c == 1)  { lcur = 0; continue; }                          /* ^A line start */
        if (c < 32) continue;                                          /* no in-band control language */
        ins_char(c);
    }
    return -1;
}

/* Stash bytes that arrived after the newline: a pasted block must run line by
 * line rather than collapsing into one command. */
static void stash_tail(const char *s, int n)
{
    if (n < 0) n = 0;
    if (n > (int)sizeof pend_text) n = (int)sizeof pend_text;
    for (int i = 0; i < n; i++) pend_text[i] = s[i];
    pend_text_n = n;
}

/* Drain the control channel, dispatching each frame AS IT IS PARSED so the
 * channel's order is the order the shell sees. */
static void ctl_poll(void)
{
    if (ctl_fd < 0) return;
    unsigned char ob[512];
    ed_dirty = 0;
    for (int rounds = 0; rounds < 16; rounds++) {
        struct rt_frame f;
        while (rt_parser_next(&cparse, &f)) {
            struct rt_rd r; rt_rd_init(&r, &f);
            int stop = 0;
            switch (f.type) {
            case RT_C_INTR:
                if (ctl_mode == CTL_EDIT) { llen = 0; lcur = 0; lbuf[0] = 0; hpos = 0; ed_dirty = 1; }
                else pend_intr = 1;
                break;
            case RT_C_EOF:
                if (ctl_mode == CTL_EDIT) { if (llen == 0) { ed_eof = 1; stop = 1; } }
                else pend_eof = 1;
                break;
            case RT_C_KEY: {
                int k = (int)rt_rd_u32(&r);
                if (ctl_mode == CTL_EDIT && !r.bad) { apply_key(k); ed_dirty = 1; }
                break;
            }
            case RT_C_SIZE: {
                int c = rt_rd_u16(&r), rw = rt_rd_u16(&r), ce = rt_rd_u16(&r);
                char n[12];
                if (!r.bad) {
                    outn_str(n, c);  env_set("LOGIT_COLS", n);
                    outn_str(n, rw); env_set("LOGIT_ROWS", n);
                    outn_str(n, ce); env_set("LOGIT_CELL", n);
                }
                break;
            }
            case RT_C_TEXT: {
                char t[512];
                int n = rt_rd_str(&r, t, sizeof t);
                if (ctl_mode == CTL_EDIT) {
                    int nl = feed_edit_text(t, n);
                    if (nl >= 0) { stash_tail(t + nl + 1, n - nl - 1); ed_done = 1; stop = 1; }
                } else {
                    for (int i = 0; i < n && pend_text_n < (int)sizeof pend_text; i++)
                        pend_text[pend_text_n++] = t[i];
                }
                break;
            }
            case RT_C_RERUN:
                rt_rd_str(&r, pend_rerun, sizeof pend_rerun);
                if (!r.bad) { have_rerun = 1; if (ctl_mode == CTL_EDIT) stop = 1; }
                break;
            default: break;
            }
            rt_parser_done(&cparse, &f);
            if (stop) { if (ed_dirty) publish_input(); return; }
        }
        int n = sys_read(ctl_fd, ob, sizeof ob);
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            int took = rt_parser_feed(&cparse, ob + off, n - off);
            if (took == 0) break;
            off += took;
        }
    }
    if (ed_dirty && ctl_mode == CTL_EDIT) publish_input();
}

/* Read one edited line into lbuf. Interactive only. */
static int edit_line(void)
{
    llen = 0; lcur = 0; lbuf[0] = 0; hpos = 0;
    ed_done = 0; ed_eof = 0;
    ctl_mode = CTL_EDIT;

    if (pend_text_n) {                        /* the tail of a pasted block */
        char t[sizeof pend_text];
        int n = pend_text_n;
        for (int i = 0; i < n; i++) t[i] = pend_text[i];
        pend_text_n = 0;
        int nl = feed_edit_text(t, n);
        if (nl >= 0) { stash_tail(t + nl + 1, n - nl - 1); publish_input(); return EDIT_LINE; }
    }
    publish_input();

    for (;;) {
        ctl_poll();
        if (have_rerun) {
            have_rerun = 0;
            c_strcpy(lbuf, pend_rerun, LINE);
            llen = c_strlen(lbuf); lcur = llen;
            publish_input();
            return EDIT_LINE;
        }
        if (ed_eof) return EDIT_EOF;
        if (ed_done) return EDIT_LINE;
        nap();
    }
}

/* ---------------------------------------------------------- tokenizer ------ */

struct tok { char *s; unsigned char op; unsigned char glob; };

/* Quote-aware split. Operators are recognized only OUTSIDE quotes, which is the
 * whole reason the old spacify()-then-split approach had to go: it turned
 * `echo "a|b"` into a pipeline. $VAR and $? expand here too (not in '...'). */
static int tokenize(const char *in, char *store, int storemax, struct tok *out, int maxtok)
{
    int n = 0, si = 0;
    const char *p = in;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n >= maxtok) break;

        if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {
            if (si + 2 >= storemax) break;
            out[n].s = store + si;
            store[si++] = *p; store[si++] = 0;
            out[n].op = (unsigned char)*p; out[n].glob = 0;
            n++; p++;
            continue;
        }

        out[n].s = store + si;
        out[n].op = 0; out[n].glob = 0;
        int q = 0;                            /* 0 none, 1 single, 2 double */
        while (*p && si < storemax - 1) {
            char c = *p;
            if (!q && (c == ' ' || c == '\t' || c == '|' || c == '<' || c == '>' || c == '&')) break;
            if (!q && c == '\'') { q = 1; p++; continue; }
            if (!q && c == '"')  { q = 2; p++; continue; }
            if (q == 1 && c == '\'') { q = 0; p++; continue; }
            if (q == 2 && c == '"')  { q = 0; p++; continue; }
            if (q != 1 && c == '\\' && p[1]) { store[si++] = p[1]; p += 2; continue; }
            if (q != 1 && c == '$' && p[1]) {
                p++;
                if (*p == '?') {
                    char t[16];
                    outn_str(t, last_status);
                    p++;
                    for (int i = 0; t[i] && si < storemax - 1; i++) store[si++] = t[i];
                    continue;
                }
                char name[64]; int k = 0;
                while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                              (*p >= '0' && *p <= '9') || *p == '_') && k < 63) name[k++] = *p++;
                name[k] = 0;
                const char *v = k ? env_get(name) : 0;
                if (v) for (int i = 0; v[i] && si < storemax - 1; i++) store[si++] = v[i];
                continue;
            }
            if (!q && (c == '*' || c == '?')) out[n].glob = 1;
            store[si++] = c;
            p++;
        }
        store[si++] = 0;
        n++;
    }
    return n;
}

/* --------------------------------------------------------------- globbing -- */

static int glob_match(const char *pat, const char *s)
{
    if (*pat == 0) return *s == 0;
    if (*pat == '*') {
        for (const char *t = s; ; t++) { if (glob_match(pat + 1, t)) return 1; if (!*t) return 0; }
    }
    if (!*s) return 0;
    if (*pat == '?' || *pat == *s) return glob_match(pat + 1, s + 1);
    return 0;
}

/* Expand one globbed token into argv. Returns how many entries were added; 0
 * means "no match", and the caller keeps the literal word (sh's rule). */
static int glob_expand(const char *word, char *store, int *si, int storemax,
                       char **argv, int argc, int maxarg)
{
    char dir[128], leaf[128];
    split_path(word, dir, sizeof dir, leaf, sizeof leaf);
    const char *d = dir[0] ? dir : ".";
    int n = dir_count(d), added = 0;
    for (int i = 0; i < n && argc + added < maxarg; i++) {
        char nm[64];
        if (dir_name(d, i, nm) == -1) continue;
        if (nm[0] == '.' && leaf[0] != '.') continue;
        if (!glob_match(leaf, nm)) continue;
        char full[192];
        if (dir[0] && !c_streq(dir, ".")) path_join(full, dir, nm, sizeof full);
        else c_strcpy(full, nm, sizeof full);
        int l = c_strlen(full);
        if (*si + l + 1 >= storemax) break;
        argv[argc + added] = store + *si;
        c_strcpy(store + *si, full, l + 1);
        *si += l + 1;
        added++;
    }
    return added;
}

/* --------------------------------------------------------------- pipeline -- */

struct cmd { char *argv[MAXARG + 1]; int argc; char *infile, *outfile; int append; };

static void run_external(char **argv, char **envp)
{
    if (!argv[0]) app_exit(0);
    char buf[160];
    if (c_strlen(argv[0]) > (int)sizeof buf - 6) {
        errs("sh: command name too long: "); errs(argv[0]); errs("\n");
        app_exit(127);
    }
    /* A NAME WITH A SLASH IS A PATH, not something to look up. Without this
     * every absolute-path command was tried as "/bin" + the path first --
     * `/bin/smptest` became `/bin/bin/smptest` -- which failed, printed
     * "[execve] /bin/bin/smptest: missing/too small (-1)" to the console, and
     * only then fell through to the path the user actually typed. The command
     * ran, so nobody chased the line; it is one wasted failing execve and one
     * misleading kernel diagnostic on every such invocation. */
    int has_slash = 0;
    for (int i = 0; argv[0][i]; i++) if (argv[0][i] == '/') { has_slash = 1; break; }
    if (!has_slash) {
        c_strcpy(buf, "/bin/", sizeof buf);
        int n = 5; for (int i = 0; argv[0][i] && n < (int)sizeof buf - 1; i++) buf[n++] = argv[0][i];
        buf[n] = 0;
        sys_execve(buf, argv, envp);
    }
    sys_execve(argv[0], argv, envp);
    errs("sh: command not found: "); errs(argv[0]); errs("\n");
    app_exit(127);
}

/* Launch every stage. The rich channel goes to exactly one stage: the last one,
 * and only when its stdout is still the terminal. */
static struct job *start_pipeline(struct cmd *cmds, int ncmd, int background, const char *text)
{
    struct job *j = job_alloc();
    if (!j) { errs("sh: too many jobs\n"); return 0; }
    j->npid = 0; j->bg = background; j->done = 0; j->interrupted = 0; j->status = 0;
    j->live_fd = -1; j->stdin_fd = -1;
    c_strcpy(j->cmd, text, sizeof j->cmd);

    int lv[2] = { -1, -1 };
    if (sys_pipe(lv) < 0) { job_release(j); errs("sh: pipe failed\n"); return 0; }

    int sp[2] = { -1, -1 };
    if (interactive) {
        if (sys_pipe(sp) < 0) { sys_close(lv[0]); sys_close(lv[1]); job_release(j); return 0; }
        if (background) { sys_close(sp[1]); sp[1] = -1; }   /* background stdin: instant EOF */
    }

    int last_rich = !background && !cmds[ncmd - 1].outfile;
    int prev_read = -1;

    for (int i = 0; i < ncmd; i++) {
        int pfd[2] = { -1, -1 };
        if (i < ncmd - 1 && sys_pipe(pfd) < 0) { errs("sh: pipe failed\n"); break; }

        int pid = sys_fork();
        if (pid < 0) {
            errs("sh: fork failed\n");
            if (pfd[0] >= 0) sys_close(pfd[0]);
            if (pfd[1] >= 0) sys_close(pfd[1]);
            break;
        }
        if (pid == 0) {                                   /* child */
            if (prev_read >= 0) sys_dup2(prev_read, 0);
            else if (interactive && sp[0] >= 0) sys_dup2(sp[0], 0);
            if (i < ncmd - 1) sys_dup2(pfd[1], 1);
            if (cmds[i].infile) {
                int f = sys_open(cmds[i].infile, O_RDONLY);
                if (f < 0) { errs("sh: cannot open input\n"); app_exit(1); }
                sys_dup2(f, 0); sys_close(f);
            }
            if (cmds[i].outfile) {
                int f = sys_open(cmds[i].outfile, O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC));
                if (f < 0) { errs("sh: cannot open output\n"); app_exit(1); }
                sys_dup2(f, 1); sys_close(f);
            }
            if (prev_read >= 0) sys_close(prev_read);
            if (pfd[0] >= 0) sys_close(pfd[0]);
            if (pfd[1] >= 0) sys_close(pfd[1]);
            if (sp[0] >= 0) sys_close(sp[0]);
            if (sp[1] >= 0) sys_close(sp[1]);
            sys_close(lv[0]);                             /* keep lv[1]: it IS the handle */
            if (ctl_fd >= 0) sys_close(ctl_fd);           /* the control channel is ours */

            int rich = (i == ncmd - 1) && last_rich;
            if (!rich && rt_isrich()) sys_close(3);
            run_external(cmds[i].argv, env_build(rich));
            app_exit(127);
        }
        if (prev_read >= 0) sys_close(prev_read);
        if (pfd[1] >= 0) sys_close(pfd[1]);
        prev_read = pfd[0];
        if (j->npid < MAXCMD) j->pids[j->npid++] = pid;
    }
    if (prev_read >= 0) sys_close(prev_read);
    sys_close(lv[1]);                                     /* only children hold it now */
    if (sp[0] >= 0) sys_close(sp[0]);
    j->live_fd = lv[0];
    j->stdin_fd = sp[1];
    sys_set_nonblock(j->live_fd);
    if (j->stdin_fd >= 0) sys_set_nonblock(j->stdin_fd);
    return j;
}

/* Wait for a foreground job, staying responsive to ^C and pumping keystrokes
 * into the job's stdin. Returns the exit status. */
static int wait_foreground(struct job *j)
{
    ctl_mode = CTL_JOB;
    pend_intr = 0; pend_eof = 0; pend_text_n = 0;
    for (;;) {
        ctl_poll();
        if (pend_intr) {
            pend_intr = 0;
            j->interrupted = 1;
            j->bg = 1;                                    /* abandoned, not killed */
            errs("^C\n");
            return 130;
        }
        if (pend_eof && j->stdin_fd >= 0) {
            pend_eof = 0;
            sys_close(j->stdin_fd); j->stdin_fd = -1;      /* real EOF for the child */
        }
        if (pend_text_n && j->stdin_fd >= 0) {
            int n = pend_text_n; pend_text_n = 0;
            for (int i = 0; i < n; ) {
                int w = sys_write(j->stdin_fd, pend_text + i, n - i);
                if (w <= 0) break;
                i += w;
            }
        }
        if (!job_running(j)) break;
        nap();
    }
    ctl_mode = CTL_EDIT;
    job_reap(j);
    int st = j->status;
    job_release(j);
    return st;
}

/* ---------------------------------------------------------------- builtins -- */

static void print_jobs(void)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (!jobs[i].used) continue;
        char n[16];
        outn_str(n, jobs[i].id);
        rt_out("["); rt_out(n); rt_out("] ");
        rt_out(job_running(&jobs[i]) ? (jobs[i].interrupted ? "abandoned " : "running   ") : "done      ");
        rt_out(jobs[i].cmd);
        rt_out("\n");
    }
}

static int builtin(struct cmd *c)
{
    if (c->argc == 0) return 1;
    const char *a0 = c->argv[0];

    /* NAME=value with nothing else is an assignment, not a command */
    if (c->argc == 1) {
        int eq = -1;
        for (int i = 0; a0[i]; i++) if (a0[i] == '=') { eq = i; break; }
        if (eq > 0) {
            char k[64];
            int n = eq < 63 ? eq : 63;
            for (int i = 0; i < n; i++) k[i] = a0[i];
            k[n] = 0;
            env_set(k, a0 + eq + 1);
            return 1;
        }
    }
    if (c_streq(a0, "exit")) app_exit(c->argc > 1 ? c_atoi(c->argv[1]) : last_status);
    if (c_streq(a0, "cd")) {
        const char *d = c->argc > 1 ? c->argv[1] : "/";
        if (sys_chdir(d) < 0) { errs("cd: no such directory: "); errs(d); errs("\n"); last_status = 1; }
        else last_status = 0;
        return 1;
    }
    if (c_streq(a0, "pwd")) { char b[128]; sys_getcwd(b, sizeof b); rt_out(b); rt_out("\n"); last_status = 0; return 1; }
    if (c_streq(a0, "export")) {
        if (c->argc > 1) {
            int eq = -1;
            for (int i = 0; c->argv[1][i]; i++) if (c->argv[1][i] == '=') { eq = i; break; }
            if (eq > 0) {
                char k[64];
                int n = eq < 63 ? eq : 63;
                for (int i = 0; i < n; i++) k[i] = c->argv[1][i];
                k[n] = 0;
                env_set(k, c->argv[1] + eq + 1);
            }
        }
        last_status = 0; return 1;
    }
    if (c_streq(a0, "unset")) { if (c->argc > 1) env_unset(c->argv[1]); last_status = 0; return 1; }
    if (c_streq(a0, "set")) {
        for (int i = 0; i < nenv; i++) { rt_out(envstore[i]); rt_out("\n"); }
        last_status = 0; return 1;
    }
    if (c_streq(a0, "history")) {
        for (int i = hcount > HISTN ? hcount - HISTN : 0; i < hcount; i++) {
            char n[16]; outn_str(n, i + 1);
            rt_out(n); rt_out("  "); rt_out(hist[i % HISTN]); rt_out("\n");
        }
        last_status = 0; return 1;
    }
    if (c_streq(a0, "jobs")) { print_jobs(); last_status = 0; return 1; }
    if (c_streq(a0, "fg") || c_streq(a0, "wait")) {
        int want = c->argc > 1 ? c_atoi(c->argv[1]) : 0;
        for (int i = 0; i < MAXJOBS; i++) {
            if (!jobs[i].used) continue;
            if (want && jobs[i].id != want) continue;
            jobs[i].bg = 0;
            last_status = wait_foreground(&jobs[i]);
            if (want) break;
        }
        return 1;
    }
    if (c_streq(a0, "help")) {
        rt_out("LogitOS shell\n");
        rt_out("builtins: cd pwd exit export unset set jobs fg wait history help\n");
        rt_out("syntax:   a | b, < >, >>, &, 'quotes', \"quotes\", $VAR, $?, * ? globs\n");
        rt_out("keys:     Tab complete, Up/Down history, ^C interrupt, ^D exit, ^A/^U edit\n");
        rt_out("rich:     show <img>  dir [path]  chart  prog  (structured output)\n");
        last_status = 0; return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- exec a line -- */

static void exec_line(char *line)
{
    static char store[LINE * 3];
    static char gstore[LINE * 3];
    struct tok tk[MAXTOK];
    int nt = tokenize(line, store, sizeof store, tk, MAXTOK);
    if (nt == 0) return;

    struct cmd cmds[MAXCMD];
    int ncmd = 0, background = 0, gi = 0;
    cmds[0].argc = 0; cmds[0].infile = cmds[0].outfile = 0; cmds[0].append = 0;
    for (int i = 0; i < nt; i++) {
        if (tk[i].op == '|') {
            cmds[ncmd].argv[cmds[ncmd].argc] = 0;
            if (++ncmd >= MAXCMD) { errs("sh: pipeline too long\n"); return; }
            cmds[ncmd].argc = 0; cmds[ncmd].infile = cmds[ncmd].outfile = 0; cmds[ncmd].append = 0;
        } else if (tk[i].op == '<') { if (i + 1 < nt) cmds[ncmd].infile = tk[++i].s; }
        else if (tk[i].op == '>') {
            if (i + 1 < nt && tk[i + 1].op == '>') { cmds[ncmd].append = 1; i++; }
            if (i + 1 < nt) cmds[ncmd].outfile = tk[++i].s;
        }
        else if (tk[i].op == '&') { background = 1; }
        else if (cmds[ncmd].argc < MAXARG) {
            if (tk[i].glob) {
                int added = glob_expand(tk[i].s, gstore, &gi, (int)sizeof gstore,
                                        cmds[ncmd].argv, cmds[ncmd].argc, MAXARG);
                if (added) { cmds[ncmd].argc += added; continue; }
            }
            cmds[ncmd].argv[cmds[ncmd].argc++] = tk[i].s;
        }
    }
    cmds[ncmd].argv[cmds[ncmd].argc] = 0;
    ncmd++;

    if (ncmd == 1 && builtin(&cmds[0])) return;

    struct job *j = start_pipeline(cmds, ncmd, background, line);
    if (!j) { last_status = 1; return; }
    if (background) {
        char n[16]; outn_str(n, j->id);
        rt_out("["); rt_out(n); rt_out("] background\n");
        last_status = 0;
        return;
    }
    last_status = wait_foreground(j);
}

/* Sweep finished background jobs so their slots (and zombies) do not pile up. */
static void reap_background(void)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (!jobs[i].used || !jobs[i].bg) continue;
        if (job_running(&jobs[i])) continue;
        job_reap(&jobs[i]);
        job_release(&jobs[i]);
    }
}

/* ------------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    /* Import the environment execve already put on our stack. */
    {
        char **e = rt_envp(argc, argv);
        if (e) for (int i = 0; e[i] && nenv < MAXENV; i++) c_strcpy(envstore[nenv++], e[i], ENVLEN);
    }
    int rich = rt_init(argc, argv);
    const char *cs = rt_getenv(argc, argv, "LOGIT_CTL");
    if (rich && cs) {
        ctl_fd = c_atoi(cs);
        if (ctl_fd >= 3) { sys_set_nonblock(ctl_fd); interactive = 1; }
        else ctl_fd = -1;
    }
    rt_parser_init(&cparse);
    rt_reset(&enc);

    if (!interactive) {
        /* Unchanged from the pre-rich shell, deliberately. */
        outs("LogitOS shell -- type 'help'\n");
        char line[LINE];
        for (;;) {
            char cwd[128]; sys_getcwd(cwd, sizeof cwd);
            outs(cwd); outs(" $ ");
            int n = readline(0, line, sizeof line);
            if (n < 0) break;
            if (n == 0) continue;
            exec_line(line);
        }
        return 0;
    }

    rt_reset(&enc);
    rt_str(&enc, "sh");
    rt_send(RT_T_HELLO, &enc);

    for (;;) {
        reap_background();
        if (edit_line() == EDIT_EOF) break;
        char line[LINE];
        c_strcpy(line, lbuf, sizeof line);
        int blank = 1;
        for (int i = 0; line[i]; i++) if (line[i] != ' ' && line[i] != '\t') { blank = 0; break; }
        if (blank) continue;
        hist_add(line);

        /* The command has left the input line and is about to become a line of
         * scrollback; publish the empty buffer so the terminal does not keep
         * showing it in both places at once. */
        llen = 0; lcur = 0; lbuf[0] = 0;
        publish_input();

        unsigned id = cmdid_next++;
        rt_reset(&enc);
        rt_u32(&enc, id);
        rt_str(&enc, line);
        rt_send(RT_T_CMD_BEGIN, &enc);

        exec_line(line);

        rt_reset(&enc);
        rt_u32(&enc, id);
        rt_u32(&enc, (unsigned)last_status);
        rt_u8(&enc, last_status == 130 ? RT_END_INTERRUPTED : 0);
        rt_send(RT_T_CMD_END, &enc);
    }
    return 0;
}
