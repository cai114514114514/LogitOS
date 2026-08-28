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

    /* ---- 11. RT_T_LM_* round trip -- the streamed-model-output frames ----
     *
     * This is the control test-lm-os cannot be: it needs no model, no QEMU,
     * runs in milliseconds, and its failure mode is exactly the "one jar, two
     * doors" trap CLAUDE.md names -- lm.c (the producer) and terminal.c (the
     * consumer) each spell the wire layout independently in a comment, and
     * nothing before this test made sure the two comments agreed with each
     * other or with logit_rich.h's own #defines. */
    {
        wire_n = 0;
        rt_reset(&e); rt_u32(&e, 7); rt_str(&e, "toy 4L d64"); rt_str(&e, "why is the sky");
        emit(RT_T_LM_BEGIN, 0, &e);
        rt_reset(&e); rt_u32(&e, 7); rt_strn(&e, "\x00", 1);   /* a real NUL token byte */
        emit(RT_T_LM_TOKEN, 0, &e);
        rt_reset(&e); rt_u32(&e, 7); rt_str(&e, " blue");
        emit(RT_T_LM_TOKEN, 0, &e);
        rt_reset(&e); rt_u32(&e, 7); rt_u8(&e, RT_LM_INTERRUPTED);
        rt_u32(&e, 2); rt_u32(&e, 1500); rt_u32(&e, 0);
        emit(RT_T_LM_END, 0, &e);
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, wire_n);

        struct rt_frame f;
        CHK(rt_parser_next(&p, &f) && f.type == RT_T_LM_BEGIN, "LM_BEGIN not parsed");
        struct rt_rd r; rt_rd_init(&r, &f);
        unsigned id = rt_rd_u32(&r);
        char model[32], prompt[32];
        rt_rd_str(&r, model, sizeof model);
        rt_rd_str(&r, prompt, sizeof prompt);
        CHK(!r.bad && id == 7, "LM_BEGIN id = %u bad=%d", id, r.bad);
        CHK(strcmp(model, "toy 4L d64") == 0, "LM_BEGIN model '%s'", model);
        CHK(strcmp(prompt, "why is the sky") == 0, "LM_BEGIN prompt '%s'", prompt);
        rt_parser_done(&p, &f);

        CHK(rt_parser_next(&p, &f) && f.type == RT_T_LM_TOKEN, "1st LM_TOKEN not parsed");
        rt_rd_init(&r, &f);
        (void)rt_rd_u32(&r);
        char tok[4]; int tn = rt_rd_str(&r, tok, sizeof tok);
        CHK(!r.bad && tn == 1 && tok[0] == 0, "NUL token mishandled (n=%d byte=%d)", tn, tok[0]);
        rt_parser_done(&p, &f);

        CHK(rt_parser_next(&p, &f) && f.type == RT_T_LM_TOKEN, "2nd LM_TOKEN not parsed");
        rt_rd_init(&r, &f);
        (void)rt_rd_u32(&r);
        rt_rd_str(&r, tok, sizeof tok);
        CHK(!r.bad && strcmp(tok, " bl") == 0, "2nd token truncated to '%s'", tok); /* dst is 4 bytes */
        rt_parser_done(&p, &f);

        CHK(rt_parser_next(&p, &f) && f.type == RT_T_LM_END, "LM_END not parsed");
        rt_rd_init(&r, &f);
        (void)rt_rd_u32(&r);
        int fl = rt_rd_u8(&r);
        unsigned ntok = rt_rd_u32(&r), ms = rt_rd_u32(&r), nf = rt_rd_u32(&r);
        CHK(!r.bad, "LM_END underflowed");
        CHK(fl == RT_LM_INTERRUPTED, "LM_END flags = %d", fl);
        CHK(ntok == 2 && ms == 1500 && nf == 0, "LM_END fields %u %u %u", ntok, ms, nf);
        rt_parser_done(&p, &f);
    }

    /* ---- 12. LM_TOKEN/LM_END for a SUPERSEDED id must be REJECTED by the
     * consumer, not just parsed -- this is terminal.c's own guard
     * (lm_open_ / lm_id_ in handle_frame), and it cannot be exercised by the
     * wire-level parser above: the parser has no notion of "which block is
     * open", that state lives in terminal.c. What this test CAN prove at this
     * layer is the wire-level half of the property the guard depends on: an
     * id is just an opaque u32 on the wire, so two frames claiming different
     * ids decode to different ids rather than being coalesced or confused --
     * if that were false, terminal.c's id check would have nothing to check
     * against. */
    {
        wire_n = 0;
        rt_reset(&e); rt_u32(&e, 1); rt_str(&e, "first");  emit(RT_T_LM_TOKEN, 0, &e);
        rt_reset(&e); rt_u32(&e, 2); rt_str(&e, "second"); emit(RT_T_LM_TOKEN, 0, &e);
        rt_parser_init(&p);
        rt_parser_feed(&p, wire, wire_n);
        struct rt_frame f; struct rt_rd r;
        rt_parser_next(&p, &f); rt_rd_init(&r, &f);
        unsigned id1 = rt_rd_u32(&r);
        rt_parser_done(&p, &f);
        rt_parser_next(&p, &f); rt_rd_init(&r, &f);
        unsigned id2 = rt_rd_u32(&r);
        rt_parser_done(&p, &f);
        CHK(id1 == 1 && id2 == 2 && id1 != id2, "LM_TOKEN ids collapsed: %u %u", id1, id2);
    }

    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails != 0;
}
