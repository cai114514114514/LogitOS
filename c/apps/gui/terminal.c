/* LogitOS Terminal.
 *
 * Not a VT100 emulator. A VT100 emulator is what you write when you cannot
 * change the programs on the other end; here the shell, the coreutils and this
 * window are all ours, so the wire is a choice. The choice is documented in
 * c/apps/coreutils/logit_rich.h -- plain text on fd 1 and fd 2, structured and
 * rich content as length-prefixed frames on a separate pipe (LRT/1).
 *
 * What that buys, in the order it shows up on screen:
 *
 *  - stdout and stderr are SEPARATE PIPES, so every line in the scrollback knows
 *    which stream it came from and errors are red without anybody emitting a
 *    colour code. A pty cannot do this: it is one byte stream by construction.
 *  - the shell reports command boundaries and exit status on the side band, so
 *    every line also knows WHICH COMMAND produced it. That gives an exit-status
 *    gutter, fold-a-command, copy-exactly-this-command's-output, click-to-rerun,
 *    and an "errors only" filter -- all exact, not inferred from prompt shapes.
 *  - rich frames become real drawn objects in the scrollback: an inline image, a
 *    progress widget, a table with rules whose rows open a file when clicked, a
 *    bar chart.
 *
 * Geometry is derived, never compiled in: the monospace cell width comes from
 * measuring the font (SYS_TEXT_MEASURE) and the grid comes from the window, so
 * the display's backing scale factor cannot leave the grid behind.
 *
 * The input line lives in the shell, not here: /bin/sh does the editing, history
 * and completion (it is the process that knows PATH and the cwd) and ships the
 * live buffer over the side band as RT_T_INPUT, which this window draws. Keys
 * that are text go down stdin; keys that are editing commands (arrows, Home,
 * End) go down the control channel, because stdin belongs to whatever child is
 * running and must not be polluted with terminal-private codes.
 */

#include "logit.h"
#include "logit_rich.h"

/* ------------------------------------------------------------- geometry -- */

#define PAD      8
#define GUTTER   12          /* exit-status / fold column                     */
#define SBW      8           /* scrollbar                                     */
#define MAXCOLS  200
#define MAXLINES 600
#define TOPBAR   20          /* cwd strip                                     */

/* The size gui_text_mono draws at. It mirrors TEXT_UI_PX in c/kernel/gui/text.h,
 * which is the size wm.c passes to text_draw_mono_sz -- an app cannot ask for it,
 * so it is named here once and BOTH the cell measurement and the line height are
 * derived from it. Deriving the line height from the cell width instead is what
 * produced overlapping table rows: a monospace advance is ~0.6em, so cell*1.8 is
 * ~1.1em, which is less than one line box. */
#define UIPX 16

static int cell = 10;        /* mono advance, points (measured)               */
static int lh = 20;          /* line height, points                           */
static int win_w = 720, win_h = 460;
static int cols = 60, rows = 20;
static int text_x, text_y, text_w, text_h;

/* --------------------------------------------------------------- theme --- */

struct pal { unsigned bg, panel, fg, dim, err, prompt, accent, ok, bad, sel, rule; };
static const struct pal dark = {
    0x181A20, 0x1F222B, 0xD8DCE4, 0x828899, 0xFF786E, 0x78C88C,
    0x6EAAFF, 0x5AC882, 0xF05A5A, 0x38496E, 0x2B2F3A
};
static const struct pal light = {
    0xFBFBFD, 0xF0F1F5, 0x22242A, 0x6B7080, 0xC02020, 0x1E7A3C,
    0x1462C8, 0x1E9050, 0xD43A3A, 0xBFD4F5, 0xDDDFE6
};
static struct pal P;
static int is_dark = 1;

static void theme_load(void)
{
    is_dark = sys_ui_dark(-1) != 0;
    P = is_dark ? dark : light;
}

/* ------------------------------------------------------------ scrollback -- */

#define S_OUT    0
#define S_ERR    1
#define S_PROMPT 2
#define S_SYS    3

#define L_TEXT 0
#define L_OBJ  1

struct tline {
    char  t[MAXCOLS + 1];
    short len;
    unsigned char kind;      /* L_TEXT / L_OBJ  */
    unsigned char stream;    /* S_*             */
    short cmd;               /* command slot, or -1                            */
    unsigned cmdid;          /* the id that slot held -- slots are a ring too   */
    short obj;               /* object slot for L_OBJ, else -1                 */
    short objgen;            /* generation, so an evicted object degrades      */
    short vrows;             /* how many display rows this line occupies       */
};

static struct tline lines[MAXLINES];
static int  lbase, lcount;                      /* ring */
static long labs;                               /* absolute index of LN(0)     */
static struct tline *LN(int i) { return &lines[(lbase + i) % MAXLINES]; }

/* ------------------------------------------------------------- commands --- */

#define MAXCMDS 96
struct cmdrec {
    unsigned id;
    int used, done, status, interrupted, folded;
    char text[96];
};
static struct cmdrec cmds[MAXCMDS];
static int      cur_cmd = -1;                   /* slot of the running command */
static unsigned cur_cmdid;

/* Slots are a ring keyed on the id, so a long session recycles them; a line that
 * outlives its command therefore has to prove the slot still holds ITS command
 * before reading a status out of it. */
static int cmd_slot(unsigned id) { return (int)(id % MAXCMDS); }

/* ------------------------------------------------------------- objects ---- */

#define O_IMAGE 1
#define O_PROG  2
#define O_TABLE 3
#define O_CHART 4

#define MAXOBJ  96
#define MAXIMG  6
#define MAXTBL  6
#define MAXCHT  8
#define MAXPRG  16

#define IMG_W 1280
#define IMG_H 800
#define THW   256
#define THH   192
static unsigned char img_scratch[IMG_W * IMG_H * 4];
static unsigned char thumb[MAXIMG][THW * THH * 4];

struct imgobj { char path[128]; int iw, ih, tw, th, dw, dh, ok; };
static struct imgobj imgs[MAXIMG];
static int nimg;

#define TCOL 6
#define TROW 48
#define TCELL 22
#define TTGT  96
struct tblobj {
    char title[48];
    int ncols, nrows;
    char hdr[TCOL][TCELL];
    char cellt[TROW][TCOL][TCELL];
    unsigned char cellk[TROW][TCOL];
    char target[TROW][TTGT];
};
static struct tblobj tbls[MAXTBL];
static int ntbl;

#define CHN 24
struct chtobj { char title[48]; int n; unsigned val[CHN]; char lab[CHN][16]; unsigned maxv; };
static struct chtobj chts[MAXCHT];
static int ncht;

struct prgobj { unsigned id; int permille, done; char label[56]; };
static struct prgobj prgs[MAXPRG];
static int nprg;

struct robj { unsigned char type; short idx; short gen; short vrows; };
static struct robj objs[MAXOBJ];
static int nobj, objgen_next = 1;

/* ---------------------------------------------------------------- pipes --- */

static int in_w = -1;        /* -> shell stdin        */
static int out_r = -1;       /* <- shell stdout       */
static int err_r = -1;       /* <- shell stderr       */
static int rich_r = -1;      /* <- shell/child frames */
static int ctl_w = -1;       /* -> shell control      */

static struct rt_parser parser;
static struct rt_enc    enc;

/* Outbound control ring.
 *
 * Every keystroke leaves through here, not through stdin: stdin belongs to
 * whatever child is running, and a terminal that writes its own control codes
 * into it would be handing them to `cat`. The ring exists because the control
 * pipe is non-blocking (a blocking write would freeze the window whenever the
 * shell is busy) and a dropped keystroke is not acceptable. */
#define COUTSZ 4096
static unsigned char cout[COUTSZ];
static int cout_head, cout_tail;

static void cout_push(unsigned char c)
{ int nt = (cout_tail + 1) % COUTSZ; if (nt != cout_head) { cout[cout_tail] = c; cout_tail = nt; } }

static void flush_ctl(void)
{
    while (cout_head != cout_tail && ctl_w >= 0) {
        unsigned char c = cout[cout_head];
        if (sys_write(ctl_w, &c, 1) == 1) cout_head = (cout_head + 1) % COUTSZ;
        else break;
    }
}

/* ------------------------------------------------------------ view state -- */

static int scroll;                 /* first visible display row               */
static int follow = 1;             /* stick to the bottom                     */
static int errs_only;              /* Ctrl+E filter                           */
static int redraw = 1;
static int alive = 1;
static unsigned long stdout_bytes; /* ordering anchor: bytes drained from fd 1 */
static unsigned cmd_base;          /* anchor baseline for the running command  */

/* input line, as reported by the shell */
static char in_prompt[96], in_buf[512];
static int  in_cur;
static int  have_input;

/* selection, in (line, col) */
static int sel_on, sel_drag;
static int sa_l, sa_c, sb_l, sb_c;
static char clip[4096];
static int clip_n;

/* deferred rich frames waiting for the text stream to catch up */
#define MAXDEFER 8
struct defer { int used; int type; unsigned seq; int len; unsigned char p[1024]; };
static struct defer defers[MAXDEFER];

/* -------------------------------------------------------------- helpers --- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static void utoa(unsigned v, char *o)
{ char t[12]; int i = 0; if (!v) { o[0] = '0'; o[1] = 0; return; }
  while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (i) o[k++] = t[--i]; o[k] = 0; }

static void itoa_s(int v, char *o)
{ if (v < 0) { o[0] = '-'; utoa((unsigned)(-v), o + 1); } else utoa((unsigned)v, o); }

/* -------------------------------------------------------- line handling --- */

static struct cmdrec *cmd_of(const struct tline *l)
{
    if (l->cmd < 0 || l->cmd >= MAXCMDS) return 0;
    struct cmdrec *c = &cmds[l->cmd];
    return (c->used && c->id == l->cmdid) ? c : 0;
}

static struct tline *new_line(int stream)
{
    struct tline *l;
    if (lcount < MAXLINES) { l = LN(lcount); lcount++; }
    else { l = LN(0); lbase = (lbase + 1) % MAXLINES; labs++; }
    l->t[0] = 0; l->len = 0; l->kind = L_TEXT;
    l->stream = (unsigned char)stream;
    l->cmd = (short)cur_cmd; l->cmdid = cur_cmdid;
    l->obj = -1; l->objgen = 0; l->vrows = 1;
    return l;
}

/* One partially-written line per stream: stdout and stderr arrive in separate
 * pipes and must not splice into each other's line.
 *
 * The open line is remembered by ABSOLUTE index, not by ring index. A ring index
 * shifts down every time an old line is evicted, so a long-running command that
 * filled the scrollback would silently start appending into the wrong line -- a
 * bug that only appears after 600 lines of output, which is exactly the kind
 * nothing notices. */
static long open_line[4] = { -1, -1, -1, -1 };

static struct tline *cur_line(int stream)
{
    long idx = open_line[stream] - labs;
    if (open_line[stream] >= 0 && idx >= 0 && idx < lcount) return LN((int)idx);
    struct tline *l = new_line(stream);
    open_line[stream] = labs + lcount - 1;
    return l;
}

static void end_line(int stream) { open_line[stream] = -1; }
static void end_all_lines(void) { for (int i = 0; i < 4; i++) open_line[i] = -1; }

static void put_char(int stream, char c)
{
    if (c == '\r') return;
    if (c == '\n') { (void)cur_line(stream); end_line(stream); return; }
    struct tline *l = cur_line(stream);
    if (c == '\b' || c == 127) { if (l->len > 0) { l->len--; l->t[l->len] = 0; } return; }
    if (c == '\t') {
        do { if (l->len < MAXCOLS) l->t[l->len++] = ' '; } while (l->len % 8 && l->len < MAXCOLS);
        l->t[l->len] = 0; return;
    }
    if (c < 32) return;                       /* no in-band control language   */
    if (l->len >= cols || l->len >= MAXCOLS) { end_line(stream); l = cur_line(stream); }
    l->t[l->len++] = c;
    l->t[l->len] = 0;
}

static void put_text(int stream, const char *s)
{ while (*s) put_char(stream, *s++); end_line(stream); }

/* --------------------------------------------------------- object attach -- */

static int obj_alloc(int type, int idx, int vrows)
{
    int slot;
    if (nobj < MAXOBJ) slot = nobj++;
    else slot = objgen_next % MAXOBJ;          /* recycle; the generation check
                                                * makes stale references degrade */
    objs[slot].type = (unsigned char)type;
    objs[slot].idx = (short)idx;
    objs[slot].gen = (short)objgen_next;
    objs[slot].vrows = (short)vrows;
    objgen_next++;
    if (objgen_next > 0x7000) objgen_next = 1;
    return slot;
}

static struct tline *attach_obj(int slot)
{
    end_all_lines();
    struct tline *l = new_line(S_OUT);
    l->kind = L_OBJ;
    l->obj = (short)slot;
    l->objgen = objs[slot].gen;
    l->vrows = objs[slot].vrows;
    return l;
}

static int obj_live(const struct tline *l)
{
    if (l->kind != L_OBJ || l->obj < 0 || l->obj >= MAXOBJ) return 0;
    return objs[l->obj].gen == l->objgen;
}

/* --------------------------------------------------------------- images --- */

static void downscale(const unsigned char *src, int sw, int sh,
                      unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * sw / dw);
            const unsigned char *s = src + ((long)sy * sw + sx) * 4;
            unsigned char *d = dst + ((long)y * dw + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

/* Decode into the shared scratch, then keep only a small thumbnail: the window
 * is a few hundred points wide, so holding a 3 MB natural-size bitmap per image
 * would be memory spent on pixels nothing can see. */
static int image_add(const char *path, int want_w)
{
    int slot = nimg < MAXIMG ? nimg++ : (objgen_next % MAXIMG);
    struct imgobj *im = &imgs[slot];
    scopy(im->path, path, sizeof im->path);
    im->ok = 0; im->iw = im->ih = im->tw = im->th = 0;
    int w = 0, h = 0;
    if (img_open(path, img_scratch, (int)sizeof img_scratch, &w, &h) == 0 && w > 0 && h > 0) {
        im->iw = w; im->ih = h;
        int tw = w, th = h;
        if (tw > THW) { th = (int)((long)th * THW / tw); tw = THW; }
        if (th > THH) { tw = (int)((long)tw * THH / th); th = THH; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        downscale(img_scratch, w, h, thumb[slot], tw, th);
        im->tw = tw; im->th = th; im->ok = 1;
    }
    (void)want_w;
    return slot;
}

/* ------------------------------------------------------- frame handling --- */

static void handle_image(struct rt_rd *r)
{
    int kind = rt_rd_u8(r);
    int wpt = rt_rd_u16(r), hpt = rt_rd_u16(r);
    char path[128];
    rt_rd_str(r, path, sizeof path);
    if (r->bad || kind != RT_IMG_PATH || !path[0]) { put_text(S_ERR, "terminal: bad image frame"); return; }

    int slot = image_add(path, wpt);
    struct imgobj *im = &imgs[slot];
    int maxw = text_w - GUTTER - 16;
    if (!im->ok) { im->dw = maxw < 200 ? maxw : 200; im->dh = lh; }
    else {
        int dw = wpt > 0 ? wpt : im->iw;
        int dh = hpt > 0 ? hpt : im->ih;
        if (wpt > 0 && hpt <= 0) dh = (int)((long)im->ih * dw / im->iw);
        if (hpt > 0 && wpt <= 0) dw = (int)((long)im->iw * dh / im->ih);
        if (dw > maxw) { dh = (int)((long)dh * maxw / dw); dw = maxw; }
        if (dh > 320) { dw = (int)((long)dw * 320 / dh); dh = 320; }
        im->dw = dw < 1 ? 1 : dw;
        im->dh = dh < 1 ? 1 : dh;
    }
    /* Layout and paint must agree on the height, so the size is computed ONCE
     * here and stored -- recomputing it in the painter is how a scrollback ends
     * up with a picture that overlaps the line after it. */
    int vr = (im->dh + lh - 1) / lh + 1;                    /* + caption row */
    int o = obj_alloc(O_IMAGE, slot, vr);
    struct tline *l = attach_obj(o);
    /* The caption is the object's plain-text shadow: it is what a selection
     * copies, so copying a screen with a picture on it yields something useful. */
    char a[12], b[12];
    itoa_s(im->iw, a); itoa_s(im->ih, b);
    scopy(l->t, path, MAXCOLS);
    int n = slen(l->t);
    if (im->ok && n + 24 < MAXCOLS) {
        l->t[n++] = ' '; l->t[n++] = '(';
        for (int i = 0; a[i]; i++) l->t[n++] = a[i];
        l->t[n++] = 'x';
        for (int i = 0; b[i]; i++) l->t[n++] = b[i];
        l->t[n++] = ')'; l->t[n] = 0;
    }
    l->len = (short)slen(l->t);
}

static void handle_progress(struct rt_rd *r)
{
    unsigned id = rt_rd_u32(r);
    int pm = rt_rd_u16(r);
    int done = rt_rd_u8(r);
    char label[56];
    rt_rd_str(r, label, sizeof label);
    if (r->bad) return;
    if (pm < 0) pm = 0;
    if (pm > 1000) pm = 1000;

    /* Update in place if this id already has a widget in the current command --
     * a progress bar that appended a line per tick would be an ASCII progress
     * bar with extra steps. */
    for (int i = lcount - 1; i >= 0 && i >= lcount - 200; i--) {
        struct tline *l = LN(i);
        if (l->cmd != cur_cmd || l->cmdid != cur_cmdid) break;
        if (obj_live(l) && objs[l->obj].type == O_PROG) {
            struct prgobj *pg = &prgs[objs[l->obj].idx];
            if (pg->id == id) {
                pg->permille = pm; pg->done = done; scopy(pg->label, label, sizeof pg->label);
                return;
            }
        }
    }
    int slot = nprg < MAXPRG ? nprg++ : (objgen_next % MAXPRG);
    prgs[slot].id = id; prgs[slot].permille = pm; prgs[slot].done = done;
    scopy(prgs[slot].label, label, sizeof prgs[slot].label);
    int o = obj_alloc(O_PROG, slot, 2);
    attach_obj(o);
}

static void handle_table(struct rt_rd *r)
{
    int nc = rt_rd_u16(r), nr = rt_rd_u16(r);
    char title[48];
    rt_rd_str(r, title, sizeof title);
    if (r->bad || nc <= 0 || nr < 0) { put_text(S_ERR, "terminal: bad table frame"); return; }
    if (nc > TCOL) nc = TCOL;
    if (nr > TROW) nr = TROW;

    int slot = ntbl < MAXTBL ? ntbl++ : (objgen_next % MAXTBL);
    struct tblobj *tb = &tbls[slot];
    scopy(tb->title, title, sizeof tb->title);
    tb->ncols = nc; tb->nrows = nr;
    for (int c = 0; c < nc; c++) rt_rd_str(r, tb->hdr[c], TCELL);
    for (int y = 0; y < nr; y++) {
        tb->target[y][0] = 0;
        for (int c = 0; c < nc; c++) {
            int k = rt_rd_u8(r);
            rt_rd_str(r, tb->cellt[y][c], TCELL);
            char tgt[TTGT];
            rt_rd_str(r, tgt, sizeof tgt);
            tb->cellk[y][c] = (unsigned char)(k > RT_CELL_NUM ? RT_CELL_TEXT : k);
            if (k == RT_CELL_LINK && tgt[0] && !tb->target[y][0]) scopy(tb->target[y], tgt, TTGT);
            if (r->bad) { tb->nrows = y; y = nr; break; }
        }
    }
    int o = obj_alloc(O_TABLE, slot, tb->nrows + 3);
    attach_obj(o);
}

static void handle_chart(struct rt_rd *r)
{
    int n = rt_rd_u16(r);
    int kind = rt_rd_u16(r);
    char title[48];
    rt_rd_str(r, title, sizeof title);
    (void)kind;
    if (r->bad || n < 0) { put_text(S_ERR, "terminal: bad chart frame"); return; }
    if (n > CHN) n = CHN;
    int slot = ncht < MAXCHT ? ncht++ : (objgen_next % MAXCHT);
    struct chtobj *ch = &chts[slot];
    scopy(ch->title, title, sizeof ch->title);
    ch->n = n; ch->maxv = 1;
    for (int i = 0; i < n; i++) {
        ch->val[i] = rt_rd_u32(r);
        rt_rd_str(r, ch->lab[i], 16);
        if (r->bad) { ch->n = i; break; }
        if (ch->val[i] > ch->maxv) ch->maxv = ch->val[i];
    }
    int o = obj_alloc(O_CHART, slot, ch->n + 3);
    attach_obj(o);
}

static void handle_frame(int type, const unsigned char *p, int len)
{
    struct rt_frame f = { type, 0, 0, p, len };
    struct rt_rd r;
    rt_rd_init(&r, &f);

    switch (type) {
    case RT_T_HELLO: break;
    case RT_T_CWD: break;                       /* drawn from the prompt instead */
    case RT_T_CMD_BEGIN: {
        unsigned id = rt_rd_u32(&r);
        char text[96];
        rt_rd_str(&r, text, sizeof text);
        if (r.bad) break;
        int slot = cmd_slot(id);
        cmds[slot].used = 1; cmds[slot].id = id; cmds[slot].done = 0;
        cmds[slot].status = 0; cmds[slot].interrupted = 0; cmds[slot].folded = 0;
        scopy(cmds[slot].text, text, sizeof cmds[slot].text);
        cur_cmd = slot; cur_cmdid = id;
        cmd_base = (unsigned)stdout_bytes;
        end_all_lines();
        /* the echoed command joins the scrollback as a prompt line */
        struct tline *l = new_line(S_PROMPT);
        scopy(l->t, text, MAXCOLS);
        l->len = (short)slen(l->t);
        end_all_lines();
        break;
    }
    case RT_T_CMD_END: {
        unsigned id = rt_rd_u32(&r);
        unsigned st = rt_rd_u32(&r);
        int fl = rt_rd_u8(&r);
        if (r.bad) break;
        int slot = cmd_slot(id);
        if (cmds[slot].used && cmds[slot].id == id) {
            cmds[slot].done = 1; cmds[slot].status = (int)st;
            cmds[slot].interrupted = (fl & RT_END_INTERRUPTED) ? 1 : 0;
        }
        end_all_lines();
        cur_cmd = -1; cur_cmdid = 0;
        break;
    }
    case RT_T_INPUT: {
        in_cur = rt_rd_u16(&r);
        rt_rd_str(&r, in_prompt, sizeof in_prompt);
        rt_rd_str(&r, in_buf, sizeof in_buf);
        if (r.bad) { in_buf[0] = 0; in_cur = 0; }
        have_input = 1;
        break;
    }
    case RT_T_IMAGE:    handle_image(&r); break;
    case RT_T_PROGRESS: handle_progress(&r); break;
    case RT_T_TABLE:    handle_table(&r); break;
    case RT_T_CHART:    handle_chart(&r); break;
    default: break;                             /* unknown type: ignore, keep text */
    }
    redraw = 1;
}

/* Frames whose anchor has not been reached yet wait here. */
static void defer_or_run(int type, unsigned seq, const unsigned char *p, int len)
{
    /* Control-ish frames are never deferred: they define the anchor itself. */
    if (type == RT_T_CMD_BEGIN || type == RT_T_CMD_END || type == RT_T_INPUT ||
        type == RT_T_HELLO || type == RT_T_CWD) { handle_frame(type, p, len); return; }
    if (stdout_bytes >= (unsigned long)cmd_base + seq || len > (int)sizeof defers[0].p) {
        handle_frame(type, p, len);
        return;
    }
    for (int i = 0; i < MAXDEFER; i++) {
        if (defers[i].used) continue;
        defers[i].used = 1; defers[i].type = type; defers[i].seq = seq; defers[i].len = len;
        for (int k = 0; k < len; k++) defers[i].p[k] = p[k];
        return;
    }
    handle_frame(type, p, len);                 /* out of slots: order over loss */
}

static void run_ready_defers(void)
{
    for (int i = 0; i < MAXDEFER; i++)
        if (defers[i].used && stdout_bytes >= (unsigned long)cmd_base + defers[i].seq) {
            defers[i].used = 0;
            handle_frame(defers[i].type, defers[i].p, defers[i].len);
        }
}

/* ------------------------------------------------------------- drawing ---- */

static void draw_text(int x, int y, unsigned color, const char *s)
{ gui_text_mono(x, y, color, cell, s); }

static void draw_span(int x, int y, unsigned color, const char *s, int from, int n)
{
    if (n <= 0) return;
    char tmp[MAXCOLS + 1];
    int k = 0;
    for (int i = 0; i < n && from + i < MAXCOLS && s[from + i]; i++) tmp[k++] = s[from + i];
    tmp[k] = 0;
    if (k) gui_text_mono(x + from * cell, y, color, cell, tmp);
}

/* selection normalized */
static void sel_norm(int *l0, int *c0, int *l1, int *c1)
{
    if (sa_l < sb_l || (sa_l == sb_l && sa_c <= sb_c)) { *l0 = sa_l; *c0 = sa_c; *l1 = sb_l; *c1 = sb_c; }
    else { *l0 = sb_l; *c0 = sb_c; *l1 = sa_l; *c1 = sa_c; }
}

static int line_visible(const struct tline *l)
{
    struct cmdrec *c = cmd_of(l);
    if (errs_only) {
        if (l->stream == S_ERR) return 1;
        if (c && c->done && c->status != 0) return 1;
        return 0;
    }
    if (c && c->folded && l->stream != S_PROMPT) return 0;
    return 1;
}

static int total_rows(void)
{
    int n = 0;
    for (int i = 0; i < lcount; i++) { struct tline *l = LN(i); if (line_visible(l)) n += l->vrows; }
    return n;
}

static void draw_image_obj(int x, int y, int w, struct robj *o)
{
    struct imgobj *im = &imgs[o->idx];
    (void)w;
    if (!im->ok) { draw_text(x, y, P.err, "[image could not be decoded]"); return; }
    gui_rect(x - 1, y - 1, im->dw + 2, im->dh + 2, P.rule);
    gui_blit(x, y, im->dw, im->dh, thumb[o->idx], im->tw, im->th);
}

static void draw_prog_obj(int x, int y, int w, struct robj *o)
{
    struct prgobj *pg = &prgs[o->idx];
    int bw = w - 90; if (bw < 60) bw = 60;
    gui_rrect(x, y + 4, bw, 10, 5, P.panel);
    int fill = bw * pg->permille / 1000;
    if (fill > 0) gui_rrect(x, y + 4, fill, 10, 5, pg->done ? P.ok : P.accent);
    char pct[16], num[12];
    utoa((unsigned)(pg->permille / 10), num);
    int k = 0; for (int i = 0; num[i]; i++) pct[k++] = num[i]; pct[k++] = '%'; pct[k] = 0;
    draw_text(x + bw + 8, y, P.dim, pct);
    if (pg->label[0]) draw_text(x, y + lh, P.dim, pg->label);
}

static void draw_table_obj(int x, int y, int w, struct robj *o)
{
    struct tblobj *tb = &tbls[o->idx];
    int nc = tb->ncols;
    int cw = (w - 8) / (nc > 0 ? nc : 1);
    int ccols = cw / cell; if (ccols < 3) ccols = 3;
    if (tb->title[0]) draw_text(x, y, P.dim, tb->title);
    int hy = y + lh;
    for (int c = 0; c < nc; c++) draw_text(x + c * cw, hy, P.accent, tb->hdr[c]);
    gui_rect(x, hy + lh - 3, w - 8, 1, P.rule);
    for (int r2 = 0; r2 < tb->nrows; r2++) {
        int ry = hy + lh + 2 + r2 * lh;
        if (r2 & 1) gui_rect(x - 2, ry - 2, w - 4, lh, P.panel);
        for (int c = 0; c < nc; c++) {
            unsigned col = tb->cellk[r2][c] == RT_CELL_LINK ? P.accent : P.fg;
            char tmp[TCELL];
            scopy(tmp, tb->cellt[r2][c], imin(TCELL, ccols + 1));
            int tx = x + c * cw;
            if (tb->cellk[r2][c] == RT_CELL_NUM) {
                int n = slen(tmp);
                tx = x + c * cw + cw - 8 - n * cell;
                if (tx < x + c * cw) tx = x + c * cw;
            }
            gui_text_mono(tx, ry, col, cell, tmp);
        }
    }
    gui_rect(x, hy + lh + 2 + tb->nrows * lh, w - 8, 1, P.rule);
}

static void draw_chart_obj(int x, int y, int w, struct robj *o)
{
    struct chtobj *ch = &chts[o->idx];
    if (ch->title[0]) draw_text(x, y, P.dim, ch->title);
    int labw = 12 * cell;
    int barw = w - labw - 70;
    if (barw < 40) barw = 40;
    for (int i = 0; i < ch->n; i++) {
        int by = y + lh + i * lh;
        char lab[16]; scopy(lab, ch->lab[i], sizeof lab);
        draw_text(x, by, P.fg, lab);
        int len = ch->maxv ? (int)((long)barw * ch->val[i] / ch->maxv) : 0;
        if (len < 1 && ch->val[i]) len = 1;
        gui_rrect(x + labw, by + 3, barw, lh - 7, 3, P.panel);
        if (len > 0) gui_rrect(x + labw, by + 3, len, lh - 7, 3, P.accent);
        char num[12]; utoa(ch->val[i], num);
        draw_text(x + labw + barw + 8, by, P.dim, num);
    }
}

static void draw_scrollbar(int total, int view)
{
    if (total <= view) return;
    int tx = win_w - PAD - SBW;
    gui_rrect(tx, text_y, SBW, text_h, SBW / 2, P.panel);
    int th = text_h * view / total; if (th < 16) th = 16;
    int ty = text_y + (text_h - th) * scroll / imax(1, total - view);
    gui_rrect(tx, ty, SBW, th, SBW / 2, P.dim);
}

static void paint(void)
{
    gui_clear(P.bg);

    /* top strip: filter/fold state, so a hidden line is never a mystery */
    gui_rect(0, 0, win_w, TOPBAR, P.panel);
    {
        char s[80];
        scopy(s, errs_only ? "errors only  (^E)" : "^C interrupt   ^E errors   ^L clear   ^V paste",
              sizeof s);
        gui_text(PAD, 3, errs_only ? P.err : P.dim, s);
    }

    int view = text_h / lh;
    int total = total_rows();
    if (follow) scroll = imax(0, total - view);
    if (scroll > imax(0, total - view)) scroll = imax(0, total - view);
    if (scroll < 0) scroll = 0;

    int l0, c0, l1, c1;
    sel_norm(&l0, &c0, &l1, &c1);

    /* A rich object can be TALLER THAN THE VIEW (a 30-row table is), and it is
     * drawn from its own top -- which is above the viewport once you scroll into
     * its middle. Without this clip the table paints straight over the hint strip
     * and the window's own chrome. */
    gui_clip(PAD, text_y, win_w - 2 * PAD, text_h);

    int row = 0;
    for (int i = 0; i < lcount; i++) {
        struct tline *l = LN(i);
        if (!line_visible(l)) continue;
        int r0 = row;
        row += l->vrows;
        if (r0 + l->vrows <= scroll) continue;
        if (r0 - scroll >= view) break;

        int y = text_y + (r0 - scroll) * lh;
        int x = text_x;

        /* gutter: command status */
        struct cmdrec *cr = cmd_of(l);
        if (cr && l->stream == S_PROMPT) {
            unsigned c = !cr->done ? P.accent : cr->interrupted ? P.dim
                        : cr->status == 0 ? P.ok : P.bad;
            gui_rrect(PAD, y + 4, 6, 6, 3, c);
        }

        if (l->kind == L_OBJ && obj_live(l)) {
            struct robj *o = &objs[l->obj];
            int ow = text_w - GUTTER;
            switch (o->type) {
            case O_IMAGE:
                draw_image_obj(x, y, ow, o);
                /* the caption is also what a selection copies, so it is the
                 * object's plain-text shadow as well as a label */
                draw_text(x, y + imgs[o->idx].dh + 3, P.dim, l->t);
                break;
            case O_PROG:  draw_prog_obj(x, y, ow, o); break;
            case O_TABLE: draw_table_obj(x, y, ow, o); break;
            case O_CHART: draw_chart_obj(x, y, ow, o); break;
            default: break;
            }
            continue;
        }

        /* selection highlight */
        if (sel_on && i >= l0 && i <= l1) {
            int from = (i == l0) ? c0 : 0;
            int to   = (i == l1) ? c1 : l->len;
            if (to > l->len) to = l->len;
            if (from < 0) from = 0;
            if (to > from) gui_rect(x + from * cell, y, (to - from) * cell, lh, P.sel);
        }

        unsigned col = l->stream == S_ERR ? P.err
                     : l->stream == S_PROMPT ? P.prompt
                     : l->stream == S_SYS ? P.dim : P.fg;
        if (l->stream == S_PROMPT) {
            draw_text(x, y, P.dim, "$");
            draw_span(x + 2 * cell, y, col, l->t, 0, l->len);
            if (cr && cr->done && cr->status != 0) {
                char st[16], n[12]; itoa_s(cr->status, n);
                int k = 0; st[k++] = '['; for (int q = 0; n[q]; q++) st[k++] = n[q]; st[k++] = ']'; st[k] = 0;
                draw_text(x + (l->len + 4) * cell, y, P.bad, st);
            }
            if (cr && cr->folded)
                draw_text(x + (l->len + 9) * cell, y, P.dim, "...");
        } else {
            draw_text(x, y, col, l->t);
        }
    }

    gui_clip(0, 0, 0, 0);
    draw_scrollbar(total, view);

    /* input line, rendered from the shell's live edit state */
    int iy = win_h - lh - 6;
    gui_rect(0, iy - 5, win_w, lh + 11, P.panel);
    if (!alive) {
        draw_text(PAD, iy, P.dim, "[shell exited]");
    } else if (have_input) {
        int px = PAD;
        draw_text(px, iy, P.prompt, in_prompt);
        int poff = slen(in_prompt);
        int avail = (win_w - 2 * PAD) / cell - poff - 1;
        if (avail < 8) avail = 8;
        int first = 0;
        if (in_cur > avail) first = in_cur - avail;
        char vis[MAXCOLS + 1];
        int k = 0;
        for (int i = first; in_buf[i] && k < avail && k < MAXCOLS; i++) vis[k++] = in_buf[i];
        vis[k] = 0;
        draw_text(px + poff * cell, iy, P.fg, vis);
        int cx = px + (poff + (in_cur - first)) * cell;
        gui_rect(cx, iy, 2, lh - 2, P.prompt);
    } else {
        draw_text(PAD, iy, P.dim, "starting shell...");
    }
    if (scroll < total - view) {
        const char *s = "^ scrolled back -- End to follow";
        draw_text(win_w - PAD - SBW - (slen(s) + 1) * cell, iy, P.dim, s);
    }
    gui_flush();
}

/* --------------------------------------------------------- hit testing ---- */

/* Map a window point to a scrollback (line, col). Returns the line index, or -1. */
static int hit_line(int mx, int my, int *col, int *lrow)
{
    if (my < text_y || my >= text_y + text_h) return -1;
    int want = scroll + (my - text_y) / lh;
    int row = 0;
    for (int i = 0; i < lcount; i++) {
        struct tline *l = LN(i);
        if (!line_visible(l)) continue;
        if (want < row + l->vrows) {
            if (col) { int c = (mx - text_x) / cell; if (c < 0) c = 0; if (c > l->len) c = l->len; *col = c; }
            if (lrow) *lrow = want - row;
            return i;
        }
        row += l->vrows;
    }
    return -1;
}

static void copy_selection(void)
{
    int l0, c0, l1, c1;
    sel_norm(&l0, &c0, &l1, &c1);
    clip_n = 0;
    for (int i = l0; i <= l1 && i < lcount; i++) {
        struct tline *l = LN(i);
        if (!line_visible(l)) continue;
        int from = (i == l0) ? c0 : 0;
        int to   = (i == l1) ? c1 : l->len;
        if (to > l->len) to = l->len;
        for (int k = from; k < to && clip_n < (int)sizeof clip - 2; k++) clip[clip_n++] = l->t[k];
        if (i != l1 && clip_n < (int)sizeof clip - 2) clip[clip_n++] = '\n';
    }
    clip[clip_n] = 0;
}

/* Copy exactly one command's output -- the thing a byte-stream terminal can only
 * approximate, because it cannot tell where a command's output began. */
static void copy_command(int slot, unsigned id)
{
    clip_n = 0;
    for (int i = 0; i < lcount; i++) {
        struct tline *l = LN(i);
        if (l->cmd != slot || l->cmdid != id || l->stream == S_PROMPT) continue;
        for (int k = 0; k < l->len && clip_n < (int)sizeof clip - 2; k++) clip[clip_n++] = l->t[k];
        if (clip_n < (int)sizeof clip - 2) clip[clip_n++] = '\n';
    }
    clip[clip_n] = 0;
}

static void ctl_send(int type, struct rt_enc *e)
{
    if (ctl_w < 0) return;
    unsigned char h[RT_HDR];
    int len = e ? e->n : 0;
    rt_hdr(h, type, 0, (unsigned)len);
    for (int i = 0; i < RT_HDR; i++) cout_push(h[i]);
    for (int i = 0; i < len; i++) cout_push(e->b[i]);
    if (e) rt_reset(e);
    flush_ctl();
}

/* Typed characters, batched into one frame per event pump. */
static char typed[256];
static int  typed_n;
static void type_char(char c) { if (typed_n < (int)sizeof typed) typed[typed_n++] = c; }
static void type_flush(void)
{
    if (!typed_n) return;
    rt_reset(&enc);
    rt_strn(&enc, typed, typed_n);
    typed_n = 0;
    ctl_send(RT_C_TEXT, &enc);
}

static void on_click(int mx, int my, int right)
{
    /* gutter click -> fold/unfold that command */
    int col = 0;
    int i = hit_line(mx, my, &col, 0);
    if (i < 0) return;
    struct tline *l = LN(i);
    struct cmdrec *cr = cmd_of(l);

    if (mx < text_x && cr) {
        cr->folded = !cr->folded;
        redraw = 1;
        return;
    }
    if (right) {
        if (cr) { copy_command(l->cmd, l->cmdid); redraw = 1; }
        return;
    }
    if (l->kind == L_OBJ && obj_live(l)) {
        struct robj *o = &objs[l->obj];
        if (o->type == O_TABLE) {
            struct tblobj *tb = &tbls[o->idx];
            /* which row of the table did we land on? */
            int lrow = 0;
            hit_line(mx, my, 0, &lrow);
            int r2 = lrow - 2;
            if (r2 >= 0 && r2 < tb->nrows && tb->target[r2][0]) {
                sys_open_path(tb->target[r2]);
                return;
            }
        }
        if (o->type == O_IMAGE) { sys_open_path(imgs[o->idx].path); return; }
        return;
    }
    /* clicking a command line re-runs it */
    if (l->stream == S_PROMPT && cr && cr->done) {
        rt_reset(&enc);
        rt_str(&enc, cr->text);
        ctl_send(RT_C_RERUN, &enc);
        follow = 1;
        return;
    }
    sel_on = 1; sel_drag = 1; sa_l = sb_l = i; sa_c = sb_c = col;
    redraw = 1;
}

/* ---------------------------------------------------------------- setup --- */

static void measure_geometry(void)
{
    /* The cell is MEASURED, not declared: gui_text_mono draws the mono font at
     * fb_ui_px() and advances by the cell we pass, so the only cell that lines
     * text up with itself is the font's own advance -- at whatever backing scale
     * the display turned out to have. */
    int adv = text_measure_px("M", 1, UIPX, 1);
    cell = adv > 0 ? adv : 10;
    if (cell < 5) cell = 5;
    if (cell > 40) cell = 40;
    lh = UIPX + UIPX / 4;                       /* one line box + 25% leading */
    if (lh < cell + 4) lh = cell + 4;

    int sw = screen_w(), sh = screen_h();
    if (sw < 320) sw = 1280;
    if (sh < 240) sh = 800;
    win_w = sw - 260; if (win_w > 900) win_w = 900; if (win_w < 420) win_w = 420;
    win_h = sh - 240; if (win_h > 620) win_h = 620; if (win_h < 300) win_h = 300;

    text_x = PAD + GUTTER;
    text_y = TOPBAR + 4;
    text_w = win_w - text_x - PAD - SBW;
    text_h = win_h - text_y - (lh + 14);
    cols = text_w / cell;
    if (cols > MAXCOLS) cols = MAXCOLS;
    if (cols < 20) cols = 20;
    rows = text_h / lh;
    if (rows < 4) rows = 4;
}

/* Build the child environment. LOGIT_RICH is the whole discovery mechanism: a
 * program that does not find it emits plain text and is none the wiser. */
static char env_term[48], env_rich[32], env_ctl[32], env_cols[32], env_rows[32], env_cell[32];

static void setenv_num(char *dst, const char *key, int v)
{
    int k = 0;
    while (*key) dst[k++] = *key++;
    char n[12]; utoa((unsigned)v, n);
    for (int i = 0; n[i]; i++) dst[k++] = n[i];
    dst[k] = 0;
}

static int spawn_shell(void)
{
    int ip[2], op[2], ep[2], rp[2], cp[2];
    if (sys_pipe(ip) < 0) return -1;
    if (sys_pipe(op) < 0) return -1;
    if (sys_pipe(ep) < 0) return -1;
    if (sys_pipe(rp) < 0) return -1;
    if (sys_pipe(cp) < 0) return -1;

    int pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Fixed fd numbers are the contract: 0/1/2 as always, 3 = rich frames
         * out, 4 = control frames in. dup2 to 5/6 first would be neater but the
         * shell has to name them somewhere, and naming them here keeps the
         * protocol readable in one place. */
        sys_dup2(ip[0], 0);
        sys_dup2(op[1], 1);
        sys_dup2(ep[1], 2);
        sys_dup2(rp[1], 3);
        sys_dup2(cp[0], 4);
        for (int i = 0; i < 2; i++) {
            if (ip[i] > 4) sys_close(ip[i]);
            if (op[i] > 4) sys_close(op[i]);
            if (ep[i] > 4) sys_close(ep[i]);
            if (rp[i] > 4) sys_close(rp[i]);
            if (cp[i] > 4) sys_close(cp[i]);
        }
        char *argv[] = { "sh", 0 };
        setenv_num(env_term, "LOGIT_TERM=logit-rich-", RT_VERSION);
        setenv_num(env_rich, "LOGIT_RICH=", 3);
        setenv_num(env_ctl,  "LOGIT_CTL=", 4);
        setenv_num(env_cols, "LOGIT_COLS=", cols);
        setenv_num(env_rows, "LOGIT_ROWS=", rows);
        setenv_num(env_cell, "LOGIT_CELL=", cell);
        char *envp[] = { env_term, env_rich, env_ctl, env_cols, env_rows, env_cell, 0 };
        sys_execve("/bin/sh", argv, envp);
        app_exit(127);
    }
    sys_close(ip[0]); sys_close(op[1]); sys_close(ep[1]); sys_close(rp[1]); sys_close(cp[0]);
    in_w = ip[1]; out_r = op[0]; err_r = ep[0]; rich_r = rp[0]; ctl_w = cp[1];
    sys_set_nonblock(out_r);
    sys_set_nonblock(err_r);
    sys_set_nonblock(rich_r);
    sys_set_nonblock(in_w);
    sys_set_nonblock(ctl_w);
    return 0;
}

/* -------------------------------------------------------------- pumping --- */

static int drain_stream(int fd, int stream)
{
    if (fd < 0) return 0;
    char ob[1024];
    int rounds = 0, n = EAGAIN_RC, got = 0;
    while (rounds++ < 64 && (n = sys_read(fd, ob, sizeof ob)) > 0) {
        for (int i = 0; i < n; i++) put_char(stream, ob[i]);
        if (stream == S_OUT) stdout_bytes += (unsigned long)n;
        got = 1; redraw = 1;
    }
    if (n == 0) return -1;                       /* EOF */
    return got;
}

static void drain_rich(void)
{
    if (rich_r < 0) return;
    unsigned char ob[1024];
    int rounds = 0;
    for (;;) {
        struct rt_frame f;
        while (rt_parser_next(&parser, &f)) {
            defer_or_run(f.type, f.seq, f.p, f.len);
            rt_parser_done(&parser, &f);
        }
        if (rounds++ >= 64) break;
        int n = sys_read(rich_r, ob, sizeof ob);
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            int took = rt_parser_feed(&parser, ob + off, n - off);
            off += took;
            if (took == 0) {
                struct rt_frame g;
                if (rt_parser_next(&parser, &g)) { defer_or_run(g.type, g.seq, g.p, g.len); rt_parser_done(&parser, &g); }
                else break;                     /* cannot make progress: drop rest */
            }
        }
    }
}

static void scroll_by(int d)
{
    int view = text_h / lh, total = total_rows();
    scroll += d;
    if (scroll < 0) scroll = 0;
    if (scroll > imax(0, total - view)) scroll = imax(0, total - view);
    follow = (scroll >= imax(0, total - view));
    redraw = 1;
}

static void key_to_shell(int code)
{
    rt_reset(&enc);
    rt_u32(&enc, (unsigned)code);
    ctl_send(RT_C_KEY, &enc);
}

static void on_key(struct logit_event *e)
{
    int a = e->a, mods = e->mods;

    if (a == KEY_PGUP) { scroll_by(-(text_h / lh) + 1); return; }
    if (a == KEY_PGDN) { scroll_by(text_h / lh - 1); return; }
    if (a == KEY_UP && (mods & EV_MOD_SHIFT)) { scroll_by(-1); return; }
    if (a == KEY_DOWN && (mods & EV_MOD_SHIFT)) { scroll_by(1); return; }
    if (a == KEY_HOME && (mods & EV_MOD_SHIFT)) { scroll = 0; follow = 0; redraw = 1; return; }
    if (a == KEY_END && (mods & EV_MOD_SHIFT)) { follow = 1; redraw = 1; return; }
    if (a > 0xFF) { key_to_shell(a); follow = 1; return; }

    switch (a) {
    case 3:                                     /* ^C, or copy when text is selected */
        if (sel_on || (mods & EV_MOD_SHIFT)) { copy_selection(); sel_on = 0; redraw = 1; return; }
        ctl_send(RT_C_INTR, 0);
        follow = 1;
        return;
    case 4:                                     /* ^D */
        ctl_send(RT_C_EOF, 0);
        return;
    case 5:                                     /* ^E: errors-only filter */
        errs_only = !errs_only; follow = 1; redraw = 1;
        return;
    case 12:                                    /* ^L: clear scrollback */
        lbase = 0; lcount = 0; labs = 0; end_all_lines(); scroll = 0; follow = 1; redraw = 1;
        return;
    case 22:                                    /* ^V: paste */
        for (int i = 0; i < clip_n; i++) type_char(clip[i]);
        follow = 1;
        return;
    default: break;
    }
    if (a > 0 && a <= 0xFF) { type_char((char)a); follow = 1; redraw = 1; }
}

/* ----------------------------------------------------------------- main --- */

void app_main(void)
{
    theme_load();
    measure_geometry();
    gui_create("Terminal", win_w, win_h);
    rt_parser_init(&parser);
    rt_reset(&enc);

    if (spawn_shell() < 0) {
        put_text(S_ERR, "terminal: cannot start /bin/sh");
        alive = 0;
    } else {
        rt_reset(&enc);
        rt_u16(&enc, cols); rt_u16(&enc, rows); rt_u16(&enc, cell);
        ctl_send(RT_C_SIZE, &enc);
    }

    /* Opened on a file (the WM's file-type fallback launches the Terminal with
     * the path as its argument): run something sensible on it. */
    {
        char arg[160];
        if (get_arg(arg, sizeof arg) > 0 && ctl_w >= 0) {
            int n = slen(arg);
            const char *cmd = (n > 3 && arg[n - 3] == '.' && arg[n - 2] == 'a' && arg[n - 1] == 's')
                              ? "as " : "show ";
            for (const char *s = cmd; *s; s++) type_char(*s);
            for (const char *s = arg; *s; s++) type_char(*s);
            type_char('\n');
        }
    }

    for (;;) {
        struct logit_event e;
        while (poll_event(&e)) {
            switch (e.type) {
            case EV_CLOSE: app_exit(0);
            case EV_THEME: theme_load(); redraw = 1; break;
            case EV_KEY:   if (alive) on_key(&e); break;
            case EV_MOUSE:
            case EV_MOUSE_R: {
                if (e.a > win_w - PAD - SBW && e.b >= text_y && e.b < text_y + text_h) {
                    int view = text_h / lh, total = total_rows();
                    scroll = (e.b - text_y) * imax(0, total - view) / imax(1, text_h);
                    follow = (scroll >= imax(0, total - view));
                    redraw = 1;
                    break;
                }
                sel_on = 0;
                on_click(e.a, e.b, e.type == EV_MOUSE_R || e.button == EV_BTN_RIGHT);
                break;
            }
            case EV_MOUSE_MOVE:
                if (sel_drag) {
                    int col = 0;
                    int i = hit_line(e.a, e.b, &col, 0);
                    if (i >= 0) { sb_l = i; sb_c = col; sel_on = 1; redraw = 1; }
                }
                break;
            case EV_MOUSE_UP:
                if (sel_drag) { sel_drag = 0; if (sa_l == sb_l && sa_c == sb_c) sel_on = 0; redraw = 1; }
                break;
            case EV_WHEEL:
                scroll_by(e.wheel * 3);
                break;
            default: break;
            }
        }

        type_flush();
        flush_ctl();

        if (alive) {
            int a1 = drain_stream(out_r, S_OUT);
            int a2 = drain_stream(err_r, S_ERR);
            drain_rich();
            run_ready_defers();
            if (a1 < 0 && a2 <= 0) {
                alive = 0;
                if (in_w >= 0) { sys_close(in_w); in_w = -1; }
                put_text(S_SYS, "[shell exited]");
                redraw = 1;
            }
        }

        if (redraw) { redraw = 0; paint(); }
        sys_yield();
    }
}
