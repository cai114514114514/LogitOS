/* Path resolution. See vfs_path.h for the rules this file implements. */

#include "vfs_path.h"

static int p_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* Append `n` bytes of `s` to buf[*len], keeping a NUL. Returns 0, or
 * VFS_ENAMETOOLONG -- never a truncated result. */
static int p_add(char *buf, int *len, int max, const char *s, int n)
{
    if (*len + n + 1 > max) return VFS_ENAMETOOLONG;
    for (int i = 0; i < n; i++) buf[*len + i] = s[i];
    *len += n;
    buf[*len] = 0;
    return 0;
}

/* Drop the last component of an absolute path already in canonical form.
 * "/a/b" -> "/a", "/a" -> "" (the root, represented as an empty prefix). */
static void p_up(char *cur, int *len)
{
    while (*len > 0 && cur[*len - 1] != '/') (*len)--;
    if (*len > 0) (*len)--;          /* eat the '/' itself */
    cur[*len] = 0;
}

int vfs_path_is_prefix(const char *pfx, const char *path)
{
    if (!pfx || !path) return 0;
    if (pfx[0] == '/' && pfx[1] == 0) return path[0] == '/';   /* "/" prefixes all */
    int i = 0;
    for (; pfx[i]; i++) if (pfx[i] != path[i]) return 0;
    return path[i] == 0 || path[i] == '/';
}

int vfs_path_split(const char *abs, char *dir, int dirmax, char *name, int namemax)
{
    if (!abs || abs[0] != '/') return VFS_EINVAL;
    int n = p_len(abs);
    if (n == 1) return VFS_EINVAL;                 /* "/" has no final component */
    int slash = 0;
    for (int i = 0; i < n; i++) if (abs[i] == '/') slash = i;
    int dlen = slash ? slash : 1;                  /* "/x" -> parent is "/" */
    if (dir) {
        if (dlen + 1 > dirmax) return VFS_ENAMETOOLONG;
        for (int i = 0; i < dlen; i++) dir[i] = abs[i];
        dir[dlen] = 0;
    }
    int nlen = n - slash - 1;
    if (name) {
        if (nlen + 1 > namemax) return VFS_ENAMETOOLONG;
        for (int i = 0; i < nlen; i++) name[i] = abs[slash + 1 + i];
        name[nlen] = 0;
    }
    return nlen;
}

/* The one walker. `env == NULL` makes it purely lexical, which is exactly
 * vfs_path_norm; with an env it additionally expands symlinks.
 *
 * The loop keeps two strings:
 *   cur  -- what has been RESOLVED so far ("" means the root). "." and ".."
 *           are applied here, which is why ".." after a symlink lands where
 *           the symlink pointed rather than where its name sat.
 *   rest -- what is left to process, always with a leading '/'.
 * A symlink splices its target onto the FRONT of rest, so the target's own
 * components (which may themselves be symlinks, or "..") go through the same
 * loop rather than through a second code path. */
static int walk(const struct vfs_path_env *env, const char *cwd, const char *in,
                char *out, int max, int follow_final)
{
    if (!out || max <= 0) return VFS_EINVAL;
    out[0] = 0;
    if (!in || !in[0]) return VFS_ENOENT;          /* the empty path is not the cwd */

    char rest[VFS_PATH_MAX * 2];
    int  rlen = 0;
    if (in[0] != '/') {
        const char *c = (cwd && cwd[0]) ? cwd : "/";
        if (c[0] != '/') return VFS_EINVAL;        /* a relative cwd is a bug, not a path */
        if (p_add(rest, &rlen, (int)sizeof rest, c, p_len(c)) < 0) return VFS_ENAMETOOLONG;
        if (rlen && rest[rlen - 1] == '/') rlen--, rest[rlen] = 0;
    }
    if (p_add(rest, &rlen, (int)sizeof rest, "/", 1) < 0) return VFS_ENAMETOOLONG;
    if (p_add(rest, &rlen, (int)sizeof rest, in, p_len(in)) < 0) return VFS_ENAMETOOLONG;

    char cur[VFS_PATH_MAX];
    int  clen = 0;
    cur[0] = 0;

    int links = 0;
    int maxlinks = (env && env->max_links > 0) ? env->max_links : VFS_SYMLOOP_MAX;

    int ri = 0;
    for (;;) {
        while (rest[ri] == '/') ri++;
        if (!rest[ri]) break;
        const char *comp = &rest[ri];
        int clen_comp = 0;
        while (rest[ri] && rest[ri] != '/') { ri++; clen_comp++; }
        /* Is anything left after this component? Decides "final component". */
        int j = ri; while (rest[j] == '/') j++;
        int is_final = (rest[j] == 0);

        if (clen_comp == 1 && comp[0] == '.') continue;
        if (clen_comp == 2 && comp[0] == '.' && comp[1] == '.') { p_up(cur, &clen); continue; }
        if (clen_comp > VFS_NAME_MAX) return VFS_ENAMETOOLONG;

        /* Tentatively extend the resolved prefix. */
        int save = clen;
        if (p_add(cur, &clen, (int)sizeof cur, "/", 1) < 0) return VFS_ENAMETOOLONG;
        if (p_add(cur, &clen, (int)sizeof cur, comp, clen_comp) < 0) return VFS_ENAMETOOLONG;

        if (env && env->visit) {
            char tgt[VFS_PATH_MAX];
            tgt[0] = 0;
            int k = env->visit(env->ctx, cur, is_final, tgt, (int)sizeof tgt);
            if (k < 0) return k;
            if (k == 1 && is_final && !follow_final) k = 0;   /* lstat semantics */
            if (k == 1) {
                if (++links > maxlinks) return VFS_ELOOP;
                if (!tgt[0]) return VFS_ENOENT;    /* an empty target names nothing */
                /* new rest = target + (what is left of rest); if the target is
                 * absolute the resolved prefix restarts at the root. */
                char nrest[VFS_PATH_MAX * 2];
                int nlen = 0;
                if (p_add(nrest, &nlen, (int)sizeof nrest, "/", 1) < 0) return VFS_ENAMETOOLONG;
                if (p_add(nrest, &nlen, (int)sizeof nrest, tgt, p_len(tgt)) < 0) return VFS_ENAMETOOLONG;
                if (rest[ri]) {
                    if (p_add(nrest, &nlen, (int)sizeof nrest, "/", 1) < 0) return VFS_ENAMETOOLONG;
                    if (p_add(nrest, &nlen, (int)sizeof nrest, &rest[ri], p_len(&rest[ri])) < 0)
                        return VFS_ENAMETOOLONG;
                }
                for (int t = 0; t <= nlen; t++) rest[t] = nrest[t];
                rlen = nlen; ri = 0;
                clen = (tgt[0] == '/') ? 0 : save;  /* relative target: back to the link's dir */
                cur[clen] = 0;
                continue;
            }
        }
    }
    (void)rlen;

    if (clen == 0) { if (max < 2) return VFS_ENAMETOOLONG; out[0] = '/'; out[1] = 0; return 1; }
    if (clen + 1 > max) return VFS_ENAMETOOLONG;
    for (int i = 0; i <= clen; i++) out[i] = cur[i];
    return clen;
}

int vfs_path_norm(const char *cwd, const char *in, char *out, int max)
{
    return walk(0, cwd, in, out, max, 0);
}

int vfs_path_resolve(const struct vfs_path_env *env, const char *cwd,
                     const char *in, char *out, int max, int follow_final)
{
    return walk(env, cwd, in, out, max, follow_final);
}
