/* Host unit tests for VFS path resolution (c/fs/vfs_path.c).
 *
 * Path resolution is where filesystem bugs live, and none of them needs a
 * disk, a process or a boot to reproduce. Every case below has been a real CVE
 * in some real kernel or container runtime:
 *
 *   ".." at the root                  -- escaping the namespace upward
 *   ".." across a mount point         -- leaving a mount you were confined to
 *   a looping symlink chain           -- unbounded recursion, stack exhaustion
 *   a symlink to an absolute path     -- resolution restarting outside the tree
 *   a path exactly at the buffer edge -- off-by-one, and silent truncation
 *                                        producing a valid path to the WRONG
 *                                        file, which is worse than an error
 *   the empty path                    -- treated as "." and acting on the cwd
 *
 * The symlink table here is a plain array so that the walker is exercised
 * through exactly the callback the kernel gives it.
 */
#include <stdio.h>
#include <string.h>

#include "vfs_path.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void eqs(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want)) { failures++; printf("  FAIL: %s (got \"%s\", want \"%s\")\n", what, got, want); }
}

static void eqi(int got, int want, const char *what)
{
    checks++;
    if (got != want) { failures++; printf("  FAIL: %s (got %d, want %d)\n", what, got, want); }
}

/* --- a symlink table, and the visit callback the walker is given ---------- */

struct link { const char *at; const char *to; };
static struct link links[32];
static int nlinks;

/* Directories the callback refuses to be traversed, to prove that a `visit`
 * returning a negative aborts the walk rather than being ignored. */
static const char *barrier;

static void link_reset(void) { nlinks = 0; barrier = 0; }
static void link_add(const char *at, const char *to)
{ links[nlinks].at = at; links[nlinks].to = to; nlinks++; }

static int visit(void *ctx, const char *abs, int is_final, char *out, int max)
{
    (void)ctx;
    if (barrier && !is_final && !strcmp(abs, barrier)) return VFS_EACCES;
    for (int i = 0; i < nlinks; i++)
        if (!strcmp(links[i].at, abs)) {
            int n = (int)strlen(links[i].to);
            if (n + 1 > max) return VFS_ENAMETOOLONG;
            memcpy(out, links[i].to, (size_t)n + 1);
            return 1;
        }
    return 0;
}

static struct vfs_path_env env = { visit, 0, 0 };

/* Helpers: N = purely lexical, R = full resolution. */
static const char *N(const char *cwd, const char *in)
{
    static char out[VFS_PATH_MAX];
    int rc = vfs_path_norm(cwd, in, out, (int)sizeof out);
    if (rc < 0) { snprintf(out, sizeof out, "E%d", rc); }
    return out;
}
static const char *R(const char *cwd, const char *in, int follow_final)
{
    static char out[VFS_PATH_MAX];
    int rc = vfs_path_resolve(&env, cwd, in, out, (int)sizeof out, follow_final);
    if (rc < 0) { snprintf(out, sizeof out, "E%d", rc); }
    return out;
}

/* --------------------------------------------------------------------------
 * 1. The lexical rules
 * ------------------------------------------------------------------------ */
static void t_lexical(void)
{
    printf("lexical normalisation\n");
    eqs(N("/", "/"),               "/",        "root is root");
    eqs(N("/", "/a/b/c"),          "/a/b/c",   "already canonical");
    eqs(N("/", "//a///b//"),       "/a/b",     "repeated and trailing slashes");
    eqs(N("/", "/a/./b"),          "/a/b",     "a single dot is dropped");
    eqs(N("/", "/a/b/."),          "/a/b",     "a trailing dot is dropped");
    eqs(N("/", "/a/b/.."),         "/a",       "dotdot removes one component");
    eqs(N("/a/b", "c"),            "/a/b/c",   "relative joins the cwd");
    eqs(N("/a/b", "../c"),         "/a/c",     "relative dotdot against the cwd");
    eqs(N("/a/b", "./"),           "/a/b",     "dot-slash is the cwd");
    eqs(N(0,   "a"),               "/a",       "a NULL cwd is the root");
    eqs(N("",  "a"),               "/a",       "an empty cwd is the root");
    eqs(N("/", ".hidden"),         "/.hidden", "a leading dot is a name, not a dot component");
    eqs(N("/", "..."),             "/...",     "three dots is a name");
}

/* --------------------------------------------------------------------------
 * 2. ".." at the root -- the escape that must not happen
 * ------------------------------------------------------------------------ */
static void t_dotdot_at_root(void)
{
    printf("\"..\" at the root\n");
    eqs(N("/", ".."),                    "/", "dotdot at the root is the root");
    eqs(N("/", "../.."),                 "/", "twice is still the root");
    eqs(N("/", "../../../../../etc"),    "/etc", "an escape attempt lands back inside");
    eqs(N("/", "/a/../.."),              "/", "walking up past the root stops there");
    eqs(N("/a", ".."),                   "/", "dotdot from a top-level cwd");
    eqs(N("/a", "../.."),                "/", "and again");
    eqs(N("/", "/../a/../b"),            "/b", "escape attempts interleaved with names");
}

/* --------------------------------------------------------------------------
 * 3. ".." across a mount point
 *
 * A mount point is an ordinary directory to the walker; what matters is that
 * ".." out of one lands in the parent filesystem's namespace rather than being
 * clamped at the mount root. The mount-table test (vfs_mount_test) then proves
 * the resulting path is dispatched to the OTHER filesystem.
 * ------------------------------------------------------------------------ */
static void t_dotdot_across_mount(void)
{
    printf("\"..\" across a mount point\n");
    eqs(N("/mnt", ".."),          "/",        "dotdot leaves the mount");
    eqs(N("/mnt/sub", "../.."),   "/",        "two levels leaves the mount");
    eqs(N("/mnt", "../mnt/x"),    "/mnt/x",   "out and back in");
    eqs(N("/", "/mnt/../etc"),    "/etc",     "crossing out mid-path");
    /* The boundary test: a mount at /mnt must not swallow /mnttab. */
    ok(vfs_path_is_prefix("/mnt", "/mnt"),      "/mnt is a prefix of itself");
    ok(vfs_path_is_prefix("/mnt", "/mnt/x"),    "/mnt is a prefix of /mnt/x");
    ok(!vfs_path_is_prefix("/mnt", "/mnttab"),  "/mnt is NOT a prefix of /mnttab");
    ok(!vfs_path_is_prefix("/mnt", "/mn"),      "/mnt is not a prefix of a shorter path");
    ok(vfs_path_is_prefix("/", "/anything"),    "the root prefixes everything");
}

/* --------------------------------------------------------------------------
 * 4. The empty path
 * ------------------------------------------------------------------------ */
static void t_empty(void)
{
    printf("the empty path\n");
    eqi(vfs_path_norm("/a", "", (char[8]){0}, 8), VFS_ENOENT, "\"\" is ENOENT, not the cwd");
    eqi(vfs_path_norm("/a", 0,  (char[8]){0}, 8), VFS_ENOENT, "NULL is ENOENT");
    /* An empty path must not resolve to the cwd: unlink("") would then delete it. */
    eqs(N("/a/b", ""), "E-2", "\"\" does not silently become the cwd");
}

/* --------------------------------------------------------------------------
 * 5. Buffer limits -- the off-by-one and the silent truncation
 * ------------------------------------------------------------------------ */
static void t_buffer_limits(void)
{
    printf("buffer limits\n");
    char out[16];
    /* "/abcdefgh" is 9 chars + NUL = 10. */
    eqi(vfs_path_norm("/", "/abcdefgh", out, 10), 9, "exactly fits: 9 chars in a 10-byte buffer");
    eqs(out, "/abcdefgh", "and the content is right");

    eqi(vfs_path_norm("/", "/abcdefgh", out, 9), VFS_ENAMETOOLONG, "one byte short is ENAMETOOLONG");
    eqi(vfs_path_norm("/", "/abcdefgh", out, 1), VFS_ENAMETOOLONG, "a 1-byte buffer cannot hold even \"/\"");
    eqi(vfs_path_norm("/", "/", out, 2), 1, "\"/\" fits exactly in 2 bytes");

    /* The property that matters: a refused result is never a valid path to a
     * different file. Ask for a name that would truncate to an existing one. */
    char t[8];
    memset(t, 'Z', sizeof t);
    eqi(vfs_path_norm("/", "/etc/passwd", t, 5), VFS_ENAMETOOLONG,
        "/etc/passwd does not truncate to /etc");
    ok(t[0] == 0 || t[0] == 'Z', "the output buffer is not left holding a shorter valid path");

    /* A single component longer than the on-disk name limit. */
    char longname[VFS_NAME_MAX + 8];
    longname[0] = '/';
    memset(longname + 1, 'x', VFS_NAME_MAX + 1);
    longname[VFS_NAME_MAX + 2] = 0;
    char big[VFS_PATH_MAX];
    eqi(vfs_path_norm("/", longname, big, (int)sizeof big), VFS_ENAMETOOLONG,
        "a component longer than NAME_MAX is refused");

    /* Exactly NAME_MAX is accepted. */
    longname[VFS_NAME_MAX + 1] = 0;
    eqi(vfs_path_norm("/", longname, big, (int)sizeof big), VFS_NAME_MAX + 1,
        "a component of exactly NAME_MAX is accepted");
}

/* --------------------------------------------------------------------------
 * 6. Symbolic links
 * ------------------------------------------------------------------------ */
static void t_symlink_basic(void)
{
    printf("symbolic links\n");
    link_reset();
    link_add("/link", "target");            /* relative */
    link_add("/abs",  "/etc/passwd");       /* absolute */
    link_add("/d/l",  "../other");          /* relative with dotdot */

    eqs(R("/", "/link", 1),      "/target",      "a relative symlink resolves beside itself");
    eqs(R("/", "/link", 0),      "/link",        "and is NOT followed when follow_final=0");
    eqs(R("/", "/abs", 1),       "/etc/passwd",  "a symlink to an absolute path restarts at the root");
    eqs(R("/", "/d/l", 1),       "/other",       "a relative target with .. resolves against the link's dir");
    eqs(R("/", "/link/x", 1),    "/target/x",    "a symlink in the middle of a path");
    eqs(R("/", "/link/x", 0),    "/target/x",    "a non-final symlink is followed even with follow_final=0");
}

static void t_symlink_absolute_escape(void)
{
    printf("symlink to an absolute path\n");
    link_reset();
    link_add("/jail/escape", "/");
    link_add("/jail/up",     "/../../..");

    eqs(R("/", "/jail/escape", 1),      "/",     "a symlink to the root resolves to the root");
    eqs(R("/", "/jail/escape/etc", 1),  "/etc",  "and the remainder continues from there");
    eqs(R("/", "/jail/up", 1),          "/",     "an absolute target full of .. still lands at the root");
    eqs(R("/", "/jail/escape/..", 1),   "/",     "dotdot after landing at the root stays at the root");
}

static void t_symlink_dotdot_after(void)
{
    printf("\"..\" after a symlink follows the RESOLVED path\n");
    link_reset();
    link_add("/a/link", "/x/y");
    /* Lexically /a/link/.. is /a. After resolution the link is /x/y, so ".."
     * must be /x. Getting this wrong is how a sandbox is escaped. */
    eqs(R("/", "/a/link/..", 1), "/x", "dotdot applies to where the link pointed");
    eqs(N("/", "/a/link/.."),    "/a", "and the purely lexical answer differs, as it should");
}

static void t_symlink_loops(void)
{
    printf("looping symlink chains\n");
    link_reset();
    link_add("/self", "self");                  /* points at itself */
    eqs(R("/", "/self", 1), "E-40", "a self-referential symlink is ELOOP, not a hang");

    link_reset();
    link_add("/a", "b"); link_add("/b", "a");   /* two-cycle */
    eqs(R("/", "/a", 1), "E-40", "a two-link cycle is ELOOP");

    link_reset();
    link_add("/x", "/y/z"); link_add("/y", "/x"); /* cycle through a directory */
    eqs(R("/", "/x", 1), "E-40", "a cycle through an intermediate component is ELOOP");

    /* A long but FINITE chain must still resolve: the bound is on depth, not on
     * having any links at all. */
    link_reset();
    link_add("/l1", "l2"); link_add("/l2", "l3"); link_add("/l3", "l4");
    link_add("/l4", "l5"); link_add("/l5", "end");
    eqs(R("/", "/l1", 1), "/end", "a 5-deep chain resolves");

    /* One link past the bound. */
    link_reset();
    {
        static char at[VFS_SYMLOOP_MAX + 4][8], to[VFS_SYMLOOP_MAX + 4][8];
        for (int i = 0; i <= VFS_SYMLOOP_MAX; i++) {
            snprintf(at[i], sizeof at[i], "/k%d", i);
            snprintf(to[i], sizeof to[i], "k%d", i + 1);
            link_add(at[i], to[i]);
        }
    }
    eqs(R("/", "/k0", 1), "E-40", "a chain one longer than the bound is ELOOP");
}

static void t_symlink_edge(void)
{
    printf("symlink edge cases\n");
    link_reset();
    link_add("/empty", "");
    eqs(R("/", "/empty", 1), "E-2", "a symlink with an empty target is ENOENT");

    link_reset();
    link_add("/dot", ".");
    eqs(R("/", "/dot", 1), "/", "a symlink to \".\" resolves to its own directory");

    link_reset();
    link_add("/mnt/link", "/mnt/../etc");
    eqs(R("/", "/mnt/link", 1), "/etc", "a symlink target that crosses a mount point");
}

/* --------------------------------------------------------------------------
 * 7. The callback may refuse -- which is how search permission is enforced
 * ------------------------------------------------------------------------ */
static void t_visit_refusal(void)
{
    printf("a refusing visit aborts the walk\n");
    link_reset();
    barrier = "/private";
    eqs(R("/", "/private/secret", 1), "E-13", "a barrier directory aborts with EACCES");
    eqs(R("/", "/private", 1),        "/private",
        "the barrier itself is reachable (a final component is not a traversal)");
    eqs(R("/", "/public/ok", 1),      "/public/ok", "an unrelated path is unaffected");
    barrier = 0;
}

int main(void)
{
    t_lexical();
    t_dotdot_at_root();
    t_dotdot_across_mount();
    t_empty();
    t_buffer_limits();
    t_symlink_basic();
    t_symlink_absolute_escape();
    t_symlink_dotdot_after();
    t_symlink_loops();
    t_symlink_edge();
    t_visit_refusal();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
