#include "aqua.h"

/* A terminal emulator: it forks + execs the real /bin/sh, wires the shell's
 * stdin/stdout/stderr to a pair of pipes (so this app is the PTY master), renders
 * the shell's output into a text grid, and feeds keystrokes to the shell. The
 * desktop Terminal now runs exactly the same sh + coreutils as the serial console. */

#define ROWS 22
#define COLS 56
#define WINW 580
#define WINH 392
#define CELL 10          /* monospace cell width (px) for the AA mono font */

static char scr[ROWS][COLS];
static int  crow, ccol;
static int  in_w = -1, out_r = -1;   /* shell stdin (write), shell stdout (read) */

static void nl(void)
{
    if (crow < ROWS - 1) crow++;
    else {
        for (int r = 0; r < ROWS - 1; r++)
            for (int c = 0; c < COLS; c++) scr[r][c] = scr[r + 1][c];
        for (int c = 0; c < COLS; c++) scr[ROWS - 1][c] = 0;
    }
    ccol = 0;
}

/* Render one output/echo byte into the grid. */
static void feed(char c)
{
    if (c == '\r') return;
    if (c == '\n') { nl(); return; }
    if (c == '\b' || c == 127) { if (ccol > 0) { ccol--; scr[crow][ccol] = 0; } return; }
    if (c == '\t') { do { if (ccol < COLS - 1) scr[crow][ccol++] = ' '; } while (ccol % 4); return; }
    if (ccol >= COLS - 1) nl();
    scr[crow][ccol++] = c;
    scr[crow][ccol] = 0;
}

/* Launch /bin/sh with its stdio on pipes. Returns 0 on success. */
static int spawn_shell(void)
{
    int inpipe[2], outpipe[2];           /* inpipe: term->sh stdin; outpipe: sh->term stdout */
    if (sys_pipe(inpipe) < 0 || sys_pipe(outpipe) < 0) return -1;

    int pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) {                      /* child -> become the shell */
        sys_dup2(inpipe[0], 0);
        sys_dup2(outpipe[1], 1);
        sys_dup2(outpipe[1], 2);
        sys_close(inpipe[0]); sys_close(inpipe[1]);
        sys_close(outpipe[0]); sys_close(outpipe[1]);
        char *argv[] = { "sh", 0 };
        sys_execve("/bin/sh", argv, 0);
        app_exit(127);                   /* exec failed */
    }
    /* parent (terminal) keeps the write end of stdin and the read end of stdout */
    sys_close(inpipe[0]);
    sys_close(outpipe[1]);
    in_w = inpipe[1];
    out_r = outpipe[0];
    sys_set_nonblock(out_r);             /* poll the shell's output without blocking */
    return 0;
}

/* Echo a string into the grid and send it to the shell's stdin (used to auto-run
 * a command when the Terminal is opened on a file). */
static void send_line(const char *s)
{
    for (const char *p = s; *p; p++) { feed(*p); sys_write(in_w, p, 1); }
}

static int ends_with(const char *s, const char *suf)
{
    int ls = 0; while (s[ls]) ls++;
    int lf = 0; while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++) if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

void app_main(void)
{
    gui_create("Terminal", WINW, WINH);
    if (spawn_shell() < 0) { feed('s'); feed('h'); feed('?'); }

    /* Opened on a file (the WM's file-type fallback launches the Terminal with the
     * path as its arg): run it. .aqs scripts run through the interpreter; any other
     * file is shown with cat. */
    {
        char arg[160];
        if (get_arg(arg, sizeof arg) > 0 && in_w >= 0) {
            send_line(ends_with(arg, ".aqs") ? "aqs " : "cat ");
            send_line(arg);
            send_line("\n");
        }
    }

    int redraw = 1, alive = 1;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_KEY && in_w >= 0 && alive) {
                char c = (char)e.a;
                if (e.a > 0xFF) continue;          /* ignore arrow/page keys for now */
                feed(c);                            /* local echo (sh on a pipe doesn't echo) */
                sys_write(in_w, &c, 1);             /* send to the shell */
                redraw = 1;
            }
        }
        /* drain any shell output (non-blocking) */
        if (out_r >= 0 && alive) {
            char ob[256];
            int n = sys_read(out_r, ob, sizeof ob);
            if (n > 0) { for (int i = 0; i < n; i++) feed(ob[i]); redraw = 1; }
            else if (n == 0) { alive = 0; feed('\n'); for (const char *s = "[shell exited]"; *s; s++) feed(*s); redraw = 1; }
            /* n == EAGAIN_RC: no data yet */
        }
        if (redraw) {
            redraw = 0;
            gui_clear(rgb(28, 30, 38));
            for (int r = 0; r < ROWS; r++)
                if (scr[r][0]) gui_text_mono(8, 6 + r * 16, rgb(210, 214, 222), CELL, scr[r]);
            gui_rect(8 + ccol * CELL, 6 + crow * 16, CELL, 16, rgb(120, 200, 140));  /* cursor */
            gui_flush();
        }
        sys_yield();
    }
}
