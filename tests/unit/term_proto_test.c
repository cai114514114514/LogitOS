/* Host test for the LRT/1 rich-terminal framing (c/apps/coreutils/logit_rich.h).
 *
 * The property that matters is not "a good frame round-trips" -- it is that
 * NOTHING a hostile or truncated stream can contain makes the parser read out of
 * bounds, spin, or hand the terminal a payload it did not check. The side band
 * is the whole reason a corrupt rich stream cannot corrupt the text screen, so
 * that claim gets tested here rather than asserted in a comment.
 */

#include <stdio.h>
#include <string.h>

#define RT_NO_SYS 1
#include "logit_rich.h"

static int fails;
#define CHK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                           printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned char wire[65536];
static int wire_n;

static void emit(int type, unsigned seq, struct rt_enc *e)
{
    unsigned char h[RT_HDR];
    rt_hdr(h, type, seq, (unsigned)e->n);
    memcpy(wire + wire_n, h, RT_HDR); wire_n += RT_HDR;
    memcpy(wire + wire_n, e->b, (size_t)e->n); wire_n += e->n;
}

/* Feed `wire` to a parser in chunks of `chunk` bytes, collecting frame types. */
static int drain(struct rt_parser *p, int chunk, int *types, char (*first)[64], int maxf)
{
    int nf = 0, off = 0;
    while (off < wire_n || 1) {
        struct rt_frame f;
        while (rt_parser_next(p, &f)) {
            if (nf < maxf) {
                types[nf] = f.type;
                struct rt_rd r; rt_rd_init(&r, &f);
                if (f.type == 100) rt_rd_str(&r, first[nf], 64);
                else first[nf][0] = 0;
            }
            nf++;
            rt_parser_done(p, &f);
        }
        if (off >= wire_n) break;
        int n = wire_n - off; if (n > chunk) n = chunk;
        int took = rt_parser_feed(p, wire + off, n);
        if (took == 0) break;                 /* buffer wedged: the loop above drains it */
        off += took;
    }
    return nf;
}

int main(void)
{
    static struct rt_parser p;
    static struct rt_enc e;
    int types[64];
    static char first[64][64];

    /* ---- 1. round trip, and every chunk size gives the same answer ------- */
    for (int chunk = 1; chunk <= 64; chunk *= 2) {
        wire_n = 0;
        rt_reset(&e); rt_str(&e, "hello");            emit(100, 0, &e);
        rt_reset(&e); rt_u32(&e, 7); rt_str(&e, "ls"); emit(RT_T_CMD_BEGIN, 11, &e);
        rt_reset(&e);                                  emit(RT_C_INTR, 0, &e);
        rt_parser_init(&p);
        int n = drain(&p, chunk, types, first, 64);
        CHK(n == 3, "chunk %d: got %d frames, want 3", chunk, n);
        if (n == 3) {
            CHK(types[0] == 100 && types[1] == RT_T_CMD_BEGIN && types[2] == RT_C_INTR,
                "chunk %d: types %d %d %d", chunk, types[0], types[1], types[2]);
            CHK(strcmp(first[0], "hello") == 0, "chunk %d: payload '%s'", chunk, first[0]);
        }
    }

    /* ---- 2. seq survives ------------------------------------------------- */
    {
        wire_n = 0;
        rt_reset(&e); rt_str(&e, "x"); emit(100, 0xDEADBEEF, &e);
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, wire_n);
        struct rt_frame f;
        CHK(rt_parser_next(&p, &f), "seq frame not parsed");
        CHK(f.seq == 0xDEADBEEFu, "seq = %u", f.seq);
        rt_parser_done(&p, &f);
    }

    /* ---- 3. garbage before a frame is skipped, not misread ---------------- */
    {
        wire_n = 0;
        const char *junk = "plain text that is not a frame at all LRT\x02 near-miss ";
        memcpy(wire, junk, strlen(junk)); wire_n = (int)strlen(junk);
        rt_reset(&e); rt_str(&e, "after"); emit(100, 0, &e);
        rt_parser_init(&p);
        int n = drain(&p, 7, types, first, 64);
        CHK(n == 1 && types[0] == 100, "garbage prefix: %d frames", n);
        CHK(strcmp(first[0], "after") == 0, "garbage prefix payload '%s'", first[0]);
        CHK(p.skipped > 0, "nothing counted as skipped");
    }

    /* ---- 4. a truncated frame produces NOTHING and blocks nothing --------- */
    {
        wire_n = 0;
        rt_reset(&e); rt_str(&e, "complete"); emit(100, 0, &e);
        int keep = wire_n - 3;                       /* cut the payload short */
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, keep);
        struct rt_frame f;
        CHK(!rt_parser_next(&p, &f), "truncated frame was accepted");
        /* the rest arrives later: it must then parse normally */
        rt_parser_feed(&p, wire + keep, wire_n - keep);
        CHK(rt_parser_next(&p, &f), "completed frame not parsed");
        rt_parser_done(&p, &f);
    }

    /* ---- 5. an impossible length is garbage, not a promise ---------------- */
    {
        unsigned char h[RT_HDR];
        rt_hdr(h, 100, 0, 0xFFFFFFFFu);              /* len the buffer can never hold */
        wire_n = 0;
        memcpy(wire, h, RT_HDR); wire_n = RT_HDR;
        rt_reset(&e); rt_str(&e, "sane"); emit(100, 0, &e);
        rt_parser_init(&p);
        int n = drain(&p, 5, types, first, 64);
        CHK(n == 1 && strcmp(first[0], "sane") == 0, "oversize len: %d frames, '%s'", n, first[0]);
        CHK(p.badlen == 1, "badlen = %ld", p.badlen);
    }

    /* ---- 6. a magic straddling a chunk boundary is still found ------------ */
    {
        wire_n = 0;
        wire[wire_n++] = 'x'; wire[wire_n++] = 'L'; wire[wire_n++] = 'R';   /* decoy */
        rt_reset(&e); rt_str(&e, "straddle"); emit(100, 0, &e);
        rt_parser_init(&p);
        int n = 0, off = 0;
        while (off < wire_n) {
            rt_parser_feed(&p, wire + off, 2);        /* two bytes at a time */
            off += 2;
            struct rt_frame f;
            while (rt_parser_next(&p, &f)) { types[n] = f.type;
                struct rt_rd r; rt_rd_init(&r, &f); rt_rd_str(&r, first[n], 64);
                n++; rt_parser_done(&p, &f); }
        }
        CHK(n == 1 && strcmp(first[0], "straddle") == 0, "straddle: %d frames '%s'", n, first[0]);
    }

    /* ---- 7. a well-framed but SHORT payload underflows safely ------------- */
    {
        wire_n = 0;
        rt_reset(&e); rt_u16(&e, 3);                 /* claims 3 columns... */
        emit(RT_T_TABLE, 0, &e);                     /* ...and stops there   */
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, wire_n);
        struct rt_frame f;
        CHK(rt_parser_next(&p, &f), "short table frame not parsed");
        struct rt_rd r; rt_rd_init(&r, &f);
        int nc = rt_rd_u16(&r);
        int nr = rt_rd_u16(&r);                      /* past the end */
        char title[32];
        int tl = rt_rd_str(&r, title, sizeof title);
        CHK(nc == 3, "ncols %d", nc);
        CHK(nr == 0 && r.bad, "underflow not flagged (nr=%d bad=%d)", nr, r.bad);
        CHK(tl == 0 && title[0] == 0, "underflowed string not emptied");
        rt_parser_done(&p, &f);
    }

    /* ---- 8. a string longer than the destination truncates, never overflows */
    {
        wire_n = 0;
        rt_reset(&e); rt_str(&e, "0123456789abcdefghij"); emit(100, 0, &e);
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, wire_n);
        struct rt_frame f;
        rt_parser_next(&p, &f);
        struct rt_rd r; rt_rd_init(&r, &f);
        char small[8];
        char canary[8];
        memset(canary, 0x5A, sizeof canary);
        int k = rt_rd_str(&r, small, (int)sizeof small);
        CHK(k == 7 && strcmp(small, "0123456") == 0, "truncation gave %d '%s'", k, small);
        for (unsigned i = 0; i < sizeof canary; i++) CHK(canary[i] == 0x5A, "canary clobbered");
        rt_parser_done(&p, &f);
    }

    /* ---- 9. encoder refuses to emit an overflowed payload ----------------- */
    {
        rt_reset(&e);
        for (int i = 0; i < RT_MAX_PAYLOAD; i++) rt_u8(&e, 0x41);
        CHK(!e.ovf, "overflowed too early at exactly RT_MAX_PAYLOAD");
        rt_u8(&e, 0x42);
        CHK(e.ovf, "overflow not flagged");
        CHK(e.n == RT_MAX_PAYLOAD, "overflow still appended (n=%d)", e.n);
    }

    /* ---- 10. a stream of pure noise never yields a frame, and terminates -- */
    {
        rt_parser_init(&p);
        unsigned seed = 12345;
        int frames = 0;
        for (int round = 0; round < 400; round++) {
            unsigned char buf[128];
            for (unsigned i = 0; i < sizeof buf; i++) {
                seed = seed * 1103515245u + 12345u;
                buf[i] = (unsigned char)(seed >> 16);
            }
            int off = 0;
            while (off < (int)sizeof buf) {
                int took = rt_parser_feed(&p, buf + off, (int)sizeof buf - off);
                struct rt_frame f;
                while (rt_parser_next(&p, &f)) {
                    /* a random 4-byte magic hit is possible; it must still be
                     * bounded and self-consistent rather than a wild read */
                    CHK(f.len >= 0 && f.len <= RT_MAX_PAYLOAD, "noise frame len %d", f.len);
                    frames++;
                    rt_parser_done(&p, &f);
                }
                if (took == 0) break;
                off += took;
            }
        }
        printf("  noise: %d accidental frames over 51200 random bytes\n", frames);
    }

    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails != 0;
}
