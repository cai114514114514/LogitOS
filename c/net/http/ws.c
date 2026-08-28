#include "ws.h"
#include "base64.h"
#include <string.h>
#include <stdlib.h>

/* ocsp_sha1() is c/crypto/hash/sha1.c's ONLY declared use, and its own header
 * comment says why a second caller does not get a crypto.h prototype: "If a
 * second caller ever appears, that is the moment to re-read this comment
 * rather than to add a prototype to crypto.h." Read it -- it still holds.
 * RFC 6455 1.3 states plainly that this hash is not a security mechanism: it
 * proves the server actually understood the WebSocket handshake (defeating a
 * naive cache/proxy replaying an old response), and no collision resistance
 * is required of it. Second local extern, same shape as ocsp.c's. */
extern void ocsp_sha1(const void *data, size_t len, uint8_t out[20]);

/* ---- handshake ----------------------------------------------------------- */

void ws_make_key(const uint8_t nonce[16], char out[25])
{
    int n = b64_encode(nonce, 16, out, 25, 1 /* pad */);
    if (n < 0) { out[0] = 0; return; }
    out[n] = 0;
}

void ws_compute_accept(const char *key, char out[29])
{
    uint8_t buf[24 + 36]; /* key is always 24 base64 chars; GUID is 36 */
    size_t klen = strlen(key);
    if (klen > sizeof buf - 36) klen = sizeof buf - 36; /* never true for a real key */
    memcpy(buf, key, klen);
    memcpy(buf + klen, WS_GUID, 36);
    uint8_t digest[20];
    ocsp_sha1(buf, klen + 36, digest);
    int n = b64_encode(digest, 20, out, 29, 1 /* pad */);
    if (n < 0) { out[0] = 0; return; }
    out[n] = 0;
}

int ws_accept_matches(const char *key, const char *server_accept)
{
    char want[29];
    ws_compute_accept(key, want);
    if (!server_accept) return 0;
    return strcmp(want, server_accept) == 0;
}

/* ---- frame codec ----------------------------------------------------------
 *
 * RFC 6455 5.2:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-------+-+-------------+-------------------------------+
 *  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 *  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 *  |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 *  | |1|2|3|       |K|             |                               |
 *  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 *  |     Extended payload length continued, if payload len == 127  |
 *  + - - - - - - - - - - - - - - - +-------------------------------+
 *  |                               |Masking-key, if MASK set to 1  |
 *  +-------------------------------+-------------------------------+
 *  | Masking-key (continued)       |          Payload Data         |
 *  +--------------------------------- - - - - - - - - - - - - - - -+
 */

void ws_mask_xor(uint8_t *data, size_t len, const uint8_t mask[4])
{
    for (size_t i = 0; i < len; i++) data[i] ^= mask[i & 3];
}

int ws_frame_write(uint8_t *out, size_t outcap, int fin, int opcode,
                    const uint8_t *payload, size_t len, const uint8_t mask[4])
{
    size_t hdr = 2;
    if (len >= 65536) hdr += 8;
    else if (len >= 126) hdr += 2;
    if (mask) hdr += 4;
    if (outcap < hdr + len) return -1;

    uint8_t *p = out;
    *p++ = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0F));
    uint8_t mbit = mask ? 0x80 : 0x00;
    if (len < 126) {
        *p++ = (uint8_t)(mbit | len);
    } else if (len < 65536) {
        *p++ = (uint8_t)(mbit | 126);
        *p++ = (uint8_t)(len >> 8);
        *p++ = (uint8_t)(len);
    } else {
        *p++ = (uint8_t)(mbit | 127);
        for (int i = 7; i >= 0; i--) *p++ = (uint8_t)((uint64_t)len >> (8 * i));
    }
    if (mask) { memcpy(p, mask, 4); p += 4; }
    if (len) memcpy(p, payload, len);
    if (mask) ws_mask_xor(p, len, mask);
    return (int)(hdr + len);
}

enum { WSP_HDR2 = 0, WSP_EXTLEN, WSP_MASK, WSP_PAYLOAD, WSP_DONE };

void ws_parser_init(struct ws_parser *p, size_t max_frame_payload, int require_masked)
{
    memset(p, 0, sizeof *p);
    p->state = WSP_HDR2;
    p->hdrneed = 2;
    p->maxpay = max_frame_payload;
    p->require_masked = require_masked;
}

void ws_parser_free(struct ws_parser *p)
{
    free(p->payload);
    p->payload = 0; p->paycap = 0;
}

void ws_parser_next(struct ws_parser *p)
{
    p->state = WSP_HDR2;
    p->hdrlen = 0;
    p->hdrneed = 2;
    p->payoff = 0;
    /* payload buffer is kept (paycap) and reused by the next frame -- avoids
     * a malloc/free per frame on a chatty connection. */
}

/* Control-frame rules a single frame can be judged by (RFC 6455 5.5): FIN
 * must be 1 (control frames are never fragmented) and the payload is at
 * most 125 bytes. */
static int is_control(int opcode) { return opcode >= 0x8; }

size_t ws_parser_feed(struct ws_parser *p, const uint8_t *data, size_t len, int *status)
{
    size_t consumed = 0;
    *status = WS_P_AGAIN;

    while (consumed < len) {
        if (p->state == WSP_HDR2) {
            while (p->hdrlen < 2 && consumed < len) p->hdrbuf[p->hdrlen++] = data[consumed++];
            if (p->hdrlen < 2) return consumed;

            p->fin    = (p->hdrbuf[0] & 0x80) != 0;
            p->rsv    = (p->hdrbuf[0] >> 4) & 0x07;
            p->opcode = p->hdrbuf[0] & 0x0F;
            p->masked = (p->hdrbuf[1] & 0x80) != 0;
            uint8_t len7 = p->hdrbuf[1] & 0x7F;

            /* Rules judgeable right here, before a single extra byte: */
            if (p->rsv != 0) { *status = WS_P_ERROR; return consumed; }              /* no extension negotiated */
            if (p->opcode != WS_OP_CONT && p->opcode != WS_OP_TEXT && p->opcode != WS_OP_BIN &&
                p->opcode != WS_OP_CLOSE && p->opcode != WS_OP_PING && p->opcode != WS_OP_PONG)
                { *status = WS_P_ERROR; return consumed; }                            /* reserved opcode */
            if (is_control(p->opcode) && (!p->fin || len7 > 125))
                { *status = WS_P_ERROR; return consumed; }                            /* fragmented/oversized control */
            if (!!p->masked != !!p->require_masked) { *status = WS_P_ERROR; return consumed; }

            if (len7 < 126) { p->paylen = len7; p->hdrneed = 0; }
            else if (len7 == 126) p->hdrneed = 2;
            else p->hdrneed = 8;
            p->hdrlen = 0;
            p->state = p->hdrneed ? WSP_EXTLEN : (p->masked ? WSP_MASK : WSP_PAYLOAD);
            if (p->state == WSP_MASK) p->hdrneed = 4;
            continue;
        }

        if (p->state == WSP_EXTLEN) {
            while (p->hdrlen < p->hdrneed && consumed < len) p->hdrbuf[p->hdrlen++] = data[consumed++];
            if (p->hdrlen < p->hdrneed) return consumed;
            uint64_t v = 0;
            for (int i = 0; i < p->hdrneed; i++) v = (v << 8) | p->hdrbuf[i];
            if (p->hdrneed == 8 && (v & (1ull << 63))) { *status = WS_P_ERROR; return consumed; }
            p->paylen = v;
            p->hdrlen = 0;
            p->state = p->masked ? WSP_MASK : WSP_PAYLOAD;
            p->hdrneed = p->masked ? 4 : 0;
            continue;
        }

        if (p->state == WSP_MASK) {
            while (p->hdrlen < 4 && consumed < len) p->hdrbuf[p->hdrlen++] = data[consumed++];
            if (p->hdrlen < 4) return consumed;
            memcpy(p->maskkey, p->hdrbuf, 4);
            p->state = WSP_PAYLOAD;
            continue;
        }

        if (p->state == WSP_PAYLOAD) {
            if (p->paylen > p->maxpay) { *status = WS_P_ERROR; return consumed; }
            if (p->payoff == 0 && p->paylen > 0) {
                if (p->paycap < p->paylen + 1) {
                    uint8_t *nb = realloc(p->payload, (size_t)p->paylen + 1);
                    if (!nb) { *status = WS_P_ERROR; return consumed; }
                    p->payload = nb;
                    p->paycap = (size_t)p->paylen + 1;
                }
            }
            uint64_t need = p->paylen - p->payoff;
            size_t avail = len - consumed;
            size_t take = need < avail ? (size_t)need : avail;
            if (take) {
                memcpy(p->payload + p->payoff, data + consumed, take);
                consumed += take;
                p->payoff += take;
            }
            if (p->payoff < p->paylen) return consumed;   /* WS_P_AGAIN, more payload to go */
            if (p->payload) {
                p->payload[p->paylen] = 0;
                if (p->masked) ws_mask_xor(p->payload, (size_t)p->paylen, p->maskkey);
            }
            p->state = WSP_DONE;
            *status = WS_P_FRAME;
            return consumed;
        }

        /* WSP_DONE: caller must call ws_parser_next() before feeding more. */
        return consumed;
    }
    return consumed;
}

/* ---- UTF-8 validation ----------------------------------------------------
 * Strict decoder: rejects overlong forms, surrogate halves and codepoints
 * past U+10FFFF, per the WHATWG/RFC 3629 profile RFC 6455 8.1 points at. */
int ws_utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = s[i];
        if (b0 < 0x80) { i++; continue; }
        int n; uint32_t cp; uint32_t min;
        if ((b0 & 0xE0) == 0xC0) { n = 1; cp = b0 & 0x1F; min = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { n = 2; cp = b0 & 0x0F; min = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { n = 3; cp = b0 & 0x07; min = 0x10000; }
        else return 0;
        if (i + 1 + (size_t)n > len) return 0;                /* truncated */
        for (int k = 1; k <= n; k++) {
            uint8_t b = s[i + (size_t)k];
            if ((b & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < min) return 0;                              /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;           /* surrogate half */
        if (cp > 0x10FFFF) return 0;
        i += (size_t)n + 1;
    }
    return 1;
}

int ws_close_code_sendable(int code)
{
    if (code == WS_CLOSE_NORMAL || code == WS_CLOSE_GOING_AWAY ||
        code == WS_CLOSE_PROTOCOL_ERROR || code == WS_CLOSE_UNSUPPORTED_DATA ||
        code == WS_CLOSE_INVALID_PAYLOAD || code == WS_CLOSE_POLICY_VIOLATION ||
        code == WS_CLOSE_MESSAGE_TOO_BIG || code == WS_CLOSE_INTERNAL_ERROR)
        return 1;
    if (code >= 3000 && code <= 4999) return 1;
    return 0;
}
