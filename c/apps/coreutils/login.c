#include "clib.h"
#include "logit_stat.h"
#include "accounts.h"

/* /bin/login -- the thing that goes between "filesystem mounted" and "desktop
 * live", and the reason any of the permission machinery under it means
 * anything.
 *
 * =========================================================================
 * WHAT THIS MACHINE LOOKED LIKE BEFORE
 * =========================================================================
 * 2.56 seconds from power-on to a live desktop with the user's files open. No
 * login, no session, nothing at all between the mount and the window manager.
 * Meanwhile c/fs/vfs.c was checking search permission on every directory of
 * every path walk, against a credential nothing could set -- so every check
 * compared a file's owner against uid 0 and passed. The enforcement was real
 * and the identity was a constant.
 *
 * =========================================================================
 * THE TWO PATHS, AND WHICH ONE AUTHENTICATES
 * =========================================================================
 * THE CONSOLE authenticates. c/kernel/gui/wm.c's init spawn runs /bin/login on
 * the serial tty instead of /bin/sh; login checks a password and execve's the
 * shell as the user, so the shell is never root and never was.
 *
 * THE DESKTOP DOES NOT, and that is a limitation stated out loud rather than
 * papered over. The window manager launches files.aex before init runs at all;
 * making the desktop wait for a display manager is a change to the WM's boot
 * order, which belongs to the line that owns that file. What DOES happen is
 * that the desktop stops being permanently root: SYS_SETSESSION sets the
 * credential every process that was never told who it belongs to inherits (see
 * c/fs/vfs_cred.h), so the moment a console login succeeds, the running
 * desktop and everything it launches afterwards are that user -- and are
 * refused what that user is refused. A greeter in front of the desktop is the
 * remaining half.
 *
 * =========================================================================
 * A MACHINE WITH NO ACCOUNTS
 * =========================================================================
 * ...has no login, and this program says so and hands over a root shell. That
 * is not a bypass, it is the truth: there is nobody to authenticate. It is
 * also what keeps every existing boot harness in the tree working unchanged,
 * because the shipped image contains no /etc/passwd and never will -- a
 * default account with a known password is the one outcome that is worse than
 * no accounts at all, since it looks like security from the outside.
 *
 * Enrolment is `login -a NAME`, root only, and it PROMPTS. Nothing here
 * generates, defaults to, or embeds a password.
 *
 * =========================================================================
 * PASSWORD ECHO
 * =========================================================================
 * The kernel's tty echoes each byte as it is read (c/kernel/exec/file.c
 * tty_read), and there is no termios here to turn that off. Rather than add a
 * kernel knob for one program, this OVERWRITES: after the tty has echoed a
 * character, login writes backspace + '*'. On a serial terminal that is
 * indistinguishable from a masked prompt, and it needs no change to a file
 * this line does not own. Backspace needs nothing -- the tty already erased
 * the character on screen, so only the buffer shortens.
 *
 * It is worth being precise about what this is NOT: the character does reach
 * the serial line for the instant before it is overwritten, so a recording of
 * the wire has the password in it. So does a recording of the wire during
 * enrolment on any machine whose console is a serial port. This is a console,
 * not a network login. */

#define PWMAX 128

/* The coreutils link no libc -- crt0_cli plus one .c plus the inline syscalls
 * in logit.h -- but c/crypto/hash/hmac_hkdf.c is ordinary C and calls both of
 * these, and clang emits calls to them for struct assignment regardless. Two
 * definitions here beats pulling the whole mini-libc arena into a program
 * whose largest allocation is a 4 KiB account store. */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *a = (unsigned char *)d;
    for (unsigned long i = 0; i < n; i++) a[i] = (unsigned char)c;
    return d;
}

/* c/crypto/kdf/pbkdf2.c. Declared here rather than by including crypto.h,
 * which drags in every hash and AEAD prototype this program does not link. */
int pwhash_make(char *out, int max, const char *password,
                unsigned iters, void (*randbytes)(unsigned char *, int));

/* pwhash_make wants a void callback; getrandom_bytes reports failure. A short
 * read here would silently produce a salt with a zero tail, so the failure is
 * turned into an all-ones salt that pwhash_make will still hash and that a
 * human reading the record can recognise as wrong -- rather than into zeroes,
 * which look exactly like a legitimate salt that happened to be small. */
static void rnd(unsigned char *p, int n)
{ if (getrandom_bytes(p, n) < 0) for (int i = 0; i < n; i++) p[i] = 0xFF; }

/* ---------------------------------------------------------------- output -- */

static void put_u(unsigned long v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) outc(t[--k]);
}

/* ------------------------------------------------------------- the store -- */

static char  store[ACCT_MAX + 1];
static int   store_len;

/* Returns 1 if the store exists and was read. A store that exists but cannot
 * be read is NOT the same as one that does not exist, and the difference has
 * to reach the caller: the first means "somebody has locked you out", the
 * second means "this machine has no accounts". */
static int store_read(int *existed)
{
    struct logit_stat st;
    *existed = (st_stat("/etc/passwd", &st) == 0);
    if (!*existed) return 0;
    int n = read_file("/etc/passwd", store, ACCT_MAX);
    if (n < 0) return 0;
    store_len = n;
    store[n] = 0;
    return 1;
}

/* ------------------------------------------------------------ the prompt -- */

/* Read a line with the echo overwritten by '*'. See the header comment. */
static int read_secret(const char *prompt, char *out, int max)
{
    outs(prompt);
    int n = 0;
    for (;;) {
        char c;
        int r = sys_read(0, &c, 1);
        if (r <= 0) { outc('\n'); return -1; }
        if (c == '\n') break;
        if (c == '\b' || c == 127) { if (n > 0) n--; continue; }
        if (n < max - 1) out[n++] = c;
        outs("\b*");                       /* the tty already echoed `c` */
    }
    out[n] = 0;
    return n;
}

/* --------------------------------------------------------------- enrol ---- */

static int enrol(const char *name)
{
    if (sys_getuid() != 0) {
        outs("login: only root may add an account\n");
        return 1;
    }
    if (!name || !name[0] || acct_len(name) >= ACCT_NAME) {
        outs("login: usage: login -a NAME\n");
        return 1;
    }
    for (int i = 0; name[i]; i++)
        if (name[i] == ':' || name[i] == '\n' || name[i] == '/' || name[i] == ' ') {
            /* A ':' would forge a field boundary and a '/' would forge a home
             * outside /home. Refuse the name rather than escape it: an escape
             * is a second parser to get wrong. */
            outs("login: a name may not contain ':' '/' or a space\n");
            return 1;
        }

    int existed = 0;
    store_read(&existed);
    struct account dup;
    if (existed && acct_find(store, store_len, name, &dup)) {
        outs("login: that account already exists\n");
        return 1;
    }

    char p1[PWMAX], p2[PWMAX];
    if (read_secret("New password: ", p1, sizeof p1) < 0) return 1;
    if (p1[0] == 0) { outs("login: refusing an empty password\n"); return 1; }
    if (read_secret("Retype password: ", p2, sizeof p2) < 0) return 1;
    if (!acct_eq(p1, p2)) { outs("login: they do not match\n"); return 1; }

    if (!getrandom_strong())
        outs("login: WARNING the entropy source is a timer, not RDSEED --\n"
             "       this salt is weaker than it looks\n");

    struct account a;
    acct_cpyn(a.name, name, acct_len(name), ACCT_NAME);
    a.uid = existed ? acct_max_uid(store, store_len) + 1u : ACCT_FIRST_UID;
    if (a.uid < ACCT_FIRST_UID) a.uid = ACCT_FIRST_UID;
    a.gid = a.uid;
    acct_cpyn(a.shell, "/bin/sh", 7, ACCT_PATH);
    {
        char h[ACCT_PATH]; int k = 0;
        const char *pre = "/home/";
        for (int i = 0; pre[i]; i++) h[k++] = pre[i];
        for (int i = 0; name[i] && k < ACCT_PATH - 1; i++) h[k++] = name[i];
        h[k] = 0;
        acct_cpyn(a.home, h, k, ACCT_PATH);
    }

    outs("hashing (");
    put_u(ACCT_ITERS);
    outs(" PBKDF2-HMAC-SHA256 iterations)... ");
    unsigned long long t0 = monotonic_ms();
    int hn = pwhash_make(a.hash, ACCT_HASH, p1, ACCT_ITERS, rnd);
    unsigned long long t1 = monotonic_ms();
    for (int i = 0; i < PWMAX; i++) { p1[i] = 0; p2[i] = 0; }
    if (hn <= 0) { outs("\nlogin: could not hash the password\n"); return 1; }
    put_u((unsigned long)(t1 - t0)); outs(" ms\n");

    int nl = acct_append(store, store_len, ACCT_MAX, &a);
    if (nl < 0) { outs("login: the account store is full\n"); return 1; }

    /* /etc first. A failure here is not fatal on its own -- the directory may
     * already exist -- so the write below is what is actually checked. */
    make_dir("/etc");
    if (write_file("/etc/passwd", store, nl) < 0) {
        outs("login: could not write /etc/passwd\n");
        return 1;
    }
    /* THE MODE IS THE POINT. Root-owned and 0600, on the medium, so it is
     * still 0600 after a power cut -- which is the property that was missing
     * before logitfs grew setattr and the reason a file could not be the
     * account store until now. If either of these is refused, say so loudly:
     * a store that is world-readable is a store with no protection at all. */
    if (st_chown("/etc/passwd", 0, 0) < 0 || st_chmod("/etc/passwd", 0600) < 0) {
        outs("login: WARNING /etc/passwd is NOT root:root 0600 -- the hash is readable\n");
        return 1;
    }
    st_chmod("/etc", 0755);

    /* A home the user actually owns, or the account is an identity with
     * nowhere to put a file. 0700 and owned by them: the refusal that proves
     * this works is a SECOND user being unable to read it. */
    make_dir("/home");
    st_chmod("/home", 0755);
    make_dir(a.home);
    if (st_chown(a.home, (int)a.uid, (int)a.gid) < 0 ||
        st_chmod(a.home, 0700) < 0)
        outs("login: WARNING the home directory is not owned by the new account\n");

    outs("ENROLLED "); outs(a.name);
    outs(" uid="); put_u(a.uid);
    outs(" gid="); put_u(a.gid);
    outs(" home="); outs(a.home);
    outc('\n');
    return 0;
}

/* --------------------------------------------------------------- login ---- */

static char env_home[ACCT_PATH + 8];
static char env_user[ACCT_NAME + 8];

static void cat2(char *d, const char *a, const char *b, int max)
{
    int k = 0;
    for (int i = 0; a[i] && k < max - 1; i++) d[k++] = a[i];
    for (int i = 0; b[i] && k < max - 1; i++) d[k++] = b[i];
    d[k] = 0;
}

/* Become `a` and run its shell. Does not return except on failure. */
static void become(const struct account *a)
{
    /* ONE call, not setuid-then-setsession or the reverse: see the comment on
     * vfs_cred_session_set() in c/fs/vfs_cred.c for why neither order is
     * implementable. This is also the moment root is given up -- everything
     * after it runs as the user, including the execve. */
    if (sys_setsession(a->uid, a->gid) < 0) {
        outs("login: could not establish the session\n");
        return;
    }

    outs("LOGIN-OK "); outs(a->name);
    outs(" uid="); put_u((unsigned long)sys_getuid());
    outs(" gid="); put_u((unsigned long)sys_getgid());
    outc('\n');

    if (sys_chdir(a->home) < 0) sys_chdir("/");

    cat2(env_home, "HOME=", a->home, sizeof env_home);
    cat2(env_user, "USER=", a->name, sizeof env_user);
    char *argv[] = { (char *)"sh", 0 };
    char *envp[] = { env_home, env_user, 0 };
    sys_execve(a->shell, argv, envp);
    outs("login: could not exec "); outs(a->shell); outc('\n');
}

/* The machine has no accounts. Say so and hand over a root shell -- see the
 * header comment for why that is honest and not a bypass. */
static void no_accounts(void)
{
    outs("login: no accounts on this machine (`login -a NAME` to create one)\n");
    char *argv[] = { (char *)"sh", 0 };
    sys_execve("/bin/sh", argv, 0);
    outs("login: could not exec /bin/sh\n");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && c_streq(argv[1], "-a"))
        return enrol(argc >= 3 ? argv[2] : "");

    if (argc >= 2 && c_streq(argv[1], "-i")) {          /* who am I */
        /* `store=` is not decoration. It is how a test asserts that the SHIPPED
         * IMAGE contains no credential -- the claim that a default account with
         * a known password was not baked in. The boot-time "no accounts" line
         * cannot carry that assertion: it is printed while the kernel is still
         * writing to the same serial port, and the two interleave character by
         * character. This line is typed at a quiet prompt. */
        struct logit_stat st;
        outs("ID uid="); put_u((unsigned long)sys_getuid());
        outs(" gid="); put_u((unsigned long)sys_getgid());
        outs(" session="); put_u((unsigned long)sys_getsession(0));
        outs(st_stat("/etc/passwd", &st) == 0 ? " store=present" : " store=absent");
        outc('\n');
        return 0;
    }

    int existed = 0;
    if (!store_read(&existed)) {
        if (existed) {
            /* The store is there and we cannot read it. Never fall through to
             * a root shell on this branch: "the account file is unreadable" is
             * exactly the state an attacker would try to produce. */
            outs("login: /etc/passwd exists but cannot be read -- refusing\n");
            return 1;
        }
        no_accounts();
        return 1;
    }

    /* Three tries, then give up and let init decide what to do with a dead
     * console. There is nothing to rate-limit against here (one seat, no
     * network login), so this is about a typo, not about brute force -- the
     * PBKDF2 cost is what makes guessing expensive. */
    for (int tries = 0; tries < 3; tries++) {
        outs("\nLOGIN: ");
        char name[ACCT_NAME];
        int n = readline(0, name, sizeof name);
        if (n < 0) return 1;
        if (n == 0) { tries--; continue; }

        char pw[PWMAX];
        if (read_secret("Password: ", pw, sizeof pw) < 0) return 1;

        outs("checking... ");
        unsigned long long t0 = monotonic_ms();
        struct account a;
        int found = acct_find(store, store_len, name, &a);
        /* Verify even when the name is unknown, against a record that cannot
         * match, so that "no such user" and "wrong password" take the same
         * time. Otherwise the prompt tells an attacker which names exist by
         * answering faster -- and here the difference would be a full second,
         * not microseconds. */
        int ok;
        if (found) ok = acct_check_password(&a, pw);
        else {
            /* The decoy is the FIRST REAL ROW of the store, not a fabricated
             * record: a hand-written one could have a different iteration
             * count -- or fail to parse and cost nothing at all -- and either
             * puts the timing difference straight back. This one is guaranteed
             * to cost exactly what a real check costs, because it is one. */
            struct account decoy; int pos = 0;
            if (acct_next(store, store_len, &pos, &decoy))
                acct_check_password(&decoy, pw);
            ok = 0;
        }
        unsigned long long t1 = monotonic_ms();
        for (int i = 0; i < PWMAX; i++) pw[i] = 0;
        put_u((unsigned long)(t1 - t0)); outs(" ms\n");

        if (ok) { become(&a); return 1; }
        outs("LOGIN-DENIED\n");
    }
    outs("login: too many attempts\n");
    return 1;
}
