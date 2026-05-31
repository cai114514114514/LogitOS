#include "aqua.h"

/* A userland shell: ls / cat / mem / ps / echo / clear / uname, all via
 * system calls. A real ring-3 process reading the keyboard event by event. */

#define ROWS 18
#define COLS 56
#define WINW 472
#define WINH 340

static char scr[ROWS][COLS];
static int  crow, ccol;
static char in[COLS];
static int  inlen;

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

static void run(const char *line)
{
    pr("$ "); pr(line); pr("\n");
    if (!line[0]) return;

    if (seq(line, "help")) {
        pr("help ls cat <f> mem ps clear uname\n");
        pr("touch <f>  rm <f>  echo <t> [> <f>]\n");
    } else if (seq(line, "ls")) {
        int n = file_count();
        for (int i = 0; i < n; i++) {
            char nm[48];
            int sz = file_name(i, nm, sizeof nm);
            pr(nm); pr("  "); pnum(sz); pr(" B\n");
        }
    } else if (starts(line, "cat ")) {
        const char *f = line + 4;
        while (*f == ' ') f++;
        static char fb[2048];
        int n = read_file(f, fb, sizeof fb - 1);
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
            char body[256];
            int n = 0;
            for (const char *t = arg; t < redir && n < 254; t++)
                body[n++] = *t;
            while (n > 0 && body[n - 1] == ' ') n--;   /* trim before '>' */
            body[n++] = '\n';
            body[n] = 0;
            if (write_file(fn, body, n) < 0) pr("echo: write failed\n");
        } else {
            pr(arg); pr("\n");
        }
    } else if (starts(line, "touch ")) {
        const char *fn = line + 6; while (*fn == ' ') fn++;
        if (write_file(fn, "", 0) < 0) pr("touch: failed\n");
    } else if (starts(line, "rm ")) {
        const char *fn = line + 3; while (*fn == ' ') fn++;
        if (delete_file(fn) < 0) pr("rm: no such file\n");
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
                if (scr[r][0]) gui_text(8, 6 + r * 16, rgb(55, 58, 66), scr[r]);
            int iy = 6 + ROWS * 16;
            gui_text(8, iy, rgb(90, 150, 240), "$ ");
            gui_text(8 + 16, iy, rgb(40, 40, 48), in);
            gui_rect(8 + 16 + inlen * 8, iy, 8, 16, rgb(90, 150, 240));
            gui_flush();
        }
        sys_yield();
    }
}
