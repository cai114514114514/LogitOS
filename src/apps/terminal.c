#include "aqua.h"

/* A userland shell with a current directory: ls/cd/pwd/mkdir/cat/touch/rm/echo/
 * mem/ps/clear/uname, all via system calls. A real ring-3 process reading the
 * keyboard event by event. */

#define ROWS 18
#define COLS 44
#define WINW 472
#define WINH 340
#define CELL 10          /* monospace cell width (px) for the AA mono font */

static char scr[ROWS][COLS];
static int  crow, ccol;
static char in[COLS];
static int  inlen;
static char cwd[128] = "/";

static void nl(void)
{
    if (crow < ROWS - 1) { crow++; }
    else {
        for (int r = 0; r < ROWS - 1; r++)
            for (int c = 0; c < COLS; c++) scr[r][c] = scr[r + 1][c];
        for (int c = 0; c < COLS; c++) scr[ROWS - 1][c] = 0;
    }
    ccol = 0;
}
static void pc(char c) { if (c == '\n') { nl(); return; } if (ccol >= COLS - 1) nl(); scr[crow][ccol++] = c; scr[crow][ccol] = 0; }
static void pr(const char *s) { while (*s) pc(*s++); }
static void pnum(int v) { char t[12]; int i = 0; if (!v) { pc('0'); return; } while (v) { t[i++] = '0' + v % 10; v /= 10; } while (i) pc(t[--i]); }

static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* --- path helpers --- */
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void pjoin(char *out, const char *dir, const char *name, int max)
{
    int n = 0;
    for (const char *p = dir; *p && n < max - 1; p++) out[n++] = *p;
    if ((n == 0 || out[n - 1] != '/') && n < max - 1) out[n++] = '/';
    for (const char *p = name; *p && n < max - 1; p++) out[n++] = *p;
    out[n] = 0;
}
static void pup(char *p)
{
    int n = slen(p);
    if (n <= 1) return;
    if (p[n - 1] == '/') n--;
    while (n > 0 && p[n - 1] != '/') n--;
    if (n <= 1) { p[0] = '/'; p[1] = 0; } else p[n - 1] = 0;
}
/* turn a user argument into an absolute path (relative -> under cwd) */
static void abspath(char *out, const char *arg, int max)
{
    if (arg[0] == '/') scpy(out, arg, max);
    else pjoin(out, cwd, arg, max);
}

static void run(const char *line)
{
    pr(cwd); pr(" $ "); pr(line); pr("\n");
    if (!line[0]) return;

    if (seq(line, "help")) {
        pr("help ls [d] cd <d> pwd mkdir <d>\n");
        pr("cat <f> touch <f> rm <f> echo <t> [> <f>]\n");
        pr("mem ps clear uname\n");
    } else if (seq(line, "ls") || starts(line, "ls ")) {
        char dpath[128];
        const char *a = line + 2; while (*a == ' ') a++;
        if (a[0]) abspath(dpath, a, sizeof dpath); else scpy(dpath, cwd, sizeof dpath);
        int n = dir_count(dpath);
        if (n < 0) pr("ls: no such directory\n");
        else for (int i = 0; i < n; i++) {
            char nm[64];
            int r = dir_name(dpath, i, nm);
            pr(nm);
            if (r == -2) pr("/");
            else { pr("  "); pnum(r); pr(" B"); }
            pr("\n");
        }
    } else if (seq(line, "cd") || starts(line, "cd ")) {
        const char *a = line + 2; while (*a == ' ') a++;
        char cand[128];
        if (!a[0]) scpy(cand, "/", sizeof cand);
        else if (seq(a, "..")) { scpy(cand, cwd, sizeof cand); pup(cand); }
        else if (seq(a, ".")) scpy(cand, cwd, sizeof cand);
        else abspath(cand, a, sizeof cand);
        if (dir_count(cand) < 0) pr("cd: no such directory\n");
        else scpy(cwd, cand, sizeof cwd);
    } else if (seq(line, "pwd")) {
        pr(cwd); pr("\n");
    } else if (starts(line, "mkdir ")) {
        const char *a = line + 6; while (*a == ' ') a++;
        char p[128]; abspath(p, a, sizeof p);
        if (make_dir(p) < 0) pr("mkdir: failed\n");
    } else if (starts(line, "cat ")) {
        const char *a = line + 4; while (*a == ' ') a++;
        char p[128]; abspath(p, a, sizeof p);
        static char fb[2048];
        int n = read_file(p, fb, sizeof fb - 1);
        if (n < 0) pr("cat: no such file\n");
        else { fb[n] = 0; pr(fb); if (n && fb[n - 1] != '\n') pr("\n"); }
    } else if (seq(line, "mem") || seq(line, "ps")) {
        static char info[1024];
        sysinfo(info, sizeof info);
        pr(info);
    } else if (seq(line, "clear")) {
        for (int r = 0; r < ROWS; r++) scr[r][0] = 0;
        crow = ccol = 0;
    } else if (starts(line, "echo ")) {
        const char *arg = line + 5;
        const char *redir = 0;
        for (const char *p = arg; *p; p++)
            if (*p == '>') { redir = p; break; }
        if (redir) {                              /* echo TEXT > FILE */
            const char *fn = redir + 1;
            while (*fn == ' ') fn++;
            char p[128]; abspath(p, fn, sizeof p);
            char body[256];
            int n = 0;
            for (const char *t = arg; t < redir && n < 254; t++) body[n++] = *t;
            while (n > 0 && body[n - 1] == ' ') n--;   /* trim before '>' */
            body[n++] = '\n';
            body[n] = 0;
            if (write_file(p, body, n) < 0) pr("echo: write failed\n");
        } else {
            pr(arg); pr("\n");
        }
    } else if (starts(line, "touch ")) {
        const char *a = line + 6; while (*a == ' ') a++;
        char p[128]; abspath(p, a, sizeof p);
        if (write_file(p, "", 0) < 0) pr("touch: failed\n");
    } else if (starts(line, "rm ")) {
        const char *a = line + 3; while (*a == ' ') a++;
        char p[128]; abspath(p, a, sizeof p);
        if (delete_file(p) < 0) pr("rm: no such file\n");
    } else if (seq(line, "uname")) {
        pr("Aqua OS x86_64 -- from scratch (M1-M8)\n");
    } else {
        pr("command not found: "); pr(line); pr("\n");
    }
}

void app_main(void)
{
    gui_create("Terminal", WINW, WINH);
    pr("Aqua shell -- type 'help'\n");

    int redraw = 1;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE)
                app_exit(0);
            if (e.type == EV_KEY) {
                char c = (char)e.a;
                if (c == '\n') { in[inlen] = 0; run(in); inlen = 0; in[0] = 0; }
                else if (c == '\b') { if (inlen > 0) in[--inlen] = 0; }
                else if (inlen < COLS - 3) { in[inlen++] = c; in[inlen] = 0; }
                redraw = 1;
            }
        }
        if (redraw) {
            redraw = 0;
            gui_clear(rgb(250, 250, 252));
            for (int r = 0; r < ROWS; r++)
                if (scr[r][0]) gui_text_mono(8, 6 + r * 16, rgb(55, 58, 66), CELL, scr[r]);
            int iy = 6 + ROWS * 16;
            gui_text_mono(8, iy, rgb(90, 150, 240), CELL, "$ ");
            gui_text_mono(8 + 2 * CELL, iy, rgb(40, 40, 48), CELL, in);
            gui_rect(8 + 2 * CELL + inlen * CELL, iy, CELL, 16, rgb(90, 150, 240));
            gui_flush();
        }
        sys_yield();
    }
}
