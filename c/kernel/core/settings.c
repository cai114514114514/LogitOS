/* The settings store.  Read c/kernel/core/settings.h first -- the format, the
 * atomicity argument and the reason this is kernel-side are all argued there
 * rather than repeated here. */

#include <stdint.h>
#include <stddef.h>
#include "settings.h"
#include "logit_abi.h"      /* SYS_SETTING_* / SETCTL_* / struct logit_setting */
#include "vfs.h"
#include "kprintf.h"
#include "crc32.h"
/* /etc/passwd, parsed by the header /bin/login parses it with -- see
 * settings_prepare_user().  The kernel gets a second READER of the account
 * store and not a second definition of its format, which is the distinction
 * that matters: a format with two parsers is a format that will disagree with
 * itself.  The header is parsing only and links nothing (the pwhash_check it
 * declares is never called from here). */
#include "accounts.h"

/* ===========================================================================
 * The schema: every key this kernel understands.
 *
 * Only settings that ARE ACTUALLY WIRED TO SOMETHING appear here.  A row in a
 * Settings window that changes a number nothing reads is worse than an absent
 * row, because it looks like it worked.  What was considered and deliberately
 * left out is listed at the bottom of this file.
 * ======================================================================== */
static const struct setting_def schema[] = {
 /* key                       label                       type         group             lo  hi          default            choices */
  { "ui.dark",               "Dark appearance",           SET_T_BOOL,  SET_G_APPEARANCE,  0, 1,          "0",               NULL },
  { "ui.accent",             "Accent colour",             SET_T_COLOR, SET_G_APPEARANCE,  0, 0xFFFFFF,   "0x5E96FF",        NULL },
  { "ui.wallpaper",          "Wallpaper image",           SET_T_STR,   SET_G_DESKTOP,     0, 0,          "/wallpaper.png",  NULL },
  { "desktop.restore_session","Reopen windows on login",  SET_T_BOOL,  SET_G_DESKTOP,     0, 1,          "1",               NULL },
  { "net.dhcp",              "Configure automatically",   SET_T_BOOL,  SET_G_NETWORK,     0, 1,          "1",               NULL },
  { "net.ip",                "IP address",                SET_T_IP,    SET_G_NETWORK,     0, 0,          "10.0.2.15",       NULL },
  { "net.mask",              "Subnet mask",               SET_T_IP,    SET_G_NETWORK,     0, 0,          "255.255.255.0",   NULL },
  { "net.gw",                "Router",                    SET_T_IP,    SET_G_NETWORK,     0, 0,          "10.0.2.2",        NULL },
  { "net.dns",               "DNS server",                SET_T_IP,    SET_G_NETWORK,     0, 0,          "10.0.2.3",        NULL },
};
#define NSCHEMA ((int)(sizeof schema / sizeof schema[0]))

/* ===========================================================================
 * The store
 * ======================================================================== */
/* `sys` = "this value came from the SYSTEM store and the user has not changed
 * it".  It is the whole layering mechanism and it is one byte: a commit writes
 * the entries with sys == 0 and nothing else, so a user's file holds only what
 * that user actually chose.  See the two-store block in settings.h. */
struct kv { char k[SET_KEYLEN]; char v[SET_VALLEN]; unsigned char sys; };
static struct kv     tab[SET_MAXKV];
static int           ntab;
static unsigned      gen;
static int           diag;
static int           loaded;

/* ---------------------------------------------------------------------------
 * The user store.  Empty until a login resolves one -- and while it is empty
 * this file is byte-for-byte the single-store machine it was, which is what
 * keeps every pre-existing settings test honest.
 * ------------------------------------------------------------------------ */
static char user_path[SET_PATHLEN];      /* "" = no user; commits go to /etc */
static char pending_path[SET_PATHLEN];   /* resolved while still root        */
static unsigned user_uid;

/* 1 while load_text() is reading the SYSTEM file, so every key it stores is
 * marked as a default rather than as something the user chose.
 *
 * IT IS ONLY EVER SET WHEN A USER STORE EXISTS, and that is not an
 * optimisation.  With no user store there is nothing to layer over: if
 * /etc/settings.conf's keys were marked `sys` on a machine with no accounts,
 * the very next commit would write a file containing only the one key that had
 * just been set and silently drop the other eight. */
static int loading_sys;

/* The serialisation buffer.  STATIC, not kmalloc'd, and that is a decision:
 * this is the path that saves the user's configuration, and the moment it most
 * needs to work is the moment the machine is under memory pressure.  A
 * persistence path that can fail because a heap allocation failed is a
 * persistence path that loses data exactly when losing it hurts.
 * SET_MAXKV * (SET_KEYLEN + SET_VALLEN + 4) = 19.9 KiB; 32 KiB covers that plus
 * the header comment and the trailing CRC line with room to spare. */
#define SET_BUFSZ 32768
static char buf[SET_BUFSZ];

/* The selftest's snapshot. Sized for the file the selftest BUILDS (the schema
 * plus two rows, ~800 bytes), not for SET_BUFSZ -- a 32 KiB static array used
 * once per boot to hold 800 bytes is waste with a straight face. */
#define SET_SELFTEST_BUFSZ 4096

/* ---------------------------------------------------------------- helpers --
 * Local, because the kernel has no string.h of its own and these are all
 * three-liners.  Every one is bounded; none of them can run off a buffer that
 * came from the disk. */
static int  s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  s_eq(const char *a, const char *b)
{ int i = 0; for (;; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } }
static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; if (cap <= 0) return; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  is_space(int c) { return c == ' ' || c == '\t' || c == '\r'; }

/* A key has to be something a human can type and a parser can round-trip: no
 * whitespace (the separator), no '=' (the delimiter), no control characters,
 * nothing above ASCII.  A key that fails this came from a damaged file. */
static int key_ok(const char *k)
{
    int n = s_len(k);
    if (n == 0 || n >= SET_KEYLEN) return 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)k[i];
        if (c <= ' ' || c >= 0x7F || c == '=' || c == '#') return 0;
    }
    return 1;
}

/* ===========================================================================
 * Number parsing.  Deliberately strict and deliberately non-throwing: every
 * one of these answers "did this parse" separately from "what did it say", so
 * a caller can tell a zero from a refusal.  That distinction is the whole
 * reason `ui.dark = banana` falls back to the default instead of to 0.
 * ======================================================================== */
static int parse_long(const char *s, long *out)
{
    if (!s || !*s) return 0;
    int i = 0, neg = 0, digits = 0, base = 10;
    long v = 0;
    while (is_space(s[i])) i++;
    if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    for (; s[i]; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        /* Overflow is a parse failure, not a wrap.  A file claiming a window
         * width of 99999999999999999999 must be refused, not silently turned
         * into whatever that is modulo 2^64. */
        if (v > (0x7FFFFFFFFFFFFFFFL - d) / base) return 0;
        v = v * base + d;
        digits++;
    }
    while (is_space(s[i])) i++;
    if (!digits || s[i]) return 0;          /* trailing garbage -> refuse */
    *out = neg ? -v : v;
    return 1;
}

static int parse_ip(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i = 0;
    for (int part = 0; part < 4; part++) {
        int d = 0, digits = 0;
        while (s[i] >= '0' && s[i] <= '9') { d = d * 10 + (s[i++] - '0'); if (++digits > 3) return 0; }
        if (!digits || d > 255) return 0;
        v = (v << 8) | (unsigned)d;
        if (part < 3) { if (s[i] != '.') return 0; i++; }
    }
    if (s[i]) return 0;
    *out = v;
    return 1;
}

/* --- formatting (no snprintf in the kernel; these are the two forms used) --- */
static int fmt_long(char *d, int cap, long v)
{
    char t[24]; int n = 0, i = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0 && i < cap - 1) d[i++] = '-';
    while (n && i < cap - 1) d[i++] = t[--n];
    d[i] = 0;
    return i;
}

static int fmt_hex6(char *d, int cap, unsigned v)
{
    static const char *H = "0123456789ABCDEF";
    int i = 0;
    if (cap < 9) { if (cap > 0) d[0] = 0; return 0; }
    d[i++] = '0'; d[i++] = 'x';
    for (int sh = 20; sh >= 0; sh -= 4) d[i++] = H[(v >> sh) & 0xF];
    d[i] = 0;
    return i;
}

/* ===========================================================================
 * Table primitives
 * ======================================================================== */
static struct kv *find(const char *key)
{
    for (int i = 0; i < ntab; i++) if (s_eq(tab[i].k, key)) return &tab[i];
    return NULL;
}

/* Put without persisting.  0, or a SET_E_*.
 *
 * A too-long value is REFUSED, not truncated.  It used to be truncated, on the
 * reasoning that the schema range check would catch anything that mattered --
 * which is true for the typed keys and false for exactly the case that turned
 * up: a consumer storing a path or a URL gets back something that is still a
 * plausible string and is not what it stored.  Silent truncation of data is a
 * corruption you discover somewhere else, later, and blame on the wrong thing.
 *
 * `from_file` distinguishes the two callers, because they want opposite
 * behaviour.  A SET from a program is a request that must fail loudly if it
 * cannot be honoured.  A line being LOADED from a damaged file must never
 * abort the load -- it is dropped and noted, exactly like any other bad line,
 * because the alternative is one over-long line costing every setting after
 * it. */
static int put_ex(const char *key, const char *val, int from_file)
{
    if (!key_ok(key)) return SET_E_BADKEY;
    if (s_len(val) >= SET_VALLEN) {
        if (from_file) diag |= SET_D_BADLINE;
        return SET_E_TOOBIG;
    }
    struct kv *e = find(key);
    if (!e) {
        if (ntab >= SET_MAXKV) { diag |= SET_D_FULL; return SET_E_FULL; }
        e = &tab[ntab++];
        s_cpy(e->k, key, SET_KEYLEN);
    }
    s_cpy(e->v, val ? val : "", SET_VALLEN);
    /* The ownership bit, set at the ONE place a value is stored.  Loading the
     * system file marks defaults; loading the user file and every set() from a
     * program mark the value as the user's -- including when it overwrites a
     * default, which is exactly the "the user changed it" case. */
    e->sys = (unsigned char)(loading_sys ? 1 : 0);
    return 0;
}

static int put(const char *key, const char *val) { return put_ex(key, val, 0); }

/* ===========================================================================
 * THE PARSER -- the part that has to survive anything.
 *
 * Contract, and it is worth stating precisely because the whole truncation
 * argument rests on it:
 *
 *   Given ANY byte string of ANY length, load_text() terminates, writes only
 *   inside `tab`, and leaves the store in a state where every schema key reads
 *   as either a valid stored value or its compiled-in default.
 *
 * The mechanism is that lines are independent and the LAST line is dropped
 * unless it was terminated.  A file cut at an arbitrary offset is therefore a
 * prefix of whole lines (each still valid) plus one partial line (never
 * parsed).  There is no offset at which a truncation can produce a line that
 * parses into something the writer never wrote.
 * ======================================================================== */
static void load_text(const char *text, int len)
{
    int i = 0;
    unsigned want_crc = 0;
    int have_crc = 0, body_end = -1;

    while (i < len) {
        /* Find the end of this line. */
        int start = i, nl = -1;
        while (i < len && text[i] != '\n') i++;
        if (i < len) { nl = i; i++; }

        if (nl < 0 && i > start) {
            /* Unterminated final line.  This is the truncation case: the
             * writer terminates every line it emits, so an unterminated tail
             * can only be a file that was cut short.  Refuse to parse it --
             * and say so, because a machine that silently boots with two
             * thirds of its settings should be able to be asked why. */
            diag |= SET_D_TRUNCATED;
        }
#ifndef SETTINGS_NO_TRUNC_GUARD
        if (nl < 0) break;
#else
        /* NEGATIVE CONTROL (tests/unit/settings_test.c): parse it anyway.
         * This is the ONE rule the truncation argument rests on, so this is
         * the build in which "truncate at every byte offset" stops being safe
         * by construction -- a file cut inside `ui.accent = 0xC81E64` becomes
         * a colour the user never chose. */
#endif

        int end = nl < 0 ? i : nl;
        while (end > start && is_space(text[end - 1])) end--;
        int b = start;
        while (b < end && is_space(text[b])) b++;
        if (b == end) continue;                       /* blank line */

        if (text[b] == '#') {
            /* The only comment we read back is the CRC diagnostic. */
            const char *tail = "# crc32 = ";
            int m = 0;
            while (tail[m] && b + m < end && text[b + m] == tail[m]) m++;
            if (!tail[m]) {
                char hx[16]; int n = 0;
                for (int p = b + m; p < end && n < 15; p++) hx[n++] = text[p];
                hx[n] = 0;
                long cv;
                char withx[20];
                withx[0] = '0'; withx[1] = 'x';
                s_cpy(withx + 2, hx, (int)sizeof withx - 2);
                if (parse_long(withx, &cv)) { want_crc = (unsigned)cv; have_crc = 1; body_end = start; }
            }
            continue;
        }

        /* key = value */
        int eq = -1;
        for (int p = b; p < end; p++) if (text[p] == '=') { eq = p; break; }
        if (eq < 0) { diag |= SET_D_BADLINE; continue; }

        int ke = eq;
        while (ke > b && is_space(text[ke - 1])) ke--;
        int vb = eq + 1;
        while (vb < end && is_space(text[vb])) vb++;

        /* Measure BEFORE copying.  Copying `min(len, SET_VALLEN-1)` bytes and
         * storing that is the silent truncation this store no longer does --
         * on the way in from a file just as much as through set().  A line
         * whose value does not fit is dropped whole and reported, so nothing
         * downstream can read a value that is a prefix of what is on disk. */
        if (ke - b >= SET_KEYLEN || end - vb >= SET_VALLEN) {
            diag |= SET_D_BADLINE;
            continue;
        }

        char k[SET_KEYLEN], v[SET_VALLEN];
        int n = 0;
        for (int p = b; p < ke; p++) k[n++] = text[p];
        k[n] = 0;
        n = 0;
        for (int p = vb; p < end; p++) v[n++] = text[p];
        v[n] = 0;

        if (!key_ok(k)) { diag |= SET_D_BADLINE; continue; }
        if (put_ex(k, v, 1) < 0) diag |= SET_D_BADLINE;
    }

    /* The CRC is a DIAGNOSTIC.  It is checked, it is reported, and it never
     * refuses the file -- a hand-edited file has a stale CRC by definition and
     * is precisely the file somebody is in the middle of repairing.  Refusing
     * it would turn "I fixed a typo" into "the desktop lost my settings". */
    if (have_crc && body_end >= 0) {
        unsigned have = crc32(text, (size_t)body_end);
        if (have != want_crc) diag |= SET_D_CRCBAD;
    }
}

/* ===========================================================================
 * Load / commit
 * ======================================================================== */
/* Read one store into the table.  Returns 1 if a file was there and parsed,
 * 0 if there was nothing to read.  Diag bits accumulate across both calls,
 * which is the honest reporting: a bad line is a bad line whichever of the two
 * files it was on, and the loader names the file on the boot log. */
static int load_one(const char *path)
{
    int sz = vfs_size(path);
    if (sz <= 0) return 0;
    if (sz > SET_BUFSZ - 1) sz = SET_BUFSZ - 1;   /* read what fits; the rest is lines we drop */
    int n = vfs_read(path, buf, sz);
    if (n <= 0) return 0;
    buf[n] = 0;
    load_text(buf, n);
    return 1;
}

int settings_load(void)
{
    ntab = 0;
    diag = 0;
    loaded = 1;

    /* SYSTEM FIRST, ALWAYS.  It is the defaults layer, and it is the only
     * layer that exists before anybody has logged in -- which is precisely the
     * moment the greeter needs a theme and a wallpaper. */
    loading_sys = (user_path[0] != 0);
    int any = load_one(SET_PATH);
    loading_sys = 0;

#ifdef SETTINGS_NEGCTL_USER_READ_SYSTEM
    /* NEGATIVE CONTROL: write to the per-user path, read from the system one.
     *
     * This is today's bug wearing a fix's clothes, and it is the control the
     * suite has to go red on. Every save appears to work -- settings_set_*
     * updates the table in RAM, the UI follows, settings_commit() writes the
     * user's file and returns 0 -- and the value is GONE at the next boot,
     * because this build never reads it back. A test that only asserts "the
     * setting changed" or "the write succeeded" cannot tell this build from
     * the real one; only a reboot can. */
#else
    if (user_path[0]) any |= load_one(user_path);
#endif

    if (!any) diag |= SET_D_NOFILE;
    return diag;
}

/* ===========================================================================
 * WHICH USER'S SETTINGS -- see the two-store block in settings.h.
 * ======================================================================== */
const char *settings_store_path(void)
{ return user_path[0] ? user_path : SET_PATH; }

int settings_prepare_user(unsigned uid)
{
    pending_path[0] = 0;

    /* THE ONLY MOMENT THIS CAN WORK. /etc/passwd is root:root 0600 and the
     * caller is about to stop being root; a lookup after the drop reads
     * nothing. See the call site in c/kernel/exec/syscall.c. */
    static char store[ACCT_MAX + 1];
    int sz = vfs_size("/etc/passwd");
    if (sz <= 0 || sz > ACCT_MAX) return -1;
    int n = vfs_read("/etc/passwd", store, sz);
    if (n <= 0) return -1;
    store[n] = 0;

    struct account a;
    int pos = 0, found = 0;
    while (acct_next(store, n, &pos, &a)) if (a.uid == uid) { found = 1; break; }
    /* Wipe the hashes out of the kernel's static buffer the moment they are no
     * longer needed. They were read with a credential that was allowed to read
     * them; leaving them sitting in a static array for the rest of the boot is
     * a copy nobody accounted for. */
    for (int i = 0; i <= n; i++) store[i] = 0;
    if (!found) return -1;

    if (!a.home[0] || a.home[0] != '/') return -1;      /* a home is absolute */
    int hl = s_len(a.home);
    if (hl + (int)sizeof(SET_USER_REL) > SET_PATHLEN) return -1;
    s_cpy(pending_path, a.home, SET_PATHLEN);
    /* "/" + "/.config/..." would be "//.config/..."; vfs_path collapses it,
     * but the string is also printed on the boot log and read by a human. */
    int o = hl;
    if (o > 1 && pending_path[o - 1] == '/') o--;
    s_cpy(pending_path + o, SET_USER_REL, SET_PATHLEN - o);
    user_uid = uid;
    return 0;
}

void settings_discard_user(void) { pending_path[0] = 0; }

void settings_adopt_user(void)
{
    if (!pending_path[0]) return;
    s_cpy(user_path, pending_path, SET_PATHLEN);
    pending_path[0] = 0;

    /* Re-read with the layering on. This runs AFTER the credential dropped, so
     * the user's own file is read as the user -- if that read is refused, the
     * file is not theirs and they get the system defaults, which is the right
     * outcome and not a silent one (the line below says which store is live). */
    int d = settings_load();
    kprintf("SETTINGS_USER uid=%u store=%s defaults=%s diag=%d keys=%d\n",
            user_uid, user_path, SET_PATH, d, ntab);
}

/* Serialise into `buf`.  Returns the length. */
static int serialise(void)
{
    int o = 0;
    const char *hdr =
        "# LogitOS settings -- one \"key = value\" per line.\n"
        "#\n"
        "# Safe to edit by hand. A line that does not parse is ignored, a value\n"
        "# outside its range falls back to the built-in default, and deleting\n"
        "# this file restores every default. The crc32 line below is a torn-write\n"
        "# diagnostic only -- it is never a reason to refuse the file, so editing\n"
        "# this file without updating it is fine.\n";
    for (int i = 0; hdr[i] && o < SET_BUFSZ - 1; i++) buf[o++] = hdr[i];

    for (int i = 0; i < ntab; i++) {
        /* ONLY WHAT THIS USER CHOSE.  A value still carrying the system store's
         * mark is a default they never touched, and copying it into their file
         * would freeze today's default into their account forever -- the next
         * change to /etc/settings.conf would reach nobody who had ever opened
         * the Settings window.  On a machine with no user store nothing is
         * marked, so this loop writes the whole table exactly as it always did. */
        if (tab[i].sys) continue;
        int need = s_len(tab[i].k) + s_len(tab[i].v) + 4;
        if (o + need >= SET_BUFSZ - 32) break;
        for (const char *p = tab[i].k; *p; p++) buf[o++] = *p;
        buf[o++] = ' '; buf[o++] = '='; buf[o++] = ' ';
        for (const char *p = tab[i].v; *p; p++) buf[o++] = *p;
        buf[o++] = '\n';                       /* EVERY line is terminated */
    }

    unsigned c = crc32(buf, (size_t)o);
    const char *ct = "# crc32 = ";
    for (int i = 0; ct[i] && o < SET_BUFSZ - 16; i++) buf[o++] = ct[i];
    static const char *H = "0123456789ABCDEF";
    for (int sh = 28; sh >= 0; sh -= 4) buf[o++] = H[(c >> sh) & 0xF];
    buf[o++] = '\n';
    return o;
}

int settings_commit(void)
{
    int n = serialise();

    /* ONE whole-file write.  That is the entire atomicity mechanism, and it is
     * sufficient -- see the argument in settings.h: logitfs_write() reallocates
     * every block (inode_write -> inode_trunc + balloc, with frees deferred to
     * commit), writes and barriers them before the commit record, and installs
     * the inode only after it.  The file is its old content or its new content
     * and never a mixture.  There is no temp file here on purpose. */
    const char *path = settings_store_path();
    /* The parent directory.  /etc for the system store; $HOME/.config for a
     * user's -- made HERE rather than at enrolment, because an account created
     * before this code existed has no .config and would otherwise never be able
     * to save anything.  A failure is not checked: it usually means "it is
     * already there", and the write below is the thing that actually reports. */
    if (user_path[0]) {
        char dir[SET_PATHLEN];
        s_cpy(dir, path, SET_PATHLEN);
        for (int i = s_len(dir) - 1; i > 0; i--) if (dir[i] == '/') { dir[i] = 0; break; }
        if (vfs_mkdir(dir) < 0) { /* already exists, or refused: the write says */ }
    } else {
        if (vfs_mkdir("/etc") < 0) { /* already exists: fine */ }
    }
    int rc = vfs_write(path, buf, n);
    if (rc < 0) { kprintf("[set] commit FAILED %s (%d bytes)\n", path, n); return SET_E_IO; }
    gen++;
    return 0;
}

int settings_reset(void)
{
    /* Delete the store this machine WRITES, not the system one.  A user
     * resetting their settings must not be able to delete root's defaults --
     * and would not be allowed to anyway, so without this the reset would
     * silently do nothing while reporting success. */
    vfs_delete(settings_store_path());
    ntab = 0;
    diag = SET_D_NOFILE;
    gen++;
    /* Their overrides are gone; the defaults underneath them are not. */
    if (user_path[0]) settings_load();
    return 0;
}

void settings_init(void)
{
    int d = settings_load();
    kprintf("[set] %s: %d keys", settings_store_path(), ntab);
    if (d & SET_D_NOFILE)     kprintf(", none stored (defaults)");
    if (d & SET_D_TRUNCATED)  kprintf(", TRUNCATED (unterminated final line dropped)");
    if (d & SET_D_BADLINE)    kprintf(", BAD LINES ignored");
    if (d & SET_D_CRCBAD)     kprintf(", crc32 MISMATCH (edited or torn)");
    if (d & SET_D_FULL)       kprintf(", TABLE FULL");
    kprintf("\n");

    /* Report every stored value the schema refuses, by name.  "The desktop
     * must survive them and SAY WHAT IT REJECTED" -- a machine that quietly
     * ignores a setting the user typed is indistinguishable from one that is
     * broken. */
    for (int i = 0; i < NSCHEMA; i++) {
        struct kv *e = find(schema[i].key);
        if (!e) continue;
        long v; unsigned ip;
        int bad = 0;
        switch (schema[i].type) {
        case SET_T_BOOL: case SET_T_INT: case SET_T_COLOR:
            bad = !parse_long(e->v, &v) || v < schema[i].lo || v > schema[i].hi; break;
        case SET_T_IP:
            bad = !parse_ip(e->v, &ip); break;
        default: break;
        }
        if (bad) {
            diag |= SET_D_OUTOFRANGE;
            kprintf("[set] REJECTED %s = \"%s\" (out of range / unparseable); using default \"%s\"\n",
                    schema[i].key, e->v, schema[i].dflt);
        }
    }
    kprintf("SETTINGS_READY diag=%d keys=%d\n", diag, ntab);

    /* The truncation sweep, on EVERY boot, for the reason the mount-time fsck
     * runs on every mount: it makes every boot harness in the tree assert it,
     * including the ones written for something else entirely. It costs about a
     * millisecond (roughly 700 offsets over a 700-byte buffer) and it runs
     * against the same load_text() the line above just used, so it is testing
     * the shipped boot path rather than a copy of it. It saves and restores the
     * live table, so the machine that comes up is the one the file describes. */
    settings_selftest();
}

/* ===========================================================================
 * Reads -- range-checked HERE, on the way out, never on the way in.
 * ======================================================================== */
const struct setting_def *settings_schema_find(const char *key)
{
    for (int i = 0; i < NSCHEMA; i++) if (s_eq(schema[i].key, key)) return &schema[i];
    return NULL;
}

/* The stored string for `key`, or NULL. */
static const char *raw(const char *key)
{
    if (!loaded) return NULL;
    struct kv *e = find(key);
    return e ? e->v : NULL;
}

int settings_get_int(const char *key, int def)
{
    const struct setting_def *d = settings_schema_find(key);
    const char *r = raw(key);
    long v;
#ifdef SETTINGS_NO_RANGE_CHECK
    /* NEGATIVE CONTROL: hand back whatever the file said. This is the build in
     * which a hand-edited `ui.dark = -1` reaches the compositor, and it exists
     * because "range-check on READ, not on write" is a claim that has to be
     * watched failing to count as tested. */
    if (r && parse_long(r, &v)) return (int)v;
#else
    if (r && parse_long(r, &v)) {
        if (!d || (v >= d->lo && v <= d->hi)) return (int)v;
        /* In range for the type but not for this key: fall through to the
         * schema default rather than to the caller's, so a nonsense file gives
         * the same machine every caller expects. */
    }
#endif
    if (d && parse_long(d->dflt, &v)) return (int)v;
    return def;
}

unsigned settings_get_color(const char *key, unsigned def)
{
    const struct setting_def *d = settings_schema_find(key);
    const char *r = raw(key);
    long v;
    if (r && parse_long(r, &v) && v >= 0 && v <= 0xFFFFFF) return (unsigned)v;
    if (d && parse_long(d->dflt, &v)) return (unsigned)v;
    return def;
}

/* Does this stored string satisfy its schema? Used by settings_get_str so that
 * EVERY read -- typed or not -- answers with the value IN FORCE rather than the
 * bytes on disk.
 *
 * That distinction matters and it is the contract SYS_SETTING_GET publishes: a
 * caller reads a setting in order to ACT on it, and handing back
 * `net.ip = 999.1.1.1` so the caller can discover for itself that it is not an
 * address just moves the validation out to every caller, which is where it gets
 * forgotten. The raw stored bytes are still reachable, through SETCTL_KVAT --
 * that is what the Settings app's "All settings" tab lists, so a human can see
 * what they actually typed in order to repair it. Effective value for acting on,
 * stored value for fixing. */
static int value_ok(const struct setting_def *d, const char *v)
{
    long n;
    unsigned ip;
    if (!d) return 1;                       /* no schema: anything is allowed */
    switch (d->type) {
    case SET_T_BOOL: case SET_T_INT: case SET_T_COLOR:
        return parse_long(v, &n) && n >= d->lo && n <= d->hi;
    case SET_T_IP:
        return parse_ip(v, &ip);
    default:
        return v[0] != 0;
    }
}

const char *settings_get_str(const char *key, const char *def)
{
    const struct setting_def *d = settings_schema_find(key);
    const char *r = raw(key);
#ifdef SETTINGS_NO_RANGE_CHECK
    if (r && r[0]) return r;                /* NEGATIVE CONTROL: see settings_get_int */
#else
    if (r && r[0] && value_ok(d, r)) return r;
#endif
    if (d && d->dflt && d->dflt[0]) return d->dflt;
    return def;
}

unsigned settings_get_ip(const char *key, unsigned def)
{
    const struct setting_def *d = settings_schema_find(key);
    const char *r = raw(key);
    unsigned v;
    if (r && parse_ip(r, &v)) return v;
    if (d && parse_ip(d->dflt, &v)) return v;
    return def;
}

/* ===========================================================================
 * Writes
 * ======================================================================== */
int settings_set_str(const char *key, const char *val, int commit)
{
    if (!loaded) settings_load();
    int rc = put(key, val);
    if (rc < 0) return rc;                 /* SET_E_*, propagated verbatim */
    return commit ? settings_commit() : 0;
}

int settings_set_int(const char *key, long val, int commit)
{
    char v[24];
    const struct setting_def *d = settings_schema_find(key);
    if (d && d->type == SET_T_COLOR) fmt_hex6(v, (int)sizeof v, (unsigned)(val & 0xFFFFFF));
    else                             fmt_long(v, (int)sizeof v, val);
    return settings_set_str(key, v, commit);
}

/* ===========================================================================
 * Introspection
 * ======================================================================== */
int settings_schema_count(void) { return NSCHEMA; }
const struct setting_def *settings_schema(int i)
{ return (i >= 0 && i < NSCHEMA) ? &schema[i] : NULL; }
int settings_kv_count(void) { return ntab; }
int settings_kv_at(int i, const char **k, const char **v)
{
    if (i < 0 || i >= ntab) return -1;
    if (k) *k = tab[i].k;
    if (v) *v = tab[i].v;
    return 0;
}
unsigned settings_gen(void) { return gen; }
int settings_diag(void) { return diag; }

/* ===========================================================================
 * Window frames.  The format is documented in settings.h; the two call sites
 * belong to the window-management line.
 * ======================================================================== */
static void frame_key(const char *app, char *out)
{
    int o = 0;
    const char *p = "win.";
    while (*p && o < SET_KEYLEN - 8) out[o++] = *p++;
    for (int i = 0; app[i] && o < SET_KEYLEN - 8; i++) {
        char c = app[i];
        /* An app name becomes part of a key, so it has to obey key_ok(). */
        out[o++] = (c <= ' ' || c >= 0x7F || c == '=' || c == '#' || c == '.') ? '_' : c;
    }
    const char *s = ".frame";
    while (*s && o < SET_KEYLEN - 1) out[o++] = *s++;
    out[o] = 0;
}

int settings_frame_load(const char *app, struct win_frame *f,
                        int screen_w, int screen_h)
{
    char key[SET_KEYLEN];
    frame_key(app, key);
    const char *r = raw(key);
    if (!r || !r[0]) return 0;

#define NFRAME 11
    long v[NFRAME];
    int n = 0, i = 0;
    while (r[i] && n < NFRAME) {
        while (is_space(r[i])) i++;
        if (!r[i]) break;
        int st = i;
        while (r[i] && !is_space(r[i])) i++;
        char tok[24];
        int m = 0;
        for (int p = st; p < i && m < 23; p++) tok[m++] = r[p];
        tok[m] = 0;
        if (!parse_long(tok, &v[n])) return 0;      /* any bad field: refuse the record */
        n++;
    }
    /* Exactly NFRAME fields.  A short record is a truncation and a long one is
     * a format from some other version; both are refused whole.  Salvaging the
     * fields that did parse is the tempting mistake -- it produces a window at
     * a position half of which the user chose and half of which is uninitialised
     * stack, and it looks like a WM bug rather than a bad settings file. */
    if (n != NFRAME || r[i]) return 0;

    /* Sanity, and this is the check that matters most.  A frame saved on a big
     * desktop must not put a window where the user cannot reach its titlebar on
     * a small one.  Refuse rather than clamp: a clamped garbage frame still
     * looks like the machine did something strange, whereas a refused one puts
     * the window exactly where a fresh machine would. */
    if (v[2] < 80 || v[3] < 60) return 0;                        /* absurdly small */
    if (v[2] > (long)screen_w * 4 || v[3] > (long)screen_h * 4) return 0;
    if (v[0] + v[2] < 40 || v[1] + v[3] < 20) return 0;          /* off the top/left */
    if (v[0] > (long)screen_w - 40 || v[1] > (long)screen_h - 20) return 0;
    if (v[5] < 0 || v[5] > 1 || v[6] < 0 || v[6] > 1) return 0;  /* zoomed/minimized are bools */
    if (v[9] < 80 || v[10] < 60) return 0;                       /* the restore rect too */
    if (v[7] > (long)screen_w - 40 || v[8] > (long)screen_h - 20) return 0;
    if (v[7] + v[9] < 40 || v[8] + v[10] < 20) return 0;

    f->x = (int)v[0]; f->y = (int)v[1]; f->w = (int)v[2];  f->h  = (int)v[3];
    f->open = v[4] ? 1 : 0;
    f->zoomed = (int)v[5]; f->minimized = (int)v[6];
    f->rx = (int)v[7]; f->ry = (int)v[8]; f->rw = (int)v[9]; f->rh = (int)v[10];
    return 1;
}

int settings_frame_save(const char *app, const struct win_frame *f, int commit)
{
    char key[SET_KEYLEN], v[SET_VALLEN];
    frame_key(app, key);
    int o = 0;
    const long fields[NFRAME] = { f->x, f->y, f->w, f->h, f->open ? 1 : 0,
                                  f->zoomed ? 1 : 0, f->minimized ? 1 : 0,
                                  f->rx, f->ry, f->rw, f->rh };
    for (int i = 0; i < NFRAME; i++) {
        if (i) v[o++] = ' ';
        o += fmt_long(v + o, (int)sizeof v - o, fields[i]);
    }
    v[o] = 0;
    return settings_set_str(key, v, commit);
}

/* ===========================================================================
 * THE TRUNCATION SWEEP, run on the machine through the real loader.
 *
 * The brief's assertion is "truncate the settings file at every byte offset
 * and boot each time".  Two hundred real QEMU reboots is hours, so this runs
 * the sweep IN the kernel, against the same load_text() the boot path calls,
 * and the boot harness additionally does a handful of real reboots at chosen
 * offsets.  What this proves is the strong half: at no offset does the loader
 * fail to produce a usable configuration.
 *
 * "Usable" is asserted, not assumed: after each truncation every schema key is
 * read back through the real getter and must land in its declared range.  A
 * parser that survived by leaving the store empty would pass a crash test and
 * fail this one.
 * ======================================================================== */
int settings_selftest(void)
{
    /* Save the live table so the sweep cannot disturb the running machine. */
    static struct kv saved[SET_MAXKV];
    int nsaved = ntab, sdiag = diag;
    for (int i = 0; i < ntab; i++) saved[i] = tab[i];

    /* Build a representative file: every schema key, a window frame, an
     * unknown key (the preservation case) and a comment. */
    ntab = 0;
    for (int i = 0; i < NSCHEMA; i++) put(schema[i].key, schema[i].dflt);
    put("win.clock.frame", "40 60 320 240 1 0 0 40 60 320 240");
    put("notify.history", "3");     /* an unknown key: the preservation case */
    int len = serialise();

    static char snap[SET_SELFTEST_BUFSZ];
    if (len > SET_SELFTEST_BUFSZ - 1) len = SET_SELFTEST_BUFSZ - 1;
    for (int i = 0; i < len; i++) snap[i] = buf[i];

    /* The two answers a truncation is allowed to produce for any key: what the
     * WHOLE file says, or what a machine with NO file says.
     *
     * "Inside its declared range" alone is too weak, and that is not a guess --
     * the negative control in tests/settings.mk demonstrated it. A file cut
     * inside `ui.accent = 0xC81E64` leaves `0xC81E6`, an entirely in-range
     * colour that nobody chose. Losing a setting to a torn file is acceptable;
     * inventing one is not. */
    static char whole[NSCHEMA][SET_VALLEN], none[NSCHEMA][SET_VALLEN];
    ntab = 0; diag = 0;
    load_text(snap, len);
    for (int i = 0; i < NSCHEMA; i++) s_cpy(whole[i], settings_get_str(schema[i].key, ""), SET_VALLEN);
    ntab = 0; diag = 0;
    for (int i = 0; i < NSCHEMA; i++) s_cpy(none[i], settings_get_str(schema[i].key, ""), SET_VALLEN);

    int failures = 0;
    for (int cut = 0; cut <= len; cut++) {
        ntab = 0; diag = 0;
        load_text(snap, cut);
        /* Every schema key must read back inside its declared range... */
        for (int i = 0; i < NSCHEMA; i++) {
            const struct setting_def *d = &schema[i];
            if (d->type == SET_T_IP) {
                unsigned ip = settings_get_ip(d->key, 0);
                if (!ip) { failures++; kprintf("[set] selftest cut=%d %s -> 0\n", cut, d->key); }
            } else if (d->type == SET_T_STR) {
                const char *s = settings_get_str(d->key, "");
                if (!s || !s[0]) { failures++; kprintf("[set] selftest cut=%d %s -> empty\n", cut, d->key); }
            } else {
                int v = settings_get_int(d->key, -999);
                if (v < d->lo || v > d->hi) {
                    failures++;
                    kprintf("[set] selftest cut=%d %s -> %d out of [%d,%d]\n",
                            cut, d->key, v, (int)d->lo, (int)d->hi);
                }
            }
            /* ...and must be one of the only two values it may be. */
            const char *v = settings_get_str(d->key, "");
            if (!s_eq(v, whole[i]) && !s_eq(v, none[i])) {
                failures++;
                kprintf("[set] selftest cut=%d INVENTED %s = \"%s\" (file \"%s\", default \"%s\")\n",
                        cut, d->key, v, whole[i], none[i]);
            }
        }
        /* And a truncated frame record must never be accepted as a valid one. */
        struct win_frame f;
        if (settings_frame_load("clock", &f, 1024, 768)) {
            if (f.w < 80 || f.h < 60 || f.x > 1024 || f.y > 768) {
                failures++;
                kprintf("[set] selftest cut=%d accepted a bad frame %d,%d %dx%d\n",
                        cut, f.x, f.y, f.w, f.h);
            }
        }
    }

    /* Garbage sweep: the hand-edited cases the brief names by hand. */
    static const char *garbage[] = {
        "ui.dark = banana\n",
        "ui.dark = -1\n",
        "ui.dark = 99999999999999999999\n",
        "ui.accent = 0xFFFFFFFF\n",
        "ui.accent = -1\n",
        "net.ip = 999.1.1.1\n",
        "net.ip = ....\n",
        "win.clock.frame = -5000 -5000 10 10 1 0 0 0 0 0 0\n",   /* off-screen + tiny */
        "win.clock.frame = 0 0 99999 99999 1 0 0 0 0 99999 99999\n",
        "win.clock.frame = 1 2 3\n",                             /* short record */
        "win.clock.frame = a b c d e f g h i j k\n",             /* not numbers */
        "win.clock.frame = 40 60 320 240 1 7 0 40 60 320 240\n", /* zoomed = 7 */
        "= 5\n",
        "ui.dark\n",
        "\n\n\n\n",
        "ui.dark = 1",                        /* unterminated: must be dropped */
        "#\n#\n#\n",
    };
    for (unsigned g = 0; g < sizeof garbage / sizeof garbage[0]; g++) {
        ntab = 0; diag = 0;
        load_text(garbage[g], s_len(garbage[g]));
        for (int i = 0; i < NSCHEMA; i++) {
            const struct setting_def *d = &schema[i];
            if (d->type == SET_T_IP) {
                if (!settings_get_ip(d->key, 0)) { failures++; kprintf("[set] garbage %u: %s -> 0\n", g, d->key); }
            } else if (d->type == SET_T_STR) {
                const char *s = settings_get_str(d->key, "");
                if (!s || !s[0]) { failures++; kprintf("[set] garbage %u: %s -> empty\n", g, d->key); }
            } else {
                int v = settings_get_int(d->key, -999);
                if (v < d->lo || v > d->hi) {
                    failures++;
                    kprintf("[set] garbage %u: %s -> %d out of range\n", g, d->key, v);
                }
            }
        }
        struct win_frame f;
        if (settings_frame_load("clock", &f, 1024, 768) &&
            (f.w < 80 || f.h < 60 || f.x > 1024 || f.y > 768)) {
            failures++;
            kprintf("[set] garbage %u accepted frame %d,%d %dx%d\n", g, f.x, f.y, f.w, f.h);
        }
    }

    /* The unterminated-final-line rule, asserted directly rather than inferred
     * from the sweep passing: "ui.dark = 1" with no newline must leave NOTHING
     * stored.  If this ever stops holding, the truncation argument above is no
     * longer an argument, and the sweep alone would not say so. */
    ntab = 0; diag = 0;
    load_text("ui.dark = 1", 11);
    if (ntab != 0 || !(diag & SET_D_TRUNCATED)) {
        failures++;
        kprintf("[set] selftest: an unterminated line was parsed (%d keys, diag %d)\n", ntab, diag);
    }

    ntab = nsaved; diag = sdiag;
    for (int i = 0; i < nsaved; i++) tab[i] = saved[i];

    kprintf("SETTINGS_SELFTEST offsets=%d failures=%d\n", len + 1, failures);
    return failures;
}

/* ===========================================================================
 * The syscall surface
 * ======================================================================== */
long settings_syscall(long num, long a, long b, long c)
{
    switch (num) {
    case SYS_SETTING_GET: {
        const char *key = (const char *)a;
        char *out = (char *)b;
        int max = (int)c;
        if (!key || !out || max <= 0) return -1;
        const char *v = settings_get_str(key, NULL);
        if (!v) return -1;
        int n = s_len(v);
        if (n > max - 1) n = max - 1;
        for (int i = 0; i < n; i++) out[i] = v[i];
        out[n] = 0;
        return n;
    }
    case SYS_SETTING_SET: {
        const char *key = (const char *)a;
        const char *val = (const char *)b;
        if (!key || !val) return -1;
        return settings_set_str(key, val, (int)c & 1);
    }
    case SYS_SETTING_ENUM: {
        int i = (int)a;
        struct logit_setting *out = (struct logit_setting *)b;
        const struct setting_def *d = settings_schema(i);
        if (!d || !out) return -1;
        s_cpy(out->key,   d->key,   (int)sizeof out->key);
        s_cpy(out->label, d->label, (int)sizeof out->label);
        s_cpy(out->dflt,  d->dflt,  (int)sizeof out->dflt);
        s_cpy(out->value, settings_get_str(d->key, d->dflt), (int)sizeof out->value);
        out->type  = d->type;
        out->group = d->group;
        out->lo    = (int)d->lo;
        out->hi    = (int)d->hi;
        return 0;
    }
    case SYS_SETTING_CTL:
        switch ((int)a) {
        case SETCTL_GEN:      return (long)settings_gen();
        case SETCTL_COMMIT:   return settings_commit();
        case SETCTL_RESET:    return settings_reset();
        case SETCTL_COUNT:    return settings_schema_count();
        case SETCTL_DIAG:     return settings_diag();
        case SETCTL_RELOAD:   return settings_load();
        case SETCTL_SELFTEST: return settings_selftest();
        case SETCTL_KVCOUNT:  return settings_kv_count();
        /* The limits, from the machine rather than from a header. A consumer
         * deciding whether its data belongs here should be able to ASK. */
        case SETCTL_LIMITS:
            switch ((int)b) {
            case SETLIM_MAXKEYS: return SET_MAXKV;
            case SETLIM_VALLEN:  return SET_VALLEN - 1;    /* usable bytes */
            case SETLIM_KEYLEN:  return SET_KEYLEN - 1;
            case SETLIM_FREE:    return SET_MAXKV - ntab;
            default: return -1;
            }
        case SETCTL_KVAT: {
            /* (SETCTL_KVAT, buf, index) -> "key=value". How an app lists keys
             * it has never heard of, which is the whole point of preserving
             * them.
             *
             * ONE bound, SET_KVLINE, and it is the size the ABI tells callers
             * to allocate. The previous version had two different and larger
             * bounds -- the second one deliberately +8 past the first -- which
             * happened to be safe only because SET_VALLEN was 80 and every
             * caller had rounded up. Raising SET_VALLEN to 160 turned that into
             * a write past the end of the Settings app's buffer, which is the
             * kind of thing a limits change is supposed to be caught by. */
            const char *k, *v;
            char *out = (char *)b;
            if (settings_kv_at((int)c, &k, &v) < 0 || !out) return -1;
            int o = 0;
            for (int i = 0; k[i] && o < SET_KVLINE - 2; i++) out[o++] = k[i];
            if (o < SET_KVLINE - 2) out[o++] = '=';
            for (int i = 0; v[i] && o < SET_KVLINE - 1; i++) out[o++] = v[i];
            out[o] = 0;
            return o;
        }
        default: return -1;
        }
    }
    return -1;
}

/* ===========================================================================
 * WHAT IS DELIBERATELY NOT A SETTING HERE, and why each is absent rather than
 * present-but-inert:
 *
 *   screen resolution -- SYS_SCREEN_INFO reports the mode; nothing SETS it.
 *     Changing it means a virtio-gpu mode set in c/kernel/gui/fb.c, which
 *     belongs to the window-management line this round.  The store would hold
 *     the number happily; the machine would ignore it.
 *
 *   font size / UI scale -- fb_pt()'s backing scale is fixed at fb_init from
 *     the device mode, same file, same owner.
 *
 *   which apps were open -- the `visible` bit of win_frame carries it and this
 *     side is done, but relaunching them at boot is a wm_launch() loop inside
 *     wm_run(), which is that line's file.  desktop.restore_session is the flag
 *     they read; it is stored and defaulted and does nothing until they do.
 *
 *   accent colour is a HALF case and is included on purpose: it is stored,
 *     validated and applied by the Settings app to itself, so it is visibly
 *     real -- but making every app follow it needs one line in aui.c's EV_THEME
 *     handler (aui_set_accent(sys_setting_color("ui.accent"))), and aui.c
 *     belongs to the toolkit line.  That is an ask, not an edit.
 * ======================================================================== */
