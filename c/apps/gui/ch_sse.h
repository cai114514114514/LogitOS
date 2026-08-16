#ifndef CH_SSE_H
#define CH_SSE_H

/* ch_sse -- Server-Sent Events framing, plus the smallest JSON reader that can
 * pull `choices[0].delta.content` out of an OpenAI-compatible stream.
 *
 * WHY THIS EXISTS AND WHY IT IS A SEPARATE FILE FROM THE APP. Everything else
 * the chat window needs already exists in this tree: TLS is a socket flag
 * (SOCK_F_TLS), and HTTP/1.1 -- request building, chunked de-framing, and an
 * incremental body sink -- is c/net/http/http1.c, whose header says in so many
 * words that the sink exists because "Server-Sent Events is an HTTP response
 * that never ends". What did NOT exist is the layer above that sink: the SSE
 * record framing, and a JSON scanner. c/apps/libc has no JSON parser and
 * pulling one in for four field names would be the wrong trade. So this file
 * is the ONLY thing the unit adds to the wire path, and it is separate from
 * ch.c so the host gate can drive it with no window, no socket and no kernel.
 *
 * THE HARD CASE, and the reason the host gate exists at all: a delta is split
 * across chunk boundaries. The network hands you `data: {"choi` and then, some
 * milliseconds later, `ces":[{"delta":{"content":"hel`. Every reader that
 * parses "the buffer it was just handed" is wrong, silently, and only against
 * a real server -- a recorded stream replayed in one lump passes. So nothing
 * here ever looks at a feed buffer as a unit: bytes accumulate into a LINE,
 * lines accumulate into a RECORD, and only a complete record is ever parsed.
 *
 * NO ALLOCATION, NO LIBC. Every bound below is a fixed array inside struct sse,
 * so the same object file links into a freestanding .aex and into a host test
 * binary. That is not tidiness: it is what makes the gate a diff against a
 * recorded byte stream rather than a mock of a network.
 *
 * WHAT IT REFUSES, and refuses OUT LOUD (there are no silent drops here -- each
 * of these calls `on_refuse` with a code and a short reason, and increments a
 * counter the caller can display):
 *
 *   - a line longer than SSE_LINE_MAX. The line is discarded to its terminator
 *     and the RECORD it was part of is poisoned, because half a JSON object
 *     parsed as if it were whole is worse than no delta at all.
 *   - a record whose accumulated `data:` payload exceeds SSE_DATA_MAX.
 *   - a record whose payload is not an object we can read: not JSON, no
 *     choices[0].delta.content, or a `content` that is not a string. A null
 *     content (the last frame of many providers carries one, with
 *     finish_reason set) is NOT a refusal -- it is a record with no delta.
 *   - a decoded delta longer than SSE_TEXT_MAX, or JSON nested deeper than
 *     SSE_JSON_DEPTH.
 *   - `\uXXXX` naming an unpaired surrogate: the code point is replaced with
 *     U+FFFD rather than dropped or emitted as invalid UTF-8, and that is a
 *     substitution, not a refusal, so it does not stop the delta.
 *
 * WHAT IT DOES NOT DO, on purpose:
 *   - no `event:` dispatch. OpenAI-compatible streams put everything on the
 *     default event type; a named-event stream would parse fine here and the
 *     name would be counted and ignored.
 *   - no `retry:` / `id:` reconnection state. Reconnecting mid-answer would
 *     need Last-Event-ID support from the endpoint, which this one does not
 *     have; pretending otherwise would resume nothing.
 *   - no JSON numbers-as-numbers. The scanner only ever DECODES strings; every
 *     other value type is skipped correctly (so it cannot be confused by a
 *     brace inside one) but never converted.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds. All of them are per-stream fixed storage inside struct sse, so
 * sizeof(struct sse) is roughly the sum below (~30 KB) and lives wherever the
 * caller puts it -- ch.c makes it static. */
#define SSE_LINE_MAX    4096    /* one SSE line, terminator excluded          */
#define SSE_DATA_MAX   16384    /* one record's accumulated data payload      */
#define SSE_TEXT_MAX    8192    /* one decoded content delta                  */
#define SSE_JSON_DEPTH    24    /* nesting the value skipper will descend     */

enum {
    SSE_OK        =  0,
    SSE_E_LINE    = -1,     /* line over SSE_LINE_MAX                          */
    SSE_E_RECORD  = -2,     /* record data over SSE_DATA_MAX                   */
    SSE_E_JSON    = -3,     /* payload was not a readable JSON object          */
    SSE_E_TEXT    = -4,     /* decoded delta over SSE_TEXT_MAX                 */
    SSE_E_TRUNC   = -5      /* stream ended before `data: [DONE]`              */
};

const char *sse_strerror(int code);

/* A content delta. `utf8` is NOT NUL-terminated and is only valid for the
 * duration of the call -- it points into the parser's own decode buffer. */
typedef void (*sse_on_delta)(void *ctx, const char *utf8, int len);
/* A refusal. `why` is a short static string; `code` is one of SSE_E_*. */
typedef void (*sse_on_refuse)(void *ctx, int code, const char *why);

struct sse {
    /* -- callbacks -- */
    sse_on_delta  on_delta;
    sse_on_refuse on_refuse;
    void         *ctx;

    /* -- line assembly -- */
    char line[SSE_LINE_MAX];
    int  line_len;
    int  line_over;         /* this line already exceeded the bound            */
    int  skip_lf;           /* last byte was CR: swallow a following LF        */
    int  bom;               /* the first line may still carry a UTF-8 BOM      */

    /* -- record assembly -- */
    char data[SSE_DATA_MAX];
    int  data_len;
    int  have_data;         /* at least one `data:` line in this record        */
    int  poisoned;          /* a line was lost: refuse the whole record        */
    int  poison_code;       /* why it was poisoned (SSE_E_LINE / SSE_E_RECORD) */

    /* -- decode scratch -- */
    char text[SSE_TEXT_MAX];

    /* -- state / counters, all readable by the app so it can show them -- */
    int      done;          /* `data: [DONE]` has been seen                    */
    int      last_err;      /* most recent SSE_E_*, 0 if none                  */
    unsigned n_records;     /* records dispatched (blank-line separated)       */
    unsigned n_deltas;      /* records that produced a content delta           */
    unsigned n_comments;    /* `:` keep-alive lines                            */
    unsigned n_fields;      /* non-`data` fields seen (event/id/retry/...)     */
    unsigned n_refused;     /* refusals reported through on_refuse             */
    unsigned n_bytes;       /* bytes fed                                       */
};

void sse_init(struct sse *s, sse_on_delta on_delta, sse_on_refuse on_refuse, void *ctx);
/* Feed however many bytes arrived -- one, or three records, or half a word.
 * Always consumes all of `len`; returns SSE_OK, or the last refusal code seen
 * during this call (the stream keeps going either way). */
int  sse_feed(struct sse *s, const void *buf, int len);
/* The peer closed. Flushes a final record that had no trailing blank line, then
 * returns SSE_OK if `[DONE]` was seen and SSE_E_TRUNC if it was not. */
int  sse_eof(struct sse *s);
int  sse_done(const struct sse *s);

/* ---- the JSON reader, exposed because the app needs it for error bodies ----
 * A 401 does not answer in SSE; it answers with a single JSON object, and the
 * useful part of it is error.message. Rather than a second scanner, the app
 * walks that with the same one.
 *
 * `path` is a component list. A component is read as an ARRAY INDEX when the
 * value it is applied to begins with '[', and as an OBJECT KEY when it begins
 * with '{' -- so {"choices","0","delta","content"} is unambiguous without a
 * separate type per component.
 *
 * Returns the decoded string length on success, or a negative SSE_E_*. `out`
 * is NUL-terminated on success. */
int sse_json_string(const char *js, int len, const char *const *path, int npath,
                    char *out, int outmax);

#ifdef __cplusplus
}
#endif
#endif /* CH_SSE_H */
