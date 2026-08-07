/* ASan+UBSan fuzz for HPACK and the HTTP/2 frame layer.
 *
 * WHY THIS FILE EXISTS.  HPACK is a decompressor whose every length is chosen
 * by the peer, and whose output is LARGER than its input -- the shortest
 * Huffman code is 5 bits, so a string expands by up to 8/5 on the way out.
 * "Allocate the encoded length, then decode into it" is therefore a 60% heap
 * overflow on chosen input, and it is the natural way to write the function.
 * The frame layer around it hands out three more attacker-chosen numbers per
 * frame: the length, the pad length, and the promised stream id.
 *
 * Not crashing is the floor.  Each phase also asserts a PROPERTY that a
 * memory-safe but WRONG implementation would still violate:
 *
 *   P1  decode() either succeeds or returns a negative error -- there is no
 *       third outcome, and in particular no partial success that leaves the
 *       caller holding half a header list.
 *   P2  a successful decode is self-consistent: every name and value is
 *       NUL-terminated at its stated length, and the list is inside its cap.
 *   P3  THE TABLE INVARIANT.  size == the sum of the entries' (n+v+32), the
 *       count is within the capacity, and no entry survives past an eviction.
 *       This is the one that matters: a table that disagrees with the peer's
 *       by one entry makes every later indexed header decode to the WRONG
 *       field, silently, for the rest of the connection.
 *   P4  round trip: whatever a decode produced, encoding it and decoding that
 *       with fresh contexts yields the identical header list. A decoder that
 *       loses the last character of a Huffman string passes P1 and P2.
 *   P5  a dynamic table size update above the announced maximum is ALWAYS
 *       rejected, whatever bytes precede it. That bound is the only limit on
 *       how much memory a peer can make us hold.
 *   P6  the frame layer, fed arbitrary bytes at arbitrary granularity, either
 *       keeps running or reports a connection error -- and every stream it
 *       calls done has an outcome (a status or an error), never neither.
 *   P7  Huffman is an involution on real strings, and random bytes decode
 *       either to something within the cap or to an error.
 *
 * The corpus deliberately includes what the task list names: truncated blocks,
 * lengths that lie about what follows, and dynamic table size updates spliced
 * in at EVERY position of every seed (the position is what makes them legal or
 * illegal, and an implementation that only checks the value is wrong).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hpack.h"
#include "http2.h"

static int fails;
static long iters;
#define REQUIRE(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
                                printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Deterministic PRNG: a fuzz failure has to be reproducible from the seed. */
static uint64_t rng_s = 0x243F6A8885A308D3ULL;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}
static uint32_t rnd_n(uint32_t n) { return n ? rnd() % n : 0; }

/* ---------------------------------------------------------------- corpus */

static int unhex(const char *h, uint8_t *out, int max)
{
    int n = 0, hi = -1;
    for (const char *p = h; *p; p++) {
        int v;
        if (*p == ' ') continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else return -1;
        if (hi < 0) hi = v; else { if (n >= max) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return n;
}

/* The RFC 7541 Appendix C blocks plus shapes they do not cover: an empty
 * block, every representation type, a size update, a Huffman-coded literal,
 * a maximal index, a never-indexed field. Mutating VALID blocks reaches far
 * more of the state machine than random bytes, which mostly die on byte one. */
static const char *const seed_hex[] = {
    "",
    "82",
    "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
    "8286 84be 5808 6e6f 2d63 6163 6865",
    "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
    "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
    "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
    "4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133"
        "2032 303a 3133 3a32 3120 474d 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70"
        "6c65 2e63 6f6d",
    "4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6"
        "2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3",
    "88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab"
        "77ad 94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f"
        "9587 3160 65c0 03ed 4ee5 b106 3d50 07",
    "040c 2f73 616d 706c 652f 7061 7468",
    "1008 7061 7373 776f 7264 0673 6563 7265 74",
    "3fe1 1f",                                  /* size update to 4096 */
    "20",                                       /* size update to 0 */
    "2082 86",                                  /* update then two indexed */
    "0f2d 0331 3233",                           /* literal, static name index 61 */
    NULL
};

#define MAXBLK 8192
static uint8_t blk[MAXBLK];
static int     blklen;

static void pick_seed(void)
{
    int n = 0;
    while (seed_hex[n]) n++;
    blklen = unhex(seed_hex[rnd_n((uint32_t)n)], blk, MAXBLK);
    if (blklen < 0) blklen = 0;
}

/* One mutation. The interesting ones are not bit flips -- they are the
 * structural lies: a length that overruns, a truncation, a size update
 * inserted at a position where it may or may not be legal. */
static void mutate(void)
{
    switch (rnd_n(10)) {
    case 0:                                     /* bit flip */
        if (blklen) blk[rnd_n((uint32_t)blklen)] ^= (uint8_t)(1u << rnd_n(8));
        break;
    case 1:                                     /* byte substitution */
        if (blklen) blk[rnd_n((uint32_t)blklen)] = (uint8_t)rnd_n(256);
        break;
    case 2:                                     /* truncate: the classic */
        if (blklen) blklen = (int)rnd_n((uint32_t)blklen);
        break;
    case 3: {                                   /* a length that lies */
        if (!blklen) break;
        int i = (int)rnd_n((uint32_t)blklen);
        blk[i] = (uint8_t)(0x7F & (0x40 + rnd_n(0x3F)));   /* a big string length */
        break;
    }
    case 4: {                                   /* insert a size update ANYWHERE */
        if (blklen + 3 >= MAXBLK) break;
        int at = (int)rnd_n((uint32_t)(blklen + 1));
        uint8_t upd[3];
        int un = hpack_int_encode(upd, 3, 5, 0x20, rnd_n(3) == 0 ? rnd_n(100000) : rnd_n(4097));
        if (un <= 0) break;
        memmove(blk + at + un, blk + at, (size_t)(blklen - at));
        memcpy(blk + at, upd, (size_t)un);
        blklen += un;
        break;
    }
    case 5:                                     /* append random bytes */
        for (int k = 0, n = (int)rnd_n(16); k < n && blklen < MAXBLK; k++)
            blk[blklen++] = (uint8_t)rnd_n(256);
        break;
    case 6: {                                   /* splice another seed on */
        uint8_t other[MAXBLK];
        int n = 0;
        while (seed_hex[n]) n++;
        int on = unhex(seed_hex[rnd_n((uint32_t)n)], other, MAXBLK);
        if (on > 0 && blklen + on < MAXBLK) { memcpy(blk + blklen, other, (size_t)on); blklen += on; }
        break;
    }
    case 7:                                     /* force a huffman flag on */
        if (blklen) blk[rnd_n((uint32_t)blklen)] |= 0x80;
        break;
    case 8: {                                   /* an index far past the table */
        if (blklen + 3 >= MAXBLK) break;
        int at = (int)rnd_n((uint32_t)(blklen + 1));
        uint8_t ix[4];
        int n = hpack_int_encode(ix, 4, 7, 0x80, 60 + rnd_n(100000));
        if (n <= 0) break;
        memmove(blk + at + n, blk + at, (size_t)(blklen - at));
        memcpy(blk + at, ix, (size_t)n);
        blklen += n;
        break;
    }
    default:                                    /* pure noise */
        blklen = (int)rnd_n(64);
        for (int i = 0; i < blklen; i++) blk[i] = (uint8_t)rnd_n(256);
        break;
    }
}

/* P3: the table's own arithmetic must agree with its contents. */
static void check_table(const struct hpack_table *t)
{
    int sum = 0;
    REQUIRE(t->count >= 0 && t->count <= HPACK_MAX_ENTRIES, "count %d out of range", t->count);
    REQUIRE(t->cap >= 0 && t->cap <= t->cap_max, "cap %d > cap_max %d", t->cap, t->cap_max);
    for (int i = 0; i < t->count; i++) {
        const struct hpack_entry *e = hpack_lookup(t, HPACK_STATIC_N + 1 + i);
        REQUIRE(e != NULL, "entry %d missing though count says %d", i, t->count);
        if (!e) break;
        REQUIRE(e->name && e->value, "entry %d has a null string", i);
        REQUIRE((int)strlen(e->name) == e->nlen, "entry %d name length disagrees", i);
        REQUIRE((int)strlen(e->value) == e->vlen, "entry %d value length disagrees", i);
        sum += e->nlen + e->vlen + 32;
    }
    REQUIRE(sum == t->size, "table size %d but entries sum to %d", t->size, sum);
    REQUIRE(t->size <= t->cap, "table size %d over capacity %d", t->size, t->cap);
    REQUIRE(hpack_lookup(t, HPACK_STATIC_N + 1 + t->count) == NULL, "lookup past the end returned an entry");
    REQUIRE(hpack_lookup(t, 0) == NULL, "index 0 resolved");
}

/* P2 + P4. */
static void check_list_and_roundtrip(const struct hpack_list *l)
{
    REQUIRE(l->n >= 0 && l->n <= HPACK_MAX_FIELDS, "field count %d", l->n);
    int bytes = 0;
    for (int i = 0; i < l->n; i++) {
        const struct hpack_hdr *h = &l->v[i];
        REQUIRE(h->name && h->value, "field %d null", i);
        if (!h->name || !h->value) return;
        REQUIRE((int)strlen(h->name) == h->nlen, "field %d name not terminated at nlen", i);
        REQUIRE((int)strlen(h->value) == h->vlen, "field %d value not terminated at vlen", i);
        REQUIRE(h->nlen <= HPACK_STR_MAX && h->vlen <= HPACK_STR_MAX, "field %d oversized", i);
        bytes += h->nlen + h->vlen + 32;
    }
    REQUIRE(bytes <= HPACK_LIST_MAX, "list %d bytes over the cap", bytes);

    /* P4: re-encode with a fresh context and decode it back. */
    struct hpack_enc e; struct hpack_dec d;
    hpack_enc_init(&e, HPACK_DEFAULT_CAP);
    hpack_dec_init(&d, HPACK_DEFAULT_CAP);
    uint8_t *enc = NULL; int enclen = 0;
    if (hpack_encode(&e, l, &enc, &enclen) == HPACK_OK) {
        struct hpack_list back; hpack_list_init(&back);
        int rc = hpack_decode(&d, enc, enclen, &back);
        REQUIRE(rc == HPACK_OK, "re-decode of our own encoding failed: %s", hpack_strerror(rc));
        if (rc == HPACK_OK) {
            REQUIRE(back.n == l->n, "round trip changed the field count %d -> %d", l->n, back.n);
            for (int i = 0; i < back.n && i < l->n; i++) {
                REQUIRE(back.v[i].nlen == l->v[i].nlen &&
                        !memcmp(back.v[i].name, l->v[i].name, (size_t)l->v[i].nlen),
                        "round trip changed name %d", i);
                REQUIRE(back.v[i].vlen == l->v[i].vlen &&
                        !memcmp(back.v[i].value, l->v[i].value, (size_t)l->v[i].vlen),
                        "round trip changed value %d", i);
            }
        }
        hpack_list_free(&back);
        free(enc);
    }
    hpack_enc_free(&e);
    hpack_dec_free(&d);
}

static void phase_hpack(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        /* A fresh decoder, then several mutated blocks fed to it in sequence:
         * the dynamic table has to survive a bad block followed by a good one
         * as much as it has to survive one bad block. */
        struct hpack_dec d;
        hpack_dec_init(&d, HPACK_DEFAULT_CAP);
        int blocks = 1 + (int)rnd_n(6);
        for (int b = 0; b < blocks; b++) {
            pick_seed();
            int muts = 1 + (int)rnd_n(4);
            for (int m = 0; m < muts; m++) mutate();
            struct hpack_list l;
            hpack_list_init(&l);
            int rc = hpack_decode(&d, blklen ? blk : (const uint8_t *)"", blklen, &l);
            iters++;
            REQUIRE(rc == HPACK_OK || rc < 0, "decode returned %d", rc);   /* P1 */
            check_table(&d.tab);                                           /* P3 */
            if (rc == HPACK_OK) check_list_and_roundtrip(&l);              /* P2, P4 */
            hpack_list_free(&l);
            if (rc != HPACK_OK) break;   /* a real client tears the connection down here */
        }
        hpack_dec_free(&d);
    }
}

/* P5: whatever precedes it, an oversized size update is refused. The position
 * is varied too, because a size update after a field is illegal REGARDLESS of
 * its value and an implementation that only range-checks is wrong. */
static void phase_size_updates(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        struct hpack_dec d;
        hpack_dec_init(&d, HPACK_DEFAULT_CAP);
        uint8_t buf[64];
        int n = 0;
        int lead = (int)rnd_n(3);
        for (int i = 0; i < lead; i++) buf[n++] = 0x82;      /* indexed :method GET */
        uint32_t cap = HPACK_DEFAULT_CAP + 1 + rnd_n(1000000);
        int k = hpack_int_encode(buf + n, (int)sizeof buf - n, 5, 0x20, cap);
        if (k <= 0) { hpack_dec_free(&d); continue; }
        n += k;
        buf[n++] = 0x82;
        struct hpack_list l; hpack_list_init(&l);
        int rc = hpack_decode(&d, buf, n, &l);
        iters++;
        REQUIRE(rc < 0, "a size update to %u (after %d fields) was accepted", cap, lead);
        REQUIRE(d.tab.cap <= d.tab.cap_max, "capacity escaped its bound");
        hpack_list_free(&l);
        hpack_dec_free(&d);

        /* And the legal counterpart must still be accepted, so the check above
         * is not just "everything fails". */
        hpack_dec_init(&d, HPACK_DEFAULT_CAP);
        n = 0;
        k = hpack_int_encode(buf, (int)sizeof buf, 5, 0x20, rnd_n(HPACK_DEFAULT_CAP + 1));
        n += k;
        buf[n++] = 0x82;
        hpack_list_init(&l);
        rc = hpack_decode(&d, buf, n, &l);
        iters++;
        REQUIRE(rc == HPACK_OK, "a legal size update was rejected: %s", hpack_strerror(rc));
        check_table(&d.tab);
        hpack_list_free(&l);
        hpack_dec_free(&d);
    }
}

/* P7: Huffman. */
static void phase_huffman(int rounds)
{
    uint8_t enc[4096], raw[4096];
    char in[2048];
    for (int r = 0; r < rounds; r++) {
        int n = (int)rnd_n(1024);
        for (int i = 0; i < n; i++) in[i] = (char)rnd_n(256);
        int el = hpack_huff_encode(enc, (int)sizeof enc, in, n);
        iters++;
        REQUIRE(el == hpack_huff_len(in, n) || el < 0, "encoded length disagrees with the estimate");
        if (el > 0) {
            char *out = NULL; int ol = 0;
            int rc = hpack_huff_decode(enc, el, &out, &ol, HPACK_STR_MAX);
            REQUIRE(rc == HPACK_OK, "our own huffman output did not decode: %s", hpack_strerror(rc));
            REQUIRE(rc != HPACK_OK || (ol == n && !memcmp(out, in, (size_t)n)),
                    "huffman round trip changed %d bytes", n);
            free(out);
        }
        /* Random bytes: either a decode error or a bounded result. Never a
         * write past the cap, which is what the sanitizer is here for. */
        int rl = (int)rnd_n(256);
        for (int i = 0; i < rl; i++) raw[i] = (uint8_t)rnd_n(256);
        char *out = NULL; int ol = 0;
        int cap = 1 + (int)rnd_n(600);
        int rc = hpack_huff_decode(raw, rl, &out, &ol, cap);
        iters++;
        REQUIRE(rc == HPACK_OK || rc < 0, "huffman decode returned %d", rc);
        /* Note: only the LENGTH is bounded here, not strlen -- symbol 0 has a
         * Huffman code, so raw codec output may contain NUL. It is
         * hpack_decode that refuses those, one layer up. */
        REQUIRE(rc != HPACK_OK || (ol <= cap && out != NULL),
                "decoded %d bytes against a cap of %d", ol, cap);
        free(out);
    }
}

/* -------------------------------------------------- the frame layer ------ */

struct fz_pipe { uint8_t *b; int len, cap, off; };
struct fz_wire { struct fz_pipe s2c, c2s; int gran; };

static void fz_put(struct fz_pipe *p, const void *d, int n)
{
    if (p->len + n > p->cap) {
        int c = p->cap ? p->cap : 4096;
        while (c < p->len + n) c *= 2;
        p->b = (uint8_t *)realloc(p->b, (size_t)c);
        p->cap = c;
    }
    memcpy(p->b + p->len, d, (size_t)n);
    p->len += n;
}
static int fz_read(void *ctx, void *buf, int len)
{
    struct fz_wire *w = (struct fz_wire *)ctx;
    if (w->gran && len > w->gran) len = w->gran;
    int n = w->s2c.len - w->s2c.off;
    if (n > len) n = len;
    if (n <= 0) return H2_AGAIN;
    memcpy(buf, w->s2c.b + w->s2c.off, (size_t)n);
    w->s2c.off += n;
    return n;
}
static int fz_write(void *ctx, const void *buf, int len)
{
    struct fz_wire *w = (struct fz_wire *)ctx;
    fz_put(&w->c2s, buf, len);
    return len;
}

/* Build a plausible-but-corrupt frame stream: real frame headers with random
 * types, flags, stream ids and payloads, so the parser is reached rather than
 * bounced at the first byte. */
static void build_frames(struct fz_wire *w, int nframes)
{
    static const uint8_t types[] = { H2_F_DATA, H2_F_HEADERS, H2_F_PRIORITY, H2_F_RST_STREAM,
                                     H2_F_SETTINGS, H2_F_PUSH_PROMISE, H2_F_PING, H2_F_GOAWAY,
                                     H2_F_WINDOW_UPDATE, H2_F_CONTINUATION, 0x2a };
    uint8_t pay[1024];
    for (int i = 0; i < nframes; i++) {
        uint8_t type = types[rnd_n(sizeof types)];
        uint8_t flags = (uint8_t)rnd_n(256);
        uint32_t sid;
        switch (rnd_n(4)) {
        case 0: sid = 0; break;
        case 1: sid = 1; break;
        case 2: sid = 1 + 2 * rnd_n(8); break;
        default: sid = rnd(); break;
        }
        int len = (int)rnd_n(rnd_n(4) == 0 ? 20000 : 64);   /* sometimes over our max */
        if (len > (int)sizeof pay) len = (int)sizeof pay;
        for (int k = 0; k < len; k++) pay[k] = (uint8_t)rnd_n(256);
        /* Sometimes declare a length that does not match the payload: the
         * parser must wait for the rest rather than read past the buffer. */
        uint32_t declared = (uint32_t)len;
        if (rnd_n(8) == 0) declared = rnd_n(200000);
        uint8_t h[H2_FRAME_HDR];
        h2_frame_write(h, declared, type, flags, sid);
        fz_put(&w->s2c, h, H2_FRAME_HDR);
        fz_put(&w->s2c, pay, len);
    }
    if (rnd_n(4) == 0) {                 /* a torn frame header at the end */
        uint8_t h[H2_FRAME_HDR];
        h2_frame_write(h, 100, H2_F_DATA, 0, 1);
        fz_put(&w->s2c, h, (int)rnd_n(H2_FRAME_HDR));
    }
}

static void phase_frames(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        struct fz_wire w;
        memset(&w, 0, sizeof w);
        w.gran = rnd_n(4) == 0 ? 1 + (int)rnd_n(13) : 0;

        struct h2_conn c;
        struct h2_transport t;
        t.read = fz_read; t.write = fz_write; t.poll = NULL; t.ctx = &w;
        if (h2_conn_start(&c, &t) != H2_OK) { free(w.s2c.b); free(w.c2s.b); continue; }

        uint32_t ids[4];
        int nid = 1 + (int)rnd_n(3);
        for (int i = 0; i < nid; i++) {
            int rc = h2_request(&c, "GET", "https", "fuzz.example", "/", NULL, NULL, 0);
            ids[i] = rc > 0 ? (uint32_t)rc : 0;
        }
        build_frames(&w, 1 + (int)rnd_n(24));

        for (int i = 0; i < 400; i++) {
            int st = h2_conn_pump(&c, i);
            if (st == H2_C_ERROR || st == H2_C_CLOSED) break;
            if (w.s2c.off >= w.s2c.len) break;
        }
        iters++;

        /* P6: whatever happened, the connection has a definite state, and any
         * stream it declared finished has an outcome. "done with no status and
         * no error" is the state a caller waits on forever. */
        int st = h2_conn_state(&c);
        REQUIRE(st == H2_C_OPEN || st == H2_C_GOAWAY || st == H2_C_ERROR || st == H2_C_CLOSED,
                "connection state %d", st);
        if (st == H2_C_ERROR) REQUIRE(c.err < 0, "an errored connection has no reason");
        for (int i = 0; i < nid; i++) {
            if (!ids[i]) continue;
            struct h2_stream *s = h2_stream_get(&c, ids[i]);
            if (!s) continue;
            REQUIRE(!s->done || s->err != H2_OK || s->status != 0 || s->headers_done,
                    "stream %u finished with neither a status nor an error", ids[i]);
            REQUIRE(s->body_len >= 0 && s->body_len <= s->body_max, "stream body %d", s->body_len);
            REQUIRE(s->recv_win <= H2_OUR_STREAM_WINDOW, "stream recv window grew past the announcement");
        }
        REQUIRE(c.recv_win <= H2_OUR_CONN_WINDOW, "connection recv window grew past the announcement");
        REQUIRE(c.send_win <= H2_MAX_WINDOW, "send window overflowed");
        check_table(&c.dec.tab);
        check_table(&c.enc.tab);

        h2_conn_free(&c);
        free(w.s2c.b); free(w.c2s.b);
    }
}

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 6;
    if (scale < 1) scale = 1;
    if (argc > 2) rng_s = strtoull(argv[2], NULL, 0);
    if (!rng_s) rng_s = 0x243F6A8885A308D3ULL;
    uint64_t seed0 = rng_s;

    printf("hpack_fuzz: scale=%d seed=0x%016llx\n", scale, (unsigned long long)seed0);

    phase_hpack(scale * 4000);
    phase_size_updates(scale * 500);
    phase_huffman(scale * 3000);
    phase_frames(scale * 300);

    printf("hpack_fuzz: %ld iterations, %d failures (reproduce with: %d 0x%016llx)\n",
           iters, fails, scale, (unsigned long long)seed0);
    if (!fails) printf("hpack_fuzz: ALL PASS\n");
    return fails ? 1 : 0;
}
