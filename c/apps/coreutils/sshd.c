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
/* 2026-09-10: the historical v1 notes above describe the old relay. Pipes
 * now poll stdout and stderr separately; unsupported PTY/subsystem requests
 * fail explicitly. Workers are joined before their caller-owned stack/slot
 * is reused. The default disk includes sshd; starting it remains explicit. */
/* 2026-09-11: PTY now uses the real kernel terminal/termios device; SFTP is
 * an account-scoped child process. recv_msg handles client-initiated rekey,
 * and opt-in direct-tcpip uses owned IPv4 descriptors. The guest gate runs
 * stock OpenSSH (including Ctrl+C, live resize and overlapping channel
 * close) plus SFTP transfers beyond 2 MiB. Full POSIX job control, concurrent
 * channels, remote forwarding and server-initiated rekey remain absent. */
/* 2026-09-11, second pass: the latter three gaps are implemented in
 * sshd_channels.h. Four independently buffered channels share each transport;
 * an opted-in loopback remote listener creates forwarded-tcpip channels.
 * Default rekey triggers are 1 GiB of conservatively counted transport bytes
 * or 1 hour; `sshd [port] [rekey_bytes] [rekey_seconds]` overrides them, with
 * 0 disabling that trigger. Normal OpenSSH guest checks cover paused stdin,
 * concurrent forwarding, cancellation/rebind and both peers initiating KEX.
 * Full POSIX process groups and job control are still absent. */
#include "logit.h"
#include "clib.h"
#include "logit_stat.h"
#include "accounts.h"
#include "../../../include/abi/pty.h"

#include "ssh.h"
#include "ssh_wire.h"
#include "ssh_packet.h"
#include "ssh_kex.h"
#include "ssh_hostkey.h"
#include "ssh_auth.h"
#include "ssh_pubkey.h"
#include "ssh_conn.h"
#include "crypto.h"

/* The coreutils link no libc (see pkgverify.c's identical note) but every
 * crypto TU here is ordinary C: clang emits memcpy/memset for struct
 * assignment and array init regardless of -ffreestanding. */
/* Also linked with the optional agent libc; these standalone fallbacks yield
 * to that runtime's strong string implementations when present. */
__attribute__((weak)) void *memcpy(void *d, const void *s, unsigned long n)
{ unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s; for (unsigned long i = 0; i < n; i++) a[i] = b[i]; return d; }
__attribute__((weak)) void *memset(void *d, int c, unsigned long n)
{ unsigned char *a = (unsigned char *)d; for (unsigned long i = 0; i < n; i++) a[i] = (unsigned char)c; return d; }
__attribute__((weak)) int memcmp(const void *a,const void *b,unsigned long n)
{ const unsigned char *x=a,*y=b;for(unsigned long i=0;i<n;i++)if(x[i]!=y[i])return (int)x[i]-y[i];return 0; }

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

/* Descriptor I/O works in a CLI process and distinguishes truncation from a
 * complete record. Account buffers are per call: simultaneous authentications
 * must never parse another connection's static scratch storage. */
static int read_record(const char *path, void *buf, int cap)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    int used = 0, n;
    while (used < cap && (n = sys_read(fd, (char *)buf + used, cap - used)) > 0)
        used += n;
    char extra;
    int tail = used == cap ? sys_read(fd, &extra, 1) : n;
    sys_close(fd);
    return tail != 0 ? -1 : used;
}

static int load_or_create_hostkey(uint8_t pub[32], uint8_t seed[32])
{
    /* Required even with an existing identity: every new X25519 exchange
     * consumes entropy. Loading yesterday's key cannot waive today's check. */
    if (!getrandom_strong()) {
        errs("sshd: refusing to start without strong session entropy\n");
        return -1;
    }
    uint8_t rec[SSH_HOSTKEY_RECORD_LEN];
    struct logit_stat st;
    if (st_lstat(HOSTKEY_PATH, &st) == 0) {
        if ((st.mode & LST_IFMT) != LST_IFREG || st.uid != 0 || (st.mode & 077) ||
            read_record(HOSTKEY_PATH, rec, sizeof rec) != (int)sizeof rec ||
            ssh_hostkey_decode(rec, sizeof rec, seed, pub) != 0) {
            errs("sshd: existing host key must be a valid root-owned private record (0600)\n");
            return -1;
        }
        uint8_t derived[32];
        ed25519_pubkey(derived, seed);
        for (int i = 0; i < 32; i++) if (derived[i] != pub[i]) {
            errs("sshd: inconsistent host key; restore it explicitly\n");
            return -1;
        }
        return 0;
    }
    if (ed25519_keypair(pub, seed, rnd) != 0) return -1;
    ssh_hostkey_encode(seed, pub, rec);
    make_dir("/etc");
    /* Set before creation, not after writing a temporarily public key. */
    int oldmask = st_umask(0077);
    int fd = sys_open(HOSTKEY_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    st_umask(oldmask);
    if (fd < 0) return -1;
    int used = 0;
    while (used < (int)sizeof rec) {
        int n = sys_write(fd, rec + used, sizeof rec - used);
        if (n <= 0) break;
        used += n;
    }
    int closed = sys_close(fd);
    if (closed < 0 || used != (int)sizeof rec || st_chown(HOSTKEY_PATH, 0, 0) < 0 ||
        st_chmod(HOSTKEY_PATH, 0600) < 0) {
        errs("sshd: cannot persist a private host key\n");
        return -1;
    }
    outs("sshd: generated a new host key at " HOSTKEY_PATH "\n");
    return 0;
}

/* ========================================================================
 * account store + authorized_keys (no I/O in c/net/ssh -- see ssh_auth.h)
 * ==================================================================== */
#define STORE_MAX 4096
#define AUTHKEYS_MAX 16384 /* multiple RSA-4096 and NIST keys in one account */

static int find_account(const char *user, struct account *out)
{
    char store[STORE_MAX + 1];
    int n = read_record("/etc/passwd", store, STORE_MAX);
    if (n < 0) return 0;
    return acct_find(store, n, user, out);
}

/* 1/0. On success fills uid/gid/home/shell via `acct`. */
static int check_password(const char *user, const char *pw, struct account *acct)
{
    char store[STORE_MAX + 1];
    int n = read_record("/etc/passwd", store, STORE_MAX);
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
    char keys[AUTHKEYS_MAX + 1];
    char path[ACCT_PATH + 32];
    path_join(path, acct->home, ".ssh/authorized_keys", (int)sizeof path);
    struct logit_stat st;
    if (st_lstat(path, &st) < 0 || (st.mode & LST_IFMT) != LST_IFREG ||
        (st.uid != 0 && st.uid != acct->uid) || (st.mode & 022)) return 0;
    int n = read_record(path, keys, AUTHKEYS_MAX);
    if (n < 0) return 0;
    return ssh_authkeys_match(keys, n, blob, bloblen);
}

/* SIGCHLD can interrupt ANY thread in this server, including accept or
 * another session's KEX read. Retry every blocking adapter without losing
 * the partial-byte offset; a signal is not a broken transport. */
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
        if (n == SIG_E_INTR) continue;
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
        if (n == SIG_E_INTR) continue;
        if (n <= 0) return -1;
        off += n;
    }
    return off;
}

/* ========================================================================
 * per-connection state
 * ==================================================================== */
#define SSHD_MAX_CONN   4 /* 32 process fds, four per session + transient fork pipes */
#define CONN_STACK_SIZE (256 * 1024)
#define PUMP_STACK_SIZE (192 * 1024)
#define WATCHDOG_STACK_SIZE (32 * 1024)
#define MAX_AUTH_TRIES  6        /* OpenSSH's own MaxAuthTries default */
#include "sshd_channel_state.h"
#define OUR_INIT_WINDOW SSHD_INPUT_CAP
/* Channel maxpacket is a DATA length to OpenSSH, while the transport bound
 * includes channel/type/string headers. Advertising 32768 made ordinary
 * full-sized upload packets exceed our 32768-byte parser buffer. */
#define OUR_MAX_PACKET  (SSH_MAX_PAYLOAD - 13u)
_Static_assert(OUR_MAX_PACKET + 13u <= SSH_MAX_PAYLOAD,"channel header fits transport");

/* Pre-auth deadline. A connection that has not AUTHENTICATED within this is
 * reaped by the watchdog below: without it, eight sockets that connect and
 * then send nothing hold every slot forever (blocking reads, no timeout
 * anywhere on the path), which is a pre-auth denial of service one
 * slowloris loop away -- the attack battery holds all eight and watches the
 * ninth get CONN_REFUSED. 30 s because a REAL client finishes pre-auth in
 * under 2 s here (QEMU TCG, measured by the boot test's own timings), and
 * there is no interactive pre-auth prompting to wait patiently for.
 *
 * POST-auth idle is deliberately NOT bounded: a long silent session is
 * legitimate SSH usage (an open shell somebody went to lunch on), and
 * OpenSSH itself ships no idle timeout by default. */
#define SSHD_PREAUTH_TIMEOUT_MS 30000

static uint8_t g_conn_stack[SSHD_MAX_CONN][CONN_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_pump_stack[SSHD_MAX_CONN][PUMP_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_watchdog_stack[WATCHDOG_STACK_SIZE] __attribute__((aligned(16)));
static volatile int g_slot_busy[SSHD_MAX_CONN];
static int g_conn_tid[SSHD_MAX_CONN];

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
    /* 2026-09-10 correction: rekey is now performed. Application packets
     * pause between our KEXINIT and completion; transport packets continue. */
    volatile int kex_busy;
    struct channel_ctx channels[SSHD_CHANNELS];
    struct remote_listener remote[SSHD_REMOTE_LISTENERS];
    uint32_t next_channel;
    unsigned long long key_bytes,key_since_ns;
    int nreplies;
    struct { int len; uint8_t data[512]; } replies[64];

    char user[64];
    struct account acct;
    int authenticated;
    int auth_tries;

    volatile int lock;

    /* Pre-auth reap state, for the watchdog thread (see
     * SSHD_PREAUTH_TIMEOUT_MS above). `gen` is bumped by the accept loop at
     * slot assignment AND by handle_connection's cleanup before the close,
     * so the watchdog can tell "this slot was recycled under me" from "my
     * reap is still aimed at the connection I decided on". */
    unsigned long long connected_ns;
    volatile unsigned gen;
    volatile int preauth_reaped;

    volatile int stopping;

    struct sshd_thread_arg main_targ, pump_targ;
};

static struct conn_ctx g_conn[SSHD_MAX_CONN];
static uint8_t g_hostpub[32], g_hostseed[32];
static unsigned long long g_rekey_bytes=1024ull*1024*1024;
static unsigned long long g_rekey_ns=3600ull*1000000000;
static volatile int g_active_conns; /* diagnostics only */
static struct sshd_thread_arg g_watchdog_targ;

/* The pre-auth reaper (see SSHD_PREAUTH_TIMEOUT_MS for the deadline's own
 * argument). Every read on the pre-auth path is BLOCKING with no timeout,
 * so an unauthenticated connection that goes silent is otherwise
 * unreclaimable -- the slot it holds is gone until process restart, and
 * eight of them are the whole budget. The reap itself is the same trick
 * output_pump uses at the other end of a connection's life:
 * sys_shutdown(SHUT_RD) turns the owner thread's pending read into an EOF,
 * and the ordinary cleanup path does the rest (close, slot free).
 *
 * The recycle race, and why it is bounded rather than closed: for a reap to
 * hit the WRONG connection, the slot's connection must tear down, the fd
 * close, the accept loop reassign that fd, AND a fresh connection take the
 * slot -- all between this thread's volatile gen re-read and the shutdown
 * call, a handful of instructions. Cleanup bumps `gen` BEFORE closing the
 * fd precisely so the re-read catches every one of those paths; if the
 * microsecond window ever loses, the damage is one freshly-connected client
 * seeing an immediate clean EOF (retryable, and PREAUTH_TIMEOUT names the
 * slot on the serial line) -- never corruption, never a cross-connection
 * byte leak, because shutdown() touches no data of its own. */
static void preauth_watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        sys_sleep_ms(1000);
        unsigned long long now = monotonic_ns();
        for (int i = 0; i < SSHD_MAX_CONN; i++) {
            if (!g_slot_busy[i]) continue;
            struct conn_ctx *cc = &g_conn[i];
            if (cc->authenticated) continue; /* post-auth idle is legitimate */
            unsigned gen_snapshot;
            int fd, expired;
            spin_lock(&cc->lock);
            gen_snapshot = cc->gen;
            fd = cc->sockfd;
            expired = !cc->preauth_reaped && cc->connected_ns != 0 &&
                      now > cc->connected_ns +
                                (unsigned long long)SSHD_PREAUTH_TIMEOUT_MS * 1000000ull;
            if (expired) cc->preauth_reaped = 1;
            spin_unlock(&cc->lock);
            if (!expired) continue;
            if (cc->gen != gen_snapshot) continue; /* recycled under us */
            outs("sshd: PREAUTH_TIMEOUT slot="); outn(i); outc('\n');
            sys_shutdown(fd, LOGIT_SHUT_RD);
        }
    }
}

static int send_msg(struct conn_ctx *cc, const uint8_t *payload, int len)
{
    struct sock_io_ctx c = { cc->sockfd };
    for (;;) {
        spin_lock(&cc->lock);
        if (!cc->kex_busy || (len > 0 && payload[0] < 50)) break;
        spin_unlock(&cc->lock);
        if (cc->stopping) return -1;
        sys_sleep_ms(1);
    }
    int rc = ssh_pkt_send(&cc->s2c, sock_write_all, &c, payload, len, rnd);
    if(rc==0)__sync_fetch_and_add(&cc->key_bytes,(unsigned long long)len+64);
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
static int do_kex(struct conn_ctx *cc, const uint8_t *incoming, int incoming_len);
static int channel_dispatch(struct conn_ctx *cc,const uint8_t *buf,int n);
static int recv_transport(struct conn_ctx *cc, uint8_t *buf, int max)
{
    struct sock_io_ctx c = { cc->sockfd };
    for (;;) {
        int n = ssh_pkt_recv(&cc->c2s, sock_read_exact, &c, buf, max);
        if(n>0)__sync_fetch_and_add(&cc->key_bytes,(unsigned long long)n+64);
        if (n < 0) { errs("sshd: receive error "); outn_fd(2, n); errs("\n"); return n; }
        if (n >= 1 && (buf[0] == SSH_MSG_IGNORE || buf[0] == SSH_MSG_DEBUG ||
                       buf[0] == SSH_MSG_UNIMPLEMENTED)) continue;
        return n;
    }
}

/* 2026-09-10: supersedes the refusal described above. Read a full transport
 * packet before inspecting its type: a rekey proposal can arrive even while
 * a caller is waiting for a tiny service/channel reply. */
static int recv_msg(struct conn_ctx *cc, uint8_t *buf, int max)
{
    uint8_t packet[SSH_MAX_PAYLOAD];
    for (;;) {
        int n = recv_transport(cc, packet, sizeof packet);
        if (n > 0 && packet[0] == SSH_MSG_KEXINIT && cc->kex_done) {
            if (do_kex(cc, packet, n) < 0) return -1;
            if(cc->authenticated)return 0;
            continue;
        }
        if (n > max) return -1;
        if (n > 0) memcpy(buf, packet, n);
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
        int got = sys_read(cc->sockfd, &ch, 1);
        if (got == SIG_E_INTR) continue;
        if (got != 1) return -1;
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
static int do_kex(struct conn_ctx *cc, const uint8_t *incoming, int incoming_len)
{
    uint8_t buf[512];
    spin_lock(&cc->lock); cc->kex_busy = 1; spin_unlock(&cc->lock);
    cc->islen = ssh_kexinit_build(cc->I_S, (int)sizeof cc->I_S, rnd);
    if (cc->islen < 0) return -1;
    if (send_msg(cc, cc->I_S, cc->islen) < 0) return -1;

    if (incoming) {
        if (incoming_len > (int)sizeof cc->I_C) return -1;
        memcpy(cc->I_C, incoming, incoming_len); cc->iclen = incoming_len;
    } else {
        uint8_t pending[SSH_MAX_PAYLOAD];
        for(;;){
            int n=recv_transport(cc,pending,sizeof pending);
            if(n<1)return -1;
            if(pending[0]==SSH_MSG_KEXINIT){
                if(n>(int)sizeof cc->I_C)return -1;
                memcpy(cc->I_C,pending,n);cc->iclen=n;break;
            }
            /* A server-initiated proposal crosses packets already in flight.
             * Dispatch them into per-channel queues; postpone their replies
             * until NEWKEYS. Dropping them corrupts an otherwise valid upload. */
            if(!cc->authenticated||pending[0]<50||channel_dispatch(cc,pending,n)<0)return -1;
        }
    }
    if (cc->iclen < 1 || cc->I_C[0] != SSH_MSG_KEXINIT) return -1;

    struct ssh_negotiated neg;
    char why[64];
    if (ssh_kexinit_negotiate(cc->I_C, cc->iclen, &neg, why, (int)sizeof why) < 0) {
        disconnect(cc, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, why);
        return -1;
    }

    int n = recv_transport(cc, buf, (int)sizeof buf);
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
    /* The session identifier is the FIRST exchange hash for every later
     * KDF and userauth signature. Replacing it rekeys to different keys. */
    if (!cc->kex_done) for (int i = 0; i < 32; i++) cc->session_id[i] = h[i];

    uint8_t nk[1] = { SSH_MSG_NEWKEYS };
    if (send_msg(cc, nk, 1) < 0) return -1;

    uint8_t iv_c2s[16], iv_s2c[16], enc_c2s[16], enc_s2c[16], mac_c2s[32], mac_s2c[32];
    ssh_kex_derive_keys(k_raw, h, cc->session_id, iv_c2s, iv_s2c, enc_c2s, enc_s2c, mac_c2s, mac_s2c);
    crypto_wipe(k_raw, 32);

    /* Our OWN NEWKEYS was just sent -- our outgoing side switches now. */
    ssh_dir_activate(&cc->s2c, enc_s2c, iv_s2c, mac_s2c);

    n = recv_transport(cc, buf, (int)sizeof buf);
    if (n < 1 || buf[0] != SSH_MSG_NEWKEYS) return -1;
    /* The client's NEWKEYS was just RECEIVED -- our incoming side switches now. */
    ssh_dir_activate(&cc->c2s, enc_c2s, iv_c2s, mac_c2s);

    crypto_wipe(enc_c2s, 16); crypto_wipe(enc_s2c, 16);
    crypto_wipe(mac_c2s, 32); crypto_wipe(mac_s2c, 32);
    /* RFC 8308: the CLIENT's ext-info-c permits our initial EXT_INFO.
     * RSA SHA-2 support must be announced here so OpenSSH can select a
     * signature algorithm for the ssh-rsa public-key encoding. Never emit
     * this initial-only extension on a later rekey. */
    const uint8_t *kexnames; int kexlen;
    if (!cc->kex_done && ssh_r_string(cc->I_C,17,cc->iclen,&kexnames,&kexlen)>=0 &&
        ssh_namelist_has(kexnames,kexlen,"ext-info-c")) {
        uint8_t ext[256]; int elen=ssh_pubkey_ext_info(ext,sizeof ext);
        if(elen<0 || send_msg(cc,ext,elen)<0)return -1;
    }
    cc->kex_done = 1; /* from here on, a THIRD-party SSH_MSG_KEXINIT means
                       * rekey, and recv_msg() refuses it -- see its comment */
    spin_lock(&cc->lock);
    struct sock_io_ctx io={cc->sockfd};
    for(int i=0;i<cc->nreplies;i++){
        if(ssh_pkt_send(&cc->s2c,sock_write_all,&io,cc->replies[i].data,cc->replies[i].len,rnd)<0){spin_unlock(&cc->lock);return -1;}
    }
    cc->nreplies=0;cc->key_bytes=0;cc->key_since_ns=monotonic_ns();
    cc->kex_busy = 0; spin_unlock(&cc->lock);
    return 0;
}

/* ========================================================================
 * userauth (RFC 4252)
 * ==================================================================== */
static int do_service_request(struct conn_ctx *cc)
{
    uint8_t buf[128];
    int n = recv_msg(cc, buf, (int)sizeof buf);
    if (n < 1 || buf[0] != SSH_MSG_SERVICE_REQUEST) { errs("sshd: service packet n/type "); outn_fd(2,n); errs("/"); outn_fd(2,n>0?buf[0]:-1); errs("\n"); return -1; }
    const uint8_t *svc; int svclen;
    if (ssh_r_string(buf, 1, n, &svc, &svclen) < 0) { errs("sshd: service string parse failed\n"); return -1; }
    if (svclen != 12 || c_strncmp((const char *)svc, "ssh-userauth", 12) != 0) { errs("sshd: unsupported service\n"); return -1; }
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

            int algok = ssh_pubkey_supported(alg, blob, bloblen);
            int userok = algok && find_account(req.user, &cc->acct);
            int keyok = userok && check_authorized_key(&cc->acct, blob, bloblen);

            if (!keyok) { fail_or_disconnect(cc, "publickey", &give_up); if (give_up) return -1; continue; }

            if (!has_sig) {
                uint8_t rep[SSH_AUTH_KEY_MAX + 64];
                int rl = ssh_build_userauth_pk_ok(alg, blob, bloblen, rep, (int)sizeof rep);
                if (send_msg(cc, rep, rl) < 0) return -1;
                continue;
            }

            uint8_t signdata[SSH_AUTH_SIGDATA_MAX];
            int sdlen = ssh_auth_pubkey_signdata(cc->session_id, req.user, req.service,
                                                 alg, blob, bloblen, signdata, (int)sizeof signdata);
            if (sdlen < 0) return -1;


            if (ssh_pubkey_verify(alg,blob,bloblen,sig,siglen,signdata,sdlen)) {
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

#include "sshd_channels.h"

static void handle_connection(void *arg)
{
    struct conn_ctx *cc=arg;
    int stage=0,rc=do_version_exchange(cc);
    if(rc==0){stage=1;rc=do_kex(cc,0,0);}
    if(rc==0){stage=2;rc=do_service_request(cc);}
    if(rc==0){stage=3;rc=do_userauth(cc);}
    if(rc==0){stage=4;rc=run_channels(cc);}
    if(rc<0){errs("sshd: connection ended at stage ");outn_fd(2,stage);errs("\n");}
    cc->gen++;sys_close(cc->sockfd);g_active_conns--;g_slot_busy[cc->slot]=0;
}

/* ========================================================================
 * accept loop
 * ==================================================================== */
int main(int argc, char **argv)
{
    int port = argc > 1 ? c_atoi(argv[1]) : 22;
    if (port <= 0 || port > 65535) port = 22;
    if(argc>2){int b=c_atoi(argv[2]);if(b<0)return 1;g_rekey_bytes=(unsigned)b;}
    if(argc>3){int secs=c_atoi(argv[3]);if(secs<0)return 1;g_rekey_ns=(unsigned long long)secs*1000000000;}
    struct logit_sigaction ignore={0};ignore.handler=1;
    if(_sys(SYS_SIGACTION,LOGIT_SIGPIPE,(long)&ignore,0)<0)return 1;

    if (sys_setgid(0) < 0 || sys_setuid(0) < 0) {
        errs("sshd: start as root; commands run as the authenticated account\n"); return 1;
    }
    if (load_or_create_hostkey(g_hostpub, g_hostseed) != 0) return 1;
    ssh_pubkey_init();
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





    /* The pre-auth reaper -- started once, before the first accept, so no
     * connection can slip in under a watchdog that does not exist yet (the
     * same "gate exists before the thing it guards" rule as SSHD_READY
     * itself: a harness watching for PREAUTH_TIMEOUT lines must be able to
     * rely on the reaper being live for EVERY connection, not just the ones
     * that arrived after some later point). */
    g_watchdog_targ.fn = preauth_watchdog;
    g_watchdog_targ.ctx = 0;
    {
        struct logit_thread_spec spec;
        spec.entry = (unsigned long)(long)&sshd_thread_entry;
        spec.stack_top = (unsigned long)(long)(g_watchdog_stack + WATCHDOG_STACK_SIZE);
        spec.stack_base = 0;
        spec.stack_len = 0;
        spec.tls = 0;
        spec.arg = (unsigned long)(long)&g_watchdog_targ;
        if (sys_thread_create(&spec) <= 0) {
            errs("SSHD_FAIL watchdog\n"); sys_close(lfd); return 1;
        }
    }

    outs("SSHD_READY port="); outn(me.port); outs(" pid=");outn(sys_getpid());outc('\n');
    _sys(SYS_FSYNC, 1, 0, 0); /* daemon logs use buffered file descriptors */

    for (;;) {
        struct logit_sockaddr peer;
        peer.family = 0; peer.port = 0; peer.addr = 0;
        int cfd = sys_accept(lfd, &peer, 0);
        if (cfd < 0) {
            if(cfd==LSK_E_AGAIN||cfd==LSK_E_INTR)continue;
            /* FULL is -4, distinct from socket-control EINTR (-9). A busy
             * process must leave the listener alive while channels drain. */
            if(cfd==LSK_E_FULL){sys_sleep_ms(10);continue;}
            errs("SSHD_FAIL accept ");outn_fd(2,cfd);errs("\n");break;
        }

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

        if (g_conn_tid[slot] > 0)
            _sys(SYS_THREAD_JOIN, g_conn_tid[slot], 0, 0);
        struct conn_ctx *cc = &g_conn[slot];
        /* Zero everything but the .bss-resident large buffers, which do not
         * need zeroing between connections (every field that matters is set
         * before use, and I_C/I_S/etc are always written before being read). */
        cc->slot = slot;
        cc->sockfd = cfd;
        cc->authenticated = 0;
        cc->auth_tries = 0;
        cc->stopping = 0;
        cc->lock = 0;
        cc->c2s.cipher_on = 0; cc->c2s.mac_on = 0; cc->c2s.seq = 0;
        cc->s2c.cipher_on = 0; cc->s2c.mac_on = 0; cc->s2c.seq = 0;
        cc->kex_done = 0; /* a REUSED slot's prior connection may have left
                           * this 1 -- a fresh connection's first KEXINIT
                           * must not be mistaken for a rekey ask */
        cc->kex_busy = 0;cc->nreplies=0;cc->key_bytes=0;cc->key_since_ns=0;
        cc->next_channel=0;
        for(int j=0;j<SSHD_CHANNELS;j++)cc->channels[j].used=0;
        for(int j=0;j<SSHD_REMOTE_LISTENERS;j++)cc->remote[j].fd=-1;
        cc->preauth_reaped = 0;
        cc->gen++;                 /* also bumped at cleanup: see the watchdog */
        cc->connected_ns = monotonic_ns();

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
        g_conn_tid[slot] = sys_thread_create(&spec);
        if (g_conn_tid[slot] <= 0) {
            errs("sshd: could not start a connection thread\n");
            sys_close(cfd);
            g_slot_busy[slot] = 0;
            g_active_conns--;
        }
    }

    sys_close(lfd);
    return 0;
}
