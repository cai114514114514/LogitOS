#include "aui.h"
#include "complete.h"

/* Code Studio -- a small IDE for AetherScript (.as). Edit with monospace + line
 * numbers + syntax highlighting, hit Run (Ctrl+R) to fork+exec /bin/as on the
 * file and watch its output in the bottom pane; a `line N` in an error jumps the
 * caret there. One file at a time. See docs/.../2026-06-08-code-studio-ide-design.md */

#define MAXT    65536
#define WINW    780
#define WINH    620
#define CELL    10                /* monospace cell width (matches Terminal; the AA mono font is 16px) */
#define ROWH    19                /* text row height (16px glyphs + 3px leading) */
#define GUTTER  58                /* line-number column (fits up to 5 digits at CELL=10) */
#define TOOLH   26                /* top toolbar           */
#define OUTH    150               /* bottom output pane    */
#define EDIT_Y  TOOLH
#define EDIT_H  (WINH - TOOLH - OUTH)
#define VIS     (EDIT_H / ROWH)   /* visible code rows     */
#define OUTBUF  16384
#define CTRL_S  0x13
#define CTRL_R  0x12

/* colors */
#define C_BG      rgb(30, 32, 40)
#define C_GUTTER  rgb(24, 26, 33)
#define C_LINENO  rgb(96, 102, 120)
#define C_TEXT    rgb(220, 223, 230)
#define C_KW      rgb(198, 120, 221)   /* keyword  (purple) */
#define C_STR     rgb(152, 195, 121)   /* string   (green)  */
#define C_COMMENT rgb(110, 118, 132)   /* comment  (grey)   */
#define C_NUM     rgb(229, 192, 123)   /* number   (amber)  */
#define C_OP      rgb(86, 182, 194)    /* operators(cyan)   */
#define C_CARET   rgb(86, 182, 194)
#define C_ERRBG   rgb(80, 40, 44)
#define C_TOOL    rgb(40, 43, 53)
#define C_OUTBG   rgb(22, 24, 30)
#define C_OUTTXT  rgb(180, 186, 198)
#define C_CMPBG   rgb(44, 47, 58)
#define C_CMPSEL  rgb(58, 96, 140)
#define C_CMPTXT  rgb(220, 223, 230)
#define C_CMPKIND rgb(120, 128, 142)

static char text[MAXT + 1];
static int  tlen;
static char fname[96];
static int  caret;        /* byte index of the cursor */
static int  top_line;     /* first visible code line  */
static int  modified;     /* 1 = unsaved edits        */

/* runner state */
static int  run_fd = -1, run_pid = -1, running;
static char out[OUTBUF + 1];
static int  outlen;
static int  err_line = -1;   /* 1-based line from an error message, else -1 */

/* completion popup state */
#define CMP_MAX  64
#define CMP_ROWS 8
static Completion cmp[CMP_MAX];
static int  cmp_n;           /* candidate count (0 = hidden) */
static int  cmp_sel, cmp_top;
static int  cmp_wstart;      /* byte index where the partial word starts */

/* ---- small helpers ---- */
static int u2s(char *b, int v) { int n = 0; char t[12]; if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = '0' + v % 10; v /= 10; } for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; return n; }
static int is_d(char c) { return c >= '0' && c <= '9'; }
static int is_a(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_an(char c) { return is_a(c) || is_d(c); }

static int line_count(void) { int n = 1; for (int i = 0; i < tlen; i++) if (text[i] == '\n') n++; return n; }
static int line_start(int ln) { if (ln <= 0) return 0; int l = 0; for (int i = 0; i < tlen; i++) if (text[i] == '\n') { if (++l == ln) return i + 1; } return tlen; }
static int line_end(int s) { int e = s; while (e < tlen && text[e] != '\n') e++; return e; }
static int caret_row(void) { int r = 0; for (int i = 0; i < caret && i < tlen; i++) if (text[i] == '\n') r++; return r; }
static int caret_col(void) { int c = 0; for (int i = caret - 1; i >= 0 && text[i] != '\n'; i--) c++; return c; }

static void scroll_to_caret(void)
{
    int r = caret_row();
    if (r < top_line) top_line = r;
    else if (r >= top_line + VIS) top_line = r - VIS + 1;
    if (top_line < 0) top_line = 0;
}

/* ---- syntax highlighting: classify the keyword set ---- */
static int is_kw(const char *s, int n)
{
    static const char *kw[] = { "def","return","if","elif","else","class","super","try","except",
        "raise","while","for","in","and","or","not","lambda","import","from","true","false","nil",
        "break","continue", 0 };
    for (int k = 0; kw[k]; k++) {
        const char *w = kw[k]; int i = 0;
        while (i < n && w[i] && w[i] == s[i]) i++;
        if (i == n && !w[i]) return 1;
    }
    return 0;
}

/* draw one source line [s,e) at (x0,y), 16px AA mono, cell-snapped to the CELL grid */
static void draw_code_line(int x0, int y, int s, int e)
{
    char buf[160];
    int i = s;
    while (i < e) {
        char c = text[i];
        int col = i - s, start = i;
        unsigned color;
        if (c == ' ' || c == '\t') { while (i < e && (text[i] == ' ' || text[i] == '\t')) i++; continue; }  /* blanks: the column grid spaces the next token */
        if (c == '#') { color = C_COMMENT; i = e; }                                   /* comment to EOL */
        else if (c == '"' || c == '\'') { char q = c; i++; while (i < e && text[i] != q) { if (text[i] == '\\' && i + 1 < e) i++; i++; } if (i < e) i++; color = C_STR; }
        else if (is_d(c)) { while (i < e && (is_an(text[i]) || text[i] == '.')) i++; color = C_NUM; }
        else if (is_a(c)) { while (i < e && is_an(text[i])) i++; color = is_kw(text + start, i - start) ? C_KW : C_TEXT; }
        else { i++; color = C_OP; }                                                   /* operator / punct */
        int n = i - start; if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
        for (int k = 0; k < n; k++) buf[k] = text[start + k];
        buf[n] = 0;
        gui_text_mono(x0 + col * CELL, y, color, CELL, buf);                          /* fixed 16px, snapped to CELL */
    }
}

/* ---- output pane ---- */
static void append_out(const char *s) { while (*s && outlen < OUTBUF) out[outlen++] = *s++; out[outlen] = 0; }
static void scan_error(void)
{
    err_line = -1;
    for (int i = 0; i + 5 < outlen; i++)
        if (out[i] == 'l' && out[i+1] == 'i' && out[i+2] == 'n' && out[i+3] == 'e' && out[i+4] == ' ') {
            int n = 0, j = i + 5; while (j < outlen && is_d(out[j])) { if (n < 1000000) n = n * 10 + (out[j] - '0'); j++; }
            if (n > 0) { err_line = n; return; }      /* first "line N" wins */
        }
}

/* ---- run /bin/as on the current file ---- */
static void run_file(void)
{
    write_file(fname, text, tlen); modified = 0;       /* save first */
    if (run_fd >= 0) { sys_close(run_fd); run_fd = -1; }
    outlen = 0; out[0] = 0; err_line = -1;
    append_out("$ as "); append_out(fname); append_out("\n");
    int p[2];
    if (sys_pipe(p) < 0) { append_out("(could not create pipe)\n"); return; }
    int pid = sys_fork();
    if (pid < 0) { append_out("(fork failed)\n"); sys_close(p[0]); sys_close(p[1]); return; }
    if (pid == 0) {
        sys_dup2(p[1], 1); sys_dup2(p[1], 2);
        sys_close(p[0]); sys_close(p[1]);
        char *argv[] = { "as", fname, 0 };
        sys_execve("/bin/as", argv, 0);
        app_exit(127);
    }
    sys_close(p[1]);
    run_fd = p[0]; run_pid = pid; running = 1;
    sys_set_nonblock(run_fd);
}

/* non-blocking: drain the pipe; EOF (read==0) means the child finished */
static int poll_run(void)
{
    if (run_fd < 0) return 0;
    char buf[512]; int r, got = 0, eof = 0;
    for (;;) {
        r = sys_read(run_fd, buf, sizeof buf);
        if (r > 0) { for (int i = 0; i < r && outlen < OUTBUF; i++) out[outlen++] = buf[i]; out[outlen] = 0; got = 1; if (r < (int)sizeof buf) break; continue; }
        if (r == 0) eof = 1;
        break;
    }
    if (eof) { sys_waitpid(run_pid, 0); sys_close(run_fd); run_fd = -1; run_pid = -1; running = 0; scan_error(); if (err_line > 0) { caret = line_start(err_line - 1); scroll_to_caret(); } got = 1; }
    return got;
}

/* ---- completion providers: enumerate + read LibAether modules off the disk ---- */
static int sd_list_modules(char names[][48], int max)
{
    int cnt = dir_count("/usr/as/lib"); if (cnt < 0) return 0;
    int out = 0; char nm[64];
    for (int i = 0; i < cnt && out < max; i++) {
        int sz = dir_name("/usr/as/lib", i, nm);   /* buf is <= 64 per the ABI */
        if (sz < 0) continue;
        int L = 0; while (nm[L]) L++;
        if (!(L > 3 && nm[L-3] == '.' && nm[L-2] == 'a' && nm[L-1] == 's')) continue;  /* only .as */
        L -= 3;
        int j = 0; for (; j < L && j < 47; j++) names[out][j] = nm[j];
        names[out][j] = 0; out++;
    }
    return out;
}
static int sd_read_module(const char *name, char *buf, int max)
{
    char path[96]; int p = 0; const char *pre = "/usr/as/lib/";
    while (pre[p]) { path[p] = pre[p]; p++; }
    int i = 0; while (name[i] && p < 88) path[p++] = name[i++];
    path[p++] = '.'; path[p++] = 'a'; path[p++] = 's'; path[p] = 0;
    return read_file(path, buf, max);
}

/* ---- completion popup ---- */
static void cmp_hide(void) { cmp_n = 0; }
static void cmp_refresh(void)
{
    cmp_n = as_complete(text, tlen, caret, cmp, CMP_MAX);
    cmp_sel = 0; cmp_top = 0;
    int p = caret;
    while (p > 0) { char c = text[p-1];
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') p--; else break; }
    cmp_wstart = p;
}
static const char *cmp_kind_name(int k)
{
    switch (k) {
        case CMP_KEYWORD: return "keyword"; case CMP_BUILTIN: return "builtin";
        case CMP_MODULE:  return "module";  case CMP_FUNC:    return "func";
        case CMP_CLASS:   return "class";   case CMP_METHOD:  return "method";
        case CMP_FIELD:   return "field";   case CMP_PARAM:   return "param";
        case CMP_IMPORT:  return "import";  case CMP_GLOBAL:  return "global";
        default:          return "local";
    }
}
/* draw the popup under the caret (crow/ccol = caret row/col) */
static void draw_cmp(int crow, int ccol)
{
    if (cmp_n <= 0) return;
    if (crow < top_line || crow >= top_line + VIS) return;
    int wcol = ccol - (caret - cmp_wstart); if (wcol < 0) wcol = 0;
    int px = GUTTER + 2 + wcol * CELL;
    int rows = cmp_n < CMP_ROWS ? cmp_n : CMP_ROWS;
    int w = 230, h = rows * ROWH + 4;
    int py = EDIT_Y + (crow - top_line) * ROWH + ROWH + 1;
    if (py + h > WINH - OUTH) py = EDIT_Y + (crow - top_line) * ROWH - h;   /* flip above */
    if (px + w > WINW) px = WINW - w - 4;
    if (px < 0) px = 0;
    gui_rect(px, py, w, h, C_CMPBG);
    gui_rect(px, py, w, 1, rgb(18, 19, 24));
    for (int r = 0; r < rows; r++) {
        int it = cmp_top + r;
        int ry = py + 2 + r * ROWH;
        if (it == cmp_sel) gui_rect(px, ry - 1, w, ROWH, C_CMPSEL);
        gui_text_mono(px + 6, ry, C_CMPTXT, CELL, cmp[it].label);
        gui_text_mono(px + w - 74, ry, C_CMPKIND, CELL, cmp_kind_name(cmp[it].kind));
    }
}

/* ---- render ---- */
static int run_btn_hit(int x, int y) { return y >= 3 && y < TOOLH - 3 && x >= WINW - 70 && x < WINW - 10; }

static void redraw(void)
{
    gui_clear(C_BG);

    /* Liquid Glass toolbar */
    gui_glass(0, 0, WINW, TOOLH, 1, 40, 43, 53, 150);
    gui_rect(0, TOOLH - 1, WINW, 1, rgb(18, 19, 24));
    gui_text(10, 6, modified ? rgb(229, 192, 123) : C_TEXT, fname);
    gui_rect(WINW - 70, 4, 60, TOOLH - 8, running ? rgb(70, 80, 96) : rgb(60, 120, 90));
    gui_text(WINW - 58, 6, rgb(240, 244, 248), running ? "..." : "Run");

    /* editor: gutter + code + caret */
    gui_rect(0, EDIT_Y, GUTTER, EDIT_H, C_GUTTER);
    int total = line_count();
    int crow = caret_row(), ccol = caret_col();
    for (int v = 0; v < VIS; v++) {
        int ln = top_line + v;
        if (ln >= total) break;
        int y = EDIT_Y + v * ROWH + 1;
        int s = line_start(ln), e = line_end(s);
        if (err_line == ln + 1) gui_rect(GUTTER, y - 1, WINW - GUTTER, ROWH, C_ERRBG);
        char num[12]; int nn = u2s(num, ln + 1); num[nn] = 0;
        gui_text_mono(GUTTER - 8 - nn * CELL, y, C_LINENO, CELL, num);   /* right-aligned */
        if (e > s) draw_code_line(GUTTER + 2, y, s, e);
    }
    if (crow >= top_line && crow < top_line + VIS)
        gui_rect(GUTTER + 2 + ccol * CELL, EDIT_Y + (crow - top_line) * ROWH + 1, 2, ROWH - 2, C_CARET);

    /* output pane */
    int oy = WINH - OUTH;
    gui_rect(0, oy, WINW, OUTH, C_OUTBG);
    gui_rect(0, oy, WINW, 1, rgb(18, 19, 24));
    gui_text(8, oy + 4, C_LINENO, "output");
    /* show the last (OUTH/ROWH - 1) lines of `out` */
    int rows = OUTH / ROWH - 1;
    int starts[64]; int ns = 0; starts[ns++] = 0;
    for (int i = 0; i < outlen && ns < 64; i++) if (out[i] == '\n') starts[ns++] = i + 1;
    int first = ns - rows; if (first < 0) first = 0;
    for (int li = first; li < ns; li++) {
        int s = starts[li]; int e = s; while (e < outlen && out[e] != '\n') e++;
        if (e > s) gui_text_run(8, oy + 22 + (li - first) * ROWH, 16, 1, C_OUTTXT, out + s, e - s);
    }

    draw_cmp(crow, ccol);
    gui_flush();
}

void app_main(void)
{
    int n = get_arg(fname, sizeof fname);
    if (n <= 0) { const char *d = "untitled.as"; int i = 0; while (d[i]) { fname[i] = d[i]; i++; } fname[i] = 0; }
    gui_create("Code Studio", WINW, WINH);
    as_complete_set_providers(sd_list_modules, sd_read_module);

    int r = read_file(fname, text, MAXT);
    if (r > 0) { tlen = r > MAXT ? MAXT : r; text[tlen] = 0; }
    redraw();

    for (;;) {
        struct aether_event e;
        int changed = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) { if (run_fd >= 0) { sys_close(run_fd); if (run_pid >= 0) sys_waitpid(run_pid, 0); } app_exit(0); }
            if (e.type == EV_MOUSE) {
                int mx = (int)e.a, my = (int)e.b;
                if (run_btn_hit(mx, my)) { run_file(); changed = 1; }
                else if (my >= EDIT_Y && my < EDIT_Y + EDIT_H) {        /* click -> place the caret */
                    int row = top_line + (my - EDIT_Y) / ROWH;
                    int tot = line_count(); if (row >= tot) row = tot - 1; if (row < 0) row = 0;
                    int s = line_start(row), e2 = line_end(s);
                    int col = (mx - GUTTER - 2) / CELL; if (col < 0) col = 0;
                    caret = s + (col < e2 - s ? col : e2 - s); changed = 1;
                } else if (my >= WINH - OUTH && err_line > 0) { caret = line_start(err_line - 1); scroll_to_caret(); changed = 1; }
            }
            if (e.type == EV_KEY) {
                int k = (int)e.a;
                if (cmp_n > 0) {                       /* popup open: intercept nav/accept/dismiss */
                    if (k == KEY_UP)   { if (cmp_sel > 0) cmp_sel--; if (cmp_sel < cmp_top) cmp_top = cmp_sel; changed = 1; continue; }
                    if (k == KEY_DOWN) { if (cmp_sel < cmp_n - 1) cmp_sel++; if (cmp_sel >= cmp_top + CMP_ROWS) cmp_top = cmp_sel - CMP_ROWS + 1; changed = 1; continue; }
                    if (k == 27)       { cmp_hide(); changed = 1; continue; }                 /* Esc */
                    if (k == '\t' || k == '\r' || k == '\n') {                                /* accept */
                        const char *ins = cmp[cmp_sel].insert;
                        int wlen = caret - cmp_wstart, ilen = 0; while (ins[ilen]) ilen++;
                        int delta = ilen - wlen;
                        if (tlen + delta < MAXT && tlen + delta >= 0) {
                            if (delta > 0) { for (int i = tlen; i >= caret; i--) text[i + delta] = text[i]; }
                            else if (delta < 0) { for (int i = caret; i <= tlen; i++) text[i + delta] = text[i]; }
                            for (int i = 0; i < ilen; i++) text[cmp_wstart + i] = ins[i];
                            tlen += delta; caret = cmp_wstart + ilen; text[tlen] = 0; modified = 1;
                        }
                        cmp_hide(); scroll_to_caret(); changed = 1; continue;
                    }
                }
                if (k == CTRL_S) { if (write_file(fname, text, tlen) >= 0) modified = 0; changed = 1; }
                else if (k == CTRL_R) { run_file(); changed = 1; }
                else if (k == KEY_LEFT)  { if (caret > 0) caret--; scroll_to_caret(); changed = 1; }
                else if (k == KEY_RIGHT) { if (caret < tlen) caret++; scroll_to_caret(); changed = 1; }
                else if (k == KEY_UP)   { int row = caret_row(); if (row > 0) { int col = caret_col(); int ps = line_start(row - 1), pe = line_end(ps); caret = ps + (col < pe - ps ? col : pe - ps); } scroll_to_caret(); changed = 1; }
                else if (k == KEY_DOWN) { int row = caret_row(); if (row < line_count() - 1) { int col = caret_col(); int ns2 = line_start(row + 1), ne = line_end(ns2); caret = ns2 + (col < ne - ns2 ? col : ne - ns2); } scroll_to_caret(); changed = 1; }
                else if (k == KEY_HOME) { caret = line_start(caret_row()); changed = 1; }
                else if (k == KEY_END)  { caret = line_end(line_start(caret_row())); changed = 1; }
                else if (k == KEY_PGDN) { top_line += VIS; int m = line_count() - 1; if (top_line > m) top_line = m; changed = 1; }
                else if (k == KEY_PGUP) { top_line -= VIS; if (top_line < 0) top_line = 0; changed = 1; }
                else if (k == '\b') { if (caret > 0) { for (int i = caret - 1; i < tlen - 1; i++) text[i] = text[i + 1]; tlen--; caret--; text[tlen] = 0; modified = 1; scroll_to_caret(); } if (cmp_n > 0) cmp_refresh(); changed = 1; }
                else if (k == '\r' || k == '\n' || k == '\t' || (k >= 32 && k < 127)) {
                    char c = (k == '\r') ? '\n' : (char)k;
                    if (tlen < MAXT) { for (int i = tlen; i > caret; i--) text[i] = text[i - 1]; text[caret++] = c; tlen++; text[tlen] = 0; modified = 1; scroll_to_caret(); }
                    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.') cmp_refresh(); else cmp_hide();
                    changed = 1;
                }
            }
        }
        if (poll_run()) changed = 1;
        if (changed) redraw();
        sys_yield();
    }
}
