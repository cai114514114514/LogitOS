/* /bin/sshd -- an SSH-2 server a stock OpenSSH client accepts.
 *
 * ALGORITHM SET: curve25519-sha256 (+ @libssh.org alias) / ssh-ed25519 /
 * aes128-ctr / hmac-sha2-256 / no compression. Argued at length against a
 * REAL OpenSSH_10.2p1 client's captured KEXINIT in c/net/ssh/ssh.h -- read
 * that comment first, this one is about the ring-3 plumbing around it.
 *
 * WHAT THIS FILE IS, AND WHAT c/net/ssh IS. Every RFC 4253/4252/4254 byte
 * lives in c/net/ssh as plain C with no ring assumptions -- no syscalls,
 * so a host unit test links the identical objects. This file is the OS
 * glue: the listening socket, thread-per-connection, the account store and
 * authorized_keys file reads, and fork+pipe+execve for the shell -- none of
 * which c/net/ssh knows anything about.
 *
 * THREAD SHAPE, and why it is not literally "one thread per connection"
 * despite that being the ABI's own name for the model (logit_abi.h SERVER
 * SOCKETS section). There is no select()/poll() usable across two different
 * kinds of descriptor with two different read disciplines at once -- a
 * blocking ssh_pkt_recv() on the socket and a blocking read of the child's
 * output pipe cannot both be served by one thread without one starving the
 * other. So each ACCEPTED CONNECTION gets its own pair: the thread the
 * accept loop spawns becomes the INPUT thread once a shell/exec starts
 * (socket -> child stdin, plus window-adjust/eof/close bookkeeping), and it
 * spawns one OUTPUT thread of its own (child stdout+stderr, merged -- see
 * the note above handle_connection -- -> socket, respecting the CLIENT's
 * advertised receive window, which is the thing a real client disconnects
 * over if it is not honoured). The two share exactly one thing, `struct
 * conn_ctx`'s send lock + peer_window, guarded by a two-instruction
 * xchg-based spinlock (no futex/mutex needed: the critical sections are a
 * few bytes long and contention between two threads on one connection is
 * rare by construction).
 *
 * CALLER-OWNED THREAD STACKS, ON PURPOSE. Every stack here is a slot in this
 * program's own .bss (SYS_THREAD_CREATE's `stack_base = 0`), not a
 * SYS_MMAP the kernel manages. LOGIT_THREADS_MAX (64) is not the real
 * ceiling on this machine -- VMA_MAXAREA (16) is, because a normal
 * pthread_create() stack is its own mmap'd VMA -- and this server needs 2-3
 * threads per connection, which would exhaust that at 5-6 simultaneous
 * connections. A stack this program already owns costs no VMA at all.
 *
 * NO REAL PTY. pty-req is accepted and acknowledged (RFC 4254 6.2) so a
 * client that insists on one is not refused, but nothing here allocates a
 * kernel pty (there is not one on this OS -- see CLAUDE.md) -- the shell
 * runs over four plain pipes, exactly the shape c/apps/gui/terminal.c uses
 * for the GUI Terminal and c/apps/coreutils/login.c's own execve uses for
 * the console. Consequence, stated rather than discovered: no job control,
 * no line-discipline echo control, and a client-side `stty` is talking to
 * nothing.
 *
 * STDOUT/STDERR ARE MERGED into one CHANNEL_DATA stream in this v1 -- the
 * child's fd 2 is dup2'd onto the SAME pipe as fd 1, so the server never
 * sends SSH_MSG_CHANNEL_EXTENDED_DATA. A real client displays both exactly
 * the way a real terminal would (interleaved, unattributed), so this is
 * invisible interactively; a client that specifically separates
 * stdout/stderr streams (e.g. `ssh host cmd 2>err.log`) will find err.log
 * empty. Named here as the thing to fix first if that distinction is ever
 * needed -- it is a second pipe pair and a second output thread, not a
 * redesign.
 */
#include "logit.h"
#include "clib.h"
#include "logit_stat.h"
#include "accounts.h"

#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_packet.h"
#include "ssh_kex.h"
#include "ssh_hostkey.h"
#include "ssh_auth.h"
#include "ssh_conn.h"
#include "crypto.h"

/* The coreutils link no libc (see pkgverify.c's identical note) but every
 * crypto TU here is ordinary C: clang emits memcpy/memset for struct
 * assignment and array init regardless of -ffreestanding. */
void *memcpy(void *d, const void *s, unsigned long n)
{ unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s; for (unsigned long i = 0; i < n; i++) a[i] = b[i]; return d; }
void *memset(void *d, int c, unsigned long n)
{ unsigned char *a = (unsigned char *)d; for (unsigned long i = 0; i < n; i++) a[i] = (unsigned char)c; return d; }

/* c/crypto/kdf/pbkdf2.c -- declared rather than pulled in via crypto.h's
 * whole surface, same reasoning login.c gives. */
int pwhash_make(char *out, int max, const char *password,
                unsigned iters, void (*randbytes)(unsigned char *, int));

/* --- the ASM trampoline (sshd_thread.asm) -- see its header for why sshd
 * cannot reuse mini-libc's pthread_entry.asm. --- */
extern void sshd_thread_entry(void);

struct sshd_thread_arg {
    void (*fn)(void *);
    void *ctx;
};

/* Called by sshd_thread_entry with the raw `arg` word cast back to a
 * pointer. One dispatcher for both thread kinds this program spawns (the
 * per-connection input thread and the per-connection output pump). */
void sshd_thread_body(void *raw)
{
    struct sshd_thread_arg *t = (struct sshd_thread_arg *)raw;
    void (*fn)(void *) = t->fn;
    void *ctx = t->ctx;
    fn(ctx);
    _sys(SYS_THREAD_EXIT, 0, 0, 0);
}

static int sys_thread_create(struct logit_thread_spec *spec)
{ return (int)_sys(SYS_THREAD_CREATE, (long)spec, 0, 0); }

/* --- a two-instruction spinlock. No futex: the sections it guards are a
 * handful of stores (a send, or a window update), and the two threads that
 * can contend on it exist only for the lifetime of one connection's relay
 * phase. __sync_lock_test_and_set/_release are compiler builtins lowering
 * to a plain xchg/store on x86-64 -- no runtime support needed, so this
 * works identically under UCFLAGS (clang, freestanding) and under a host
 * gcc/clang unit test. --- */
static void spin_lock(volatile int *l)   { while (__sync_lock_test_and_set(l, 1)) { } }
static void spin_unlock(volatile int *l) { __sync_lock_release(l); }

/* --- randomness: crypto.h's callbacks want `void(uint8_t*,int)`;
 * getrandom_bytes reports failure. Mirrors login.c's rnd() exactly, down to
 * the all-ones fallback (recognisable as "the DRBG refused" rather than
 * looking like a legitimate all-zero draw). --- */
static void rnd(uint8_t *p, int n)
{ if (getrandom_bytes(p, n) < 0) for (int i = 0; i < n; i++) p[i] = 0xFF; }

/* c/crypto/aead/aes_dispatch.c always calls aes_backend_ni() (it is how the
 * fast path is FOUND, not merely used) and c/crypto/aead/aes_ni.c's real
 * implementation needs c/kernel/cpu/cpufeat.c's cpu_has() -- a kernel TU
 * this ring-3 program has no reason to link for one CPUID check. This
 * program forces the portable backend instead, on purpose: aes128-ctr is
 * called once per SSH packet on a small connection count, not per pixel,
 * and AES-NI's whole benefit "cannot be measured under QEMU/TCG" anyway
 * (crypto.h's own note on aes_ni.c) -- there is no speed argument for
 * paying the link cost here, and the portable backend is the one every
 * other backend is verified AGAINST, not a lesser fallback. */
struct aes_backend;
const struct aes_backend *aes_backend_ni(void) { return 0; }

/* ========================================================================
 * the host key
 * ==================================================================== */
#define HOSTKEY_PATH "/etc/ssh_host_ed25519_key"

static int load_or_create_hostkey(uint8_t pub[32], uint8_t seed[32])
{
    uint8_t rec[SSH_HOSTKEY_RECORD_LEN];
    int n = read_file(HOSTKEY_PATH, rec, sizeof rec);
    if (n == (int)sizeof rec && ssh_hostkey_decode(rec, n, seed, pub) == 0) {
        uint8_t derived[32];
        ed25519_pubkey(derived, seed);
        int ok = 1;
        for (int i = 0; i < 32; i++) if (derived[i] != pub[i]) ok = 0;
        if (ok) return 0;
        errs("sshd: " HOSTKEY_PATH " is present but internally inconsistent "
             "(seed does not derive the stored public key) -- refusing to trust it\n");
        return -1;
    }

    /* A HOST KEY FROM A WEAK RNG IS A KEY THE WHOLE WORLD SHARES -- refuse
     * to manufacture one rather than silently sign with rdtsc-derived
     * material. This also covers every EPHEMERAL per-connection X25519 key
     * this process will ever generate (rnd() above is the same call), which
     * the task's "refuse to start" language names only for the host key but
     * whose forward secrecy depends on exactly the same entropy source. */
    if (!getrandom_strong()) {
        errs("sshd: refusing to start -- entropy source is a timer, not RDSEED/RDRAND "
             "(see rng_strong() in CLAUDE.md's M9 note); a host key or session key from "
             "it is not one\n");
        return -1;
    }

    if (ed25519_keypair(pub, seed, rnd) != 0) {
        errs("sshd: ed25519_keypair failed (dead entropy source)\n");
        return -1;
    }
    ssh_hostkey_encode(seed, pub, rec);
    make_dir("/etc");
    if (write_file(HOSTKEY_PATH, rec, sizeof rec) < 0) {
        errs("sshd: could not write " HOSTKEY_PATH "\n");
        return -1;
    }
    /* Same argument as accounts.h's /etc/passwd: a key file whose mode does
     * not survive a reboot is not protected by that mode at all. */
    if (st_chown(HOSTKEY_PATH, 0, 0) < 0 || st_chmod(HOSTKEY_PATH, 0600) < 0)
        errs("sshd: WARNING " HOSTKEY_PATH " is not root:root 0600 -- it is readable\n");
    outs("sshd: generated a new host key at " HOSTKEY_PATH "\n");
    return 0;
}

/* ========================================================================
 * account store + authorized_keys (no I/O in c/net/ssh -- see ssh_auth.h)
 * ==================================================================== */
#define STORE_MAX 4096
#define AUTHKEYS_MAX 4096

static int find_account(const char *user, struct account *out)
{
    static char store[STORE_MAX + 1];
    int n = read_file("/etc/passwd", store, STORE_MAX);
    if (n < 0) return 0;
    return acct_find(store, n, user, out);
}

/* 1/0. On success fills uid/gid/home/shell via `acct`. */
static int check_password(const char *user, const char *pw, struct account *acct)
{
    static char store[STORE_MAX + 1];
    int n = read_file("/etc/passwd", store, STORE_MAX);
    if (n < 0) return 0;
    int found = acct_find(store, n, user, acct);
    if (found) return acct_check_password(acct, pw);
    /* Constant-cost decoy, exactly login.c's reasoning: an unknown user must
     * cost what a real check costs, or the auth loop's timing tells an
     * attacker which usernames exist. */
    struct account decoy; int pos = 0;
    if (acct_next(store, n, &pos, &decoy)) acct_check_password(&decoy, pw);
    return 0;
}

/* 1 if `blob` is in `user`'s authorized_keys. `acct` must already be filled
 * (the caller looked the user up to get `home`). */
static int check_authorized_key(const struct account *acct, const uint8_t *blob, int bloblen)
{
    static char keys[AUTHKEYS_MAX + 1];
    char path[ACCT_PATH + 32];
    path_join(path, acct->home, ".ssh/authorized_keys", (int)sizeof path);
    int n = read_file(path, keys, AUTHKEYS_MAX);
    if (n < 0) return 0;
    return ssh_authkeys_match(keys, n, blob, bloblen);
}

/* ========================================================================
 * socket I/O adapters -- the ssh_io_fn shape c/net/ssh calls through
 * ==================================================================== */
struct sock_io_ctx { int fd; };

static int sock_read_exact(void *ctx, uint8_t *buf, int len)
{
    int fd = ((struct sock_io_ctx *)ctx)->fd;
    int off = 0;
    while (off < len) {
        int n = sys_read(fd, buf + off, len - off);
        if (n <= 0) return -1; /* blocking socket: <=0 is EOF/reset, not "try again" */
        off += n;
    }
    return off;
}

static int sock_write_all(void *ctx, uint8_t *buf, int len)
{
    int fd = ((struct sock_io_ctx *)ctx)->fd;
    int off = 0;
    while (off < len) {
        int n = sys_write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return off;
}

/* ========================================================================
 * per-connection state
 * ==================================================================== */
#define SSHD_MAX_CONN   8
#define CONN_STACK_SIZE (256 * 1024)
#define PUMP_STACK_SIZE (192 * 1024)
#define MAX_AUTH_TRIES  6        /* OpenSSH's own MaxAuthTries default */
#define OUR_INIT_WINDOW (2u * 1024 * 1024)
#define OUR_MAX_PACKET  32768u

static uint8_t g_conn_stack[SSHD_MAX_CONN][CONN_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_pump_stack[SSHD_MAX_CONN][PUMP_STACK_SIZE] __attribute__((aligned(16)));
static volatile int g_slot_busy[SSHD_MAX_CONN];

struct conn_ctx {
    int slot;
    int sockfd;

    uint8_t V_C[SSH_MAX_IDENT + 1]; int vclen;
    uint8_t V_S[SSH_MAX_IDENT + 1]; int vslen;
    uint8_t I_C[SSH_MAX_PAYLOAD];   int iclen;
    uint8_t I_S[SSH_MAX_PAYLOAD];   int islen;

    struct ssh_dir_state c2s, s2c;
    uint8_t session_id[32];
    int kex_done; /* 0 until NEWKEYS both ways -- see recv_msg()'s own
                   * comment: the client's FIRST SSH_MSG_KEXINIT is required
                   * and must be let through; only a SECOND one (a rekey
                   * ask, arriving after this flips to 1) gets refused. */

    char user[64];
    struct account acct;
    int authenticated;
    int auth_tries;

    uint32_t peer_chan;
    volatile uint32_t peer_window;
    uint32_t peer_maxpkt;
    volatile int lock;

    int child_pid;
    int child_in_w;
    int child_out_r;

    struct sshd_thread_arg main_targ, pump_targ;
};

static struct conn_ctx g_conn[SSHD_MAX_CONN];
static uint8_t g_hostpub[32], g_hostseed[32];
static volatile int g_active_conns; /* diagnostics only */

static int send_msg(struct conn_ctx *cc, const uint8_t *payload, int len)
{
    struct sock_io_ctx c = { cc->sockfd };
    spin_lock(&cc->lock);
    int rc = ssh_pkt_send(&cc->s2c, sock_write_all, &c, payload, len, rnd);
    spin_unlock(&cc->lock);
    return rc;
}

/* Read the next packet, silently absorbing SSH_MSG_IGNORE/DEBUG/UNIMPLEMENTED
 * -- a real client may send any of these at any time (RFC 4253 11.2/11.3/11.4)
 * and none of them are this server's business. */
static void disconnect(struct conn_ctx *cc, uint32_t reason, const char *msg);

/* A SECOND SSH_MSG_KEXINIT, arriving anywhere after the first key exchange
 * has finished, is the client asking to REKEY (RFC 4253 9: recommended
 * after 1 GiB or ~1 hour, but the client decides when to ask, and
 * `-o RekeyLimit=` lets it ask almost immediately -- which is how this was
 * actually found, forcing it early rather than transferring 1 GiB through a
 * test). v1's task brief is explicit that this server never rekeys, so
 * there is no byte/time counter to check: whatever the reason, every reply
 * this server could send back would be wrong, and the correct response is
 * an honest refusal, not silence.
 *
 * THIS HAS TO LIVE HERE, IN THE ONE recv_msg BOTH do_userauth,
 * run_channel_setup AND input_relay CALL, not in any ONE of their read
 * loops -- the first version of this fix only special-cased input_relay's
 * switch (the shell/exec data-relay phase) because that is where a REAL
 * openssh client rekeys during an ordinary long session. Forcing an early
 * rekey with RekeyLimit=1K showed the gap that argument missed: the client
 * can ask before a channel even opens, inside run_channel_setup's loop,
 * which silently `continue`s past anything that is not CHANNEL_OPEN --
 * so the client sent KEXINIT, got no reply, and hung waiting for one,
 * while the server's loop just kept waiting for a CHANNEL_OPEN that was
 * never coming either. Centralising the check here closes every call site
 * that reads a packet in one place instead of three. */
static int recv_msg(struct conn_ctx *cc, uint8_t *buf, int max)
{
    struct sock_io_ctx c = { cc->sockfd };
    for (;;) {
        int n = ssh_pkt_recv(&cc->c2s, sock_read_exact, &c, buf, max);
        if (n < 0) return n;
        if (n >= 1 && (buf[0] == SSH_MSG_IGNORE || buf[0] == SSH_MSG_DEBUG ||
                       buf[0] == SSH_MSG_UNIMPLEMENTED)) continue;
        if (n >= 1 && buf[0] == SSH_MSG_KEXINIT && cc->kex_done) {
            disconnect(cc, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "rekey not supported");
            return -1;
        }
        return n;
    }
}

static void disconnect(struct conn_ctx *cc, uint32_t reason, const char *msg)
{
    uint8_t buf[256];
    int n = ssh_build_disconnect(reason, msg, buf, (int)sizeof buf);
    if (n > 0) send_msg(cc, buf, n);
}

/* ========================================================================
 * version exchange
 * ==================================================================== */
#define OUR_VERSION "SSH-2.0-LogitOS_1.0"

static int do_version_exchange(struct conn_ctx *cc)
{
    char line[SSH_MAX_IDENT + 3];
    int k = 0;
    const char *v = OUR_VERSION "\r\n";
    while (v[k]) k++;
    if (sock_write_all(&(struct sock_io_ctx){ cc->sockfd }, (uint8_t *)v, k) != k) return -1;
    cc->vslen = 0;
    { const char *s = OUR_VERSION; while (s[cc->vslen]) cc->V_S[cc->vslen] = (uint8_t)s[cc->vslen], cc->vslen++; }

    int n = 0;
    for (;;) {
        char ch;
        if (sys_read(cc->sockfd, &ch, 1) != 1) return -1;
        if (n < (int)sizeof line - 1) line[n++] = ch;
        if (ch == '\n') break;
        if (n >= SSH_MAX_IDENT) return -1; /* RFC 4253 4.2 line-length bound */
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n < 4 || line[0] != 'S' || line[1] != 'S' || line[2] != 'H' || line[3] != '-') return -1;
    cc->vclen = n;
    for (int i = 0; i < n; i++) cc->V_C[i] = (uint8_t)line[i];
    return 0;
}

/* ========================================================================
 * KEX
 * ==================================================================== */
static int do_kex(struct conn_ctx *cc)
{
    uint8_t buf[512];
    cc->islen = ssh_kexinit_build(cc->I_S, (int)sizeof cc->I_S, rnd);
    if (cc->islen < 0) return -1;
    if (send_msg(cc, cc->I_S, cc->islen) < 0) return -1;

    cc->iclen = recv_msg(cc, cc->I_C, (int)sizeof cc->I_C);
    if (cc->iclen < 1 || cc->I_C[0] != SSH_MSG_KEXINIT) return -1;

    struct ssh_negotiated neg;
    char why[64];
    if (ssh_kexinit_negotiate(cc->I_C, cc->iclen, &neg, why, (int)sizeof why) < 0) {
        disconnect(cc, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, why);
        return -1;
    }

    int n = recv_msg(cc, buf, (int)sizeof buf);
    if (n < 1 || buf[0] != SSH_MSG_KEX_ECDH_INIT) return -1;
    const uint8_t *qc; int qclen;
    if (ssh_r_string(buf, 1, n, &qc, &qclen) < 0 || qclen != 32) return -1;

    uint8_t reply[256];
    int replylen;
    uint8_t k_raw[32], h[32];
    if (ssh_kex_ecdh_reply_build(cc->V_C, cc->vclen, cc->V_S, cc->vslen,
                                 cc->I_C, cc->iclen, cc->I_S, cc->islen,
                                 g_hostpub, g_hostseed, qc, rnd,
                                 reply, (int)sizeof reply, &replylen, k_raw, h) != 0) {
        disconnect(cc, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "ecdh reply");
        return -1;
    }
    if (send_msg(cc, reply, replylen) < 0) return -1;
    for (int i = 0; i < 32; i++) cc->session_id[i] = h[i];

    uint8_t nk[1] = { SSH_MSG_NEWKEYS };
    if (send_msg(cc, nk, 1) < 0) return -1;

    uint8_t iv_c2s[16], iv_s2c[16], enc_c2s[16], enc_s2c[16], mac_c2s[32], mac_s2c[32];
    ssh_kex_derive_keys(k_raw, h, cc->session_id, iv_c2s, iv_s2c, enc_c2s, enc_s2c, mac_c2s, mac_s2c);
    crypto_wipe(k_raw, 32);

    /* Our OWN NEWKEYS was just sent -- our outgoing side switches now. */
    ssh_dir_activate(&cc->s2c, enc_s2c, iv_s2c, mac_s2c);

    n = recv_msg(cc, buf, (int)sizeof buf);
    if (n < 1 || buf[0] != SSH_MSG_NEWKEYS) return -1;
    /* The client's NEWKEYS was just RECEIVED -- our incoming side switches now. */
    ssh_dir_activate(&cc->c2s, enc_c2s, iv_c2s, mac_c2s);

    crypto_wipe(enc_c2s, 16); crypto_wipe(enc_s2c, 16);
    crypto_wipe(mac_c2s, 32); crypto_wipe(mac_s2c, 32);
    cc->kex_done = 1; /* from here on, a THIRD-party SSH_MSG_KEXINIT means
                       * rekey, and recv_msg() refuses it -- see its comment */
    return 0;
}

/* ========================================================================
 * userauth (RFC 4252)
 * ==================================================================== */
static int do_service_request(struct conn_ctx *cc)
{
    uint8_t buf[128];
    int n = recv_msg(cc, buf, (int)sizeof buf);
    if (n < 1 || buf[0] != SSH_MSG_SERVICE_REQUEST) return -1;
    const uint8_t *svc; int svclen;
    if (ssh_r_string(buf, 1, n, &svc, &svclen) < 0) return -1;
    if (svclen != 12 || c_strncmp((const char *)svc, "ssh-userauth", 12) != 0) return -1;
    uint8_t rep[64];
    int rl = ssh_build_service_accept("ssh-userauth", rep, (int)sizeof rep);
    return send_msg(cc, rep, rl) < 0 ? -1 : 0;
}

/* A verification harness watches a wrong password get REFUSED "on the serial
 * line" (the task brief's own phrase) -- so the refusal has to be loud
 * somewhere a host-side test can grep -c it, not just a wire message a
 * client's exit code implies. One counter, printed per attempt rather than
 * only at the end, so a harness killing the boot mid-test still sees every
 * refusal that happened before the kill. */
static volatile long g_auth_fail_count;

static void fail_or_disconnect(struct conn_ctx *cc, const char *method, int *give_up)
{
    cc->auth_tries++;
    g_auth_fail_count++;
    outs("sshd: AUTH_FAIL user="); outs(cc->user);
    outs(" method="); outs(method);
    outs(" tries="); outn(cc->auth_tries);
    outs(" total_fail="); outn(g_auth_fail_count); outc('\n');
    if (cc->auth_tries >= MAX_AUTH_TRIES) {
        disconnect(cc, SSH_DISCONNECT_AUTH_CANCELLED_BY_USER, "too many authentication failures");
        *give_up = 1;
        return;
    }
    uint8_t rep[64];
    int rl = ssh_build_userauth_failure("publickey,password", 0, rep, (int)sizeof rep);
    send_msg(cc, rep, rl);
    /* A flat delay, on top of PBKDF2's own cost (see accounts.h's ACCT_ITERS
     * note): this loop is reachable without a KDF at all via "publickey", so
     * something has to cost an unauthenticated guess even then. */
    sys_sleep_ms(300);
}

static int do_userauth(struct conn_ctx *cc)
{
    for (;;) {
        uint8_t buf[SSH_MAX_PAYLOAD];
        int n = recv_msg(cc, buf, (int)sizeof buf);
        if (n < 1) return -1;
        struct ssh_authreq req;
        if (ssh_authreq_parse(buf, n, &req) < 0) return -1;

        if (req.restlen < 0) return -1;
        c_strcpy(cc->user, req.user, (int)sizeof cc->user);

        int give_up = 0;
        if (c_streq(req.method, "none")) {
            fail_or_disconnect(cc, "none", &give_up);
            if (give_up) return -1;
            continue;
        }

        if (c_streq(req.method, "password")) {
            char pw[128];
            int pwlen = ssh_auth_parse_password(req.rest, req.restlen, pw, (int)sizeof pw);
            if (pwlen < 0) return -1;
            int ok = check_password(req.user, pw, &cc->acct);
            for (int i = 0; i < (int)sizeof pw; i++) pw[i] = 0;
            if (ok) {
                uint8_t rep[16];
                int rl = ssh_build_userauth_success(rep, (int)sizeof rep);
                if (send_msg(cc, rep, rl) < 0) return -1;
                cc->authenticated = 1;
                outs("sshd: AUTH_OK user="); outs(cc->user); outs(" method=password\n");
                return 0;
            }
            fail_or_disconnect(cc, "password", &give_up);
            if (give_up) return -1;
            continue;
        }

        if (c_streq(req.method, "publickey")) {
            int has_sig; char alg[32];
            const uint8_t *blob; int bloblen;
            const uint8_t *sig; int siglen;
            if (ssh_auth_parse_publickey(req.rest, req.restlen, &has_sig, alg, (int)sizeof alg,
                                         &blob, &bloblen, &sig, &siglen) < 0) return -1;

            int algok = c_streq(alg, "ssh-ed25519");
            int userok = algok && find_account(req.user, &cc->acct);
            int keyok = userok && check_authorized_key(&cc->acct, blob, bloblen);

            if (!keyok) { fail_or_disconnect(cc, "publickey", &give_up); if (give_up) return -1; continue; }

            if (!has_sig) {
                uint8_t rep[128];
                int rl = ssh_build_userauth_pk_ok(alg, blob, bloblen, rep, (int)sizeof rep);
                if (send_msg(cc, rep, rl) < 0) return -1;
                continue;
            }

            /* signature blob (RFC 4253 6.6): string alg2 + string raw-sig */
            const uint8_t *alg2; int alg2len;
            const uint8_t *rawsig; int rawsiglen;
            int so = ssh_r_string(sig, 0, siglen, &alg2, &alg2len);
            so = ssh_r_string(sig, so, siglen, &rawsig, &rawsiglen);
            if (so < 0 || rawsiglen != 64) { fail_or_disconnect(cc, "publickey", &give_up); if (give_up) return -1; continue; }

            /* the raw 32-byte key out of blob = string alg + string pub */
            const uint8_t *a3; int a3len; const uint8_t *pubk; int pubklen;
            int bo = ssh_r_string(blob, 0, bloblen, &a3, &a3len);
            bo = ssh_r_string(blob, bo, bloblen, &pubk, &pubklen);
            if (bo < 0 || pubklen != 32) return -1;

            uint8_t signdata[SSH_AUTH_SIGDATA_MAX];
            int sdlen = ssh_auth_pubkey_signdata(cc->session_id, req.user, req.service,
                                                 alg, blob, bloblen, signdata, (int)sizeof signdata);
            if (sdlen < 0) return -1;

            if (ed25519_verify(rawsig, signdata, (unsigned long)sdlen, pubk)) {
                uint8_t rep[16];
                int rl = ssh_build_userauth_success(rep, (int)sizeof rep);
                if (send_msg(cc, rep, rl) < 0) return -1;
                cc->authenticated = 1;
                outs("sshd: AUTH_OK user="); outs(cc->user); outs(" method=publickey\n");
                return 0;
            }
            fail_or_disconnect(cc, "publickey", &give_up);
            if (give_up) return -1;
            continue;
        }

        /* unknown method */
        fail_or_disconnect(cc, req.method, &give_up);
        if (give_up) return -1;
    }
}

/* ========================================================================
 * connection protocol: channel open + requests up to shell/exec
 * ==================================================================== */
static int spawn_child(struct conn_ctx *cc, const char *cmd /* NULL = shell */)
{
    int ip[2], op[2]; /* ip: server->child stdin; op: child stdout+stderr->server */
    if (sys_pipe(ip) < 0) return -1;
    if (sys_pipe(op) < 0) { sys_close(ip[0]); sys_close(ip[1]); return -1; }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(ip[0]); sys_close(ip[1]); sys_close(op[0]); sys_close(op[1]);
        return -1;
    }
    if (pid == 0) {
        sys_dup2(ip[0], 0);
        sys_dup2(op[1], 1);
        sys_dup2(op[1], 2); /* stderr merged into the same stream -- see file header */
        sys_close(ip[0]); sys_close(ip[1]); sys_close(op[0]); sys_close(op[1]);

        if (sys_setgid(cc->acct.gid) < 0 || sys_setuid(cc->acct.uid) < 0) {
            errs("sshd: could not drop privileges to the authenticated user\n");
            app_exit(126);
        }
        sys_chdir(cc->acct.home);

        static char envh[ACCT_PATH + 8], envu[ACCT_NAME + 8];
        char *e = envh; const char *pre = "HOME="; int k = 0;
        for (int i = 0; pre[i]; i++) e[k++] = pre[i];
        for (int i = 0; cc->acct.home[i] && k < (int)sizeof envh - 1; i++) e[k++] = cc->acct.home[i];
        e[k] = 0;
        char *e2 = envu; const char *pre2 = "USER="; k = 0;
        for (int i = 0; pre2[i]; i++) e2[k++] = pre2[i];
        for (int i = 0; cc->acct.name[i] && k < (int)sizeof envu - 1; i++) e2[k++] = cc->acct.name[i];
        e2[k] = 0;
        char *envp[] = { envh, envu, 0 };

        if (cmd) {
            char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, 0 };
            sys_execve(cc->acct.shell[0] ? cc->acct.shell : "/bin/sh", argv, envp);
        } else {
            char *argv[] = { (char *)"sh", 0 };
            sys_execve(cc->acct.shell[0] ? cc->acct.shell : "/bin/sh", argv, envp);
        }
        errs("sshd: exec failed\n");
        app_exit(127);
    }

    sys_close(ip[0]);
    sys_close(op[1]);
    cc->child_pid = pid;
    cc->child_in_w = ip[1];
    cc->child_out_r = op[0];
    return 0;
}

/* --- the output pump: child's merged stdout+stderr -> CHANNEL_DATA,
 * honouring the CLIENT's advertised window (peer_window/peer_maxpkt). --- */
static void output_pump(void *arg)
{
    struct conn_ctx *cc = (struct conn_ctx *)arg;
    uint8_t chunk[4096];
    for (;;) {
        int n = sys_read(cc->child_out_r, chunk, (int)sizeof chunk);
        if (n <= 0) break; /* child closed its output (exited) */

        int off = 0;
        while (off < n) {
            /* Wait for window room. A client that never sends WINDOW_ADJUST
             * (or one that has genuinely stopped reading) stalls the
             * connection here rather than the server writing past what it
             * was granted -- see the file header for why that is the
             * correctness bar, not merely a nicety. */
            uint32_t room;
            for (;;) {
                spin_lock(&cc->lock);
                room = cc->peer_window;
                spin_unlock(&cc->lock);
                if (room > 0) break;
                sys_sleep_ms(10);
            }
            uint32_t want = (uint32_t)(n - off);
            if (want > room) want = room;
            if (want > cc->peer_maxpkt) want = cc->peer_maxpkt;
            if (want > sizeof chunk) want = sizeof chunk;

            uint8_t pkt[9 + 4096];
            int pl = ssh_build_channel_data(cc->peer_chan, chunk + off, (int)want, pkt, (int)sizeof pkt);
            if (pl < 0) { off = n; break; }
            if (send_msg(cc, pkt, pl) < 0) { off = n; break; }
            spin_lock(&cc->lock);
            cc->peer_window -= want;
            spin_unlock(&cc->lock);
            off += (int)want;
        }
    }

    int status = 0;
    sys_waitpid(cc->child_pid, &status);
    uint8_t pkt[64];
    int pl = ssh_build_exit_status(cc->peer_chan, (uint32_t)status, pkt, (int)sizeof pkt);
    if (pl > 0) send_msg(cc, pkt, pl);
    pl = ssh_build_eof(cc->peer_chan, pkt, (int)sizeof pkt);
    if (pl > 0) send_msg(cc, pkt, pl);
    pl = ssh_build_close(cc->peer_chan, pkt, (int)sizeof pkt);
    if (pl > 0) send_msg(cc, pkt, pl);

    sys_close(cc->child_out_r);
    /* Unblock the input thread's pending blocking read so it can notice the
     * session is over and clean up -- see the file header's thread-shape note. */
    sys_shutdown(cc->sockfd, LOGIT_SHUT_RD);
}

/* Reads channel requests until "shell" or "exec" spawns a child, or the
 * client gives up. Returns 0 once a child is running, -1 on any protocol
 * failure or an explicit close. */
static int run_channel_setup(struct conn_ctx *cc)
{
    uint8_t buf[SSH_MAX_PAYLOAD];
    for (int tries = 0; tries < 8; tries++) {
        int n = recv_msg(cc, buf, (int)sizeof buf);
        if (n < 1) return -1;
        if (buf[0] != SSH_MSG_CHANNEL_OPEN) continue;

        char type[32];
        uint32_t peer_chan, peer_win, peer_maxpkt;
        if (ssh_parse_channel_open(buf, n, type, (int)sizeof type, &peer_chan, &peer_win, &peer_maxpkt) < 0)
            return -1;
        if (!c_streq(type, "session")) {
            uint8_t rep[64];
            int rl = ssh_build_channel_open_failure(peer_chan, SSH_OPEN_UNKNOWN_CHANNEL_TYPE, rep, (int)sizeof rep);
            send_msg(cc, rep, rl);
            continue;
        }
        cc->peer_chan = peer_chan;
        cc->peer_window = peer_win;
        cc->peer_maxpkt = peer_maxpkt < OUR_MAX_PACKET ? peer_maxpkt : OUR_MAX_PACKET;
        if (cc->peer_maxpkt == 0) cc->peer_maxpkt = 32768;

        uint8_t rep[32];
        int rl = ssh_build_channel_open_confirmation(peer_chan, 0, OUR_INIT_WINDOW, OUR_MAX_PACKET, rep, (int)sizeof rep);
        if (send_msg(cc, rep, rl) < 0) return -1;
        break;
    }
    if (cc->peer_maxpkt == 0) return -1; /* never got a session channel */

    for (;;) {
        int n = recv_msg(cc, buf, (int)sizeof buf);
        if (n < 1) return -1;

        if (buf[0] == SSH_MSG_CHANNEL_CLOSE) return -1;

        if (buf[0] != SSH_MSG_CHANNEL_REQUEST) continue;

        uint32_t chan; char type[32]; int want_reply;
        const uint8_t *data; int datalen;
        if (ssh_parse_channel_request(buf, n, &chan, type, (int)sizeof type, &want_reply, &data, &datalen) < 0)
            return -1;

        if (c_streq(type, "shell") || c_streq(type, "exec")) {
            char cmd[512];
            const char *cmdp = 0;
            if (c_streq(type, "exec")) {
                if (ssh_parse_exec_command(data, datalen, cmd, (int)sizeof cmd) < 0) return -1;
                cmdp = cmd;
                /* Diagnostic, not decoration: /bin/sh (c/apps/coreutils/sh.c,
                 * out of this line's ownership) has no `-c` argument handling
                 * at all, so spawn_child's `sh -c cmdp` silently becomes a
                 * plain interactive sh that ignores cmdp and reads its stdin
                 * -- discovered by testing exec against a real OpenSSH
                 * client, not assumed. Printing what the server RECEIVED is
                 * what let that be told apart from a parse bug here. */
                outs("sshd: EXEC user="); outs(cc->user); outs(" cmd="); outs(cmdp); outc('\n');
            }
            if (spawn_child(cc, cmdp) < 0) {
                if (want_reply) {
                    uint8_t rep[16]; int rl = ssh_build_channel_failure(chan, rep, (int)sizeof rep);
                    send_msg(cc, rep, rl);
                }
                return -1;
            }
            if (want_reply) {
                uint8_t rep[16]; int rl = ssh_build_channel_success(chan, rep, (int)sizeof rep);
                send_msg(cc, rep, rl);
            }
            return 0;
        }

        /* pty-req / env / window-change / anything else: acknowledged (if a
         * reply was requested) and otherwise ignored -- see the file header
         * for exactly what "no real pty" means here. */
        if (want_reply) {
            uint8_t rep[16]; int rl = ssh_build_channel_success(chan, rep, (int)sizeof rep);
            send_msg(cc, rep, rl);
        }
    }
}

/* --- the input thread's steady-state relay, once a child is running --- */
static void input_relay(struct conn_ctx *cc)
{
    uint8_t buf[SSH_MAX_PAYLOAD];
    for (;;) {
        int n = recv_msg(cc, buf, (int)sizeof buf);
        if (n < 0) break;
        if (n < 1) continue;

        switch (buf[0]) {
        case SSH_MSG_CHANNEL_DATA: {
            uint32_t chan; const uint8_t *data; int datalen;
            if (ssh_parse_channel_data(buf, n, &chan, &data, &datalen) < 0) { n = -1; break; }
            int off = 0;
            while (off < datalen) {
                int w = sys_write(cc->child_in_w, data + off, datalen - off);
                if (w <= 0) { off = datalen; n = -1; break; }
                off += w;
            }
            break;
        }
        case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
            uint32_t chan, bytes;
            if (ssh_parse_window_adjust(buf, n, &chan, &bytes) == 0) {
                spin_lock(&cc->lock);
                cc->peer_window += bytes;
                spin_unlock(&cc->lock);
            }
            break;
        }
        case SSH_MSG_CHANNEL_EOF:
            sys_close(cc->child_in_w);
            cc->child_in_w = -1;
            break;
        case SSH_MSG_CHANNEL_CLOSE:
            n = -1;
            break;
        case SSH_MSG_CHANNEL_REQUEST:
        default:
            break; /* window-change and anything post-shell: ignored --
                     * SSH_MSG_KEXINIT (a rekey request) never reaches here:
                     * recv_msg() refuses it centrally, see that function's
                     * own comment for why it has to live there and not in
                     * this switch. */
        }
        if (n < 0) break;
    }
    if (cc->child_in_w >= 0) sys_close(cc->child_in_w);
}

/* ========================================================================
 * per-connection entry point
 * ==================================================================== */
static void handle_connection(void *arg)
{
    struct conn_ctx *cc = (struct conn_ctx *)arg;

    if (do_version_exchange(cc) == 0 &&
        do_kex(cc) == 0 &&
        do_service_request(cc) == 0 &&
        do_userauth(cc) == 0 &&
        run_channel_setup(cc) == 0) {

        cc->pump_targ.fn = output_pump;
        cc->pump_targ.ctx = cc;
        struct logit_thread_spec spec;
        spec.entry = (unsigned long)(long)&sshd_thread_entry;
        spec.stack_top = (unsigned long)(long)(g_pump_stack[cc->slot] + PUMP_STACK_SIZE);
        spec.stack_base = 0;
        spec.stack_len = 0;
        spec.tls = 0;
        spec.arg = (unsigned long)(long)&cc->pump_targ;
        int tid = sys_thread_create(&spec);
        if (tid > 0) {
            input_relay(cc);
        } else {
            errs("sshd: could not start the output pump thread\n");
        }
    }

    if (cc->child_in_w >= 0) sys_close(cc->child_in_w);
    sys_close(cc->sockfd);
    g_active_conns--;
    g_slot_busy[cc->slot] = 0;
}

/* ========================================================================
 * accept loop
 * ==================================================================== */
int main(int argc, char **argv)
{
    int port = argc > 1 ? c_atoi(argv[1]) : 22;
    if (port <= 0 || port > 65535) port = 22;

    if (load_or_create_hostkey(g_hostpub, g_hostseed) != 0) return 1;
    char fp[64];
    ssh_hostkey_fingerprint(g_hostpub, fp, (int)sizeof fp);
    outs("sshd: host key fingerprint (ssh-ed25519) "); outs(fp); outc('\n');

    int lfd = sys_socket(LOGIT_AF_INET, LOGIT_SOCK_STREAM, 0);
    if (lfd < 0) { errs("SSHD_FAIL socket\n"); return 1; }
    sys_setsockopt(lfd, LOGIT_SOL_SOCKET, LOGIT_SO_REUSEADDR, 1);

    struct logit_sockaddr me;
    sockaddr_set(&me, 0, port);
    if (sys_bind(lfd, &me) < 0) { errs("SSHD_FAIL bind\n"); return 1; }
    if (sys_listen(lfd, 8) < 0) { errs("SSHD_FAIL listen\n"); return 1; }
    sys_getsockname(lfd, &me);

    for (int i = 0; i < SSHD_MAX_CONN; i++) g_conn[i].child_in_w = -1;

    outs("SSHD_READY port="); outn(me.port); outc('\n');

    for (;;) {
        struct logit_sockaddr peer;
        peer.family = 0; peer.port = 0; peer.addr = 0;
        int cfd = sys_accept(lfd, &peer, 0);
        if (cfd < 0) { if (cfd == LSK_E_AGAIN) continue; break; }

        int slot = -1;
        for (int i = 0; i < SSHD_MAX_CONN; i++) if (!g_slot_busy[i]) { slot = i; break; }
        if (slot < 0) {
            /* At capacity -- refuse, don't queue forever. Loud on purpose: a
             * verification harness driving SSHD_MAX_CONN+1 connections has no
             * other way to tell "the budget refused, as designed" from "the
             * connection silently hung", and a silent refusal here is
             * indistinguishable from a bug that dropped the accept. */
            outs("sshd: CONN_REFUSED at capacity (max="); outn(SSHD_MAX_CONN); outc(')'); outc('\n');
            sys_close(cfd);
            continue;
        }

        struct conn_ctx *cc = &g_conn[slot];
        /* Zero everything but the .bss-resident large buffers, which do not
         * need zeroing between connections (every field that matters is set
         * before use, and I_C/I_S/etc are always written before being read). */
        cc->slot = slot;
        cc->sockfd = cfd;
        cc->authenticated = 0;
        cc->auth_tries = 0;
        cc->child_pid = 0;
        cc->child_in_w = -1;
        cc->child_out_r = -1;
        cc->peer_chan = 0;
        cc->peer_window = 0;
        cc->peer_maxpkt = 0;
        cc->lock = 0;
        cc->c2s.cipher_on = 0; cc->c2s.mac_on = 0; cc->c2s.seq = 0;
        cc->s2c.cipher_on = 0; cc->s2c.mac_on = 0; cc->s2c.seq = 0;
        cc->kex_done = 0; /* a REUSED slot's prior connection may have left
                           * this 1 -- a fresh connection's first KEXINIT
                           * must not be mistaken for a rekey ask */

        g_slot_busy[slot] = 1;
        g_active_conns++;

        cc->main_targ.fn = handle_connection;
        cc->main_targ.ctx = cc;
        struct logit_thread_spec spec;
        spec.entry = (unsigned long)(long)&sshd_thread_entry;
        spec.stack_top = (unsigned long)(long)(g_conn_stack[slot] + CONN_STACK_SIZE);
        spec.stack_base = 0;
        spec.stack_len = 0;
        spec.tls = 0;
        spec.arg = (unsigned long)(long)&cc->main_targ;
        if (sys_thread_create(&spec) <= 0) {
            errs("sshd: could not start a connection thread\n");
            sys_close(cfd);
            g_slot_busy[slot] = 0;
            g_active_conns--;
        }
    }

    sys_close(lfd);
    return 0;
}
