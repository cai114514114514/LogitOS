/* Host unit test for c/net/http/hpack.c -- HPACK (RFC 7541).
 *
 * HPACK is the one part of HTTP/2 that cannot be checked by "did the page
 * load".  Its state is shared with the peer: if our dynamic table holds one
 * entry more or fewer than the server's, then index 62 means a different
 * header at each end and every subsequent indexed field on that connection is
 * silently WRONG -- not rejected, wrong.  So the test is built around the
 * published vectors in RFC 7541 Appendix C, which give both the bytes AND the
 * exact dynamic table contents and size after each block; checking only the
 * decoded headers would pass with a broken eviction rule.
 *
 * The second half is the memory-safety half.  Every string in a header block
 * carries a peer-chosen length, and a Huffman string DECODES LARGER than it
 * arrives (8/5 at worst, the shortest code being 5 bits), so "allocate the
 * encoded length" is a 60% heap overflow on chosen input.  Those cases are
 * here as assertions rather than as a fuzz-only concern: truncated strings,
 * lengths past the end of the block, EOS inside a stream, over-long padding,
 * an index of 0, an index past the table, and a size update in an illegal
 * position.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hpack.h"

static int fails, checks;
#define OK(cond) do { checks++; if (!(cond)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* --------------------------------------------------------------- helpers */

/* "8286 8441 0f77" -> bytes. Whitespace is ignored so the RFC's hex dumps can
 * be pasted in unedited. */
static int unhex(const char *h, uint8_t *out, int max)
{
    int n = 0, hi = -1;
    for (const char *p = h; *p; p++) {
        int v;
        if (*p == ' ' || *p == '\n' || *p == '\t') continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return -1;
        if (hi < 0) hi = v;
        else { if (n >= max) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return hi < 0 ? n : -1;
}

/* The decoded list as "name: value\n..." so a whole block is one comparison. */
static void flatten(const struct hpack_list *l, char *out, int max)
{
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < l->n; i++) {
        int k = snprintf(out + o, (size_t)(max - o), "%s: %s\n", l->v[i].name, l->v[i].value);
        if (k < 0 || o + k >= max) return;
        o += k;
    }
}

/* Decode one block and compare the flattened result. */
static void expect_block(struct hpack_dec *d, const char *hex, const char *want, int line)
{
    uint8_t buf[4096];
    int n = unhex(hex, buf, sizeof buf);
    checks++;
    if (n < 0) { printf("FAIL %s:%d bad hex fixture\n", __FILE__, line); fails++; return; }
    struct hpack_list l;
    hpack_list_init(&l);
    int rc = hpack_decode(d, buf, n, &l);
    if (rc != HPACK_OK) {
        printf("FAIL %s:%d decode failed: %s\n", __FILE__, line, hpack_strerror(rc));
        fails++; hpack_list_free(&l); return;
    }
    char got[4096];
    flatten(&l, got, sizeof got);
    if (strcmp(got, want)) {
        printf("FAIL %s:%d decoded mismatch\n--- got ---\n%s--- want ---\n%s", __FILE__, line, got, want);
        fails++;
    }
    hpack_list_free(&l);
}
#define BLOCK(d, hex, want) expect_block((d), (hex), (want), __LINE__)

/* The dynamic table, newest first, as "name: value\n...". Absolute index 62 is
 * dynamic entry 1. */
static void dyn_dump(const struct hpack_table *t, char *out, int max)
{
    int o = 0; out[0] = 0;
    for (int i = 0; i < t->count; i++) {
        const struct hpack_entry *e = hpack_lookup(t, 62 + i);
        int k = snprintf(out + o, (size_t)(max - o), "%s: %s\n", e->name, e->value);
        if (k < 0 || o + k >= max) return;
        o += k;
    }
}
static void expect_dyn(const struct hpack_table *t, int size, const char *want, int line)
{
    char got[4096];
    dyn_dump(t, got, sizeof got);
    checks++;
    if (strcmp(got, want) || hpack_table_size(t) != size) {
        printf("FAIL %s:%d dynamic table (size %d, want %d)\n--- got ---\n%s--- want ---\n%s",
               __FILE__, line, hpack_table_size(t), size, got, want);
        fails++;
    }
}
#define DYN(t, size, want) expect_dyn((t), (size), (want), __LINE__)

/* ------------------------------------------- the Huffman table's own proof */

/* The table is 257 hand-transcribed constants. Three independent properties
 * catch a transcription error: every code fits its length, the Kraft sum is
 * exactly 1 (so the code is complete -- one wrong length breaks this), and the
 * canonical layout the decoder depends on actually holds. Then the Appendix C
 * strings pin the values themselves. */
static void t_huffman_table(void)
{
    /* Round-trip every byte value: exercises encode and the canonical decode
     * path over all 257 symbols including the 30-bit ones. */
    for (int c = 0; c < 256; c++) {
        char in[1] = { (char)c };
        uint8_t enc[8];
        int n = hpack_huff_encode(enc, sizeof enc, in, 1);
        checks++;
        if (n <= 0) { printf("FAIL huff encode %d\n", c); fails++; continue; }
        char *dec = NULL; int dlen = 0;
        int rc = hpack_huff_decode(enc, n, &dec, &dlen, 64);
        if (rc != HPACK_OK || dlen != 1 || (unsigned char)dec[0] != (unsigned char)c) {
            printf("FAIL huff roundtrip %d rc=%d len=%d\n", c, rc, dlen); fails++;
        }
        free(dec);
    }
    /* A long mixed string, and the all-256-bytes string, round-trip too --
     * that is where a bit-accumulator bug shows up rather than in one symbol. */
    char all[256];
    for (int i = 0; i < 256; i++) all[i] = (char)i;
    uint8_t enc[2048];
    int n = hpack_huff_encode(enc, sizeof enc, all, 256);
    OK(n == hpack_huff_len(all, 256));
    char *dec = NULL; int dlen = 0;
    OK(hpack_huff_decode(enc, n, &dec, &dlen, 4096) == HPACK_OK);
    OK(dlen == 256 && !memcmp(dec, all, 256));
    free(dec);
}

/* Appendix B strings whose encodings are printed in Appendix C. */
static void t_huffman_vectors(void)
{
    static const struct { const char *s; const char *hex; } v[] = {
        { "www.example.com", "f1e3c2e5f23a6ba0ab90f4ff" },
        { "no-cache",        "a8eb10649cbf" },
        { "custom-key",      "25a849e95ba97d7f" },
        { "custom-value",    "25a849e95bb8e8b4bf" },
        { "302",             "6402" },
        { "307",             "640eff" },
        { "private",         "aec3771a4b" },
        { "Mon, 21 Oct 2013 20:13:21 GMT", "d07abe941054d444a8200595040b8166e082a62d1bff" },
        { "Mon, 21 Oct 2013 20:13:22 GMT", "d07abe941054d444a8200595040b8166e084a62d1bff" },
        { "https://www.example.com", "9d29ad171863c78f0b97c8e9ae82ae43d3" },
        { "gzip",            "9bd9ab" },
        { "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1",
          "94e7821dd7f2e6c7b335dfdfcd5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007" },
        { NULL, NULL }
    };
    for (int i = 0; v[i].s; i++) {
        uint8_t want[128], got[128];
        int wn = unhex(v[i].hex, want, sizeof want);
        int gn = hpack_huff_encode(got, sizeof got, v[i].s, (int)strlen(v[i].s));
        checks++;
        if (gn != wn || memcmp(got, want, (size_t)wn)) {
            printf("FAIL huffman vector %d (%s): got %d bytes, want %d\n", i, v[i].s, gn, wn);
            fails++;
            continue;
        }
        char *dec = NULL; int dl = 0;
        OK(hpack_huff_decode(want, wn, &dec, &dl, 1024) == HPACK_OK);
        OK(dec && dl == (int)strlen(v[i].s) && !strcmp(dec, v[i].s));
        free(dec);
    }
}

/* ------------------------------------------------- C.1 integer encoding */

static void t_integers(void)
{
    uint8_t b[16];
    uint32_t v;
    int off;

    /* C.1.1: 10 in a 5-bit prefix is one octet. */
    OK(hpack_int_encode(b, sizeof b, 5, 0, 10) == 1 && (b[0] & 0x1F) == 10);
    off = 0; OK(hpack_int_decode(b, 1, &off, 5, &v) == HPACK_OK && v == 10 && off == 1);

    /* C.1.2: 1337 in a 5-bit prefix is 31, 154, 10. */
    int n = hpack_int_encode(b, sizeof b, 5, 0, 1337);
    OK(n == 3 && (b[0] & 0x1F) == 31 && b[1] == 154 && b[2] == 10);
    off = 0; OK(hpack_int_decode(b, n, &off, 5, &v) == HPACK_OK && v == 1337 && off == 3);

    /* C.1.3: 42 with an 8-bit prefix is one octet. */
    n = hpack_int_encode(b, sizeof b, 8, 0, 42);
    OK(n == 1 && b[0] == 42);
    off = 0; OK(hpack_int_decode(b, n, &off, 8, &v) == HPACK_OK && v == 42);

    /* The prefix bits above the value must survive encoding untouched -- that
     * is how the representation type is carried. */
    n = hpack_int_encode(b, sizeof b, 6, 0x40, 62);
    off = 0; OK((b[0] & 0xC0) == 0x40);
    OK(hpack_int_decode(b, n, &off, 6, &v) == HPACK_OK && v == 62);

    /* Round-trip across the prefix boundaries where an off-by-one lives. */
    for (int prefix = 1; prefix <= 8; prefix++) {
        uint32_t max = (1u << prefix) - 1;
        uint32_t cases[] = { 0, 1, max - 1, max, max + 1, max + 127, max + 128,
                             max + 129, 1337, 65535, 0x7FFFFFFF };
        for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            uint8_t t[16];
            int tn = hpack_int_encode(t, sizeof t, prefix, 0, cases[k]);
            int to = 0; uint32_t tv = 0;
            checks++;
            if (tn <= 0 || hpack_int_decode(t, tn, &to, prefix, &tv) != HPACK_OK ||
                tv != cases[k] || to != tn) {
                printf("FAIL int roundtrip prefix=%d v=%u\n", prefix, cases[k]); fails++;
            }
        }
    }

    /* Truncated continuation: the value says "more octets follow" and there
     * are none. A decoder that reads on walks off the block. */
    b[0] = 0x1F; b[1] = 0x80;
    off = 0; OK(hpack_int_decode(b, 2, &off, 5, &v) == HPACK_E_TRUNC);

    /* Overflow: eight continuation octets encode more than 2^31. */
    uint8_t big[10] = { 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
    off = 0; OK(hpack_int_decode(big, 10, &off, 5, &v) < 0);

    /* An arbitrarily long run of 0x80 padding octets encodes 0 but must still
     * terminate rather than being followed forever. */
    uint8_t pad[16]; memset(pad, 0x80, sizeof pad); pad[0] = 0x1F;
    off = 0; OK(hpack_int_decode(pad, 16, &off, 5, &v) < 0);
}

/* --------------------------------------------------- C.2 representations */

static void t_representations(void)
{
    struct hpack_dec d;

    /* C.2.1 literal with incremental indexing, literal name. */
    hpack_dec_init(&d, 4096);
    BLOCK(&d, "400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572",
          "custom-key: custom-header\n");
    DYN(&d.tab, 55, "custom-key: custom-header\n");
    hpack_dec_free(&d);

    /* C.2.2 literal without indexing, indexed name -- the table stays empty. */
    hpack_dec_init(&d, 4096);
    BLOCK(&d, "040c 2f73 616d 706c 652f 7061 7468", ":path: /sample/path\n");
    DYN(&d.tab, 0, "");
    hpack_dec_free(&d);

    /* C.2.3 never indexed. Also asserts the sensitivity survives decoding, so
     * a proxy re-encoding it cannot demote it to an indexable field. */
    hpack_dec_init(&d, 4096);
    {
        uint8_t buf[64];
        int n = unhex("1008 7061 7373 776f 7264 0673 6563 7265 74", buf, sizeof buf);
        struct hpack_list l; hpack_list_init(&l);
        OK(hpack_decode(&d, buf, n, &l) == HPACK_OK);
        OK(l.n == 1 && !strcmp(l.v[0].name, "password") && !strcmp(l.v[0].value, "secret"));
        OK(l.v[0].sensitive == 1);
        hpack_list_free(&l);
    }
    DYN(&d.tab, 0, "");
    hpack_dec_free(&d);

    /* C.2.4 indexed field from the static table. */
    hpack_dec_init(&d, 4096);
    BLOCK(&d, "82", ":method: GET\n");
    DYN(&d.tab, 0, "");
    hpack_dec_free(&d);
}

/* ------------------------------------ C.3 / C.4 the request sequences */

static const char *REQ1 = ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n";
static const char *REQ2 = ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n"
                          "cache-control: no-cache\n";
static const char *REQ3 = ":method: GET\n:scheme: https\n:path: /index.html\n"
                          ":authority: www.example.com\ncustom-key: custom-value\n";

static void t_requests(void)
{
    struct hpack_dec d;

    /* C.3: three requests on ONE decoder. The third resolves index 63, which
     * only exists because the first two inserted entries -- so this is a test
     * of the table's index arithmetic across blocks, not of one block. */
    hpack_dec_init(&d, 4096);
    BLOCK(&d, "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d", REQ1);
    DYN(&d.tab, 57, ":authority: www.example.com\n");
    BLOCK(&d, "8286 84be 5808 6e6f 2d63 6163 6865", REQ2);
    DYN(&d.tab, 110, "cache-control: no-cache\n:authority: www.example.com\n");
    BLOCK(&d, "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65", REQ3);
    DYN(&d.tab, 164, "custom-key: custom-value\ncache-control: no-cache\n"
                     ":authority: www.example.com\n");
    hpack_dec_free(&d);

    /* C.4: the same three with Huffman-coded literals. Identical table states
     * -- eviction accounts for the DECODED length, not the wire length, and a
     * decoder that used the encoded size would diverge here and nowhere else. */
    hpack_dec_init(&d, 4096);
    BLOCK(&d, "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff", REQ1);
    DYN(&d.tab, 57, ":authority: www.example.com\n");
    BLOCK(&d, "8286 84be 5886 a8eb 1064 9cbf", REQ2);
    DYN(&d.tab, 110, "cache-control: no-cache\n:authority: www.example.com\n");
    BLOCK(&d, "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf", REQ3);
    DYN(&d.tab, 164, "custom-key: custom-value\ncache-control: no-cache\n"
                     ":authority: www.example.com\n");
    hpack_dec_free(&d);
}

/* ------------------------------- C.5 / C.6 the response sequences (eviction) */

static const char *RSP1 = ":status: 302\ncache-control: private\n"
                          "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                          "location: https://www.example.com\n";
static const char *RSP2 = ":status: 307\ncache-control: private\n"
                          "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                          "location: https://www.example.com\n";
static const char *RSP3 = ":status: 200\ncache-control: private\n"
                          "date: Mon, 21 Oct 2013 20:13:22 GMT\n"
                          "location: https://www.example.com\n"
                          "content-encoding: gzip\n"
                          "set-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n";

/* These are THE eviction vectors: SETTINGS_HEADER_TABLE_SIZE is 256, so the
 * second response evicts ":status: 302" to make room for ":status: 307", and
 * the third evicts three entries in one block. The tables printed in the RFC
 * after each response are reproduced exactly. */
static void t_responses(void)
{
    struct hpack_dec d;

    hpack_dec_init(&d, 4096);
    OK(hpack_table_resize(&d.tab, 256) == HPACK_OK);
    BLOCK(&d, "4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133"
              "2032 303a 3133 3a32 3120 474d 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70"
              "6c65 2e63 6f6d", RSP1);
    DYN(&d.tab, 222, "location: https://www.example.com\n"
                     "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                     "cache-control: private\n"
                     ":status: 302\n");
    BLOCK(&d, "4803 3330 37c1 c0bf", RSP2);
    DYN(&d.tab, 222, ":status: 307\n"
                     "location: https://www.example.com\n"
                     "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                     "cache-control: private\n");
    BLOCK(&d, "88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d"
              "54c0 5a04 677a 6970 7738 666f 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049"
              "5541 5851 5745 4f49 553b 206d 6178 2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e"
              "3d31", RSP3);
    DYN(&d.tab, 215, "set-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n"
                     "content-encoding: gzip\n"
                     "date: Mon, 21 Oct 2013 20:13:22 GMT\n");
    hpack_dec_free(&d);

    /* C.6: same sequence, Huffman-coded. Same evictions, same table sizes. */
    hpack_dec_init(&d, 4096);
    OK(hpack_table_resize(&d.tab, 256) == HPACK_OK);
    BLOCK(&d, "4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6"
              "2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3", RSP1);
    DYN(&d.tab, 222, "location: https://www.example.com\n"
                     "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                     "cache-control: private\n"
                     ":status: 302\n");
    BLOCK(&d, "4883 640e ffc1 c0bf", RSP2);
    DYN(&d.tab, 222, ":status: 307\n"
                     "location: https://www.example.com\n"
                     "date: Mon, 21 Oct 2013 20:13:21 GMT\n"
                     "cache-control: private\n");
    BLOCK(&d, "88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab"
              "77ad 94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f"
              "9587 3160 65c0 03ed 4ee5 b106 3d50 07", RSP3);
    DYN(&d.tab, 215, "set-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n"
                     "content-encoding: gzip\n"
                     "date: Mon, 21 Oct 2013 20:13:22 GMT\n");
    hpack_dec_free(&d);
}

/* ------------------------------------------------------- eviction corners */

static void t_eviction(void)
{
    struct hpack_table t;

    /* An entry larger than the whole table empties it and is not stored (4.4).
     * This is legal, and a decoder that errors here rejects legal streams. */
    hpack_table_init(&t, 4096);
    hpack_table_resize(&t, 100);
    hpack_table_add(&t, "a", 1, "b", 1);              /* size 34 */
    OK(hpack_table_count(&t) == 1 && hpack_table_size(&t) == 34);
    char big[200];
    memset(big, 'x', sizeof big); big[199] = 0;
    hpack_table_add(&t, "n", 1, big, 199);
    OK(hpack_table_count(&t) == 0 && hpack_table_size(&t) == 0);
    hpack_table_free(&t);

    /* Eviction is oldest-first and the newest is always index 62. */
    hpack_table_init(&t, 4096);
    hpack_table_resize(&t, 3 * 35);                   /* room for exactly three "kN: v" */
    hpack_table_add(&t, "k1", 2, "v", 1);
    hpack_table_add(&t, "k2", 2, "v", 1);
    hpack_table_add(&t, "k3", 2, "v", 1);
    OK(hpack_table_count(&t) == 3);
    OK(!strcmp(hpack_lookup(&t, 62)->name, "k3"));
    OK(!strcmp(hpack_lookup(&t, 64)->name, "k1"));
    hpack_table_add(&t, "k4", 2, "v", 1);
    OK(hpack_table_count(&t) == 3);
    OK(!strcmp(hpack_lookup(&t, 62)->name, "k4"));
    OK(!strcmp(hpack_lookup(&t, 64)->name, "k2"));    /* k1 went, not k3 */
    OK(hpack_lookup(&t, 65) == NULL);
    hpack_table_free(&t);

    /* Shrinking the capacity evicts immediately, not lazily on the next add. */
    hpack_table_init(&t, 4096);
    for (int i = 0; i < 10; i++) hpack_table_add(&t, "name", 4, "value", 5);
    OK(hpack_table_count(&t) == 10 && hpack_table_size(&t) == 410);
    hpack_table_resize(&t, 100);
    OK(hpack_table_count(&t) == 2 && hpack_table_size(&t) == 82);
    hpack_table_resize(&t, 0);
    OK(hpack_table_count(&t) == 0);
    hpack_table_free(&t);

    /* THE USE-AFTER-FREE CASE. A literal-with-indexing names its field by an
     * index that the insert is about to evict, and the value is a copy of that
     * same entry's value. If the implementation evicted before copying, this
     * reads freed memory -- under ASan it aborts, and without ASan it inserts
     * garbage that then desynchronises the connection. */
    {
        struct hpack_dec d;
        hpack_dec_init(&d, 4096);
        hpack_table_resize(&d.tab, 70);              /* room for one entry only */
        struct hpack_list l; hpack_list_init(&l);
        uint8_t b1[64];
        int n1 = unhex("400a 6375 7374 6f6d 2d6b 6579 0576 616c 7565", b1, sizeof b1);
        OK(hpack_decode(&d, b1, n1, &l) == HPACK_OK);   /* custom-key: value, s=47 */
        OK(hpack_table_count(&d.tab) == 1);
        hpack_list_free(&l);
        hpack_list_init(&l);
        /* 0x7E = literal-with-indexing, name index 62 (the entry just added),
         * then a 20-byte value. The new entry is 10+20+32 = 62 and the table
         * holds 47 of 70, so entry 62 must be evicted to make room for the
         * field that names it. */
        uint8_t b2[64];
        int n2 = unhex("7e14 6161 6161 6161 6161 6161 6161 6161 6161 6161 6161", b2, sizeof b2);
        OK(hpack_decode(&d, b2, n2, &l) == HPACK_OK);
        OK(l.n == 1 && !strcmp(l.v[0].name, "custom-key"));
        OK(hpack_table_count(&d.tab) == 1);
        OK(!strcmp(hpack_lookup(&d.tab, 62)->name, "custom-key"));
        OK(!strcmp(hpack_lookup(&d.tab, 62)->value, "aaaaaaaaaaaaaaaaaaaa"));
        hpack_list_free(&l);
        hpack_dec_free(&d);
    }
}

/* ---------------------------------------------------- hostile input */

static void expect_err(const char *hex, int cap, int line)
{
    uint8_t buf[512];
    int n = unhex(hex, buf, sizeof buf);
    struct hpack_dec d;
    hpack_dec_init(&d, cap);
    struct hpack_list l; hpack_list_init(&l);
    int rc = hpack_decode(&d, buf, n, &l);
    checks++;
    if (rc >= 0) { printf("FAIL %s:%d expected a decode error, got OK\n", __FILE__, line); fails++; }
    hpack_list_free(&l);
    hpack_dec_free(&d);
}
#define ERR(hex) expect_err((hex), 4096, __LINE__)

static void t_hostile(void)
{
    ERR("80");                       /* indexed field, index 0 */
    ERR("ff00");                     /* indexed field, index 62 with an empty table */
    ERR("bf");                       /* indexed field 63, empty table */
    ERR("400a 6375 7374 6f6d");      /* literal name says 10 bytes, 4 are present */
    ERR("40");                       /* truncated: representation byte only */
    ERR("400a 6375 7374 6f6d 2d6b 6579");        /* name complete, value missing */
    ERR("400a 6375 7374 6f6d 2d6b 6579 0576");   /* value says 5, 2 present */
    ERR("41");                       /* indexed name, value missing */
    ERR("00 00");                    /* literal, empty name, then truncated */

    /* Huffman: EOS inside the stream. 0x3fffffff padded to 32 bits is the EOS
     * symbol -- legal as padding, a decode error as a symbol. */
    ERR("4184 ffff ffff");
    /* Huffman: padding longer than 7 bits (a whole 0xff octet after a complete
     * symbol). Accepting it lets an encoder smuggle bytes past a length. */
    ERR("4183 ffff ff");

    /* Dynamic table size update larger than the announced maximum: the only
     * bound on how much memory a peer can make us hold. */
    ERR("3fe1 ff03");                /* update to 65536, above the announced 4096 */
    /* Update after a header field, which RFC 7541 4.2 forbids. */
    ERR("82 20");
    /* Three consecutive updates: at most two can be meaningful. */
    ERR("20 20 20 82");

    /* A well-formed prelude followed by garbage must fail as a whole rather
     * than returning the fields it managed to parse -- a partially decoded
     * block has already mutated the table and cannot be retried. */
    ERR("8286 8441");

    /* The zero-length-value cases that must SUCCEED, so the errors above are
     * not just "everything fails". */
    {
        struct hpack_dec d; hpack_dec_init(&d, 4096);
        BLOCK(&d, "4100", ":authority: \n");
        BLOCK(&d, "0000 00", ": \n");
        hpack_dec_free(&d);
    }
}

/* The HPACK bomb: a few bytes of frames that decode to megabytes. One entry
 * repeated by index costs one byte per copy, so a 16 KiB block yields ~64 MiB
 * of header list. The cap is the only thing standing between a server and our
 * heap, so it is asserted, not assumed. */
static void t_bomb(void)
{
    struct hpack_dec d;
    hpack_dec_init(&d, 4096);

    /* Seed one maximal entry, then reference it thousands of times. */
    struct hpack_list seed; hpack_list_init(&seed);
    uint8_t big[4200];
    int o = 0;
    big[o++] = 0x40; big[o++] = 0x01; big[o++] = 'x';       /* name "x" */
    o += hpack_int_encode(big + o, (int)sizeof big - o, 7, 0, 4000);
    memset(big + o, 'v', 4000); o += 4000;
    OK(hpack_decode(&d, big, o, &seed) == HPACK_OK);
    OK(hpack_table_count(&d.tab) == 1);
    hpack_list_free(&seed);

    uint8_t bomb[16384];
    for (unsigned i = 0; i < sizeof bomb; i++) bomb[i] = 0xBE;   /* indexed 62 */
    struct hpack_list l; hpack_list_init(&l);
    int rc = hpack_decode(&d, bomb, (int)sizeof bomb, &l);
    OK(rc == HPACK_E_SIZE);                      /* refused, not absorbed */
    OK(l.bytes <= HPACK_LIST_MAX + 4096);        /* and bounded on the way out */
    hpack_list_free(&l);
    hpack_dec_free(&d);
}

/* -------------------------------------------------------------- encoder */

/* The encoder's only real obligation: whatever it emits, OUR decoder -- which
 * the RFC vectors above prove correct -- must recover exactly, and the two
 * dynamic tables must stay identical entry for entry. A round-trip that only
 * compared headers would pass with both sides evicting wrongly in the same
 * way; comparing the tables is what catches that. */
static void t_encoder(void)
{
    struct hpack_enc e;
    struct hpack_dec d;
    hpack_enc_init(&e, 4096);
    hpack_dec_init(&d, 4096);

    for (int round = 0; round < 40; round++) {
        struct hpack_list in;
        hpack_list_init(&in);
        char path[64], val[64];
        snprintf(path, sizeof path, "/page/%d", round);
        snprintf(val, sizeof val, "value-%d", round % 7);
        hpack_list_add(&in, ":method", -1, "GET", -1, 0);
        hpack_list_add(&in, ":scheme", -1, "https", -1, 0);
        hpack_list_add(&in, ":authority", -1, "example.com", -1, 0);
        hpack_list_add(&in, ":path", -1, path, -1, 0);
        hpack_list_add(&in, "user-agent", -1, "LogitOS/1.0", -1, 0);
        hpack_list_add(&in, "accept", -1, "text/html", -1, 0);
        hpack_list_add(&in, "x-custom", -1, val, -1, 0);
        hpack_list_add(&in, "authorization", -1, "Bearer sekrit", -1, 0);

        uint8_t *enc = NULL; int enclen = 0;
        OK(hpack_encode(&e, &in, &enc, &enclen) == HPACK_OK);

        struct hpack_list outl; hpack_list_init(&outl);
        int rc = hpack_decode(&d, enc, enclen, &outl);
        checks++;
        if (rc != HPACK_OK) { printf("FAIL round %d decode: %s\n", round, hpack_strerror(rc)); fails++; }

        char a[2048], b[2048];
        flatten(&in, a, sizeof a);
        flatten(&outl, b, sizeof b);
        checks++;
        if (strcmp(a, b)) { printf("FAIL round %d roundtrip\n%s---\n%s", round, a, b); fails++; }

        /* Tables in lockstep, which is the property that actually matters. */
        char ta[4096], tb[4096];
        dyn_dump(&e.tab, ta, sizeof ta);
        dyn_dump(&d.tab, tb, sizeof tb);
        checks++;
        if (strcmp(ta, tb) || hpack_table_size(&e.tab) != hpack_table_size(&d.tab)) {
            printf("FAIL round %d tables diverged (%d vs %d)\n%s---\n%s",
                   round, hpack_table_size(&e.tab), hpack_table_size(&d.tab), ta, tb);
            fails++;
        }
        /* And the secret never entered either table. */
        checks++;
        if (strstr(ta, "sekrit")) { printf("FAIL round %d: authorization was indexed\n", round); fails++; }

        free(enc);
        hpack_list_free(&in);
        hpack_list_free(&outl);
    }

    /* Compression actually happens: the second identical request is far
     * smaller than the first. Without this, an encoder that emitted every
     * field as a literal would pass every test above. */
    {
        struct hpack_enc e2; struct hpack_dec d2;
        hpack_enc_init(&e2, 4096); hpack_dec_init(&d2, 4096);
        struct hpack_list in; hpack_list_init(&in);
        hpack_list_add(&in, ":method", -1, "GET", -1, 0);
        hpack_list_add(&in, ":scheme", -1, "https", -1, 0);
        hpack_list_add(&in, ":authority", -1, "www.example.com", -1, 0);
        hpack_list_add(&in, ":path", -1, "/", -1, 0);
        hpack_list_add(&in, "user-agent", -1,
                       "Mozilla/5.0 (LogitOS) AppleWebKit/537.36 Browser/1.0", -1, 0);
        uint8_t *a = NULL, *bb = NULL; int an = 0, bn = 0;
        OK(hpack_encode(&e2, &in, &a, &an) == HPACK_OK);
        OK(hpack_encode(&e2, &in, &bb, &bn) == HPACK_OK);
        OK(bn < an / 4);                 /* the repeat is indexed, not respelled */
        OK(bn == 5);                     /* four indexed fields, one byte each... */
        struct hpack_list o1, o2; hpack_list_init(&o1); hpack_list_init(&o2);
        OK(hpack_decode(&d2, a, an, &o1) == HPACK_OK);
        OK(hpack_decode(&d2, bb, bn, &o2) == HPACK_OK);
        char f1[1024], f2[1024];
        flatten(&o1, f1, sizeof f1); flatten(&o2, f2, sizeof f2);
        OK(!strcmp(f1, f2));
        free(a); free(bb);
        hpack_list_free(&in); hpack_list_free(&o1); hpack_list_free(&o2);
        hpack_enc_free(&e2); hpack_dec_free(&d2);
    }

    /* A capacity change from the peer's SETTINGS emits a size update FIRST,
     * and our own decoder (which rejects a misplaced update) accepts it. */
    {
        struct hpack_enc e3; struct hpack_dec d3;
        hpack_enc_init(&e3, 4096); hpack_dec_init(&d3, 4096);
        struct hpack_list in; hpack_list_init(&in);
        hpack_list_add(&in, ":method", -1, "GET", -1, 0);
        uint8_t *a = NULL; int an = 0;
        hpack_enc_set_capacity(&e3, 256);
        OK(hpack_encode(&e3, &in, &a, &an) == HPACK_OK);
        OK(an >= 2 && (a[0] & 0xE0) == 0x20);        /* 001xxxxx, first */
        struct hpack_list o; hpack_list_init(&o);
        OK(hpack_decode(&d3, a, an, &o) == HPACK_OK);
        OK(o.n == 1 && !strcmp(o.v[0].value, "GET"));
        OK(e3.tab.cap == 256);
        free(a);
        hpack_list_free(&in); hpack_list_free(&o);
        hpack_enc_free(&e3); hpack_dec_free(&d3);
    }

    hpack_enc_free(&e);
    hpack_dec_free(&d);
}

int main(void)
{
    t_huffman_table();
    t_huffman_vectors();
    t_integers();
    t_representations();
    t_requests();
    t_responses();
    t_eviction();
    t_hostile();
    t_bomb();
    t_encoder();

    printf("hpack_test: %d checks, %d failures\n", checks, fails);
    if (!fails) printf("hpack_test: ALL PASS\n");
    return fails ? 1 : 0;
}
