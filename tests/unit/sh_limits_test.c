/* Host test for /bin/sh's LIMITS -- not its features.
 *
 * Every bound in c/apps/coreutils/sh.c used to be a silent truncation that ran
 * the shortened command with status 0 (measured on device 2026-08-20: `echo`
 * with 40 arguments printed 31; a 600-byte line ran as its first 506 bytes).
 * This file pins the replacement contract, bound by bound:
 *
 *   AT the limit     the command runs, whole;
 *   PAST the limit   nothing runs -- not the prefix -- and stderr names the
 *                    limit it refused on, with the constant's value in it.
 *
 * Compiles the REAL sh.c against tests/unit/sh_hoststub.h, exactly as
 * sh_edit_test.c does, so the tokenizer, the glob expander, the argv builder
 * and the line reader under test are the ones the OS runs. The negative
 * control (tests/exec.mk: test-sh-limits-negctl, -DSH_LIMITS_NEGCTL) puts
 * every truncation back the way it shipped, and THIS file must then fail on
 * the refusal checks and ONLY those: the at-the-limit checks must keep
 * passing against it, or the suite is measuring "did the shell break" rather
 * than "does the shell refuse". */

#include "sh_hoststub.h"

#define main sh_main
#include "sh.c"
#undef main

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHK(cond, ...) do { checks++; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                     printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define OK(...) do { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- wiring: fd 1 = stdout, fd 2 = stderr, both captured ---------------- */
static int fd_out_r, fd_err_r;

static void wire_up(void)
{
    int po = stub_pipe_alloc(), pe = stub_pipe_alloc();
    stub_fd_bind(1, po, 1);
    stub_fd_bind(2, pe, 1);
    fd_out_r = stub_fd_alloc(po, 0);
    fd_err_r = stub_fd_alloc(pe, 0);
}

static int cap(int fd, char *out, int max)
{
    int n = 0, r;
    while (n < max - 1 && (r = sys_read(fd, out + n, max - 1 - n)) > 0) n += r;
    out[n] = 0;
    return n;
}

/* The phantom children never run, so a foreground job would wait forever on
 * its liveness pipe; the nap hook is where the stub lets them "exit". */
static void on_nap(int naps) { (void)naps; stub_child_exit_all(); }

/* Build "echo a a a ..." with `words` words in total (the command included). */
static void words_line(char *line, int max, int words)
{
    int n = 0;
    const char *w = "echo";
    for (int i = 0; w[i]; i++) line[n++] = w[i];
    for (int i = 1; i < words && n + 3 < max; i++) { line[n++] = ' '; line[n++] = 'a'; }
    line[n] = 0;
}

static char errbuf[4096], outbuf[4096];

/* ---- 1. tokenize: the word bound and the byte bound --------------------- */
static void t_tokenize(void)
{
    static char store[STORE];
    static struct tok tk[MAXTOK];
    static char line[LINE];

    /* Exactly MAXTOK words: accepted, all of them. */
    int n = 0;
    for (int i = 0; i < MAXTOK; i++) { if (i) line[n++] = ' '; line[n++] = 'w'; }
    line[n] = 0;
    int nt = tokenize(line, store, sizeof store, tk, MAXTOK);
    CHK(nt == MAXTOK, "MAXTOK (%d) words must tokenize whole: got %d", MAXTOK, nt);

    /* One more: refused, not the first MAXTOK. */
    line[n++] = ' '; line[n++] = 'w'; line[n] = 0;
    nt = tokenize(line, store, sizeof store, tk, MAXTOK);
    CHK(nt == TOK_E_WORDS, "MAXTOK+1 words must be TOK_E_WORDS (%d), got %d", TOK_E_WORDS, nt);

    /* The byte bound is reached by EXPANSION, not by line length: $A is two
     * bytes in the line and 150 in the store. 100 references = 15,000 bytes
     * fits a 16 KiB arena; 120 = 18,000 does not. */
    char big[ENVLEN];
    memset(big, 'x', 150); big[150] = 0;
    env_set("A", big);
    n = 0;
    for (int i = 0; i < 5; i++) line[n++] = "echo "[i];
    for (int i = 0; i < 100; i++) { line[n++] = '$'; line[n++] = 'A'; }
    line[n] = 0;
    nt = tokenize(line, store, sizeof store, tk, MAXTOK);
    CHK(nt == 2 && (int)strlen(tk[1].s) == 15000,
        "100 x $A (15,000 bytes) must expand whole: nt %d len %d", nt, nt == 2 ? (int)strlen(tk[1].s) : -1);
    CHK(tok_used == 5 + 15001, "tok_used must report the store the tokens took: %d", tok_used);

    for (int i = 0; i < 20; i++) { line[n++] = '$'; line[n++] = 'A'; }
    line[n] = 0;
    nt = tokenize(line, store, sizeof store, tk, MAXTOK);
    CHK(nt == TOK_E_BYTES, "120 x $A (18,000 bytes) past a %d-byte arena must be TOK_E_BYTES, got %d", STORE, nt);
    env_unset("A");
    OK("tokenize: %d words accepted, %d refused; 15,000 expanded bytes accepted, 18,000 refused", MAXTOK, MAXTOK + 1);
}

/* ---- 2. exec_line: the argv bound ---------------------------------------- */
static void t_argv(void)
{
    static char line[LINE];

    /* MAXARG words including the command: runs. Observable as one fork and
     * nothing on stderr. */
    words_line(line, sizeof line, MAXARG);
    int pid0 = stub_next_pid;
    last_status = 99;
    exec_line(line);
    cap(fd_err_r, errbuf, sizeof errbuf);
    CHK(stub_next_pid == pid0 + 1, "a %d-word command must fork exactly once (forks: %d)", MAXARG, stub_next_pid - pid0);
    CHK(errbuf[0] == 0, "a %d-word command must print nothing on stderr: '%s'", MAXARG, errbuf);
    CHK(last_status == 0, "a %d-word command must succeed: status %d", MAXARG, last_status);

    /* MAXARG+1 words: refused -- no fork, status 1, the limit in the message. */
    words_line(line, sizeof line, MAXARG + 1);
    pid0 = stub_next_pid;
    last_status = 0;
    exec_line(line);
    cap(fd_err_r, errbuf, sizeof errbuf);
    char want[32]; snprintf(want, sizeof want, "limit %d ", MAXARG);
    CHK(stub_next_pid == pid0, "a %d-word command must NOT fork (forks: %d) -- running the prefix is the bug", MAXARG + 1, stub_next_pid - pid0);
    CHK(strstr(errbuf, "too many words in one command") && strstr(errbuf, want),
        "the refusal must name the limit (%s): '%s'", want, errbuf);
    CHK(strstr(errbuf, "not run") != 0, "the refusal must say the command was not run: '%s'", errbuf);
    CHK(last_status == 1, "a refused command must leave status 1, got %d", last_status);
    OK("argv: %d words run, %d words refused with the limit named", MAXARG, MAXARG + 1);
}

/* ---- 3. glob: more matches than there is room for ------------------------ */
static void t_glob(void)
{
    /* The stub's "." holds notes.txt, note2.txt, pics. */
    static char store[256];
    char *argv[8];
    int si = 0;

    int added = glob_expand("note*", store, &si, (int)sizeof store, argv, 0, 2);
    CHK(added == 2, "two matches into two slots must expand: got %d", added);

    si = 0;
    added = glob_expand("note*", store, &si, (int)sizeof store, argv, 0, 1);
    CHK(added == GLOB_E_ARGS, "two matches into ONE slot must be GLOB_E_ARGS (%d), not 1: got %d", GLOB_E_ARGS, added);

    si = 0;
    added = glob_expand("note*", store, &si, 12, argv, 0, 8);   /* "notes.txt" fits 12, the second does not */
    CHK(added == GLOB_E_BYTES, "two matches into a 12-byte store must be GLOB_E_BYTES (%d): got %d", GLOB_E_BYTES, added);

    si = 0;
    added = glob_expand("zzz*", store, &si, (int)sizeof store, argv, 0, 1);
    CHK(added == 0, "no match must still be 0 (the literal word is kept): got %d", added);

    /* Through exec_line: a line whose glob overflows the command is refused. */
    int pid0 = stub_next_pid;
    last_status = 0;
    static char line[LINE];
    /* MAXARG-1 plain words and then a glob that matches two names: one would
     * fit, two do not, so the whole command is refused rather than getting
     * whichever match happened to come first in the directory. */
    words_line(line, sizeof line, MAXARG - 1);
    strcat(line, " note*");
    exec_line(line);
    cap(fd_err_r, errbuf, sizeof errbuf);
    CHK(stub_next_pid == pid0, "a glob that overflows the command must not run it (forks: %d)", stub_next_pid - pid0);
    CHK(strstr(errbuf, "glob matches more names") != 0, "the refusal must blame the glob: '%s'", errbuf);
    OK("glob: a partial expansion is a refusal, at the slot bound and at the byte bound");
}

/* ---- 4. the non-interactive line bound ----------------------------------- */
static void t_readline(void)
{
    int p = stub_pipe_alloc();
    int rfd = stub_fd_alloc(p, 0), wfd = stub_fd_alloc(p, 1);
    static char feed[LINE + 64], buf[LINE];

    /* LINE-1 bytes: the longest line that fits, read whole. */
    memset(feed, 'y', LINE - 1); feed[LINE - 1] = '\n'; feed[LINE] = 0;
    sys_write(wfd, feed, LINE);
    int n = sh_readline(rfd, buf, LINE);
    CHK(n == LINE - 1, "a %d-byte line must be read whole: got %d", LINE - 1, n);
    CHK((int)strlen(buf) == LINE - 1 && buf[0] == 'y' && buf[LINE - 2] == 'y', "...and intact");

    /* LINE bytes: one too many. Refused, and the NEXT line is the next line
     * -- the tail of the long one was consumed, not handed back as a command. */
    memset(feed, 'y', LINE); feed[LINE] = '\n'; feed[LINE + 1] = 0;
    sys_write(wfd, feed, LINE + 1);
    sys_write(wfd, "echo after\n", 11);
    n = sh_readline(rfd, buf, LINE);
    CHK(n == READ_E_LONG, "a %d-byte line must be READ_E_LONG (%d), not its prefix: got %d", LINE, READ_E_LONG, n);
    n = sh_readline(rfd, buf, LINE);
    CHK(n == 10 && !strcmp(buf, "echo after"), "the line after a refused one must arrive intact: '%s'", buf);

    /* EOF with nothing read. */
    sys_close(wfd);
    n = sh_readline(rfd, buf, LINE);
    CHK(n == -1, "EOF must be -1, got %d", n);
    sys_close(rfd);
    OK("readline: %d bytes read whole, %d refused whole, the following line intact", LINE - 1, LINE);
}

/* ---- 5. the history arena ------------------------------------------------ */
static void t_history(void)
{
    hist_n = 0; hist_used = 0; hcount = 0;
    char e[LINE];
    for (int i = 0; i < 100; i++) { snprintf(e, sizeof e, "cmd-%d", i); hist_add(e); }
    CHK(hist_n == HISTN, "100 entries retain HISTN (%d): %d", HISTN, hist_n);
    CHK(hcount == 100, "hcount counts every add: %d", hcount);
    CHK(hist_at(1) && !strcmp(hist_at(1), "cmd-99"), "newest is cmd-99: '%s'", hist_at(1) ? hist_at(1) : "(null)");
    CHK(hist_at(HISTN) && !strcmp(hist_at(HISTN), "cmd-36"), "oldest retained is cmd-36: '%s'", hist_at(HISTN) ? hist_at(HISTN) : "(null)");
    CHK(hist_at(HISTN + 1) == 0, "past the retained set is NULL");

    /* A maximal line is stored WHOLE, evicting as many old ones as it takes. */
    memset(e, 'L', LINE - 1); e[LINE - 1] = 0;
    hist_add(e);
    CHK(hist_at(1) && (int)strlen(hist_at(1)) == LINE - 1, "a %d-byte line is retained whole: %d", LINE - 1,
        hist_at(1) ? (int)strlen(hist_at(1)) : -1);
    CHK(hist_used <= HIST_BYTES, "the arena never overflows: %d of %d", hist_used, HIST_BYTES);

    /* Eviction by BYTES, not only by count: 1000-byte entries run out of arena
     * long before HISTN, and the retained ones are the newest, in order. */
    for (int i = 0; i < 30; i++) { memset(e, 'a' + i, 999); e[999] = 0; hist_add(e); }
    CHK(hist_n < HISTN && hist_n >= 14, "1000-byte entries are bounded by bytes: %d retained (used %d)", hist_n, hist_used);
    CHK(hist_used <= HIST_BYTES, "the arena never overflows: %d of %d", hist_used, HIST_BYTES);
    int ordered = 1;
    for (int b = 1; b <= hist_n; b++)
        if (!hist_at(b) || hist_at(b)[0] != 'a' + 29 - (b - 1)) ordered = 0;
    CHK(ordered, "the retained entries are the newest, newest first");
    CHK(hist_at(1) && !strcmp(hist_at(1), hist_at(1)), "hist_at is stable");
    OK("history: %d entries in %d bytes, whole lines only, oldest evicted first", hist_n, hist_used);
}

int main(void)
{
    wire_up();
    stub_nap_hook = on_nap;
    t_tokenize();
    t_argv();
    t_glob();
    t_readline();
    t_history();
    cap(fd_out_r, outbuf, sizeof outbuf);
    printf("%d checks, %d failures (LINE %d, MAXTOK %d, MAXARG %d, STORE %d)\n",
           checks, fails, LINE, MAXTOK, MAXARG, STORE);
    return fails ? 1 : 0;
}
