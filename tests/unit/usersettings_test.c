/* The PER-USER settings store, host-side.
 *
 * This compiles the REAL c/kernel/core/settings.c against a small RAM
 * filesystem, so the layering, the ownership bit, the commit path and the
 * /etc/passwd lookup under test are the ones that boot.
 *
 * WHAT IT IS FOR. Until this landed there was one settings file,
 * /etc/settings.conf, root:root 0600, written through vfs_write() with the
 * CALLING process's credential. From the moment /bin/login started dropping
 * the desktop to a real uid, a user toggling dark mode was refused and the
 * choice was gone at the next boot. The fix is a per-user store over a
 * read-only system one, and the claims that need watching are:
 *
 *   1. A user's value WINS over the system default.
 *   2. A user's file holds ONLY what that user changed -- so a later change to
 *      a system default still reaches them.
 *   3. The system file is NEVER written by a user's commit.
 *   4. A machine with NO user is byte-for-byte the single-store machine it was.
 *   5. It SURVIVES A RELOAD -- which is this file's stand-in for a reboot, and
 *      the only assertion the negative control cannot pass.
 *   6. Two users do not see each other's settings.
 *
 * THE NEGATIVE CONTROL, -DSETTINGS_NEGCTL_USER_READ_SYSTEM, is today's bug
 * wearing a fix's clothes: every save is written to the per-user path and every
 * read comes from the system one. Setting a value appears to work, the value is
 * live for the rest of the session, the commit returns 0, the file on disk is
 * correct -- and it is gone at the next boot. Claims 1-4 and 6 all still pass
 * under it. Only claim 5 fails, which is why it is written down as its own
 * claim rather than folded into the others.
 *
 * ON EXIT STATUS: this binary exits non-zero when an assertion fails, under the
 * control flag as much as without it. The Makefile fragment requires the
 * control run to fail. (tests/unit/settings_test.c inverts its own status
 * instead; both work, and one of them has to be read carefully at 2am.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "settings.h"
#include "logit_abi.h"

/* ------------------------------------------------------------- the RAM FS --
 * Several files this time, not one: the whole point is that there are two
 * stores and a third file (/etc/passwd) that says where the second one is.
 * Ownership is modelled too, because the reason the real code creates the
 * user's file AS ROOT is a permission rule, and a fake filesystem with no
 * permissions would quietly make that code look unnecessary. */
#define NFILE 8
#define FSZ   8192
static struct {
    char  path[128];
    char  data[FSZ];
    int   len;                 /* -1 = does not exist */
    int   isdir;
    unsigned uid, gid, mode;
} fs[NFILE];
static unsigned cur_uid = 0, cur_gid = 0;      /* who the "process" is */
static int fs_writes[NFILE];

static int slot(const char *p)
{
    for (int i = 0; i < NFILE; i++) if (fs[i].len >= 0 && !strcmp(fs[i].path, p)) return i;
    return -1;
}
static int slot_new(const char *p)
{
    for (int i = 0; i < NFILE; i++)
        if (fs[i].len < 0) { snprintf(fs[i].path, sizeof fs[i].path, "%s", p); return i; }
    return -1;
}
static void fs_reset(void)
{
    for (int i = 0; i < NFILE; i++) { fs[i].len = -1; fs[i].path[0] = 0; fs_writes[i] = 0; }
    cur_uid = cur_gid = 0;
}
static int fs_put(const char *p, const char *text, unsigned uid, unsigned gid, unsigned mode)
{
    int i = slot(p); if (i < 0) i = slot_new(p);
    if (i < 0) return -1;
    fs[i].len = (int)strlen(text);
    memcpy(fs[i].data, text, (size_t)fs[i].len);
    fs[i].isdir = 0; fs[i].uid = uid; fs[i].gid = gid; fs[i].mode = mode;
    return 0;
}
static const char *fs_get(const char *p)
{
    int i = slot(p);
    if (i < 0) return NULL;
    fs[i].data[fs[i].len] = 0;
    return fs[i].data;
}

/* The permission rule that matters here, and only that one: writing a file you
 * do not own, whose mode does not grant you write, is refused. Root is not. */
static int may_write(int i)
{
    if (cur_uid == 0) return 1;
    if (fs[i].uid == cur_uid) return (fs[i].mode & 0200) != 0;
    return (fs[i].mode & 0002) != 0;
}

int vfs_size(const char *p) { int i = slot(p); return i < 0 ? -1 : fs[i].len; }

int vfs_read(const char *p, void *b, int max)
{
    int i = slot(p);
    if (i < 0) return -1;
    if (cur_uid && fs[i].uid != cur_uid && !(fs[i].mode & 0004)) return -1;
    if (cur_uid && fs[i].uid == cur_uid && !(fs[i].mode & 0400)) return -1;
    int n = fs[i].len < max ? fs[i].len : max;
    memcpy(b, fs[i].data, (size_t)n);
    return n;
}

int vfs_write(const char *p, const void *b, int size)
{
    int i = slot(p);
    if (i >= 0) { if (!may_write(i)) return -1; }
    else {
        /* A NEW name. Created owned by root whatever the caller is -- which is
         * not a simplification, it is the real behaviour this code had to be
         * built around: c/fs/vfs.c writes the creator into the RAM metadata
         * table while logitfs's own getattr, which every later check reads,
         * still says root. See settings_prepare_user(). */
        i = slot_new(p);
        if (i < 0) return -1;
        fs[i].uid = 0; fs[i].gid = 0; fs[i].mode = 0644; fs[i].isdir = 0;
    }
    if (size > FSZ) return -1;
    memcpy(fs[i].data, b, (size_t)size);
    fs[i].len = size;
    fs_writes[i]++;
    return size;
}

int vfs_delete(const char *p) { int i = slot(p); if (i < 0) return -1; fs[i].len = -1; return 0; }

int vfs_mkdir(const char *p)
{
    if (slot(p) >= 0) return -1;
    int i = slot_new(p);
    if (i < 0) return -1;
    fs[i].len = 0; fs[i].isdir = 1; fs[i].uid = 0; fs[i].gid = 0; fs[i].mode = 0755;
    return 0;
}

int vfs_chown(const char *p, unsigned uid, unsigned gid)
{
    int i = slot(p);
    if (i < 0) return -1;
    if (cur_uid != 0) return -1;                 /* chown is root only */
    fs[i].uid = uid; fs[i].gid = gid;
    return 0;
}

int vfs_chmod(const char *p, unsigned mode)
{
    int i = slot(p);
    if (i < 0) return -1;
    if (cur_uid != 0 && fs[i].uid != cur_uid) return -1;
    fs[i].mode = mode;
    return 0;
}

void kprintf(const char *fmt, ...)
{
    if (!getenv("SETTINGS_VERBOSE")) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------- assertions --*/
static int checks, fails;
static void okf(int cond, const char *fmt, ...)
{
    checks++;
    if (cond) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    fputs("  FAIL: ", stdout); vprintf(fmt, ap); fputc('\n', stdout);
    va_end(ap);
}
static int has(const char *hay, const char *needle)
{ return hay && strstr(hay, needle) != NULL; }

/* A store with two accounts, in the real /etc/passwd format. The hashes are
 * NOT credentials and nothing here checks a password: this file exercises the
 * uid -> home lookup, and acct_parse_line only requires the field to be
 * non-empty. A real hash in a test file is a credential in a test file. */
#define PASSWD \
    "alice:$pbkdf2-sha256$1$AAAA$AAAA:1000:1000:/home/alice:/bin/sh\n" \
    "bob:$pbkdf2-sha256$1$BBBB$BBBB:1001:1001:/home/bob:/bin/sh\n"

/* Log in as `uid`, exactly the way the kernel does it: resolve while root,
 * then drop, then adopt. Getting this order wrong is the bug the split exists
 * to prevent, so the harness reproduces the order rather than papering it. */
static int login_as(unsigned uid, unsigned gid)
{
    cur_uid = 0; cur_gid = 0;                    /* /bin/login is still root */
    int rc = settings_prepare_user(uid);
    cur_uid = uid; cur_gid = gid;                /* SYS_SETSESSION drops here */
    if (rc == 0) settings_adopt_user();
    else         settings_discard_user();
    return rc;
}

/* The stand-in for a reboot: everything in RAM is gone, the files are not. */
static void reboot_as(unsigned uid, unsigned gid)
{
    settings_discard_user();
    cur_uid = 0; cur_gid = 0;
    settings_load();                             /* boot: system store only */
    if (uid) login_as(uid, gid);
}

int main(void)
{
    /* ================================================================== 1 ==
     * A MACHINE WITH NO ACCOUNTS is the machine that was here before. Every
     * pre-existing settings test asserts this behaviour, so it is asserted
     * first: one store, all keys in it, written by root. */
    fs_reset();
    fs_put(SET_PATH, "ui.dark = 0\nui.accent = 0x112233\n", 0, 0, 0600);
    settings_load();
    okf(settings_get_int("ui.dark", 9) == 0, "no-user: ui.dark should be 0");
    okf(settings_get_color("ui.accent", 0) == 0x112233u, "no-user: accent from the system file");
    okf(!strcmp(settings_store_path(), SET_PATH), "no-user: the store is %s", settings_store_path());

    settings_set_int("ui.dark", 1, 1);
    {
        const char *sys = fs_get(SET_PATH);
        okf(has(sys, "ui.dark = 1"), "no-user: the change went to the system file");
        /* THE REGRESSION THIS WHOLE MECHANISM COULD HAVE CAUSED: with no user
         * store, nothing is marked as a default, so a commit must still write
         * the WHOLE table. Marking system keys unconditionally would have
         * silently dropped every key but the one just set. */
        okf(has(sys, "ui.accent = 0x112233"),
            "no-user: the OTHER keys were dropped from the system file -- a commit\n"
            "        with no user store must still write the whole table");
    }

    /* ================================================================== 2 ==
     * WHAT THE LOOKUP REFUSES, and it is asserted BEFORE anybody logs in on
     * purpose: the claim is "a refused lookup leaves the store where it was",
     * and the only state in which that claim has teeth is the one where the
     * store has not already been moved. Ordering as an assertion. */
    fs_reset();
    fs_put(SET_PATH, "ui.dark = 0\nui.accent = 0x112233\n", 0, 0, 0644);
    fs_put("/etc/passwd", PASSWD, 0, 0, 0600);
    settings_load();

    okf(settings_prepare_user(4242) < 0, "an unknown uid resolved a home");
    settings_discard_user();
    okf(!strcmp(settings_store_path(), SET_PATH),
        "an unknown uid moved the store to %s", settings_store_path());

    /* A non-root caller cannot read /etc/passwd, so the lookup fails -- which
     * is the only privilege check settings_prepare_user() needs, and this is
     * the assertion that says so. */
    cur_uid = 1000; cur_gid = 1000;
    okf(settings_prepare_user(1000) < 0,
        "a NON-ROOT caller resolved a home out of a 0600 /etc/passwd");
    cur_uid = 0; cur_gid = 0;
    settings_discard_user();

    /* A home that is not absolute is refused rather than joined onto whatever
     * the current directory happens to be. */
    fs_put("/etc/passwd", "eve:x:1002:1002:relative/home:/bin/sh\n", 0, 0, 0600);
    okf(settings_prepare_user(1002) < 0, "a relative home was accepted");
    settings_discard_user();
    fs_put("/etc/passwd", PASSWD, 0, 0, 0600);

    /* ================================================================== 3 ==
     * A USER LOGS IN. The home comes from /etc/passwd, and the file must end
     * up owned BY THEM -- which is the only reason they can write it twice. */

    okf(login_as(1000, 1000) == 0, "alice: the login did not resolve a store");
    okf(!strcmp(settings_store_path(), "/home/alice/.config/settings.conf"),
        "alice: the store is %s", settings_store_path());
    {
        int i = slot("/home/alice/.config/settings.conf");
        okf(i >= 0, "alice: the store file was not created at login");
        if (i >= 0) {
            okf(fs[i].uid == 1000, "alice: the store is owned by uid %u, not 1000", fs[i].uid);
            okf(fs[i].mode == 0600, "alice: the store is mode %o, not 600", fs[i].mode);
        }
        int d = slot("/home/alice/.config");
        okf(d >= 0 && fs[d].uid == 1000 && fs[d].mode == 0700,
            "alice: .config is not hers, 0700");
    }

    /* CLAIM 1: the system default is visible to her... */
    okf(settings_get_color("ui.accent", 0) == 0x112233u,
        "alice: the system default did not reach her");
    okf(settings_get_int("ui.dark", 9) == 0, "alice: ui.dark should start at the default 0");

    /* ...and her value wins over it. */
    okf(settings_set_int("ui.dark", 1, 1) == 0, "alice: the commit was refused");
    okf(settings_get_int("ui.dark", 9) == 1, "alice: her own value did not win");

    /* CLAIM 3: the system file is untouched. Asserted on the BYTES, not on a
     * write counter -- a store that rewrote /etc/settings.conf with identical
     * content would still be a store that needed root to save a preference. */
    okf(!has(fs_get(SET_PATH), "ui.dark = 1"),
        "alice: HER CHOICE WAS WRITTEN INTO THE SYSTEM FILE");
    {
        int i = slot(SET_PATH);
        okf(i >= 0 && fs_writes[i] == 0, "alice: the system file was written %d times",
            i >= 0 ? fs_writes[i] : -1);
    }

    /* CLAIM 2: her file holds ONLY what she changed. */
    {
        const char *u = fs_get("/home/alice/.config/settings.conf");
        okf(has(u, "ui.dark = 1"), "alice: her file does not hold her value");
        okf(!has(u, "ui.accent"),
            "alice: her file froze a system DEFAULT into her account -- a later\n"
            "        change to /etc/settings.conf would then never reach her");
    }

    /* CLAIM 5: IT SURVIVES A REBOOT. The one the negative control fails. */
    reboot_as(1000, 1000);
    okf(settings_get_int("ui.dark", 9) == 1,
        "alice: HER SETTING DID NOT SURVIVE THE REBOOT -- this is the original bug,\n"
        "        or a build that writes the user's file and reads the system one");

    /* ...and a later change to a system default still reaches her, because it
     * was never copied into her file. */
    cur_uid = 0;
    fs_put(SET_PATH, "ui.dark = 0\nui.accent = 0x445566\n", 0, 0, 0600);
    reboot_as(1000, 1000);
    okf(settings_get_color("ui.accent", 0) == 0x445566u,
        "alice: a changed system default did not reach a user who never overrode it");
    okf(settings_get_int("ui.dark", 9) == 1, "alice: her override was lost by the reload");

    /* ================================================================== 4 ==
     * A SECOND USER. Bob must see the system defaults and NONE of alice's. */
    reboot_as(1001, 1001);
    okf(!strcmp(settings_store_path(), "/home/bob/.config/settings.conf"),
        "bob: the store is %s", settings_store_path());
    okf(settings_get_int("ui.dark", 9) == 0,
        "bob: HE SEES ALICE'S DARK MODE -- the stores are not per user");
    okf(settings_get_color("ui.accent", 0) == 0x445566u,
        "bob: the system default did not reach him");

    settings_set_int("ui.dark", 1, 1);
    okf(has(fs_get("/home/bob/.config/settings.conf"), "ui.dark = 1"),
        "bob: his own save did not land in his own file");
    reboot_as(1000, 1000);
    okf(settings_get_int("ui.dark", 9) == 1, "alice: bob's save disturbed hers");

    /* ================================================================== 5 ==
     * RESET is the user's, not root's. */
    settings_reset();
    okf(slot("/home/alice/.config/settings.conf") < 0, "reset: her file was not removed");
    okf(slot(SET_PATH) >= 0, "reset: A USER'S RESET DELETED THE SYSTEM STORE");
    okf(settings_get_color("ui.accent", 0) == 0x445566u,
        "reset: the defaults underneath her overrides did not come back");

    /* ================================================================== 6 ==
     * THE DEFAULTS MUST NOT DEPEND ON /etc's MODE. A user is not root, so
     * whether they can read /etc/settings.conf depends on a mode nobody in
     * this file controls. The system layer is snapshotted while it can be
     * read, so a 0600 defaults file costs nothing -- without that snapshot
     * this machine loses every default the day somebody tightens the mode,
     * and would do it silently. */
    cur_uid = 0; cur_gid = 0;
    fs_put(SET_PATH, "ui.dark = 0\nui.accent = 0x778899\n", 0, 0, 0600);
    reboot_as(1001, 1001);
    okf(settings_get_color("ui.accent", 0) == 0x778899u,
        "a 0600 system defaults file left a logged-in user with NO defaults");

    printf("%s: %d checks, %d failures\n",
           fails ? "usersettings FAIL" : "usersettings ok", checks, fails);
    return fails ? 1 : 0;
}
