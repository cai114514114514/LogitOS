/* Deterministic adversarial fuzzer for every c/net/ssh parser that touches
 * network bytes -- the whole pre-auth attack surface of the daemon, plus the
 * post-auth channel parsers, plus authorized_keys parsing.
 *
 * House shape (x509_fuzz.c / http1_fuzz.c / demux_fuzz.c): standalone C, no
 * libFuzzer, a seeded PRNG and a mutation loop, SEED= and SCALE= env vars for
 * reproducibility, and a sabotage build (-DSSH_FUZZ_SABOTAGE, see ssh_wire.c)
 * that MUST trip AddressSanitizer -- that build is what proves the clean runs
 * below are evidence rather than a loop that never reaches a bound.
 *
 * Structure-aware, not blind: seeds are built with the library's own
 * builders (a real KEXINIT, a real userauth password request, a real
 * channel-open, a real encrypted packet stream), then mutated with bit
 * flips, byte substitutions, length-field +-deltas, truncation, extension
 * and splice. Blind random bytes barely get past the first u32; mutated
 * well-formed messages reach the deep fields (the signature walk, the
 * name-list negotiation, the padding arithmetic) in every run.
 *
 * Every seed buffer is malloc'd to its EXACT size so that any read past the
 * end is an ASan heap-buffer-overflow, not a silent read of adjacent stack.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_packet.h"
#include "ssh_kex.h"
#include "ssh_auth.h"
#include "ssh_conn.h"
#include "base64.h"
#include "crypto.h"

/* --- deterministic PRNG (splitmix64) --- */
static uint64_t g_st;
static uint64_t rnd(void)
{
    g_st += 0x9E3779B97F4A7C15ull;
    uint64_t z = g_st;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t rnd_below(uint32_t n) { return (uint32_t)(rnd() % n); }

static void test_rnd(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

/* --- the seed corpus --- */
#define MAX_SEEDS 16
struct seed { uint8_t *buf; int len; const char *what; };
static struct seed seeds[MAX_SEEDS];
static int nseeds;

static void add_seed(const uint8_t *buf, int len, const char *what)
{
    uint8_t *p = malloc((size_t)len);        /* exact size: the whole point */
    memcpy(p, buf, (size_t)len);
    seeds[nseeds].buf = p;
    seeds[nseeds].len = len;
    seeds[nseeds].what = what;
    nseeds++;
}

static void build_seeds(void)
{
    uint8_t b[1024];
    int n;

    /* 1. a real KEXINIT, straight from the builder sshd itself uses */
    n = ssh_kexinit_build(b, (int)sizeof b, test_rnd);
    if (n > 0) add_seed(b, n, "kexinit");

    /* 2. userauth/password */
    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_USERAUTH_REQUEST);
    n = ssh_w_cstring(b, n, (int)sizeof b, "attacker");
    n = ssh_w_cstring(b, n, (int)sizeof b, "ssh-connection");
    n = ssh_w_cstring(b, n, (int)sizeof b, "password");
    n = ssh_w_bool(b, n, (int)sizeof b, 0);
    n = ssh_w_cstring(b, n, (int)sizeof b, "guessed-password");
    if (n > 0) add_seed(b, n, "userauth-password");

    /* 3. userauth/publickey with a signature blob (the deepest parse walk) */
    uint8_t keyblob[128];
    int ko = ssh_w_cstring(keyblob, 0, (int)sizeof keyblob, "ssh-ed25519");
    uint8_t fakepub[32]; test_rnd(fakepub, 32);
    ko = ssh_w_string(keyblob, ko, (int)sizeof keyblob, fakepub, 32);
    uint8_t sigblob[128];
    uint8_t fakesig[64]; test_rnd(fakesig, 64);
    int so = ssh_w_cstring(sigblob, 0, (int)sizeof sigblob, "ssh-ed25519");
    so = ssh_w_string(sigblob, so, (int)sizeof sigblob, fakesig, 64);
    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_USERAUTH_REQUEST);
    n = ssh_w_cstring(b, n, (int)sizeof b, "attacker");
    n = ssh_w_cstring(b, n, (int)sizeof b, "ssh-connection");
    n = ssh_w_cstring(b, n, (int)sizeof b, "publickey");
    n = ssh_w_bool(b, n, (int)sizeof b, 1);
    n = ssh_w_string(b, n, (int)sizeof b, keyblob, ko);
    n = ssh_w_string(b, n, (int)sizeof b, sigblob, so);
    if (n > 0) add_seed(b, n, "userauth-publickey");

    /* 4. channel open / request-exec / data / window adjust / close */
    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_CHANNEL_OPEN);
    n = ssh_w_cstring(b, n, (int)sizeof b, "session");
    n = ssh_w_u32(b, n, (int)sizeof b, 7);
    n = ssh_w_u32(b, n, (int)sizeof b, 0x200000);
    n = ssh_w_u32(b, n, (int)sizeof b, 32768);
    if (n > 0) add_seed(b, n, "channel-open");

    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_CHANNEL_REQUEST);
    n = ssh_w_u32(b, n, (int)sizeof b, 0);
    n = ssh_w_cstring(b, n, (int)sizeof b, "exec");
    n = ssh_w_bool(b, n, (int)sizeof b, 1);
    n = ssh_w_cstring(b, n, (int)sizeof b, "echo pwned");
    if (n > 0) add_seed(b, n, "channel-request-exec");

    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_CHANNEL_DATA);
    n = ssh_w_u32(b, n, (int)sizeof b, 0);
    n = ssh_w_string(b, n, (int)sizeof b, (const uint8_t *)"payload", 7);
    if (n > 0) add_seed(b, n, "channel-data");

    n = 0;
    n = ssh_w_u8(b, n, (int)sizeof b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    n = ssh_w_u32(b, n, (int)sizeof b, 0);
    n = ssh_w_u32(b, n, (int)sizeof b, 0xFFFFFFFF);
    if (n > 0) add_seed(b, n, "window-adjust");

    /* 5. authorized_keys: one real line, base64 from this directory's own
     * encoder, so the b64 walk gets well-formed input to mutate. */
    uint8_t pubblob[64];
    int po = ssh_w_cstring(pubblob, 0, (int)sizeof pubblob, "ssh-ed25519");
    po = ssh_w_string(pubblob, po, (int)sizeof pubblob, fakepub, 32);
    char b64[128];
    int bl = b64_encode(pubblob, po, b64, (int)sizeof b64 - 1, 1);
    if (bl > 0) {
        b64[bl] = 0;   /* b64_encode does not terminate; snprintf needs a string */
        char ak[256];
        int an = snprintf(ak, sizeof ak, "ssh-ed25519 %s attacker@host\n", b64);
        if (an > 0 && an < (int)sizeof ak) add_seed((const uint8_t *)ak, an, "authorized_keys");
    }
}

/* --- mutation --- */
static uint8_t *mutate(const struct seed *s, int *outlen)
{
    int len = s->len;
    /* length-changing first: truncate / extend, sometimes wildly */
    uint32_t op = rnd_below(100);
    if (op < 12) len = rnd_below((uint32_t)s->len + 1);                       /* truncate */
    else if (op < 24) len = s->len + 1 + (int)rnd_below(64);                  /* extend a bit */
    else if (op < 28) len = s->len + (int)rnd_below(8192);                    /* extend a lot */
    if (len < 0) len = 0;
    uint8_t *m = malloc((size_t)len + 1);
    memcpy(m, s->buf, (size_t)(len < s->len ? len : s->len));
    for (int i = s->len; i < len; i++) m[i] = (uint8_t)rnd();                /* junk tail */

    int nmut = 1 + (int)rnd_below(6);
    for (int k = 0; k < nmut && len > 0; k++) {
        switch (rnd_below(5)) {
        case 0: { /* bit flip */
            int i = (int)rnd_below((uint32_t)len);
            m[i] ^= (uint8_t)(1u << rnd_below(8));
            break;
        }
        case 1: { /* random byte */
            m[(int)rnd_below((uint32_t)len)] = (uint8_t)rnd();
            break;
        }
        case 2: { /* length-field poke: a u32 anywhere, +- a small delta --
                   * the attacker-controlled string lengths are exactly this */
            if (len >= 4) {
                int i = (int)rnd_below((uint32_t)(len - 3));
                uint32_t v = ((uint32_t)m[i] << 24) | ((uint32_t)m[i+1] << 16) |
                             ((uint32_t)m[i+2] << 8) | (uint32_t)m[i+3];
                v += (uint32_t)rnd() % 60000 - 30000;
                m[i] = (uint8_t)(v >> 24); m[i+1] = (uint8_t)(v >> 16);
                m[i+2] = (uint8_t)(v >> 8); m[i+3] = (uint8_t)v;
            }
            break;
        }
        case 3: { /* splice a chunk of ANOTHER seed in */
            if (nseeds > 1) {
                const struct seed *o = &seeds[rnd_below((uint32_t)nseeds)];
                int take = (int)rnd_below((uint32_t)o->len + 1);
                int at = (int)rnd_below((uint32_t)len + 1);
                if (at + take > len) take = len - at;
                if (take > 0) memcpy(m + at, o->buf, (size_t)take);
            }
            break;
        }
        case 4: { /* zero a run -- empties strings/lists the way truncation
                   * cannot: the length prefix survives, the content dies */
            int at = (int)rnd_below((uint32_t)len);
            int run = 1 + (int)rnd_below(16);
            if (at + run > len) run = len - at;
            memset(m + at, 0, (size_t)run);
            break;
        }
        }
    }
    *outlen = len;
    return m;
}

/* --- consumers: the sink keeps every parse result live --- */
static volatile uint64_t g_sink;
static void sink_bytes(const uint8_t *p, int n) { for (int i = 0; i < n; i++) g_sink += p[i]; }

/* memory-backed ssh_io_fn for ssh_pkt_recv */
struct memrd { const uint8_t *p; int len; int off; };
static int mem_read(void *ctx, uint8_t *buf, int len)
{
    struct memrd *m = ctx;
    if (m->off + len > m->len) return -1;
    memcpy(buf, m->p + m->off, (size_t)len);
    m->off += len;
    return len;
}

static void exercise(const uint8_t *m, int len)
{
    struct ssh_authreq ar;
    if (ssh_authreq_parse(m, len, &ar) == 0) {
        char pw[128];
        if (ssh_auth_parse_password(ar.rest, ar.restlen, pw, (int)sizeof pw) > 0)
            sink_bytes((const uint8_t *)pw, 1);   /* consumed; reading all of pw
                                                   * would read uninitialized */
        /* zeroed on purpose: parse failure must leave these NULL, or the
         * signdata call below reads whatever the previous iteration left in
         * this stack slot -- a stale pointer into freed memory, which is
         * exactly the use-after-free ASan reported the first time this
         * fuzzer ran (the harness bug was found by the harness; the product
         * parsers survived it). */
        int has_sig = 0; char alg[32] = {0};
        const uint8_t *blob = 0, *sig = 0; int bloblen = 0, siglen = 0;
        if (ssh_auth_parse_publickey(ar.rest, ar.restlen, &has_sig, alg, (int)sizeof alg,
                                     &blob, &bloblen, &sig, &siglen) == 0) {
            sink_bytes(blob, bloblen);            /* THE string-length consumer:
                                                   * sabotaged bounds read past
                                                   * the malloc here */
            if (has_sig) sink_bytes(sig, siglen);
            sink_bytes((const uint8_t *)alg, (int)strlen(alg));
        }
        /* signdata wants a 32-byte session id and NUL-terminated C strings;
         * the mutation is neither, so only the BLOB comes from the mutation
         * (bounded by parse success, which bounds it to the seed's bytes). */
        static const uint8_t fake_sid[32] = {1};
        uint8_t sd[1024];
        int sl = ssh_auth_pubkey_signdata(fake_sid, "user", "ssh-connection",
                                          "ssh-ed25519", blob ? blob : m,
                                          blob ? bloblen : (len < 32 ? len : 32),
                                          sd, (int)sizeof sd);
        if (sl > 0) sink_bytes(sd, sl);
    }

    struct ssh_negotiated neg; char why[64];
    if (ssh_kexinit_negotiate(m, len, &neg, why, (int)sizeof why) == 0)
        g_sink += neg.kex[0] + neg.cipher_c2s[0];

    char type[32]; uint32_t pc, pw_, pm;
    if (ssh_parse_channel_open(m, len, type, (int)sizeof type, &pc, &pw_, &pm) == 0)
        g_sink += pc + pw_ + pm;
    uint32_t ch; const uint8_t *d; int dl;
    if (ssh_parse_channel_data(m, len, &ch, &d, &dl) == 0)
        sink_bytes(d, dl);
    uint32_t ch2, by;
    if (ssh_parse_window_adjust(m, len, &ch2, &by) == 0)
        g_sink += ch2 + by;
    uint32_t ch3; char rt[32]; int wr; const uint8_t *rd; int rdl;
    if (ssh_parse_channel_request(m, len, &ch3, rt, (int)sizeof rt, &wr, &rd, &rdl) == 0) {
        char cmd[512];
        if (ssh_parse_exec_command(rd, rdl, cmd, (int)sizeof cmd) == 0)
            sink_bytes((const uint8_t *)cmd, (int)strlen(cmd));
    }
    if (ssh_parse_close(m, len, &ch) == 0) g_sink += ch;

    /* authorized_keys text vs a random key blob */
    uint8_t keyblob[64];
    int ko = ssh_w_cstring(keyblob, 0, (int)sizeof keyblob, "ssh-ed25519");
    ko = ssh_w_bytes(keyblob, ko, (int)sizeof keyblob, m, len < 32 ? len : 32);
    if (ssh_authkeys_match((const char *)m, len, keyblob, ko))
        g_sink++;

    /* the packet layer: feed the mutation as a wire stream, both phases */
    struct memrd mr = { m, len, 0 };
    struct ssh_dir_state st; memset(&st, 0, sizeof st);
    uint8_t out[SSH_MAX_PAYLOAD];
    int n = ssh_pkt_recv(&st, mem_read, &mr, out, (int)sizeof out);
    if (n > 0) sink_bytes(out, n);
    /* and the encrypted phase: fixed keys, so decrypt-garbage walks the MAC
     * and padding arithmetic too */
    uint8_t ek[16], iv[16], mk[32];
    for (int i = 0; i < 16; i++) { ek[i] = (uint8_t)(i * 11); iv[i] = (uint8_t)(i * 7 + 1); }
    for (int i = 0; i < 32; i++) mk[i] = (uint8_t)(i * 5 + 3);
    struct memrd mr2 = { m, len, 0 };
    struct ssh_dir_state st2; memset(&st2, 0, sizeof st2);
    ssh_dir_activate(&st2, ek, iv, mk);
    n = ssh_pkt_recv(&st2, mem_read, &mr2, out, (int)sizeof out);
    if (n > 0) sink_bytes(out, n);

    /* name-list machinery on its own */
    if (ssh_namelist_has(m, len, "curve25519-sha256")) g_sink++;
    char chosen[96];
    if (ssh_negotiate(m, len, "curve25519-sha256,ssh-ed25519", chosen, (int)sizeof chosen) > 0)
        g_sink += chosen[0];
}

int main(void)
{
    const char *seed_s = getenv("SEED");
    const char *scale_s = getenv("SCALE");
    g_st = seed_s ? strtoull(seed_s, 0, 10) : 1;
    long scale = scale_s ? atol(scale_s) : 20000;

    build_seeds();
    if (nseeds < 8) { fprintf(stderr, "seed corpus incomplete: %d\n", nseeds); return 2; }

    for (long i = 0; i < scale; i++) {
        const struct seed *s = &seeds[rnd_below((uint32_t)nseeds)];
        int len;
        uint8_t *m = mutate(s, &len);
        exercise(m, len);
        free(m);
    }
    printf("ssh_attack_fuzz: %ld mutations over %d seeds, sink=%llu, no crash\n",
           scale, nseeds, (unsigned long long)g_sink);
    return 0;
}
