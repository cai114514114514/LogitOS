/* THE ACCOUNT STORE, THE PASSWORD CHECK, AND THE REFUSAL.
 *
 * Three claims are made by the M32 identity work and this file tests all
 * three, in the order they have to hold:
 *
 *   1. /etc/passwd parses and round-trips. A store is a file that a program
 *      writes and another program reads, and the two have to be the same
 *      parser -- so both this test and /bin/login include
 *      c/apps/coreutils/accounts.h rather than each having their own.
 *   2. A WRONG PASSWORD IS REFUSED. This is the claim the negative control
 *      attacks, and it is the reason the check lives in a shared header
 *      instead of inside login.c: `make test-login-negctl` builds this same
 *      file with -DLOGIN_NEGCTL_ACCEPT_ANY, in which every password is
 *      correct, and REQUIRES the run to fail.
 *   3. THE FILESYSTEM REFUSES THINGS IT USED TO PERMIT. An identity that
 *      changes no outcome is decoration, so the last section drives the real
 *      vmeta_permission() -- the same function c/fs/vfs.c calls on every path
 *      walk -- with the credentials login now hands out, and asserts what a
 *      non-root uid can no longer do.
 *
 * WHAT THIS TEST CANNOT SEE, and why the boot harness exists: everything here
 * runs in one process against RAM. It cannot show that a mode survives a
 * reboot, that the store is still 0600 after a power cut, or that the shell
 * the machine actually spawns is not root. tests/boot/run-login-test.sh does
 * that across four real boots with no -snapshot. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "crypto.h"         /* pbkdf2 / pwhash_make -- the real primitives */
#include "logit_abi.h"      /* ID_NGROUPS_MAX */
#include "accounts.h"
#include "vfs_meta.h"
#include "vfs_path.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

/* c/fs/vfs_meta.c asks the process table whether the caller is in a
 * supplementary group, through a weak symbol. There is no process table here,
 * so this test IS the process table -- which is also the only way to drive
 * that branch deterministically. */
static uint32_t supp[ID_NGROUPS_MAX];
static int      nsupp;
int vfs_cred_ingroup(uint32_t gid)
{ for (int i = 0; i < nsupp; i++) if (supp[i] == gid) return 1; return 0; }

static void randbytes(uint8_t *p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + 3); }

/* A deliberately cheap cost. The point of this file is the LOGIC around the
 * hash, and 120000 iterations x a dozen checks is a minute of CI for no extra
 * coverage -- tests/unit/pwhash_test.c already runs the published vectors at
 * the real counts. The count travels inside each record, which is exactly the
 * property being relied on here. */
#define T_ITERS 2000u

int main(void)
{
    /* ---- 1. the store parses ------------------------------------------- */
    {
        struct account a;
        const char *L = "alice:$pbkdf2-sha256$120000$AAAA$BBBB:1000:1001:/home/alice:/bin/sh";
        ok(acct_parse_line(L, (int)strlen(L), &a) == 1, "a well-formed row parses");
        ok(strcmp(a.name, "alice") == 0, "  name");
        ok(strcmp(a.hash, "$pbkdf2-sha256$120000$AAAA$BBBB") == 0, "  hash record, colons and all");
        ok(a.uid == 1000 && a.gid == 1001, "  uid and gid are separate fields");
        ok(strcmp(a.home, "/home/alice") == 0, "  home");
        ok(strcmp(a.shell, "/bin/sh") == 0, "  shell");

        ok(acct_parse_line("# a comment", 11, &a) == 0, "a comment is not an account");
        ok(acct_parse_line("", 0, &a) == 0, "a blank line is not an account");
        ok(acct_parse_line("alice:x:1:2:/h", 14, &a) == 0, "five fields is malformed, not an account");
        ok(acct_parse_line("alice:x:not-a-number:2:/h:/s", 28, &a) == 0,
           "a non-numeric uid is malformed -- NOT silently 0, which would be root");
        ok(acct_parse_line("alice:x:1000:2:/h:/s:extra", 26, &a) == 0,
           "a seventh field is malformed (a smuggled colon must not shift the uid)");
    }

    /* A corrupt row costs one account, not the store. */
    {
        const char *store =
            "# LogitOS accounts\n"
            "alice:$pbkdf2-sha256$1$AA$BB:1000:1000:/home/alice:/bin/sh\n"
            "GARBAGE WITHOUT COLONS\n"
            "\n"
            "bob:$pbkdf2-sha256$1$CC$DD:1001:1001:/home/bob:/bin/sh\n";
        int len = (int)strlen(store);
        struct account a;
        ok(acct_find(store, len, "alice", &a) && a.uid == 1000, "alice is found before the garbage");
        ok(acct_find(store, len, "bob", &a) && a.uid == 1001, "bob is found AFTER the garbage");
        ok(!acct_find(store, len, "carol", &a), "a name that is not there is not found");
        ok(!acct_find(store, len, "GARBAGE", &a), "the garbage line is not an account");
        ok(acct_max_uid(store, len) == 1001, "max uid skips the garbage");
    }

    /* ---- append round-trips through the parser -------------------------- */
    {
        char buf[ACCT_MAX + 1];
        int len = 0;
        struct account w = { "carol", "$pbkdf2-sha256$1$EE$FF", 1002, 1002, "/home/carol", "/bin/sh" };
        len = acct_append(buf, len, ACCT_MAX, &w);
        ok(len > 0, "append writes a row");
        struct account r;
        ok(acct_find(buf, len, "carol", &r), "  and the parser reads it back");
        ok(r.uid == 1002 && strcmp(r.home, "/home/carol") == 0 && strcmp(r.hash, w.hash) == 0,
           "  every field survives the round trip");

        struct account w2 = { "dave", "$pbkdf2-sha256$1$GG$HH", 1003, 1003, "/home/dave", "/bin/sh" };
        len = acct_append(buf, len, ACCT_MAX, &w2);
        ok(acct_find(buf, len, "carol", &r) && acct_find(buf, len, "dave", &r),
           "a second append does not eat the first row");
        ok(acct_max_uid(buf, len) == 1003, "max uid tracks the newest row");

        /* Full store: refuse rather than truncate. A truncated row is a row
         * that parses as something else. */
        char tiny[40];
        ok(acct_append(tiny, 0, (int)sizeof tiny, &w) < 0, "a store too small is refused, not truncated");
    }

    /* ---- 2. the password check ------------------------------------------
     * THE SECTION THE NEGATIVE CONTROL MUST BREAK. */
    {
        struct account a;
        memset(&a, 0, sizeof a);
        strcpy(a.name, "alice");
        strcpy(a.home, "/home/alice"); strcpy(a.shell, "/bin/sh");
        a.uid = a.gid = 1000;
        int n = pwhash_make(a.hash, ACCT_HASH, "correct horse battery staple", T_ITERS, randbytes);
        ok(n > 0, "a record is produced");
        ok(strncmp(a.hash, "$pbkdf2-sha256$", 15) == 0, "  and it is the format the store expects");

        ok(acct_check_password(&a, "correct horse battery staple") == 1,
           "the right password is accepted");
        ok(acct_check_password(&a, "correct horse battery stapl") == 0,
           "REFUSAL: a password one character short is rejected");
        ok(acct_check_password(&a, "Correct horse battery staple") == 0,
           "REFUSAL: a password differing in case is rejected");
        ok(acct_check_password(&a, "") == 0,
           "REFUSAL: the empty password is rejected");
        ok(acct_check_password(&a, "correct horse battery staple ") == 0,
           "REFUSAL: a trailing space is a different password");

        /* Tamper with the stored digest. This is the stolen-disk case: an
         * attacker who can write the store but cannot compute a preimage. */
        struct account t = a;
        t.hash[(int)strlen(t.hash) - 2] ^= 1;
        ok(acct_check_password(&t, "correct horse battery staple") == 0,
           "REFUSAL: a record with one bit flipped in the digest no longer verifies");

        /* And the salt: two accounts with the SAME password must not share a
         * digest, or the store leaks equality between users to anyone who can
         * read it. Driven through pbkdf2() directly because pwhash_make picks
         * its own salt and the whole point here is to control both. */
        {
            uint8_t s1[16], s2[16], dka[32], dkb[32];
            randbytes(s1, 16);
            for (int i = 0; i < 16; i++) s2[i] = (uint8_t)(200 - i);
            pbkdf2(32, (const uint8_t *)"same", 4, s1, 16, T_ITERS, dka, 32);
            pbkdf2(32, (const uint8_t *)"same", 4, s2, 16, T_ITERS, dkb, 32);
            ok(memcmp(dka, dkb, 32) != 0,
               "two accounts with the same password get different digests (the salt is used)");
        }

        /* The iteration count lives in the record, which is what makes
         * ACCT_ITERS changeable without a flag day. Prove it: a record made at
         * one cost still verifies when the compiled-in constant is a different
         * number entirely. */
        struct account oldrec;
        memset(&oldrec, 0, sizeof oldrec);
        pwhash_make(oldrec.hash, ACCT_HASH, "hunter2", 977u, randbytes);
        ok(977u != ACCT_ITERS, "  (the test cost differs from the shipped one)");
        ok(acct_check_password(&oldrec, "hunter2") == 1,
           "a record enrolled at a DIFFERENT cost still verifies -- raising ACCT_ITERS is not a flag day");
        ok(acct_check_password(&oldrec, "hunter3") == 0,
           "REFUSAL: ...and still refuses the wrong password at that cost");
    }

    /* ---- 3. what the filesystem now refuses -----------------------------
     * The real vmeta_permission(), the same one every path walk goes through.
     * `root` here is the credential every process had before M32 -- so a check
     * that passes for root and fails for `user` is precisely an outcome that
     * CHANGED because identity now exists. */
    {
        struct vcred root = { 0, 0 };
        struct vcred user = { 1000, 1000 };
        struct vcred other = { 1001, 1001 };

        /* /etc/passwd as /bin/login leaves it. */
        struct vattr store; vattr_clear(&store);
        store.mode = 0600; store.uid = 0; store.gid = 0; store.type = VT_REG;

        ok(vmeta_permission(&store, &root, MAY_READ) == 0,
           "root reads the account store");
        ok(vmeta_permission(&store, &user, MAY_READ) == VFS_EACCES,
           "REFUSAL: a logged-in user cannot read /etc/passwd -- this used to be allowed");
        ok(vmeta_permission(&store, &user, MAY_WRITE) == VFS_EACCES,
           "REFUSAL: nor write it, so the hash cannot be replaced");

        /* The root directory: 0755 root:root, the default for anything mkfs
         * packed and nobody chmod'd. */
        struct vattr slash; vattr_clear(&slash);
        slash.mode = 0755; slash.uid = 0; slash.gid = 0; slash.type = VT_DIR;
        ok(vmeta_permission(&slash, &user, MAY_WRITE | MAY_EXEC) == VFS_EACCES,
           "REFUSAL: a user cannot create a file in / -- this used to be allowed");
        ok(vmeta_permission(&slash, &user, MAY_READ | MAY_EXEC) == 0,
           "...but can still list and traverse it, which is what 0755 says");

        /* A home the user owns. */
        struct vattr home; vattr_clear(&home);
        home.mode = 0700; home.uid = 1000; home.gid = 1000; home.type = VT_DIR;
        ok(vmeta_permission(&home, &user, MAY_READ | MAY_WRITE | MAY_EXEC) == 0,
           "a user has full run of their own home");
        ok(vmeta_permission(&home, &other, MAY_READ) == VFS_EACCES,
           "REFUSAL: a SECOND user cannot read the first one's home");
        ok(vmeta_permission(&home, &other, MAY_EXEC) == VFS_EACCES,
           "REFUSAL: ...nor traverse into it, which is what actually stops a path walk");
        ok(vmeta_permission(&home, &root, MAY_READ | MAY_WRITE) == 0,
           "root still reaches it -- dropping privilege is one-way, not a sandbox");

        /* root does NOT get to execute something with no x bit anywhere. */
        struct vattr notes; vattr_clear(&notes);
        notes.mode = 0644; notes.uid = 0; notes.gid = 0; notes.type = VT_REG;
        ok(vmeta_permission(&notes, &root, MAY_EXEC) == VFS_EACCES,
           "REFUSAL: even root does not execute a file with no execute bit");

        /* Group access, through the credential's PRIMARY gid. */
        struct vattr shared; vattr_clear(&shared);
        shared.mode = 0640; shared.uid = 0; shared.gid = 1000; shared.type = VT_REG;
        ok(vmeta_permission(&shared, &user, MAY_READ) == 0,
           "a 0640 group-owned file is readable by its group");
        ok(vmeta_permission(&shared, &user, MAY_WRITE) == VFS_EACCES,
           "REFUSAL: ...and not writable, because 0640 says r-- for the group");
        ok(vmeta_permission(&shared, &other, MAY_READ) == VFS_EACCES,
           "REFUSAL: and not readable by someone in neither the owner nor the group");

        /* SUPPLEMENTARY groups: the same file, the same `other` credential,
         * with membership added through SYS_SETGROUPS. The only difference
         * between these two assertions is the group list, which is what makes
         * this a test of the group list. */
        nsupp = 0;
        ok(vmeta_permission(&shared, &other, MAY_READ) == VFS_EACCES,
           "REFUSAL: with an empty supplementary set, still refused");
        supp[0] = 1000; nsupp = 1;
        ok(vmeta_permission(&shared, &other, MAY_READ) == 0,
           "a supplementary group grants exactly what the primary one would");
        ok(vmeta_permission(&shared, &other, MAY_WRITE) == VFS_EACCES,
           "REFUSAL: ...and no more than that -- 0640 is still r-- for the group");
        supp[0] = 4242; nsupp = 1;
        ok(vmeta_permission(&shared, &other, MAY_READ) == VFS_EACCES,
           "REFUSAL: membership of some OTHER group grants nothing");
        nsupp = 0;
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
