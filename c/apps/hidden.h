#ifndef LOGIT_HIDDEN_H
#define LOGIT_HIDDEN_H

/* Which directory entries a browser SHOWS. Presentation only.
 *
 * This is not a permission check and must never be mistaken for one. The
 * kernel's listing syscalls (SYS_DIR_COUNT / SYS_DIR_NAME) return everything,
 * and they should: an entry that is hidden is still openable by path, still
 * copied when its parent is copied, and still deleted when its parent is
 * deleted. Access control lives in c/fs/vfs_cred.c, where it is enforced on
 * every vfs_* path against the caller's uid/gid. Hiding a name from a listing
 * protects nobody; it only stops the user tripping over the machine's
 * furniture. Keeping the two mechanisms apart is the point of this header --
 * a filter that ran in the kernel would make `cd /bin` work while `ls /` denied
 * /bin existed, which is worse than showing everything.
 *
 * TWO RULES, and the callers do not apply the same pair:
 *
 *   hidden_dotfile()  a leading '.', the Unix convention. Universal: every
 *                     browser honours it, and every browser offers a way past
 *                     it (`ls -a`, Ctrl+H in Files).
 *
 *   hidden_system()   the machine's own directories at the root. These CANNOT
 *                     use the dot convention -- /bin is on the shell's PATH,
 *                     /fonts is opened by name at boot by text_init() -- so
 *                     they are named here instead. This is the macOS split:
 *                     the Terminal shows /bin because a shell without /bin is
 *                     a lie; the Finder does not, because a person looking for
 *                     their documents did not ask about the linker.
 *
 * `ls` applies the first rule only. Files applies both. */

static inline int lh_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* True for a dot-prefixed name. "." and ".." are not hidden by this rule --
 * they are structure, and a caller that shows them wants them deliberately. */
static inline int hidden_dotfile(const char *name)
{
    if (name[0] != '.') return 0;
    if (!name[1]) return 0;                          /* "."  */
    if (name[1] == '.' && !name[2]) return 0;        /* ".." */
    return 1;
}

/* True for one of the machine's own entries. `dir` is the directory being
 * listed; only the ROOT is filtered, because only the root mixes the system's
 * furniture in with the user's files. Inside /bin everything is visible again:
 * someone who navigated there asked to be there. */
static inline int hidden_system(const char *dir, const char *name, int is_dir)
{
    if (!lh_streq(dir, "/")) return 0;

    if (is_dir) {
        /* Four groups, and the reason differs by group.
         *
         *   the machine's own      bin, sbin, usr, etc, fonts, licenses, ime,
         *                          pkg -- furniture. /etc and /sbin were as
         *                          visible as /docs until this line named them,
         *                          which is why the root read as a developer's
         *                          working directory rather than a place to
         *                          keep files.
         *
         *   on-device test corpora jssem, jsperf, jscallee -- packed by
         *                          tests/{jssem,jsperf}.mk into the $(DISK)
         *                          recipe so the guest gates have their inputs.
         *                          They are HIDDEN rather than removed: the
         *                          gates read them by absolute path, and a
         *                          corpus a harness needs is not junk just
         *                          because a person should not trip over it.
         *
         *   runtime and app state  proc, run, state, browser -- not packed by
         *                          mkfs at all, which is why a reading of the
         *                          $(DISK) recipe misses them: /proc is
         *                          synthesised, /run and /state are created at
         *                          boot, /browser is a --preserve root carried
         *                          across repacks. They only appear on a
         *                          machine that has BOOTED, which is why the
         *                          first version of this list -- derived from
         *                          the Makefile -- did not contain one of them.
         *                          Check this list against a SCREENSHOT.
         *
         * What is deliberately still visible: docs, media, www -- content. If
         * this list ever grows to cover those too, the question to ask is
         * whether the root still has a reason to exist. */
        static const char *sysdir[] = {
            "bin", "sbin", "usr", "etc", "fonts", "licenses", "ime", "pkg",
            "jssem", "jsperf", "jscallee",
            "proc", "run", "state", "browser",
        };
        for (unsigned i = 0; i < sizeof sysdir / sizeof sysdir[0]; i++)
            if (lh_streq(name, sysdir[i])) return 1;
        return 0;
    }

    /* The .aex bundles. The Dock is what launches an application; these are the
     * raw images it launches, and 22 of them sitting in the root is the single
     * biggest reason the root does not look like a place a person keeps files.
     * One `return 0` here brings them back if that judgement is wrong. */
    int n = 0; while (name[n]) n++;
    if (n > 4 && lh_streq(name + n - 4, ".aex")) return 1;
    return 0;
}

#endif /* LOGIT_HIDDEN_H */
