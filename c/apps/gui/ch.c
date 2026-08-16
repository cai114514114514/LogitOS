/* ch -- the chat window. An assistant reply arrives token by token and the
 * text grows in front of you.
 *
 * ============================================================================
 * WHY THIS IS A NATIVE APP AND NOT A PAGE IN THE BROWSER
 *
 * The obvious route was to open the vendor's web app. It does not work and it
 * cannot be made to: the site is behind an SMS login this machine has no way to
 * complete, and the bare origin serves an SEO shell rather than the chat client.
 * So the choice was between a fake and a program. The interesting part is that
 * a program costs almost nothing here, because every piece already exists:
 *
 *   TLS         is a flag on a socket -- sock_open(host, port, SOCK_F_TLS).
 *               The whole TLS 1.3 client, certificate chain and 130-root trust
 *               store live in the kernel behind that one bit.
 *   HTTP/1.1    is c/net/http/http1.c, already in ring 3. Its header explains
 *               that its body SINK exists precisely because "Server-Sent Events
 *               is an HTTP response that never ends" -- so incremental
 *               delivery and chunked de-framing are not this file's problem.
 *   the window  is aui, the immediate-mode toolkit every GUI app links.
 *
 * What did NOT exist is SSE record framing and a JSON reader, and that is the
 * only new wire code in this unit: c/apps/gui/ch_sse.c, deliberately separate
 * so it can be driven by a host test with no window, no socket and no kernel
 * (`make test-ch-host`, and its negative control `make test-ch-negctl`).
 *
 * ============================================================================
 * WHERE THE CREDENTIAL LIVES, AND WHY IT IS A FILE
 *
 * The endpoint, the model and the key are read from /etc/ai.conf.
 *
 * The alternative was the settings store (c/kernel/core/settings.c, the `app.`
 * namespace), and it was rejected for one concrete reason: that store is
 * ENUMERABLE. `setting_enum`/`setting_kv_at` walk every key/value pair on the
 * machine, the Settings window lists them, and `/bin/pref` prints them. A
 * credential put there is not merely readable, it is on a list -- it would be
 * printed by a program whose author never thought about secrets. A file has to
 * be named to be read.
 *
 * THE LIMIT, STATED RATHER THAN IMPLIED: this is a better place, not a safe
 * one. LogitOS has no per-file secrecy today -- vfs_meta's modes live in RAM
 * and do not survive a reboot (see the Storage note in CLAUDE.md), and any
 * process holding CAP_FS can read any path. So /etc/ai.conf is readable by
 * every program on this machine. The right fix is durable file modes plus a
 * capability that scopes a process to a path prefix; both are named in this
 * unit's `not_done` and neither is in this file's power.
 *
 * WHAT THIS PROGRAM WILL NEVER DO WITH THE KEY. It is read into one static
 * buffer, used to build exactly one header value, and never printed: not in a
 * status line, not in an error, not in a request dump (there is no request
 * dump), and not in the parse errors below -- which is why a malformed config
 * line is reported by LINE NUMBER and never by content. A key line is the
 * likeliest line to be malformed, and "malformed: key = sk-..." on the serial
 * console would be the leak.
 *
 * ============================================================================
 * HOW THE REPAINT COST IS BOUNDED -- the whole point is that text GROWS in
 * front of the user, so the naive answer (repaint on every delta) is the one
 * that is actually tempting, and it is wrong: a fast endpoint emits tokens
 * faster than this compositor can draw a window, and the reply then arrives
 * more slowly than the network delivered it.
 *
 *   1. TIME, not tokens. A delta marks the window dirty; the window is
 *      repainted at most once every CH_REPAINT_MS (40 ms = 25 fps). So the
 *      repaint count is bounded by the stream's DURATION and is independent of
 *      how many tokens it contained. The app prints both numbers on the serial
 *      console when the stream ends (CH_STREAM_END repaints=.. deltas=..), so
 *      the ratio is measured rather than asserted.
 *   2. WRAPPING IS INCREMENTAL. Appending to the transcript re-measures ONE
 *      word -- the one that just completed -- not the paragraph and not the
 *      transcript. A full re-wrap happens only on EV_RESIZE.
 *   3. ONLY VISIBLE LINES ARE DRAWN. The paint loop runs over the lines
 *      intersecting the viewport, so a 400-line transcript costs the same
 *      frame as a 4-line one.
 *   4. The stream is repainted UNCONDITIONALLY once when it ends, so the last
 *      token is never left waiting for a timer that has stopped ticking.
 *
 * ============================================================================
 * WHAT THIS PROGRAM DOES NOT DO (in the file, not in a ticket)
 *   - No system prompt, no temperature, no tool calls, no attachments.
 *   - No retry and no reconnection. A dropped stream is reported and the turn
 *     is left as it stands; Last-Event-ID resumption would need the endpoint to
 *     support it and would silently resume nothing if it did not.
 *   - One request in flight. Sending is refused while a stream is running.
 *   - The reply is plain text. No Markdown, so a table arrives as pipes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aui.h"
#include "http1.h"
#include "ch_sse.h"

/* http1.c's content-decoding path, stubbed exactly as /bin/h2check stubs it and
 * for the same reason: every request this program sends carries
 * `Accept-Encoding: identity`, so no response can reach the inflater. The stub
 * RETURNS AN ERROR rather than success -- if a server ignores the request
 * header and sends gzip anyway, http1.c reports H1_E_ENCODING and this window
 * says so, which is the honest outcome. Linking the real Rust inflater would
 * add ~40 KB to a binary for a path that must not be taken. */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }

/* ------------------------------------------------------------------ bounds */
#define CH_CONF_PATH      "/etc/ai.conf"
#define CH_CONF_MAX       4096
#define CH_HOST_MAX       128
#define CH_PATH_MAX       192
#define CH_MODEL_MAX      64
#define CH_KEY_MAX        512
#define CH_PROMPT_MAX     1024
#define CH_TR_MAX         32768      /* the whole conversation, as text        */
#define CH_MAXLINE        2048       /* wrapped display lines                  */
#define CH_MAXTURN        32
#define CH_BODY_MAX       24576      /* the JSON request body                  */
#define CH_ERRBODY_MAX    2048       /* a non-200 response, kept for its message */
#define CH_REPAINT_MS     40         /* >= 40 ms between stream-driven repaints */
#define CH_IDLE_MS        30000      /* no progress for this long -> give up   */
#define CH_STATUS_MAX     256

/* ------------------------------------------------------------------ config */
struct conf {
    char host[CH_HOST_MAX];
    char path[CH_PATH_MAX];
    char model[CH_MODEL_MAX];
    char key[CH_KEY_MAX];            /* never printed, never logged           */
    int  port;
    int  tls;
    int  have_host, have_model, have_key;
};
static struct conf g_conf;
static int  g_conf_ok;
static char g_conf_err[CH_STATUS_MAX];

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Copy `n` bytes and NUL-terminate; 0 if it did not fit. */
static int copy_bounded(char *dst, int cap, const char *src, int n)
{
    if (n < 0 || n >= cap) return 0;
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
    return 1;
}

static void conf_fail(int line, const char *why)
{
    /* LINE NUMBER AND CAUSE ONLY. Never the line's text -- see the header. */
    snprintf(g_conf_err, sizeof g_conf_err, "%s line %d: %s", CH_CONF_PATH, line, why);
}

/* /etc/ai.conf:  blank lines, `# comment` lines, and `name = value`.
 * Anything else is a refusal, named. An unknown name is a refusal too: a typo
 * that is quietly ignored is how a program ends up talking to the wrong host
 * with the user certain it was told otherwise. */
static int conf_load(void)
{
    static char buf[CH_CONF_MAX + 1];
    int n = read_file(CH_CONF_PATH, buf, CH_CONF_MAX);
    if (n < 0) {
        snprintf(g_conf_err, sizeof g_conf_err,
                 "%s is missing. Create it with host/port/path/tls/model/key.",
                 CH_CONF_PATH);
        return 0;
    }
    if (n >= CH_CONF_MAX) {
        snprintf(g_conf_err, sizeof g_conf_err,
                 "%s is larger than %d bytes", CH_CONF_PATH, CH_CONF_MAX);
        return 0;
    }
    buf[n] = 0;

    memset(&g_conf, 0, sizeof g_conf);
    g_conf.port = -1;
    g_conf.tls  = -1;
    snprintf(g_conf.path, sizeof g_conf.path, "%s", "/v1/chat/completions");

    int line = 0, i = 0;
    while (i <= n) {
        int s = i;
        while (i < n && buf[i] != '\n') i++;
        int e = i;
        if (e > s && buf[e - 1] == '\r') e--;
        i++;
        line++;
        while (s < e && (buf[s] == ' ' || buf[s] == '\t')) s++;
        while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;
        if (s >= e) { if (s >= n) break; continue; }        /* blank */
        if (buf[s] == '#') continue;                        /* comment */

        int eq = s;
        while (eq < e && buf[eq] != '=') eq++;
        if (eq >= e) { conf_fail(line, "not a `name = value` line"); return 0; }
        int ne = eq;
        while (ne > s && (buf[ne - 1] == ' ' || buf[ne - 1] == '\t')) ne--;
        int vs = eq + 1;
        while (vs < e && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;

        char name[32];
        if (!copy_bounded(name, sizeof name, buf + s, ne - s)) {
            conf_fail(line, "the setting name is too long");
            return 0;
        }
        const char *v = buf + vs;
        int vl = e - vs;

        if (str_eq(name, "host")) {
            if (!copy_bounded(g_conf.host, sizeof g_conf.host, v, vl))
                { conf_fail(line, "host is empty or too long"); return 0; }
            g_conf.have_host = vl > 0;
        } else if (str_eq(name, "path")) {
            if (!vl || !copy_bounded(g_conf.path, sizeof g_conf.path, v, vl))
                { conf_fail(line, "path is empty or too long"); return 0; }
        } else if (str_eq(name, "model")) {
            if (!copy_bounded(g_conf.model, sizeof g_conf.model, v, vl))
                { conf_fail(line, "model is too long"); return 0; }
            g_conf.have_model = vl > 0;
        } else if (str_eq(name, "key")) {
            /* An EMPTY key is legal and means "send no Authorization header",
             * which is what a local endpoint wants. It is not the same as an
             * ABSENT `key` line: that is a refusal, because "I forgot to
             * configure the key" and "this endpoint needs none" must not look
             * alike. The window says which one is in force. */
            if (!copy_bounded(g_conf.key, sizeof g_conf.key, v, vl))
                { conf_fail(line, "key is too long"); return 0; }
            g_conf.have_key = 1;
        } else if (str_eq(name, "port")) {
            int p = 0, k = 0;
            for (; k < vl; k++) {
                if (v[k] < '0' || v[k] > '9') { conf_fail(line, "port is not a number"); return 0; }
                p = p * 10 + (v[k] - '0');
                if (p > 65535) { conf_fail(line, "port is out of range"); return 0; }
            }
            if (!k || p == 0) { conf_fail(line, "port is out of range"); return 0; }
            g_conf.port = p;
        } else if (str_eq(name, "tls")) {
            if (vl == 1 && v[0] == '0') g_conf.tls = 0;
            else if (vl == 1 && v[0] == '1') g_conf.tls = 1;
            else { conf_fail(line, "tls must be 0 or 1"); return 0; }
        } else {
            char q[64];
            snprintf(q, sizeof q, "unknown setting `%s`", name);
            conf_fail(line, q);
            return 0;
        }
    }

    if (!g_conf.have_host)  { snprintf(g_conf_err, sizeof g_conf_err,
        "%s has no `host` line", CH_CONF_PATH); return 0; }
    if (!g_conf.have_model) { snprintf(g_conf_err, sizeof g_conf_err,
        "%s has no `model` line", CH_CONF_PATH); return 0; }
    if (!g_conf.have_key)   { snprintf(g_conf_err, sizeof g_conf_err,
        "%s has no `key` line (use `key =` for an endpoint that needs none)",
        CH_CONF_PATH); return 0; }
    if (g_conf.tls < 0)     { snprintf(g_conf_err, sizeof g_conf_err,
        "%s has no `tls` line (0 or 1)", CH_CONF_PATH); return 0; }
    if (g_conf.port < 0) g_conf.port = g_conf.tls ? 443 : 80;
    return 1;
}

/* ------------------------------------------------------- transcript + wrap */

static char tr[CH_TR_MAX];
static int  tr_len;
static int  tr_full;                 /* the buffer filled: said out loud      */

enum { ROLE_USER = 0, ROLE_AI = 1, ROLE_SYS = 2 };
struct turn { int role, off, len; };
static struct turn turns[CH_MAXTURN];
static int nturn;
static int turns_dropped;            /* trimmed to fit a request: said out loud */

struct dline { int off, len, role, label; };
static struct dline lines[CH_MAXLINE];
static int nline;
static int lines_full;

static int wrap_maxw = 400;
static int wrap_line_start, wrap_line_w, wrap_word_start, wrap_role;
static int space_w;

#define CH_FS       AUI_FS_BODY
#define CH_LH       21

static void push_line(int off, int len, int role, int label)
{
    if (nline >= CH_MAXLINE) { lines_full = 1; return; }
    lines[nline].off = off; lines[nline].len = len;
    lines[nline].role = role; lines[nline].label = label;
    nline++;
}

static int mw(int off, int len)
{
    if (len <= 0) return 0;
    return text_measure_px(tr + off, len, CH_FS, 0);
}

/* One UTF-8 sequence's byte length at `i`, clamped to `end`. Only used by the
 * hard-break path, which is the one place this file walks characters. */
static int u8adv(int i, int end)
{
    unsigned char c = (unsigned char)tr[i];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
            (c & 0xF8) == 0xF0 ? 4 : 1;
    if (i + n > end) n = 1;
    return n;
}

/* Commit the pending word, which is tr[wrap_word_start .. end). THIS is the
 * function that keeps wrapping O(1) amortised: exactly one measurement of one
 * word, however long the transcript is. */
static void flush_word(int end)
{
    int wl = end - wrap_word_start;
    if (wl <= 0) { wrap_word_start = end; return; }
    int w = mw(wrap_word_start, wl);

    if (wrap_line_w > 0 && wrap_line_w + w > wrap_maxw) {
        push_line(wrap_line_start, wrap_word_start - wrap_line_start, wrap_role, 0);
        wrap_line_start = wrap_word_start;
        wrap_line_w = 0;
    }
    if (w > wrap_maxw && wrap_line_w == 0) {
        /* A single word wider than the column -- a URL, or a language with no
         * spaces. This is the ONLY path that measures per character, and it
         * runs at most once per such word. */
        int s = wrap_word_start, cw = 0;
        for (int i = wrap_word_start; i < end; ) {
            int adv = u8adv(i, end);
            int pw = mw(i, adv);
            if (cw + pw > wrap_maxw && i > s) { push_line(s, i - s, wrap_role, 0); s = i; cw = 0; }
            cw += pw; i += adv;
        }
        wrap_line_start = s;
        wrap_line_w = cw;
        wrap_word_start = end;
        return;
    }
    wrap_line_w += w;
    wrap_word_start = end;
}

static void wrap_begin_turn(int role, int off)
{
    push_line(0, 0, role, 1);           /* the role label line */
    wrap_role = role;
    wrap_line_start = wrap_word_start = off;
    wrap_line_w = 0;
}

static void wrap_bytes(int from, int to)
{
    for (int i = from; i < to; i++) {
        char c = tr[i];
        if (c == '\n') {
            flush_word(i);
            push_line(wrap_line_start, i - wrap_line_start, wrap_role, 0);
            wrap_line_start = wrap_word_start = i + 1;
            wrap_line_w = 0;
        } else if (c == ' ' || c == '\t') {
            flush_word(i);
            wrap_line_w += space_w;
            wrap_word_start = i + 1;
        }
    }
}

static void wrap_end_turn(int end)
{
    flush_word(end);
    push_line(wrap_line_start, end - wrap_line_start, wrap_role, 0);
    wrap_line_start = wrap_word_start = end;
    wrap_line_w = 0;
}

static int turn_open;                /* the last turn is still receiving text */

/* The whole transcript, from scratch. EV_RESIZE only -- never per delta.
 *
 * The LAST turn is the one to be careful with: it is left open only if it is
 * genuinely still streaming, because an open turn's tail is drawn as a live
 * partial line instead of a committed one. Leaving a FINISHED turn open here
 * (the first version did) loses its last line the moment the window is
 * resized -- the tail is drawn, so it still looks right, until the next turn
 * begins and overwrites the row. */
static void rewrap(void)
{
    nline = 0; lines_full = 0;
    for (int t = 0; t < nturn; t++) {
        wrap_begin_turn(turns[t].role, turns[t].off);
        wrap_bytes(turns[t].off, turns[t].off + turns[t].len);
        if (t < nturn - 1 || !turn_open)
            wrap_end_turn(turns[t].off + turns[t].len);
    }
}

/* Append to the open turn. Returns 0 and sets tr_full if the buffer is done. */
static int tr_append(const char *s, int n)
{
    if (n <= 0) return 1;
    if (tr_len + n > CH_TR_MAX) { tr_full = 1; return 0; }
    int from = tr_len;
    memcpy(tr + tr_len, s, (size_t)n);
    tr_len += n;
    turns[nturn - 1].len += n;
    wrap_bytes(from, tr_len);
    return 1;
}

static void turn_open_new(int role)
{
    if (nturn >= CH_MAXTURN) {
        /* Oldest-first, and counted. The transcript text stays where it is --
         * only the turn record goes -- so nothing already on screen moves. */
        for (int i = 1; i < nturn; i++) turns[i - 1] = turns[i];
        nturn--;
        turns_dropped++;
    }
    turns[nturn].role = role;
    turns[nturn].off = tr_len;
    turns[nturn].len = 0;
    nturn++;
    wrap_begin_turn(role, tr_len);
    turn_open = 1;
}

static void turn_close(void)
{
    if (!turn_open) return;
    wrap_end_turn(tr_len);
    turn_open = 0;
}

static void say(int role, const char *s)
{
    turn_close();
    turn_open_new(role);
    tr_append(s, (int)strlen(s));
    turn_close();
}

/* ---------------------------------------------------------------- the wire */

enum { ST_IDLE = 0, ST_DIAL, ST_STREAM, ST_ERROR };

static int  g_state;
static int  g_fd = -1;
static struct h1_conn g_conn;
static int  g_conn_live;
static struct sse g_sse;
static char g_status[CH_STATUS_MAX];
static char g_errbody[CH_ERRBODY_MAX];
static int  g_errbody_len;
static int  g_code;                  /* HTTP status of the current exchange   */
static int  g_chunked;               /* the response used chunked framing     */
static unsigned long long g_t0, g_last_progress;
static uint64_t g_progress;
static unsigned g_repaints, g_frames_skipped;
static unsigned g_fnv;
static int  g_dirty;
static unsigned long long g_last_paint;

static void status(const char *s) { snprintf(g_status, sizeof g_status, "%s", s); }

/* ---- h1 transport over the non-blocking socket ABI (browser_rt.c's shape) */
static int tr_read(void *ctx, void *buf, int len)
{
    (void)ctx;
    int n = sock_recv(g_fd, buf, len);
    if (n > 0) return n;
    int bits = sock_poll(g_fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return H1_TERR;
    if (n < 0 || (bits & SOCK_P_EOF)) return H1_EOF;   /* drain first, then EOF */
    return H1_AGAIN;
}
static int tr_write(void *ctx, const void *buf, int len)
{
    (void)ctx;
    int n = sock_send(g_fd, buf, len);
    if (n > 0) return n;
    if (n < 0) return H1_TERR;
    return H1_AGAIN;
}
static int tr_pollw(void *ctx, int want_write)
{
    (void)ctx;
    if (!want_write) return 1;
    int bits = sock_poll(g_fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return -1;
    return (bits & SOCK_P_WRITABLE) ? 1 : 0;
}

/* ---- the SSE callbacks ---- */
static void on_delta(void *ctx, const char *u, int n)
{
    (void)ctx;
    if (!tr_append(u, n)) {
        status("transcript full -- the reply was cut here");
        return;
    }
    /* FNV-1a over the reply as it is assembled. It is printed at the end of the
     * stream so a harness can compare it with the same hash of what the server
     * SENT -- which is what turns "some text appeared" into "the exact bytes
     * survived being cut into TCP segments, chunks and SSE records". Nothing in
     * the program uses it; it exists to be checked from outside. */
    for (int i = 0; i < n; i++) g_fnv = (g_fnv ^ (unsigned char)u[i]) * 16777619u;
    /* Once, on the first token. The connect message ("waiting for the first
     * token") is a lie the moment one arrives, and leaving it up while the
     * window fills with text is the kind of small dishonesty that teaches a
     * user to stop reading the status line. Set once rather than per delta so
     * a later refusal message is not overwritten by the next token. */
    if (g_sse.n_deltas == 1) status("receiving the reply...");
    g_dirty = 1;                     /* rate-limited in the main loop */
}

static void on_refuse(void *ctx, int code, const char *why)
{
    (void)ctx;
    snprintf(g_status, sizeof g_status, "stream: %s", why);
    printf("CH_REFUSE code=%d %s\n", code, why);
    g_dirty = 1;
}

/* The body sink. Below 200 the body is not an event stream at all -- it is a
 * one-shot JSON error object -- so it is kept whole and mined for its message
 * once the exchange finishes. Branching HERE is what lets one registration
 * cover both, and http1.c hands the sink the entity body with chunk framing
 * already removed. */
static int body_sink(void *ctx, const uint8_t *data, int len)
{
    (void)ctx;
    /* THE STATUS COMES FROM THE PARSER, NOT FROM OUR COPY OF IT, and that is a
     * bug worth the sentence. `g_code` is filled in by pump() AFTER
     * h1_conn_pump returns -- but one call to h1_conn_pump can finish the
     * headers AND deliver the first body bytes, and the sink runs inside it. So
     * the first chunk of every stream arrived while g_code was still 0, was
     * filed as an error body, and vanished. The symptom was tiny and looked
     * like anything but this: 226 tokens out of 227, one keep-alive comment
     * missing, and a reply that was correct to read. The FNV of the assembled
     * text against the FNV of what the server sent is what caught it, which is
     * the argument for that hash existing. */
    if (g_conn.resp.code == 200) { sse_feed(&g_sse, data, len); return H1_OK; }
    int room = CH_ERRBODY_MAX - 1 - g_errbody_len;
    if (room > 0) {
        int k = len < room ? len : room;
        memcpy(g_errbody + g_errbody_len, data, (size_t)k);
        g_errbody_len += k;
        g_errbody[g_errbody_len] = 0;
    }
    return H1_OK;
}

/* ---- JSON out ---- */
static int jesc(char *o, int cap, const char *s, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = 0;
        char u[8];
        switch (c) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if (c < 0x20) { snprintf(u, sizeof u, "\\u%04x", c); rep = u; }
        }
        if (rep) {
            int rl = (int)strlen(rep);
            if (k + rl >= cap) return -1;
            memcpy(o + k, rep, (size_t)rl); k += rl;
        } else {
            if (k + 1 >= cap) return -1;
            o[k++] = (char)c;
        }
    }
    o[k] = 0;
    return k;
}

/* Build the chat/completions body from the turns we hold. Oldest turns are
 * dropped -- and COUNTED, and shown -- if the whole conversation will not fit;
 * silently sending half a conversation would make the model's answers look
 * arbitrary with nothing on screen to explain why. */
static int build_body(char *out, int cap)
{
    int first = 0;
    for (;;) {
        int k = snprintf(out, (size_t)cap, "{\"model\":\"%s\",\"stream\":true,\"messages\":[",
                         g_conf.model);
        if (k < 0 || k >= cap) return -1;
        int ok = 1, wrote = 0;
        for (int t = first; t < nturn; t++) {
            /* ROLE_SYS turns are this window's own chatter, not conversation.
             * A zero-length turn is skipped too, and that is not cosmetic: the
             * assistant's turn is opened BEFORE the request is built (it is
             * what the reply streams into), so without this every request would
             * end with {"role":"assistant","content":""} -- which several
             * OpenAI-compatible servers reject outright. */
            if (turns[t].role == ROLE_SYS || turns[t].len == 0) continue;
            const char *role = turns[t].role == ROLE_USER ? "user" : "assistant";
            int need = snprintf(out + k, (size_t)(cap - k), "%s{\"role\":\"%s\",\"content\":\"",
                                wrote ? "," : "", role);
            if (need < 0 || k + need >= cap - 4) { ok = 0; break; }
            k += need;
            int e = jesc(out + k, cap - k - 4, tr + turns[t].off, turns[t].len);
            if (e < 0) { ok = 0; break; }
            k += e;
            if (k + 2 >= cap) { ok = 0; break; }
            out[k++] = '"'; out[k++] = '}';
            wrote = 1;
        }
        if (ok && k + 3 < cap) { out[k++] = ']'; out[k++] = '}'; out[k] = 0; return k; }
        if (first >= nturn - 1) return -1;      /* even one turn will not fit */
        first++;
        turns_dropped++;
    }
}

static void wire_close(void)
{
    if (g_conn_live) { h1_conn_free(&g_conn); g_conn_live = 0; }
    if (g_fd >= 0)   { sock_close(g_fd); g_fd = -1; }
}

static const char *sock_why(int rc)
{
    switch (rc) {
    case SOCK_E_ARG:    return "bad argument";
    case SOCK_E_DNS:    return "the host name did not resolve";
    case SOCK_E_CONN:   return "the connection was refused or the network is down";
    case SOCK_E_TLS:    return "TLS refused: handshake or certificate verification failed";
    case SOCK_E_NOSLOT: return "the kernel socket table is full";
    }
    return "unknown socket error";
}

static void fail(const char *what)
{
    wire_close();
    turn_close();
    g_state = ST_ERROR;
    status(what);
    printf("CH_ERROR %s\n", what);
    g_dirty = 1;
}

static void send_prompt(const char *prompt)
{
    if (g_state == ST_DIAL || g_state == ST_STREAM) {
        status("a reply is still arriving -- one request at a time");
        return;
    }
    /* THE CONFIG IS RE-READ ON EVERY SEND, not cached from startup. One
     * read_file per request is nothing next to a TLS handshake, and it means
     * editing /etc/ai.conf takes effect on the next message instead of on the
     * next launch -- which is what you want while you are getting the endpoint
     * right, and it is also what lets the boot harness walk the 401 / 429 /
     * truncated / TLS-refused paths without restarting the app. */
    g_conf_ok = conf_load();
    if (!g_conf_ok) { fail(g_conf_err); return; }
    g_errbody_len = 0; g_errbody[0] = 0;
    g_code = 0; g_chunked = 0; g_fnv = 2166136261u; g_frames_skipped = 0;

    say(ROLE_USER, prompt);
    turn_open_new(ROLE_AI);                 /* the reply grows into this turn */

    static char body[CH_BODY_MAX];
    int blen = build_body(body, CH_BODY_MAX);
    if (blen < 0) { fail("the conversation does not fit in one request"); return; }

    struct h1_request q;
    if (h1_request_init(&q, "POST", g_conf.path) != H1_OK) { fail("out of memory"); return; }
    char hostval[CH_HOST_MAX + 8];
    if ((g_conf.tls && g_conf.port != 443) || (!g_conf.tls && g_conf.port != 80))
        snprintf(hostval, sizeof hostval, "%s:%d", g_conf.host, g_conf.port);
    else
        snprintf(hostval, sizeof hostval, "%s", g_conf.host);
    char clen[24];
    snprintf(clen, sizeof clen, "%d", blen);
    h1_request_set_header(&q, "Host", hostval);
    h1_request_set_header(&q, "Content-Type", "application/json");
    h1_request_set_header(&q, "Content-Length", clen);
    h1_request_set_header(&q, "Accept", "text/event-stream");
    /* identity, so the sink can deliver bytes as they arrive: a compressed body
     * is a whole-message transform and http1.c would (correctly) buffer it,
     * which would defeat the entire point of this program. */
    h1_request_set_header(&q, "Accept-Encoding", "identity");
    h1_request_set_header(&q, "Connection", "close");
    if (g_conf.key[0]) {
        static char auth[CH_KEY_MAX + 16];
        snprintf(auth, sizeof auth, "Bearer %s", g_conf.key);
        h1_request_set_header(&q, "Authorization", auth);
        /* auth is static and long-lived on purpose: h1_request_build copies it
         * before this returns, and it is never handed to printf. */
    }
    h1_request_set_body(&q, body, blen);

    char *req = 0; int reqlen = 0;
    int rc = h1_request_build(&q, &req, &reqlen);
    h1_request_free(&q);
    if (rc != H1_OK) { fail("could not build the request"); return; }

    int flags = g_conf.tls ? (SOCK_F_TLS | SOCK_F_ALPN_HTTP11) : 0;
    g_fd = sock_open(g_conf.host, g_conf.port, flags);
    if (g_fd < 0) {
        char m[CH_STATUS_MAX];
        snprintf(m, sizeof m, "connect to %s:%d failed -- %s",
                 g_conf.host, g_conf.port, sock_why(g_fd));
        free(req);
        fail(m);
        return;
    }
    struct h1_transport t = { tr_read, tr_write, tr_pollw, 0 };
    if (h1_conn_start(&g_conn, &t, req, reqlen) != H1_OK) {
        free(req);
        fail("h1_conn_start failed");
        return;
    }
    g_conn_live = 1;
    sse_init(&g_sse, on_delta, on_refuse, 0);
    h1_response_sink(&g_conn.resp, body_sink, 0);

    g_state = ST_DIAL;
    g_t0 = g_last_progress = monotonic_ms();
    g_progress = 0;
    g_repaints = 0;
    status("connecting...");
    printf("CH_STREAM_BEGIN host=%s port=%d tls=%d model=%s auth=%s\n",
           g_conf.host, g_conf.port, g_conf.tls, g_conf.model,
           g_conf.key[0] ? "bearer" : "none");   /* never the key itself */
    g_dirty = 1;
}

/* The end of an exchange, whatever kind of end it was. */
static void finish(void)
{
    int trunc = sse_eof(&g_sse);
    turn_close();
    wire_close();
    g_state = ST_IDLE;

    unsigned long long ms = monotonic_ms() - g_t0;
    if (g_code != 200) {
        char msg[256];
        const char *detail = 0;
        static const char *const P[2] = { "error", "message" };
        static char m[256];
        if (g_errbody_len > 0 &&
            sse_json_string(g_errbody, g_errbody_len, P, 2, m, sizeof m) > 0)
            detail = m;
        if (g_code == 401)
            snprintf(msg, sizeof msg, "401: the endpoint rejected the key%s%s",
                     detail ? " -- " : "", detail ? detail : "");
        else if (g_code == 429)
            snprintf(msg, sizeof msg, "429: rate limited%s%s",
                     detail ? " -- " : "", detail ? detail : "");
        else
            snprintf(msg, sizeof msg, "HTTP %d%s%s", g_code,
                     detail ? " -- " : "", detail ? detail : "");
        status(msg);
        printf("CH_HTTP_FAIL code=%d\n", g_code);
        g_state = ST_ERROR;
    } else if (trunc == SSE_E_TRUNC) {
        char msg[192];
        snprintf(msg, sizeof msg,
                 "the stream ended without [DONE] after %u token%s -- the reply is incomplete",
                 g_sse.n_deltas, g_sse.n_deltas == 1 ? "" : "s");
        status(msg);
        printf("CH_TRUNCATED deltas=%u\n", g_sse.n_deltas);
        g_state = ST_ERROR;
    } else {
        char msg[192];
        snprintf(msg, sizeof msg, "%u tokens in %u ms%s%s",
                 g_sse.n_deltas, (unsigned)ms,
                 g_chunked ? ", chunked" : "",
                 g_sse.n_refused ? ", some records refused" : "");
        status(msg);
    }
    printf("CH_STREAM_END deltas=%u records=%u comments=%u refused=%u "
           "bytes=%u repaints=%u skipped=%u ms=%u chunked=%d code=%d fnv=%08x\n",
           g_sse.n_deltas, g_sse.n_records, g_sse.n_comments, g_sse.n_refused,
           g_sse.n_bytes, g_repaints, g_frames_skipped, (unsigned)ms,
           g_chunked, g_code, g_fnv);
    g_dirty = 1;
}

/* One non-blocking step of the exchange. Called every pass of the main loop. */
static void pump(void)
{
    if (!g_conn_live) return;

    int bits = sock_poll(g_fd);
    if (bits >= 0 && (bits & SOCK_P_ERROR)) {
        char m[CH_STATUS_MAX];
        snprintf(m, sizeof m, "%s", sock_why(SOCK_ERR_CODE(bits)));
        fail(m);
        return;
    }
    if (g_state == ST_DIAL && bits >= 0 && (bits & SOCK_P_CONNECTED)) {
        g_state = ST_STREAM;
        status(g_conf.tls ? "connected (TLS), waiting for the first token..."
                          : "connected, waiting for the first token...");
        g_dirty = 1;
    }

    int st = h1_conn_pump(&g_conn);

    if (!g_code && h1_response_headers_done(&g_conn.resp)) {
        g_code = g_conn.resp.code;
        g_chunked = g_conn.resp.chunked;
        if (g_code == 200 && !h1_response_streaming(&g_conn.resp)) {
            /* Said out loud rather than tolerated: the sink was declined, so
             * the body will be delivered whole and the window will fill in one
             * jump. That is the failure this program exists to avoid. */
            status("the response is not being delivered incrementally "
                   "(a Content-Encoding forced buffering)");
        }
        printf("CH_HEADERS code=%d chunked=%d streaming=%d\n",
               g_code, g_chunked, h1_response_streaming(&g_conn.resp));
        g_dirty = 1;
    }

    uint64_t p = h1_conn_progress(&g_conn);
    unsigned long long now = monotonic_ms();
    if (p != g_progress) { g_progress = p; g_last_progress = now; }
    else if (now - g_last_progress > CH_IDLE_MS) {
        fail("no data for 30 s -- giving up on this stream");
        return;
    }

    if (st == H1_C_DONE) { finish(); return; }
    if (st == H1_C_ERROR) {
        /* A close-delimited SSE stream ends at EOF, and http1.c calls that
         * H1_E_TRUNC when it was expecting a framed body. Distinguish: if we
         * have a 200 and the SSE layer saw [DONE], the exchange succeeded and
         * the transport error is just how the server hung up. */
        if (g_code == 200 && sse_done(&g_sse)) { finish(); return; }
        char m[CH_STATUS_MAX];
        snprintf(m, sizeof m, "transport: %s", h1_strerror(g_conn.err));
        if (g_code == 200) finish(); else fail(m);
        return;
    }
}

/* ------------------------------------------------------------------- paint */

static char inbuf[CH_PROMPT_MAX];
static int  scroll_px;
static int  follow = 1;              /* stick to the bottom while streaming   */
static int  pending_send;

static const char *role_label(int r)
{
    return r == ROLE_USER ? "You" : r == ROLE_AI ? "Assistant" : "ch";
}

static void frame(void)
{
    int W = aui_width(), H = aui_height();
    aui_begin(AUI_BG);

    /* --- header: what this window is talking to. The key is described, never
     * shown: "bearer" or "no key", and nothing else. --- */
    int hh = 46;
    aui_fill(0, 0, W, hh, AUI_SURFACE);
    aui_hairline(0, hh - 1, W);
    char who[CH_STATUS_MAX];
    if (g_conf_ok)
        snprintf(who, sizeof who, "%s%s:%d%s  ·  %s  ·  %s",
                 g_conf.tls ? "https://" : "http://", g_conf.host, g_conf.port,
                 g_conf.path, g_conf.model, g_conf.key[0] ? "bearer key" : "no key");
    else
        snprintf(who, sizeof who, "not configured");
    aui_text_sz(AUI_PAD, 8, "Chat", AUI_TEXT, AUI_FS_LABEL);
    aui_text_ellipsis(AUI_PAD, 26, W - 2 * AUI_PAD, who, AUI_MUTED, AUI_FS_CAPTION);

    /* --- footer: prompt + send --- */
    int fh = AUI_H_CTL + 2 * AUI_SP(2);
    int fy = H - fh;
    aui_fill(0, fy, W, fh, AUI_SURFACE);
    aui_hairline(0, fy, W);
    int btnw = 76;
    int busy = (g_state == ST_DIAL || g_state == ST_STREAM);
    /* The field stays USABLE when the config is broken. It is tempting to grey
     * it out -- but the config is re-read on every send, so a user who fixes
     * /etc/ai.conf in another window must be able to try again without
     * relaunching, and a disabled field would make a fixable state look
     * terminal. */
    if (aui_textfield_ex(AUI_PAD, fy + AUI_SP(2), W - 2 * AUI_PAD - btnw - AUI_GAP,
                         inbuf, sizeof inbuf,
                         g_conf_ok ? "Ask something" : "fix /etc/ai.conf, then send",
                         !busy))
        pending_send = 1;
    if (aui_button_ex(W - AUI_PAD - btnw, fy + AUI_SP(2), btnw, AUI_H_CTL,
                      busy ? "..." : "Send", AUI_V_PRIMARY, !busy && inbuf[0]))
        pending_send = 1;

    /* --- status strip --- */
    int sy = fy - 22;
    unsigned col = (g_state == ST_ERROR) ? AUI_ERROR
                 : busy ? AUI_ACCENT : AUI_MUTED;
    aui_text_ellipsis(AUI_PAD, sy + 4, W - 2 * AUI_PAD - 130, g_status, col, AUI_FS_CAPTION);
    if (busy) {
        /* The live token count, drawn BESIDE the status rather than into it, so
         * a refusal reported mid-stream stays on screen while the counter keeps
         * moving. Two facts, two places. */
        char n[40];
        snprintf(n, sizeof n, "%u tokens", g_sse.n_deltas);
        aui_text_sz(W - AUI_PAD - 108, sy + 4, n, AUI_MUTED, AUI_FS_CAPTION);
        aui_spinner(W - AUI_PAD - 10, sy + 9, 7);
    }
    if (tr_full || lines_full || turns_dropped) {
        char n[96];
        snprintf(n, sizeof n, "%s%s%s",
                 tr_full ? "transcript full " : "",
                 lines_full ? "line table full " : "",
                 turns_dropped ? "older turns dropped" : "");
        aui_text_sz(W - AUI_PAD - 200, 8, n, AUI_WARNING, AUI_FS_CAPTION);
    }

    /* --- transcript --- */
    int ty = hh, th = sy - hh;
    if (th < CH_LH) th = CH_LH;
    int content = (nline + 2) * CH_LH + 8;
    if (follow) {
        scroll_px = content - th;
        if (scroll_px < 0) scroll_px = 0;
    }
    aui_scroll_begin(0, ty, W, th, &scroll_px, content);
    /* ONLY THE VISIBLE LINES. This is the third of the four bounds in the
     * header: a long transcript costs the same frame as a short one. */
    int first = scroll_px / CH_LH - 1, last = (scroll_px + th) / CH_LH + 1;
    if (first < 0) first = 0;
    if (last > nline) last = nline;
    for (int i = first; i < last; i++) {
        int y = i * CH_LH + 4;
        if (lines[i].label) {
            aui_text_sz(AUI_PAD, y, role_label(lines[i].role),
                        lines[i].role == ROLE_USER ? AUI_ACCENT :
                        lines[i].role == ROLE_SYS  ? AUI_WARNING : AUI_SUCCESS,
                        AUI_FS_CAPTION);
        } else if (lines[i].len > 0) {
            /* aui_text_n, NOT gui_text_run: inside a scroll container the two
             * are in different coordinate systems. See aui.c. */
            aui_text_n(AUI_PAD + 12, y, tr + lines[i].off, lines[i].len,
                       AUI_TEXT, CH_FS);
        }
    }
    /* The live tail: the part of the open turn that has not been committed to a
     * line yet. Without this the window would only move once per WORD. */
    if (turn_open && tr_len > wrap_line_start && nline < CH_MAXLINE) {
        aui_text_n(AUI_PAD + 12, nline * CH_LH + 4, tr + wrap_line_start,
                   tr_len - wrap_line_start, AUI_TEXT, CH_FS);
    }
    aui_scroll_end();

    aui_end();
    g_repaints++;
    g_last_paint = monotonic_ms();
    g_dirty = 0;
}

/* --------------------------------------------------------------- app_main */

void app_main(void)
{
    int w = 720, h = 540;
    gui_create("Chat", w, h);
    aui_set_size(w, h);
    wrap_maxw = w - 2 * AUI_PAD - 24;
    space_w = text_measure_px(" ", 1, CH_FS, 0);
    if (space_w <= 0) space_w = 4;

    g_conf_ok = conf_load();
    if (g_conf_ok) {
        status("ready");
        say(ROLE_SYS, "Type a prompt and press Enter. The reply arrives one "
                      "token at a time.");
        printf("CH_CONF_OK host=%s port=%d tls=%d model=%s auth=%s\n",
               g_conf.host, g_conf.port, g_conf.tls, g_conf.model,
               g_conf.key[0] ? "bearer" : "none");
    } else {
        /* THE LOUD REFUSAL. Not a blank window, not a crash, and specific
         * enough to act on: which file, which line, what is wrong with it. */
        g_state = ST_ERROR;
        status(g_conf_err);
        say(ROLE_SYS, g_conf_err);
        say(ROLE_SYS, "Expected /etc/ai.conf:\n"
                      "  host  = api.example.com\n"
                      "  port  = 443\n"
                      "  path  = /v1/chat/completions\n"
                      "  tls   = 1\n"
                      "  model = the-model-name\n"
                      "  key   = <your key>      (leave empty for a local endpoint)");
        printf("CH_CONF_REFUSED %s\n", g_conf_err);
    }
    /* Focus the prompt field before the window is first shown, so you can type
     * the moment it appears. aui assigns widget ids in DRAW order, so the id is
     * not known until a frame has run -- hence draw, then walk focus to the
     * first focusable widget (the field), then draw again. Asking for id 1 by
     * hand would work today and break the day a widget is added above it. */
    frame();
    aui_focus_next(+1);
    frame();
    printf("CH_READY\n");

    for (;;) {
        struct logit_event e;
        int drew = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) {
                /* MID-STREAM CLOSE. The socket is the thing that outlives this
                 * process if it is not closed here: the kernel's socket table
                 * is 8 entries, so a leaked one is a machine-wide resource, not
                 * a private one. */
                if (g_state == ST_DIAL || g_state == ST_STREAM)
                    printf("CH_CLOSED_MIDSTREAM deltas=%u bytes=%u\n",
                           g_sse.n_deltas, g_sse.n_bytes);
                wire_close();
                app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                aui_set_size(e.a, e.b);
                wrap_maxw = e.a - 2 * AUI_PAD - 24;
                if (wrap_maxw < 80) wrap_maxw = 80;
                int was_open = turn_open;
                rewrap();
                turn_open = was_open;
            }
            if (e.type == EV_WHEEL) follow = 0;
            /* The event TYPE, never its value. A window that says nothing when
             * a key reaches it is indistinguishable from one the keys never
             * reached, and telling those apart from outside took an hour of
             * this unit's build. The character is deliberately absent: the
             * prompt is the user's text and the console is a log. */
            if (e.type != EV_MOUSE_MOVE) printf("CH_EV type=%d\n", e.type);
            if (e.type == EV_MOUSE_MOVE) {
                aui_feed(&e);
                if (aui_want_repaint()) { frame(); drew = 1; }
                aui_feed_done();
                continue;
            }
            aui_feed(&e);
            frame(); drew = 1;
            aui_feed_done();
            if (pending_send) {
                pending_send = 0;
                if (inbuf[0]) {
                    follow = 1;
                    char p[CH_PROMPT_MAX];
                    snprintf(p, sizeof p, "%s", inbuf);
                    inbuf[0] = 0;
                    send_prompt(p);
                    frame();
                }
            }
        }

        if (g_state == ST_DIAL || g_state == ST_STREAM) pump();

        /* THE REPAINT BUDGET. A delta only sets g_dirty; the window is redrawn
         * at most every CH_REPAINT_MS. Bounded by the stream's duration, not by
         * its token count -- and the last frame is forced by finish(), which
         * sets g_dirty with the state no longer streaming. */
        if (g_dirty && !drew) {
            unsigned long long now = monotonic_ms();
            if (now - g_last_paint >= CH_REPAINT_MS ||
                (g_state != ST_DIAL && g_state != ST_STREAM))
                frame();
            else
                g_frames_skipped++;
        }
        sys_yield();
    }
}
