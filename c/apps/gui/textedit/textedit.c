/* TextEdit -- a small text editor.
 *
 * WHAT WAS WRONG WITH IT, and none of it was the editing. The status bar was
 * `gui_rect(..., rgb(236, 238, 242))` with a `rgb(214, 216, 222)` rule over it:
 * two light-mode colours written into the source, so in dark mode this window
 * had a white strip glued to the bottom of a near-black page. The window size
 * was three constants that had to agree, the monospace advance was assumed to
 * be 8 pixels regardless of the font or the backing scale, and text past the
 * bottom edge was drawn off the window rather than scrolled to -- so a file
 * longer than eighteen lines could be typed into and never seen.
 *
 * Everything visible now comes from the toolkit's tokens and from
 * aui_width()/aui_height() on the frame it is drawn, and the advance is
 * measured from the font actually loaded.
 *
 * STILL DELIBERATELY SMALL: one buffer, append-and-backspace, no selection, no
 * undo, no mouse caret placement. This is the app Finder opens a .txt with, and
 * growing it into an editor is a different piece of work from making it stop
 * looking wrong.
 *
 * 2026-09: the selected document/work layout now adds a movable UTF-8 caret,
 * selection, clipboard and one-step undo. The older append-only and dirty-tail
 * arguments below describe the implementation this replaces, not its limits.
 * Layout now caches line starts and uses AUI text primitives for invalidation. */
#include "aui.h"
#include "textedit_document.h"
#include <stdio.h>
#include "../../../lib/agent/sdk.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAXT   ((int)AEX_AGENT_DOCUMENT_MAX)
#define CTRL_S 0x13

static char *text;
static int  tlen;
static char fname[AG_PATH];
static uint64_t agent_task,agent_revision=1;
static int agent_dirty,load_refused;
static const char *agent_notice;
static int caret,anchor,editor_focus=1,follow_caret=1,sidebar=1,view_mode;
static int te_font=18,te_mono,te_weight;
static unsigned long long sync_after;
static unsigned edit_epoch;
static void work_refresh(int enable);
static void ask_logit(void);


static int load_file(void)
{
    int fd=open(fname,O_RDONLY);
    if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;int rc=0;
    if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<0)rc=-1;
    else if((uint64_t)st.st_size>MAXT)rc=-2;
    int got=0;
    while(!rc&&got<MAXT){long n=read(fd,text+got,(size_t)(MAXT-got));
        if(n<0){rc=-1;break;}if(!n)break;got+=(int)n;}
    /* A file growing after fstat must be refused, never silently truncated. */
    if(!rc&&got==MAXT){char extra;long n=read(fd,&extra,1);if(n)rc=n>0?-2:-1;}
    if(close(fd)<0)rc=-1;
    if(!rc)tlen=got;
    return rc;
}

static void agent_sync(void)
{
    if(!agent_task||!agent_dirty)return;
    struct ag_message m={.type=AG_EDIT,.task=agent_task,.revision=agent_revision,.bytes=(uint32_t)tlen};void *reply=0;
    int r=ag_call(&m,text,&reply);free(reply);
    if(!r){agent_revision=m.revision;agent_dirty=0;agent_notice=0;}
    else agent_notice=r==AG_E_CONFLICT?"版本已变化，本地编辑已保留，请先核对":"任务服务暂不可用，本地编辑已保留";
}
static int agent_refresh(void)
{
    static unsigned long long next;
    if(!agent_task||monotonic_ms()<next)return 0;next=monotonic_ms()+1500;
    struct ag_message m={.type=AG_DOCUMENT,.task=agent_task};void *doc=0;int r=ag_call(&m,0,&doc);
    if(!r&&m.revision>agent_revision&&m.bytes<=(unsigned)MAXT){
        if(agent_dirty&&(m.bytes!=(unsigned)tlen||memcmp(text,doc,m.bytes))){
            agent_notice="其他窗口更新了文档，本地编辑已保留";free(doc);return 1;}
        if(m.bytes)memcpy(text,doc,m.bytes);tlen=(int)m.bytes;text[tlen]=0;agent_revision=m.revision;
        agent_dirty=0;caret=anchor=tlen;edit_epoch++;
        agent_notice="已载入最新文档版本";}
    free(doc);work_refresh(0);return 1;
}
static int save_document(void)
{
    agent_sync();
    if(agent_task){if(agent_dirty)return -1;
        struct ag_message m={.type=AG_SAVE,.task=agent_task,.revision=agent_revision};void *out=0;int r=ag_call(&m,0,&out);free(out);
        agent_notice=r<0?"保存失败，当前文档已保留":"已保存新的文档版本";return r;}
    int fd=open(fname,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
    int off=0,r=0;while(off<tlen){long n=write(fd,text+off,(size_t)(tlen-off));if(n<=0){r=-1;break;}off+=(int)n;}
    if(!r&&fsync(fd)<0)r=-1;if(close(fd)<0)r=-1;return r;
}
static int  saved;          /* 1 just after a successful save, 0 once edited */
static int  scroll;         /* first visible line */

/* Window geometry, remembered in the "app." preference namespace (see the
 * SYS_SETTING_ENUM comment in logit_abi.h): app.textedit.w / app.textedit.h,
 * per-user, no schema entry needed -- the first thing in this codebase that
 * used the store for anything other than the nine hardcoded machine keys.
 *
 * geom_dirty + commit-at-close, not commit-per-resize: a window being dragged
 * by its corner fires EV_RESIZE many times a second, and setting_set(...,
 * commit=0) is a RAM-only update -- cheap regardless of how often it runs --
 * while setting_commit() is a whole-file LogitFS write. Committing on every
 * frame of a drag would turn "resize the window" into "hammer the disk"; the
 * store's own doc comment says as much ("set several keys with commit=0 and
 * finish with setting_commit() -- rather than N of them"), and a resize drag
 * is exactly that batch, just spread across frames instead of one call. */
#define TE_W_DEFAULT 1120
#define TE_H_DEFAULT 660
#define TE_W_MIN     240
#define TE_H_MIN     160
static int geom_dirty;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- UTF-8, at the one place a raw EV_KEY codepoint becomes bytes in `text`
 * ----
 *
 * logit_abi.h: EV_KEY's `a` is "a character, or a KEY_* code ... all > 0xFF
 * so they never collide with a character". Above ASCII, `a` is either one of
 * the eight enumerated navigation codes or a Unicode code point -- the pinyin
 * IME commits CJK this way. `(char)a` truncates a code point to its low byte;
 * te_apply_key and te_utf8_encode are the fix, factored out so a host harness
 * can drive them without a window (no gui_create, no poll_event --
 * text/tlen/saved are the same file statics app_main uses).
 *
 * THAT HARNESS DOES NOT EXIST. This comment named `tests/unit/textedit_test.c`
 * in the present tense; `ls` says no such file, and neither the root Makefile
 * nor any of the 105 fragments under tests/ mentions the word. The factoring is
 * real and still the right shape -- te_apply_key, te_utf8_encode, and now
 * te_cp_len/te_fit/te_walk are all pure functions over the file statics, which
 * is exactly what a host gate needs -- but a cited gate that is not in the tree
 * reads as coverage and is not, so it is named as absent here rather than left
 * to be believed. The same harness would cover the wrap and the caret below.
 *
 * -DAUI_BYTE_BACKSPACE is the negative control: it reverts backspace to
 * removing one BYTE, the original bug -- a CJK character deleted this way
 * leaves its lead byte's continuation bytes in the buffer, decodable as
 * neither the old character nor a new one. Named to match aui.c's control
 * rather than invented separately: both text-entry sites share one flag and
 * one falsifiable claim ("delete a whole character"). */
static int te_is_nav_key(int a)
{
    return a == KEY_UP || a == KEY_DOWN || a == KEY_PGUP || a == KEY_PGDN ||
           a == KEY_HOME || a == KEY_END || a == KEY_LEFT || a == KEY_RIGHT;
}

static int te_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int te_is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* One key against `text`/`tlen`/`saved` -- the whole EV_KEY handler pulled out
 * of app_main's event loop. `wrote` reports whether Ctrl+S was actually asked
 * to write (the test has no filesystem, so it can only check the request was
 * made, not that it landed). Returns 1 if anything changed that a repaint
 * should reflect, 0 for a key this app ignores (navigation) or a no-op
 * (buffer full). */
/* The old handler discarded navigation and edited only the tail. Selection
 * replacement now has one capacity gate, shared by typing, paste and markup. */
static char *undo_text;static int undo_len,undo_caret,undo_anchor,undo_valid;
static void remember_edit(void)
{
    if(!undo_text)undo_text=malloc(MAXT+1);
    if(undo_text){memcpy(undo_text,text,(size_t)tlen+1);undo_len=tlen;undo_caret=caret;undo_anchor=anchor;undo_valid=1;}
}
static void edited(void)
{saved=0;agent_dirty=agent_task!=0;agent_notice=0;edit_epoch++;follow_caret=1;sync_after=monotonic_ms()+300;}
static int replace_selection(const char *s,int n)
{
    int selected=caret>anchor?caret-anchor:anchor-caret;
    if(n>MAXT-(tlen-selected)){agent_notice="文档上限为 1 MiB，内容未被截断";return 0;}
    remember_edit();if(ted_replace(text,&tlen,MAXT,&caret,&anchor,s,n)<0)return 0;
    edited();return 1;
}
static int te_apply_key(int a,int mods,int *wrote)
{
    if(a==CTRL_S){*wrote=1;return 1;}
    if(a==1){anchor=0;caret=tlen;follow_caret=1;return 1;}
    if(a==26&&undo_valid){
        char *swap=text;text=undo_text;undo_text=swap;int n=tlen;tlen=undo_len;undo_len=n;
        n=caret;caret=undo_caret;undo_caret=n;n=anchor;anchor=undo_anchor;undo_anchor=n;edited();return 1;}
    if(a==3||a==24){int lo=caret<anchor?caret:anchor,hi=caret>anchor?caret:anchor;
        if(hi>lo&&clip_set(CLIP_F_TEXT,text+lo,hi-lo)>=0&&a==24)return replace_selection("",0);return 0;}
    if(a==22){int n=clip_len(CLIP_F_TEXT);if(n<=0)return 0;
        int selected=caret>anchor?caret-anchor:anchor-caret;
        if(n>MAXT-(tlen-selected)){agent_notice="粘贴超出 1 MiB，内容未被截断";return 1;}
        char *b=malloc((size_t)n+1);if(!b)return 0;
        int got=clip_get(CLIP_F_TEXT,b,n),r=0;if(got==n&&ag_utf8(b,(unsigned)n))r=replace_selection(b,n);free(b);return r;}
    if(te_is_nav_key(a)){
        int p=caret;
        if(a==KEY_LEFT)p=ted_prev(text,p);
        if(a==KEY_RIGHT)p=ted_next(text,p,tlen);
        if(a==KEY_HOME){if(mods&EV_MOD_CTRL)p=0;else while(p>0&&text[p-1]!='\n')p--;}
        if(a==KEY_END){if(mods&EV_MOD_CTRL)p=tlen;else while(p<tlen&&text[p]!='\n')p++;}
        if(a==KEY_UP||a==KEY_DOWN||a==KEY_PGUP||a==KEY_PGDN){
            int start=p;while(start>0&&text[start-1]!='\n')start--;
            int col=0;for(int q=start;q<p;q=ted_next(text,q,tlen))col++;
            int steps=(a==KEY_PGUP||a==KEY_PGDN)?12:1;
            while(steps--){if(a==KEY_UP||a==KEY_PGUP){if(!start)break;start--;while(start>0&&text[start-1]!='\n')start--;}
                else {while(start<tlen&&text[start]!='\n')start++;if(start<tlen)start++;}}
            p=start;while(col--&&p<tlen&&text[p]!='\n')p=ted_next(text,p,tlen);
        }
        caret=p;if(!(mods&EV_MOD_SHIFT))anchor=p;follow_caret=1;return 1;
    }
    if(a=='\b'||a==127){
        if(caret==anchor){if(a=='\b')anchor=ted_prev(text,caret);else anchor=ted_next(text,caret,tlen);}
        if(caret==anchor)return 0;return replace_selection("",0);
    }
    if(a=='\r')a='\n';
    if(a=='\n'||a=='\t'||(a>=32&&a<=0x10ffff&&!(a>=0xd800&&a<=0xdfff))){
        char b[4];int n=te_utf8_encode((unsigned)a,b);return replace_selection(b,n);}
    return 0;
}

static void itoa_(int v, char *b)
{
    char t[16]; int n = 0, p = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

/* Parses a bare non-negative int, or returns -1 for anything that is not one
 * -- empty, a stray letter, a sign. -1 (rather than 0, a real width nobody
 * wants) is what tells load_geometry() to fall back to the default instead of
 * opening a window an out-of-range or hand-edited settings file asked for. */
static int atoi_or_neg(const char *s)
{
    if (!s || !s[0]) return -1;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        int d = s[i] - '0';
        /* Overflow is a REFUSAL here, not silent wraparound -- a settings
         * file holding "app.textedit.w = 99999999999999999999" (a torn write,
         * a hand edit, or a value some future caller wrote for a wider field)
         * must not carry `v` past INT_MAX as undefined behaviour and hope
         * clampi() downstream happens to net it out. That was the bug: it
         * compiled, it usually even looked fine (clampi's <=/>= still hold for
         * whatever an optimiser's chosen wrapped value happens to be), and
         * UBSan still flagged a real signed-overflow at this line. The
         * kernel's own number parser (parse_long, c/kernel/core/settings.c)
         * already refuses overflow instead of wrapping it -- this is the same
         * rule, checked before the multiply so it can never itself overflow. */
        if (v > (2147483647 - d) / 10) return -1;
        v = v * 10 + d;
    }
    return v;
}

/* Reads app.textedit.{w,h}; falls back to the default whenever a value is
 * missing, unparseable, or out of a sane range for the CURRENT screen -- a
 * garbage or stale (different-resolution) settings file must never open a
 * window off-screen or too small to use. */
static void load_geometry(int *w, int *h)
{
    char buf[LOGIT_SET_VALMAX];
    int sw = screen_w(), sh = screen_h();
    int gw = TE_W_DEFAULT, gh = TE_H_DEFAULT;

    if (setting_get("app.textedit.w", buf, (int)sizeof buf) > 0) {
        int v = atoi_or_neg(buf);
        if (v >= 900) gw = v;
    }
    if (setting_get("app.textedit.h", buf, (int)sizeof buf) > 0) {
        int v = atoi_or_neg(buf);
        if (v >= 640) gh = v;
    }
    *w = sw > 0 ? clampi(gw, TE_W_MIN, sw-40) : gw;
    *h = sh > 0 ? clampi(gh, TE_H_MIN, sh-140) : gh;
}

/* ---- Wrapping and the caret: ONE walk, in CODE POINTS and REAL ADVANCES ----
 *
 * What stood here counted BYTES and multiplied by the width of "M":
 *
 *     for (int i = 0; i < tlen; i++)
 *         if (text[i] == '\n')      { line++; col = 0; }
 *         else if (col + 1 >= cols) { line++; col = 1; }
 *         else                      { col++; }
 *     ...  caret x = col * text_measure_px("M", 1, px, 1)
 *
 * and that is ONE bug with two symptoms, which is why both are fixed here and
 * not in two places: `col` is the caret AND it is the wrap. 你好 is six UTF-8
 * bytes and two glyphs, so the caret advanced six cells for two characters and
 * the wrap budget was spent three times too fast -- and because the break was
 * taken at a byte index it could land between a lead byte and its continuations
 * and split one character across two display lines.
 *
 * The three were watched failing together before anything was changed, against
 * a host stub of SYS_TEXT_MEASURE with an 8 px 'M' and a 15 px Han glyph:
 *
 *   你好                     caret x = 48, the glyphs end at 30   -> an 18 px gap
 *   20 Han, 150 px column    wrapped into 4 lines; 10 per line fit -> 2
 *   the same buffer          2 breaks landed inside a UTF-8 sequence
 *
 * The exact pixel numbers belong to that stub -- the real ones are whatever
 * ui.ttf and mono.ttf give and only the device can say -- but the shape does
 * not depend on the ratio, and neither does the third row.
 *
 * This is the third instance of the trap aui.c:1459 records: the pinyin IME
 * commits CJK, and every piece of arithmetic in a toolkit that predates it
 * still counts in bytes.
 *
 * THE CELL WAS NEVER RIGHT EITHER, not even for the font it named. The run is
 * drawn with mono=1, which selects mono.ttf -- "printable ASCII plus NBSP, no
 * CJK at all" -- so every CJK code point falls through tl_fonts()' preference
 * order in c/kernel/gui/text.c to ui.ttf and is drawn at THAT font's advance.
 * text_measure_px("M", ...) cannot see that; nothing that measures one
 * character and multiplies can. The file header above already records the same
 * mistake in its first form ("the monospace advance was assumed to be 8 pixels
 * regardless of the font or the backing scale"), so this file has been bitten
 * here before and the lesson to keep is the general one: no cell arithmetic.
 *
 * So every width below is text_measure_px() over the ACTUAL prefix, with
 * mono=1 -- the same length-delimited run, the same font-preference order and
 * the same shaping the draw will use. That is the invariant text.c states for
 * itself: "text_measure and text_draw_run must agree at the same px ... a
 * separate measuring path would drift a few pixels per line in a way nobody
 * could reproduce, so there is not one." te_walk() is that single path on this
 * side: it measures and draws in one function behind a `draw` flag, exactly
 * like layout()'s, so the wrap the caret is placed against cannot be a
 * different wrap from the one on screen.
 *
 * COST, measured rather than estimated, and it is not flat. te_fit() is ONE
 * text_measure_px for a line that fits and a bisection for one that does not,
 * and draw() runs the walk TWICE -- the scroll depends on the caret's line, the
 * caret is at the end of the buffer, so the whole text is walked before the
 * first glyph can be placed. Counted on the host against a stub of
 * SYS_TEXT_MEASURE, at the full MAXT buffer and a 500 px text column:
 *
 *   7,900 B, a newline every 60 chars   131 lines    130 calls/walk    260/repaint
 *   7,900 B, no newlines at all         128 lines  1,612 calls/walk  3,224/repaint
 *
 * The first row is the ordinary case and is one call per line. The second is
 * 12.6 per line because te_fit's upper bound is the whole LOGICAL line, so
 * every break bisects 7,900 bytes rather than the ~60 it will land in. Both are
 * O(text), not O(visible), which is inherent to a caret that lives at the end.
 * If either ever shows up in a profile, the fix is a line-start cache keyed on
 * (tlen, avail, px) -- not a second, cheaper wrap, which is the mistake this
 * whole comment is about. */

/* Bytes in the UTF-8 sequence starting at `i`: at least 1, never past `limit`,
 * never more than the 4 a legal sequence can occupy.
 *
 * Counted from CONTINUATION bytes rather than decoded out of the lead byte on
 * purpose -- a truncated or malformed sequence still advances by one, so no
 * input can stall the walk and no break can be taken inside a sequence. The
 * cap at 4 is not decoration: this is te_fit's floor, the one place a line is
 * allowed to be wider than the window, and without it a file of 2,000 bare
 * continuation bytes glues into a single 2,000-byte "code point" that te_fits
 * never gets to refuse -- straight into gui_text_run's silent clamp at 1023.
 * Four is the largest a real sequence can be, so the cap can only ever bind on
 * input that was already malformed. */
static int te_cp_len(int i, int limit)
{
    int n = 1;
    while (n < 4 && i + n < limit && te_is_cont(text[i + n])) n++;
    return n;
}

/* Largest UTF-8 boundary at or below `n` bytes past `off`. */
static int te_bound(int off, int n)
{
    while (n > 0 && te_is_cont(text[off + n])) n--;
    return n;
}

/* Do the first `n` bytes at `off` fit in `avail` device px?
 *
 * A width of 0 for a NON-EMPTY run is a REFUSAL, not a measurement of zero, and
 * reading it as "fits" is how this fix would have shipped its own silent
 * truncation. wm.c's SYS_TEXT_MEASURE answers 0 when len exceeds USER_TEXT_MAX
 * (1024) and when px is out of range, and SYS_GUI_TEXT_RUN then clamps the draw
 * to 1023 bytes without telling anyone -- so a 2,000-byte line with no newline
 * in it would have "fitted", drawn its first 1023 bytes, and dropped the rest
 * with no symptom at all. Refusing here keeps that limit spelled ONCE, in the
 * kernel that owns it, instead of a 1024 copied into this file to disagree with
 * it later (one jar, two doors); and if the kernel's limit ever moves, this
 * side answers with a short line rather than a lost one. */
static int te_fits(int off, int n, int avail, int px)
{
    if (n <= 0) return 1;
    int w = text_measure_px(text + off, n, px, te_mono|te_weight);
    return w > 0 && w <= avail;
}

/* Bytes of [start, limit) that fit in `avail`: the whole run when it fits, else
 * the largest prefix that does -- always on a code point boundary, and never 0,
 * so a single code point wider than the entire line takes a line to itself
 * instead of looping forever. Bisection is sound because glyph advances are
 * non-negative, so a longer prefix is never narrower. */
static int te_fit(int start, int limit, int avail, int px)
{
    int n = limit - start;
    if (n <= 0) return 0;
    if (te_fits(start, n, avail, px)) return n;

    int lo = 0, hi = n;              /* lo fits and is aligned; hi is known not to */
    for (;;) {
        int mid = te_bound(start, lo + (hi - lo) / 2);
        if (mid <= lo) mid = lo + te_cp_len(start + lo, limit);  /* next boundary up */
        if (mid >= hi) break;                                    /* none strictly between */
        if (te_fits(start, mid, avail, px)) lo = mid; else hi = mid;
    }
    return lo > 0 ? lo : te_cp_len(start, limit);
}

/* One display line beginning at `start`. *drawlen is the byte range to draw (a
 * terminating '\n' is consumed, not drawn); *next is where the following
 * display line begins. Returns 1 if there IS a following line -- a '\n' was
 * consumed or the line was wrapped -- and 0 at the end of the buffer.
 *
 * Progress is guaranteed on the `more` path (te_fit's floor is one code point,
 * and the newline branch steps past the newline), which is what makes the caller
 * a `for(;;)` that cannot spin. */
/* The walk. Always measures; draws the visible lines when `draw` is set.
 * Reports the display-line count, the caret's line, and the caret's x offset
 * from the left edge of the text in px -- from the real advances of the prefix
 * it follows, not from a column index.
 *
 * The caret is at the END of the buffer: this app appends and backspaces and
 * te_apply_key drops every navigation key, so there is no caret to move. That
 * is what makes the last line the walk produces the caret's line, and its whole
 * drawn extent the caret's prefix -- one measurement of exactly the run that
 * was handed to gui_text_run, so the two cannot disagree by a kerning pair. */
/* ---- what changed this frame, in terms ONLY this file can compute ----
 *
 * aui.c's own generic per-primitive diff (aui.c section 5a-flush) already
 * tracks every call this file makes THROUGH aui.c -- aui_round (the page
 * surface), aui_fill (the cursor, the status strip), aui_hairline,
 * aui_text_ellipsis, aui_text_sz -- automatically and correctly, because
 * every one of those bottoms out to a gui_rect/gui_blit/gui_text_run call
 * aui.c's own macros already intercept. The ONE thing that mechanism cannot
 * see is te_walk()'s raw gui_text_run() calls for the paragraph text below:
 * that call is textually inside THIS translation unit (this file includes
 * aui.h, which includes logit.h, and calls logit.h's gui_text_run directly),
 * where aui.c's interception macros are simply not in scope -- they are
 * #define'd inside aui.c and apply only to that file's own text. te_prev_*
 * below is what lets this file compute that ONE gap itself and hand it to
 * aui_end_rect() as an extra hint, unioned with whatever aui's own tracking
 * already found -- never instead of it. */
/* Raw text draws and tail-only dirty ranges used to live here. The work view
 * uses aui_text_n for every visible run: its shared invalidation also erases
 * middle-of-document edits, selections, review highlights and disappearing rows. */
#include "textedit_work.inc"

void app_main(void)
{
    text=malloc(MAXT+1);
    if(!text)app_exit(1);
    text[0]=0;
    int n = get_arg(fname, sizeof fname);
    if (n <= 0) {
        const char *d = "/untitled.txt";
        int i = 0; while (d[i]) { fname[i] = d[i]; i++; } fname[i] = 0;
    }
    int w, h;
    load_geometry(&w, &h);
    gui_create(fname, w, h);
    aui_set_size(w, h);

    if(fname[0]!='/'){char absolute[AG_PATH];absolute[0]='/';
        size_t len=strlen(fname);if(len+2>sizeof absolute)app_exit(1);
        memcpy(absolute+1,fname,len+1);memcpy(fname,absolute,len+2);}
    struct stat st;
    if(stat(fname,&st)==0&&(uint64_t)st.st_size>MAXT){load_refused=1;agent_notice="Document exceeds 1 MiB; file not opened";}
    else {
        struct ag_message m={.type=AG_DOCUMENT,.bytes=(uint32_t)strlen(fname)};void *doc=0;
        int r=ag_call(&m,fname,&doc);
        if(!r&&m.bytes<=MAXT){if(m.bytes)memcpy(text,doc,m.bytes);tlen=(int)m.bytes;agent_task=m.task;agent_revision=m.revision;}
        else {r=load_file();if(r<0){load_refused=1;agent_notice=r==-2?
            "Document exceeds 1 MiB; file not opened":"Document read failed; file not opened";}}
        free(doc);text[tlen]=0;
        if(!ag_utf8(text,(uint32_t)tlen)){tlen=0;text[0]=0;load_refused=1;agent_notice="Document is not valid UTF-8; file not opened";}
    }
    saved = 1;caret=anchor=tlen;editor_focus=tlen==0;follow_caret=tlen==0;work_refresh(1);
    draw();

    for (;;) {
        struct logit_event e;
        int changed = agent_refresh();
        while (poll_event(&e)) {
            if(e.type==EV_MOUSE||e.type==EV_MOUSE_UP||e.type==EV_MOUSE_MOVE){
                if(e.type==EV_MOUSE&&work_citation_click(e.a,e.b)){aui_feed_done();draw();continue;}
                if(e.type==EV_MOUSE&&work_body_hit(e.a,e.b)){
                    editor_focus=1;view_mode=0;aui_set_focus(-1);work_place_caret(e.a,e.b,e.mods);follow_caret=1;
                }else if(e.type==EV_MOUSE)editor_focus=0;
                aui_feed(&e);draw();aui_feed_done();continue;}
            if (e.type == EV_CLOSE) {
                agent_sync();
                if(agent_task&&agent_dirty){agent_notice="Keep this window open: edits are not synchronized";changed=1;continue;}
                /* Commit here, not per-resize -- see geom_dirty's comment
                 * above. A session that never touched the window size never
                 * touches the disk for this at all. */
                if (geom_dirty) setting_commit();
                app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                aui_set_size(e.a, e.b); changed = 1;
                char b[16];
                itoa_(e.a, b); setting_set("app.textedit.w", b, 0);
                itoa_(e.b, b); setting_set("app.textedit.h", b, 0);
                geom_dirty = 1;
            }
            if (e.type == EV_THEME)  changed = 1;
            if (e.type == EV_WHEEL)  { scroll += e.wheel*40; if (scroll < 0) scroll = 0;follow_caret=0;changed=1; }
            if (e.type == EV_KEY) {
                if(e.a==12){ask_logit();changed=1;continue;}
                if(!editor_focus){aui_feed(&e);draw();aui_feed_done();continue;}
                if(e.a==18&&agent_task&&!agent_dirty){
                    struct ag_message m={.type=AG_DOCUMENT,.task=agent_task};void *doc=0;
                    if(!ag_call(&m,0,&doc)&&m.bytes<=MAXT){if(m.bytes)memcpy(text,doc,m.bytes);
                        tlen=(int)m.bytes;text[tlen]=0;agent_revision=m.revision;agent_dirty=0;agent_notice="已载入任务文档";caret=anchor=tlen;edit_epoch++;changed=1;}
                    free(doc);continue;}
                if(load_refused||source_open||view_mode)continue;
                int previous=tlen;
                int wrote = 0;
                if (te_apply_key(e.a,e.mods, &wrote)) changed = 1;
                if(previous!=tlen){agent_dirty=agent_task!=0;agent_notice=0;}
                else if(e.a>=32&&(unsigned)tlen>=MAXT-3)agent_notice="Document limit: 1 MiB";
                if (wrote) { if (save_document() >= 0) saved = 1; changed = 1; }
            }
        }
        /* Coalesce typing into a checkpoint, but submit/save/close still call
         * agent_sync synchronously. One key no longer rewrites a 1 MiB snapshot. */
        if(agent_dirty&&monotonic_ms()>=sync_after){agent_sync();changed=1;sync_after=monotonic_ms()+1500;}
        if (changed) draw();
        wait_idle(agent_task?250:0);   /* was sys_yield(): a spin. input-driven; the caret blink is drawn from get_time */
    }
}

int main(void)
{
    struct aex_agent_identity identity;
    if(ag_self(&identity)<0)return 1;
    if(identity.mode==AEX_ACT_WORKER)return ag_worker(AG_EDITOR);
    app_main();return 0;
}
