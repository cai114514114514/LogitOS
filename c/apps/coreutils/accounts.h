#ifndef LOGIT_ACCOUNTS_H
#define LOGIT_ACCOUNTS_H

/* The account store: /etc/passwd, and the parser for it.
 *
 * =========================================================================
 * WHERE IT LIVES AND WHO MAY WRITE IT -- decide this first, because it is the
 * only question here whose wrong answer cannot be fixed later.
 * =========================================================================
 *
 * /etc/passwd on LogitFS. Owned root:root, mode 0600, created by /bin/login
 * on the first enrolment and never shipped in the image.
 *
 * THIS WAS NOT DEFENSIBLE UNTIL RECENTLY, and the reasoning that used to
 * forbid it is worth repeating because it was right at the time. The package
 * trust root (c/crypto/trust/pkgroots.inc) is COMPILED INTO THE ISO rather
 * than kept in a file, and the stated reason was: "LogitFS has no
 * reboot-surviving ownership, so /etc/trust/keys would be writable by exactly
 * the code the signature constrains." A mode that lives in RAM is a mode that
 * is 0644 root:root again on the next boot, so a file protected only by its
 * mode was protected only until the machine restarted -- which is no
 * protection at all against an attacker who can write a file and wait.
 *
 * That changed when logitfs grew getattr/setattr and the mode and the owner
 * went onto the medium (c/fs/logitfs.c, verified across four real reboots by
 * tests/boot/run-statmeta-test.sh). A 0600 root:root file is now 0600
 * root:root after a power cut, so "only root may write the account store" is
 * a statement the machine can keep rather than a comment.
 *
 * THE TRUST ROOT IS STILL THE FIRST BOOT, and that is stated rather than
 * hidden. Before any account exists every process is root (see the session in
 * c/fs/vfs_cred.h), so whoever reaches the console first can enrol. There is
 * no signature over the store and no secure boot underneath it; the claim is
 * "after enrolment, an unprivileged process cannot read or alter the store",
 * not "the store cannot be planted". Closing the second one needs a chain
 * that starts before this filesystem, which this machine does not have.
 *
 * NOTHING IS SHIPPED IN THE IMAGE. No default account, no default password,
 * no /etc/passwd at all. A machine with no store has no login, exactly as
 * before, and /bin/login says so and hands over a root shell -- which is
 * honest, because a machine with no accounts genuinely has no one to
 * authenticate. Enrolment is `login -a NAME`, which PROMPTS. A known password
 * baked into an image is worse than no password at all, because it looks like
 * security from the outside.
 *
 * WHY THERE IS NO /etc/shadow. Shadow exists because /etc/passwd has to be
 * world-readable: every `ls -l` maps a uid to a name. Nothing on this machine
 * does that -- ls prints numbers -- so there is no reader that needs the row
 * without needing the hash, and splitting the file would buy nothing while
 * adding a second file to keep in sync and a second mode to get wrong. When
 * something needs uid -> name unprivileged, the split becomes necessary and
 * this comment is the note that says so.
 *
 * =========================================================================
 * THE FORMAT
 * =========================================================================
 *
 *   # comment lines and blank lines are ignored
 *   name:$pbkdf2-sha256$<iters>$<salt-b64>$<dk-b64>:uid:gid:home:shell
 *
 * Six colon-separated fields, i.e. POSIX passwd without gecos and with the
 * hash where the `x` would be. The hash record is produced and checked by
 * c/crypto/kdf/pbkdf2.c and CARRIES ITS OWN ITERATION COUNT, so the cost can
 * be raised later without invalidating a single existing row: a record made
 * at 120000 still verifies after the constant below moves.
 *
 * This header is PARSING ONLY, so that the host test and the on-device
 * program share one implementation of it rather than two that agree today. */

#define ACCT_NAME  32
#define ACCT_HASH  160
#define ACCT_PATH  64
#define ACCT_MAX   4096          /* the whole store; ~8 rows, which is plenty */

/* THE PBKDF2 COST, and the number the crypto line asked to have revisited.
 *
 * c/crypto/kdf/pbkdf2.c defaults to 600000 -- OWASP's 2023 figure for
 * HMAC-SHA-256 -- and measured 471 ms for it on the DEVELOPMENT HOST. This
 * machine is not the development host: it runs under QEMU TCG, which is
 * roughly an order of magnitude slower, so 600000 is five to ten seconds of a
 * frozen console with no output on it. A login that takes eight seconds is a
 * login whose cost the next person lowers to zero, and a frozen console is
 * indistinguishable from a hang.
 *
 * 120000 is chosen instead. The reasoning, in full, because a lowered work
 * factor deserves an argument and not a shrug:
 *
 *   - The threat this defends against is a stolen disk image, offline. There
 *     is ONE account on this machine and no password database to bulk-crack.
 *     Stretching buys the difference between a weak password falling in
 *     minutes and falling in hours; it never saves a bad password, and it is
 *     not what saves a good one.
 *   - The factor lost against 600000 is 5x, which is under three bits of
 *     password entropy. One extra character of a random password is worth
 *     more than the whole reduction.
 *   - What is gained is a login that answers in about a second under TCG,
 *     which is the difference between a mechanism people use and a mechanism
 *     people turn off.
 *
 * AND THE WAIT IS MADE VISIBLE ANYWAY: /bin/login prints "checking..." before
 * it starts and the elapsed milliseconds after, so a slow verify reads as
 * work being done rather than as a machine that has stopped. Both halves of
 * the instruction, because either alone is half an answer.
 *
 * Changing this number does NOT invalidate existing rows -- the count lives
 * in each record. Raising it affects only accounts enrolled afterwards. */
#define ACCT_ITERS 120000u

/* The first uid handed out. 1000 for the usual reason: it is out of the way
 * of anything that might later want a fixed low id, and it is the number a
 * person reading `stat` output already knows how to interpret. */
#define ACCT_FIRST_UID 1000u

struct account {
    char     name[ACCT_NAME];
    char     hash[ACCT_HASH];
    unsigned uid, gid;
    char     home[ACCT_PATH];
    char     shell[ACCT_PATH];
};

/* --- tiny string helpers, so this header needs no libc ------------------- */

static inline int acct_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static inline int acct_eq(const char *a, const char *b)
{ int i = 0; if (!a || !b) return 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static inline void acct_cpyn(char *d, const char *s, int n, int max)
{
    int i = 0;
    for (; i < n && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* Parse one line (no newline) into `a`. Returns 1 on success, 0 for a comment,
 * a blank line, or anything malformed. A malformed row is SKIPPED rather than
 * failing the whole store: a store one byte of which got corrupted should cost
 * one account, not every account. */
static inline int acct_parse_line(const char *line, int len, struct account *a)
{
    if (!line || !a || len <= 0) return 0;
    while (len && (line[0] == ' ' || line[0] == '\t')) { line++; len--; }
    if (!len || line[0] == '#') return 0;

    /* EXACTLY six fields. Stopping at six and ignoring the rest would accept
     * "alice:x:1000:1000:/h:/sh:anything" as a valid row, which means a store
     * an attacker can append one byte to parses as something its author did
     * not write -- and the fields that shift are the uid and the shell. A
     * seventh field is a malformed row and malformed rows are skipped. */
    const char *f[6]; int fl[6], nf = 0;
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || line[i] == ':') {
            if (nf >= 6) return 0;
            f[nf] = line + start; fl[nf] = i - start; nf++;
            start = i + 1;
        }
    }
    if (nf != 6) return 0;
    if (fl[0] <= 0 || fl[0] >= ACCT_NAME) return 0;
    if (fl[1] <= 0 || fl[1] >= ACCT_HASH) return 0;
    if (fl[2] <= 0 || fl[3] <= 0) return 0;

    acct_cpyn(a->name,  f[0], fl[0], ACCT_NAME);
    acct_cpyn(a->hash,  f[1], fl[1], ACCT_HASH);
    a->uid = 0; for (int i = 0; i < fl[2]; i++) {
        if (f[2][i] < '0' || f[2][i] > '9') return 0;
        a->uid = a->uid * 10u + (unsigned)(f[2][i] - '0');
    }
    a->gid = 0; for (int i = 0; i < fl[3]; i++) {
        if (f[3][i] < '0' || f[3][i] > '9') return 0;
        a->gid = a->gid * 10u + (unsigned)(f[3][i] - '0');
    }
    acct_cpyn(a->home,  f[4], fl[4], ACCT_PATH);
    acct_cpyn(a->shell, f[5], fl[5], ACCT_PATH);
    if (!a->home[0])  acct_cpyn(a->home,  "/",       1, ACCT_PATH);
    if (!a->shell[0]) acct_cpyn(a->shell, "/bin/sh", 7, ACCT_PATH);
    return 1;
}

/* Iterate the store. `*pos` starts at 0; returns 1 while an account was read. */
static inline int acct_next(const char *text, int len, int *pos, struct account *a)
{
    while (*pos < len) {
        int s = *pos, e = s;
        while (e < len && text[e] != '\n') e++;
        *pos = (e < len) ? e + 1 : len;
        if (acct_parse_line(text + s, e - s, a)) return 1;
    }
    return 0;
}

static inline int acct_find(const char *text, int len, const char *name, struct account *a)
{
    int pos = 0;
    while (acct_next(text, len, &pos, a)) if (acct_eq(a->name, name)) return 1;
    return 0;
}

/* The highest uid in the store, or 0 if there are none. What the next
 * enrolment adds one to -- reusing a freed uid would hand a new person the
 * files of the old one, which is the same class of bug the pid sweep in
 * c/fs/vfs_cred.c exists to prevent. */
static inline unsigned acct_max_uid(const char *text, int len)
{
    struct account a; int pos = 0; unsigned m = 0;
    while (acct_next(text, len, &pos, &a)) if (a.uid > m) m = a.uid;
    return m;
}

/* Append `a` as a line. Returns the new length, or -1 if it would not fit. */
static inline int acct_append(char *text, int len, int max, const struct account *a)
{
    char num[12];
    const char *parts[6];
    parts[0] = a->name; parts[1] = a->hash;
    parts[4] = a->home; parts[5] = a->shell;

    int n = len;
    if (n && text[n-1] != '\n') { if (n >= max) return -1; text[n++] = '\n'; }

    for (int fi = 0; fi < 6; fi++) {
        const char *s;
        if (fi == 2 || fi == 3) {
            unsigned v = (fi == 2) ? a->uid : a->gid;
            int k = 0; if (!v) num[k++] = '0';
            while (v) { num[k++] = (char)('0' + v % 10u); v /= 10u; }
            for (int j = 0; j < k / 2; j++) { char t = num[j]; num[j] = num[k-1-j]; num[k-1-j] = t; }
            num[k] = 0; s = num;
        } else s = parts[fi];
        for (int i = 0; s[i]; i++) { if (n >= max) return -1; text[n++] = s[i]; }
        if (fi < 5) { if (n >= max) return -1; text[n++] = ':'; }
    }
    if (n >= max) return -1;
    text[n++] = '\n';
    return n;
}

/* --- the check ------------------------------------------------------------
 *
 * Split out of /bin/login so that the negative control is ONE definition that
 * the host suite and the device build share. See the target `test-login-negctl`
 * in the Makefile: under LOGIN_NEGCTL_ACCEPT_ANY the login screen appears, the
 * prompt reads exactly the same, the flow works, a human sees precisely what
 * they expect -- and every password is correct. A suite that cannot go red on
 * that build is not testing authentication, it is testing that a prompt was
 * printed.
 *
 * Callers must link c/crypto/kdf/pbkdf2.c (+ hmac and sha256). Declared here
 * rather than including crypto.h so that a caller which only parses the store
 * -- mini-libc's getpwnam, for one -- does not have to link a KDF. */
int pwhash_check(const char *record, const char *password);

static inline int acct_check_password(const struct account *a, const char *pw)
{
#ifdef LOGIN_NEGCTL_ACCEPT_ANY
    (void)a; (void)pw;
    return 1;
#else
    return pwhash_check(a->hash, pw);
#endif
}

#endif /* LOGIT_ACCOUNTS_H */
