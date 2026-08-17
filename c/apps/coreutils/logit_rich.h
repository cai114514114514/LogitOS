#ifndef LOGIT_RICH_H
#define LOGIT_RICH_H

/* ===========================================================================
 * LRT/1 -- the LogitOS rich-terminal protocol.
 *
 * WHY THIS IS NOT ESCAPE SEQUENCES
 * --------------------------------
 * A Unix terminal receives one byte stream and has to find structure inside it,
 * because nobody can change what `ls` emits. That is a constraint of the 1970s
 * deployment model, not a law: here the shell, every coreutil and the terminal
 * are all ours, so we get to pick the wire.
 *
 * We pick a SIDE BAND: plain text stays on fd 1 (and fd 2) exactly as it is
 * today, and structured/rich content travels as length-prefixed binary frames on
 * a SEPARATE pipe. Four reasons, all specific to this system rather than to
 * taste:
 *
 *  1. Redirection stops being a bug. `cmd > file` and `cmd | wc` are the cases
 *     every in-band scheme gets wrong -- iTerm2 and Kitty both put their image
 *     bytes into your file unless the program remembers to call isatty(), and
 *     isatty() is a lie under `tee`, `script` and tmux. Here the shell -- the
 *     ONLY component that knows the pipeline topology -- hands the rich fd to a
 *     stage only when that stage's stdout is the terminal itself. A redirected
 *     stage does not have the channel, so the file cannot contain protocol bytes
 *     even if the program is buggy. The property is structural, not a check.
 *
 *  2. A corrupt rich stream cannot corrupt the screen. Text and frames are
 *     parsed by two parsers reading two pipes. In band, a truncated escape eats
 *     the text that follows it -- the classic "my prompt is magenta now" -- and
 *     the damage is unbounded. Here the worst case is a picture that does not
 *     appear.
 *
 *  3. Binary payloads need no escaping. Frames are length-prefixed, and nothing
 *     downstream is scanning for a terminator, so there is no base64 tax and no
 *     "what if the payload contains the terminator" question.
 *
 *  4. The channel is bidirectional in a way an escape stream is not. The
 *     terminal->shell direction (RT_C_*) carries the interrupt key and the
 *     window geometry as typed frames, instead of overloading control
 *     characters on the shell's stdin -- which matters because stdin belongs to
 *     whatever child is running, not to the shell.
 *
 * The price, paid honestly: two streams have no intrinsic order. Every frame
 * therefore carries `seq`, the producer's own fd-1 byte count at the moment it
 * was emitted, and the terminal defers a frame until it has consumed that many
 * bytes of the command's output. See the ORDERING note below for the exact
 * guarantee and its one documented imprecision.
 *
 * THE THREE QUESTIONS
 * -------------------
 * - Output redirected to a file, or piped onward? The producer never sees the
 *   rich channel (the shell withheld it), rt_isrich() is 0, and the program
 *   takes its plain-text path. The file gets bytes from fd 1 only.
 * - A rich-aware program under a terminal that does not speak LRT? Nothing sets
 *   LOGIT_RICH in the environment, rt_isrich() is 0, same plain path. And if the
 *   channel dies mid-run (write returns < 0), rich mode LATCHES OFF for the rest
 *   of the process rather than half-emitting frames.
 * - How does a program discover which it is talking to? The environment, read
 *   through the SysV envp that execve already builds -- LOGIT_TERM names the
 *   protocol and version, LOGIT_RICH gives the fd. No new syscall exists or is
 *   needed; this whole protocol is built out of pipes, fork/exec and fds.
 *
 * ORDERING
 * --------
 * Within one command: text the producer wrote through rt_out() before emitting a
 * frame is always placed before that frame. The terminal establishes a byte
 * baseline when it sees RT_T_CMD_BEGIN and holds each frame until the command's
 * output has caught up to the frame's `seq`.
 * The one imprecision: if a child's first output overtakes CMD_BEGIN in the
 * terminal's read loop, the baseline is too high and the block renders LATER
 * than intended. It can never render early, and it can never corrupt text.
 *
 * WIRE FORMAT (little-endian, 16-byte header, no padding anywhere)
 * ---------------------------------------------------------------
 *   0  'L' 'R' 'T' 0x01     magic + protocol version
 *   4  u16 type             RT_T_* (producer->terminal) or RT_C_* (terminal->shell)
 *   6  u16 flags            reserved, must be 0
 *   8  u32 seq              producer's fd-1 byte count when the frame was emitted
 *  12  u32 len              payload length
 *  16  payload[len]
 *
 * Strings inside a payload are u16 length + bytes, never NUL-terminated: a
 * length-prefixed string cannot be truncated into a different valid string.
 *
 * This header is pure: no syscalls, no libc. Define RT_NO_SYS to get only the
 * format + parser (that is what the host unit test compiles).
 * =========================================================================== */

#define RT_MAGIC0 'L'
#define RT_MAGIC1 'R'
#define RT_MAGIC2 'T'
#define RT_VERSION 1

#define RT_HDR         16
#define RT_MAX_PAYLOAD 16384
#define RT_BUFSZ       (RT_MAX_PAYLOAD + 2 * RT_HDR)

/* ---- producer -> terminal ------------------------------------------------ */
#define RT_T_HELLO      1  /* str name                                          */
#define RT_T_CMD_BEGIN  2  /* u32 id, str cmdline          (from the shell)      */
#define RT_T_CMD_END    3  /* u32 id, u32 status, u8 flags (from the shell)      */
#define RT_T_INPUT      4  /* u16 cursor, str prompt, str buffer (live edit line)*/
#define RT_T_IMAGE      5  /* u8 kind, u16 w_pt, u16 h_pt, str path              */
#define RT_T_PROGRESS   6  /* u32 id, u16 permille, u8 done, str label           */
#define RT_T_TABLE      7  /* u16 ncols, u16 nrows, str title,                    */
                           /*   ncols * str header,                              */
                           /*   nrows*ncols * (u8 cellkind, str text, str target) */
#define RT_T_CHART      8  /* u16 n, u16 kind, str title, n*(u32 value, str label)*/
#define RT_T_VIDEO      9  /* u8 kind, u16 w_pt, u16 h_pt, u16 flags, str path   */
#define RT_T_CLEAR     10  /* no payload: discard the scrollback                 */
/* 11 is deliberately unassigned. An unknown type is ignored by the terminal (see
 * the default case in handle_frame), so the protocol grows without a version
 * bump -- but only types that EXIST are listed here.
 *
 * RT_T_CLEAR is what `clear` is, on this system. The Unix answer is two escape
 * bytes (ESC [ 2 J), and this terminal deliberately has no escape parser: a
 * character grid that interprets its own input is the in-band control language
 * this protocol exists to refuse, and the demand survey found exactly ONE
 * program in the whole tree emitting escapes -- c/apps/coreutils/clear.c. A VT
 * parser for one caller would be a second, weaker copy of the side band. So
 * `clear` says what it means on the channel that carries meaning, and keeps the
 * escape bytes for fd 1 when there is no channel (the serial console, which
 * really is a VT). */

/* ---- terminal -> shell (control channel) --------------------------------- */
#define RT_C_INTR      64  /* ^C: abandon the foreground job                     */
#define RT_C_SIZE      65  /* u16 cols, u16 rows, u16 cell_pt                    */
#define RT_C_RERUN     66  /* str cmdline: run this again                        */
#define RT_C_KEY       67  /* u32 keycode (KEY_* from logit_abi.h)               */
#define RT_C_EOF       68  /* ^D: close the running job's stdin                  */
#define RT_C_TEXT      69  /* str: literal typed characters (batched)            */

/* RT_T_IMAGE kinds */
#define RT_IMG_PATH 0      /* payload names an ABSOLUTE path the terminal decodes */

/* RT_T_VIDEO kinds and flags.
 *
 * A video travels BY PATH for the same reason an image does, and it matters
 * more here: there is no RGBA-over-the-wire path in this protocol, and a single
 * 320x240 frame is 300 KB against a 16 KiB maximum payload. Sending 30 of those
 * a second down a pipe would be the wrong design even if the payload limit did
 * not exist -- it would copy every pixel twice and serialize the decoder behind
 * a pipe writer.
 *
 * So the frame says WHICH FILE, and the terminal decodes it. That is also why
 * this is a type of its own rather than an image frame per decoded picture: a
 * video frame OWNS A REGION of the scrollback and updates in place, where a
 * picture-per-image-frame would append a new scrollback line 30 times a second
 * (and repaint the whole window each time). The numbers behind that choice are
 * in the header of the video section in c/apps/gui/terminal.c. */
#define RT_VID_PATH  0     /* payload names an ABSOLUTE path the terminal decodes */
#define RT_VID_LOOP  1     /* flags: restart at the end instead of holding the
                            * last frame                                        */

/* RT_T_TABLE cell kinds */
#define RT_CELL_TEXT 0
#define RT_CELL_LINK 1     /* clicking it opens `target` with its associated app  */
#define RT_CELL_NUM  2     /* right-aligned                                       */

/* RT_T_CMD_END flags */
#define RT_END_INTERRUPTED 1

/* ---------------------------------------------------------------- encode -- */

struct rt_enc { unsigned char b[RT_MAX_PAYLOAD]; int n; int ovf; };

static inline void rt_reset(struct rt_enc *e) { e->n = 0; e->ovf = 0; }

static inline void rt_raw(struct rt_enc *e, const void *p, int n)
{
    const unsigned char *s = (const unsigned char *)p;
    if (n < 0 || e->n + n > RT_MAX_PAYLOAD) { e->ovf = 1; return; }
    for (int i = 0; i < n; i++) e->b[e->n + i] = s[i];
    e->n += n;
}
static inline void rt_u8(struct rt_enc *e, int v)
{ unsigned char c = (unsigned char)v; rt_raw(e, &c, 1); }
static inline void rt_u16(struct rt_enc *e, int v)
{ unsigned char c[2]; c[0] = (unsigned char)(v & 0xFF); c[1] = (unsigned char)((v >> 8) & 0xFF); rt_raw(e, c, 2); }
static inline void rt_u32(struct rt_enc *e, unsigned v)
{ unsigned char c[4]; c[0] = (unsigned char)(v); c[1] = (unsigned char)(v >> 8);
  c[2] = (unsigned char)(v >> 16); c[3] = (unsigned char)(v >> 24); rt_raw(e, c, 4); }

static inline int rt_slen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static inline void rt_str(struct rt_enc *e, const char *s)
{
    int n = rt_slen(s);
    if (n > 0xFFFF) n = 0xFFFF;
    rt_u16(e, n);
    rt_raw(e, s, n);
}
static inline void rt_strn(struct rt_enc *e, const char *s, int n)
{
    if (n < 0) n = 0;
    if (n > 0xFFFF) n = 0xFFFF;
    rt_u16(e, n);
    rt_raw(e, s, n);
}

/* Serialize a header into `out` (>= RT_HDR bytes). Split from the fd write so
 * the host test can build frames without a kernel underneath it. */
static inline void rt_hdr(unsigned char *out, int type, unsigned seq, unsigned len)
{
    out[0] = RT_MAGIC0; out[1] = RT_MAGIC1; out[2] = RT_MAGIC2; out[3] = RT_VERSION;
    out[4] = (unsigned char)(type & 0xFF);  out[5] = (unsigned char)((type >> 8) & 0xFF);
    out[6] = 0; out[7] = 0;
    out[8]  = (unsigned char)(seq);        out[9]  = (unsigned char)(seq >> 8);
    out[10] = (unsigned char)(seq >> 16);  out[11] = (unsigned char)(seq >> 24);
    out[12] = (unsigned char)(len);        out[13] = (unsigned char)(len >> 8);
    out[14] = (unsigned char)(len >> 16);  out[15] = (unsigned char)(len >> 24);
}

/* ---------------------------------------------------------------- decode -- */

struct rt_frame {
    int type, flags;
    unsigned seq;
    const unsigned char *p;   /* payload, inside the parser's buffer */
    int len;
};

/* A byte-stream parser that resynchronizes. Garbage between frames is skipped
 * to the next magic and counted; an impossible length is treated as garbage
 * rather than trusted (trusting it is how a parser hangs forever waiting for
 * bytes that will never come). */
struct rt_parser {
    unsigned char b[RT_BUFSZ];
    int  n;
    long skipped;     /* bytes discarded during resync                  */
    long badlen;      /* frames rejected because len > RT_MAX_PAYLOAD    */
};

static inline void rt_parser_init(struct rt_parser *p) { p->n = 0; p->skipped = 0; p->badlen = 0; }

/* Append bytes; returns how many were accepted (the rest must be re-offered
 * after rt_parser_next() has drained a frame). */
static inline int rt_parser_feed(struct rt_parser *p, const void *data, int len)
{
    const unsigned char *s = (const unsigned char *)data;
    int room = RT_BUFSZ - p->n;
    int n = len < room ? len : room;
    for (int i = 0; i < n; i++) p->b[p->n + i] = s[i];
    p->n += n;
    return n;
}

static inline unsigned rt_rd32(const unsigned char *b)
{ return (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24); }
static inline int rt_rd16(const unsigned char *b)
{ return (int)b[0] | ((int)b[1] << 8); }

static inline void rt_drop(struct rt_parser *p, int k)
{
    if (k <= 0) return;
    if (k > p->n) k = p->n;
    for (int i = k; i < p->n; i++) p->b[i - k] = p->b[i];
    p->n -= k;
}

/* 1 = a frame is in *out (valid until the next call), 0 = need more bytes. */
static inline int rt_parser_next(struct rt_parser *p, struct rt_frame *out)
{
    for (;;) {
        /* resync: find the magic */
        int i = 0;
        while (i + 4 <= p->n &&
               !(p->b[i] == RT_MAGIC0 && p->b[i + 1] == RT_MAGIC1 &&
                 p->b[i + 2] == RT_MAGIC2 && p->b[i + 3] == RT_VERSION))
            i++;
        if (i + 4 > p->n) {
            /* No magic in view. Keep the last 3 bytes: a magic can straddle. */
            int keep = p->n < 3 ? p->n : 3;
            p->skipped += p->n - keep;
            rt_drop(p, p->n - keep);
            return 0;
        }
        if (i) { p->skipped += i; rt_drop(p, i); }

        if (p->n < RT_HDR) return 0;                    /* header still arriving */
        unsigned len = rt_rd32(p->b + 12);
        if (len > (unsigned)RT_MAX_PAYLOAD) {           /* cannot be one of ours */
            p->badlen++;
            p->skipped += 1;
            rt_drop(p, 1);                              /* step past and resync   */
            continue;
        }
        if ((unsigned)p->n < RT_HDR + len) return 0;    /* payload still arriving */

        out->type  = rt_rd16(p->b + 4);
        out->flags = rt_rd16(p->b + 6);
        out->seq   = rt_rd32(p->b + 8);
        out->len   = (int)len;
        /* The payload is handed back as a pointer INTO the parser buffer -- a
         * copy would cost a second RT_MAX_PAYLOAD buffer for nothing. The frame
         * therefore stays at the head of the buffer until the caller says it is
         * finished with it; that is what rt_parser_done() is for, and calling
         * next() twice without it simply returns the same frame again. */
        out->p = p->b + RT_HDR;
        return 1;
    }
}

/* Release the frame returned by the last rt_parser_next(). Kept separate so the
 * payload pointer stays valid for the whole time the caller is using it. */
static inline void rt_parser_done(struct rt_parser *p, const struct rt_frame *f)
{ rt_drop(p, RT_HDR + f->len); }

/* Payload readers. Every one is bounds-checked against the frame length and
 * returns a default on underflow, so a truncated-but-well-framed payload can
 * only produce an empty/zero field -- never a read past the buffer. */
struct rt_rd { const unsigned char *p; int n, off, bad; };

static inline void rt_rd_init(struct rt_rd *r, const struct rt_frame *f)
{ r->p = f->p; r->n = f->len; r->off = 0; r->bad = 0; }

static inline int rt_rd_u8(struct rt_rd *r)
{ if (r->off + 1 > r->n) { r->bad = 1; return 0; } return r->p[r->off++]; }
static inline int rt_rd_u16(struct rt_rd *r)
{ if (r->off + 2 > r->n) { r->bad = 1; return 0; } int v = rt_rd16(r->p + r->off); r->off += 2; return v; }
static inline unsigned rt_rd_u32(struct rt_rd *r)
{ if (r->off + 4 > r->n) { r->bad = 1; return 0; } unsigned v = rt_rd32(r->p + r->off); r->off += 4; return v; }

/* Copy a length-prefixed string into `dst` (always NUL-terminated). Returns the
 * number of bytes stored (which may be less than the wire length if dst is
 * smaller -- truncation, never overflow). */
static inline int rt_rd_str(struct rt_rd *r, char *dst, int max)
{
    int len = rt_rd_u16(r);
    if (r->bad || r->off + len > r->n) { r->bad = 1; if (max > 0) dst[0] = 0; return 0; }
    int k = len < max - 1 ? len : max - 1;
    if (k < 0) k = 0;
    for (int i = 0; i < k; i++) dst[i] = (char)r->p[r->off + i];
    if (max > 0) dst[k] = 0;
    r->off += len;
    return k;
}

/* ------------------------------------------------------- syscall helpers -- */
#ifndef RT_NO_SYS
#include "logit.h"

/* Per-process rich state. These live in whichever single translation unit
 * includes this header -- sh, a coreutil, or the terminal -- which is all any
 * of them has. */
static int      rt_fd    = -1;     /* the rich channel, or -1                */
static int      rt_ver   = 0;      /* protocol version the terminal offered  */
static unsigned rt_seq   = 0;      /* bytes this process has written to fd 1 */
static int      rt_cols_ = 80, rt_rows_ = 24, rt_cell_ = 10;

static inline int rt_isrich(void) { return rt_fd >= 0; }
static inline unsigned rt_tell(void) { return rt_seq; }
static inline int rt_cols(void) { return rt_cols_; }
static inline int rt_rows(void) { return rt_rows_; }

static inline int rt_atoi(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

static inline int rt_prefix(const char *s, const char *pfx)
{ int i = 0; while (pfx[i]) { if (s[i] != pfx[i]) return 0; i++; } return i; }

/* The SysV stack execve() builds is [argc][argv..][NULL][envp..][NULL], and
 * crt0_cli hands main() argc and argv -- so envp is simply argv[argc+1]. No new
 * syscall, no getenv() in clib: the environment was already there. */
static inline char **rt_envp(int argc, char **argv) { return &argv[argc + 1]; }

static inline const char *rt_getenv(int argc, char **argv, const char *name)
{
    char **e = rt_envp(argc, argv);
    if (!e) return 0;
    for (int i = 0; e[i]; i++) {
        int k = 0;
        while (name[k] && e[i][k] == name[k]) k++;
        if (!name[k] && e[i][k] == '=') return e[i] + k + 1;
    }
    return 0;
}

/* Discover the terminal. Call once from main(); returns 1 if the rich channel is
 * live. Everything else in this header degrades to plain text when it is not. */
static inline int rt_init(int argc, char **argv)
{
    const char *term = rt_getenv(argc, argv, "LOGIT_TERM");
    const char *fd   = rt_getenv(argc, argv, "LOGIT_RICH");
    const char *co   = rt_getenv(argc, argv, "LOGIT_COLS");
    const char *ro   = rt_getenv(argc, argv, "LOGIT_ROWS");
    const char *ce   = rt_getenv(argc, argv, "LOGIT_CELL");
    if (co) rt_cols_ = rt_atoi(co);
    if (ro) rt_rows_ = rt_atoi(ro);
    if (ce) rt_cell_ = rt_atoi(ce);
    if (rt_cols_ < 20) rt_cols_ = 20;
    if (rt_rows_ < 4) rt_rows_ = 4;
    if (!term || !fd) return 0;
    /* "logit-rich-<n>": refuse a version we do not implement rather than send
     * frames a future terminal would mis-parse. */
    int k = rt_prefix(term, "logit-rich-");
    if (!k) return 0;
    rt_ver = rt_atoi(term + k);
    if (rt_ver < 1) return 0;
    int n = rt_atoi(fd);
    if (n < 3) return 0;                 /* 0/1/2 are never the side band */
    rt_fd = n;
    return 1;
}

/* Explicitly attach a channel (the terminal uses this for its own control
 * writes; the shell uses it after building a child's environment). */
static inline void rt_attach(int fd) { rt_fd = fd; rt_ver = RT_VERSION; }

static inline int rt_write_all(int fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int done = 0;
    while (done < len) {
        int n = sys_write(fd, p + done, len - done);
        if (n == EAGAIN_RC) { sys_yield(); continue; }
        if (n <= 0) return -1;
        done += n;
    }
    return done;
}

/* Emit a frame. A failed write LATCHES rich mode off: half a frame on the wire
 * is exactly the state this protocol exists to make impossible, and a producer
 * that keeps trying after the terminal is gone would block forever on a full
 * pipe. */
static inline int rt_send(int type, struct rt_enc *e)
{
    if (rt_fd < 0) return -1;
    if (e && e->ovf) { rt_reset(e); return -1; }     /* never send a truncated payload */
    unsigned char h[RT_HDR];
    int len = e ? e->n : 0;
    rt_hdr(h, type, rt_seq, (unsigned)len);
    if (rt_write_all(rt_fd, h, RT_HDR) < 0 ||
        (len && rt_write_all(rt_fd, e->b, len) < 0)) {
        rt_fd = -1;
        return -1;
    }
    if (e) rt_reset(e);
    return 0;
}

/* Plain output that keeps the ordering anchor honest. A rich-aware program
 * should write its text through these; a program that does not know the
 * protocol writes to fd 1 directly and simply has no anchor. */
static inline int rt_out(const char *s)
{
    int n = rt_slen(s);
    if (n <= 0) return 0;
    int w = rt_write_all(1, s, n);
    if (w > 0) rt_seq += (unsigned)w;
    return w;
}
static inline int rt_outn(const char *s, int n)
{
    if (n <= 0) return 0;
    int w = rt_write_all(1, s, n);
    if (w > 0) rt_seq += (unsigned)w;
    return w;
}

#endif /* RT_NO_SYS */
#endif /* LOGIT_RICH_H */
