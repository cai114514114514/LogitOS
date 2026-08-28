#ifndef LOGIT_WS_H
#define LOGIT_WS_H

#include <stdint.h>
#include <stddef.h>

/* RFC 6455 -- the WebSocket wire protocol, transport-free.
 *
 * Same shape as http1.c and for the same reason: this file opens no socket,
 * blocks nowhere, and allocates only what a bounded, already-validated frame
 * header says it needs -- so it is host-testable with an in-memory transport
 * and shares nothing with the JS surface in c/apps/browser/js_websocket.c,
 * which owns the socket, the handshake exchange (over c/net/http/http1.c's
 * h1_conn -- a WebSocket Upgrade IS an HTTP/1.1 exchange until the 101), the
 * message-assembly across fragments, and the JS event dispatch.
 *
 * WHAT THIS FILE DOES: the Sec-WebSocket-Key/Accept pair (RFC 6455 4.2.2,
 * section 1.3's worked example is the test oracle), frame serialisation with
 * MANDATORY client-to-server masking, and an incremental frame parser that
 * enforces the per-frame protocol rules a single frame can be judged by in
 * isolation (RSV bits, unknown opcodes, fragmented/oversized control frames,
 * the masking direction). Rules that need MORE than one frame -- continuation
 * sequencing, the close handshake, ping->pong, UTF-8 validation of an
 * assembled message -- belong to the caller, because they need state this
 * parser does not keep (RFC 6455 5.4, 5.5.1).
 */

enum {
    WS_OP_CONT  = 0x0,
    WS_OP_TEXT  = 0x1,
    WS_OP_BIN   = 0x2,
    /* 0x3-0x7 reserved for future non-control frames */
    WS_OP_CLOSE = 0x8,
    WS_OP_PING  = 0x9,
    WS_OP_PONG  = 0xA,
    /* 0xB-0xF reserved for future control frames */
};

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ---- handshake --------------------------------------------------------- */

/* Base64 of a 16-byte nonce -> Sec-WebSocket-Key. out must hold >=25 bytes
 * (24 base64 chars incl. "==" padding, + NUL). RFC 6455 4.1: "a randomly
 * selected 16-byte value that has been base64-encoded". */
void ws_make_key(const uint8_t nonce[16], char out[25]);

/* SHA-1(key || GUID), base64-encoded -- RFC 6455 4.2.2 #5. `key` is the
 * NUL-terminated Sec-WebSocket-Key value we sent (not the response). out must
 * hold >=29 bytes (28 base64 chars + NUL). */
void ws_compute_accept(const char *key, char out[29]);

/* 1 if `server_accept` (the Sec-WebSocket-Accept header value we received)
 * equals ws_compute_accept(key). Base64 is case-sensitive so this is a plain
 * byte compare, deliberately not case-folded like the header NAMES around it. */
int ws_accept_matches(const char *key, const char *server_accept);

/* ---- frame codec -------------------------------------------------------- */

#define WS_HDR_MAX 14   /* 1 (byte0) + 1 (byte1) + 8 (ext len) + 4 (mask) */

/* Serialize one frame into out/outcap. `mask`, if non-NULL, is the 4-byte
 * masking key and is APPLIED to a copy of `payload` written into `out` --
 * the caller's payload buffer is never mutated. RFC 6455 5.1: a client MUST
 * mask every frame it sends; NULL is for the unmasked SERVER side, which
 * only ws_test's in-memory peer plays here. Returns bytes written, or -1 if
 * outcap is too small for the header+payload. */
int ws_frame_write(uint8_t *out, size_t outcap, int fin, int opcode,
                    const uint8_t *payload, size_t len, const uint8_t mask[4]);

/* XOR-mask/unmask len bytes of data in place with the 4-byte key, cycling --
 * the operation is its own inverse (RFC 6455 5.3). */
void ws_mask_xor(uint8_t *data, size_t len, const uint8_t mask[4]);

enum {
    WS_P_AGAIN = 0,   /* need more bytes; nothing decoded yet */
    WS_P_FRAME = 1,   /* one full frame decoded -- fields below are valid */
    WS_P_ERROR = -1,  /* a rule a SINGLE frame can violate was violated; the
                        * caller must fail the connection (Close 1002) and stop
                        * feeding this parser -- its state is not advanced past
                        * the error. */
};

struct ws_parser {
    int      state;        /* WSP_* internal */
    uint8_t  hdrbuf[WS_HDR_MAX];
    int      hdrlen, hdrneed;

    /* decoded header, valid once WS_P_FRAME is returned */
    int      fin, rsv, opcode, masked;
    uint8_t  maskkey[4];
    uint64_t paylen;

    /* payload accumulator -- owned, grows to paylen, NUL-padded at [paylen] */
    uint64_t payoff;
    uint8_t *payload;
    size_t   paycap;

    /* configuration */
    size_t   maxpay;        /* a header declaring more than this is WS_P_ERROR */
    int      require_masked;/* 1: incoming frames MUST be masked (we are a
                              * server); 0: MUST NOT be (we are a client --
                              * the only side js_websocket.c plays). Either
                              * direction violated is WS_P_ERROR -- RFC 6455
                              * 5.1 "a server MUST NOT mask", 5.3 "the client
                              * MUST mask". */
};

void   ws_parser_init(struct ws_parser *p, size_t max_frame_payload, int require_masked);
void   ws_parser_free(struct ws_parser *p);
/* Feed bytes; returns bytes CONSUMED (may be less than len -- the remainder
 * belongs to the next call) and sets *status to a WS_P_* above. On
 * WS_P_FRAME, call ws_parser_next() before feeding more -- the decoded frame
 * fields stay valid until then. */
size_t ws_parser_feed(struct ws_parser *p, const uint8_t *data, size_t len, int *status);
void   ws_parser_next(struct ws_parser *p);

/* ---- UTF-8 validation (RFC 6455 5.6 / 8.1) ------------------------------
 * Strict: rejects overlong encodings, UTF-16 surrogates (U+D800-U+DFFF) and
 * codepoints above U+10FFFF. A text frame or a Close reason failing this is
 * a protocol error (close code 1007). */
int ws_utf8_valid(const uint8_t *s, size_t len);

/* ---- close codes (RFC 6455 7.4) ----------------------------------------- */
enum {
    WS_CLOSE_NORMAL           = 1000,
    WS_CLOSE_GOING_AWAY       = 1001,
    WS_CLOSE_PROTOCOL_ERROR   = 1002,
    WS_CLOSE_UNSUPPORTED_DATA = 1003,
    WS_CLOSE_NO_STATUS        = 1005, /* never sent on the wire */
    WS_CLOSE_ABNORMAL         = 1006, /* never sent on the wire */
    WS_CLOSE_INVALID_PAYLOAD  = 1007,
    WS_CLOSE_POLICY_VIOLATION = 1008,
    WS_CLOSE_MESSAGE_TOO_BIG  = 1009,
    WS_CLOSE_INTERNAL_ERROR   = 1011,
};
/* 1 if `code` is legal to SEND in a Close frame (RFC 6455 7.4.1/7.4.2: the
 * defined codes, or 3000-4999). 1005/1006 are explicitly excluded -- they
 * describe a closure that carried no Close frame at all. */
int ws_close_code_sendable(int code);

#endif /* LOGIT_WS_H */
