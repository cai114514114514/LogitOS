#include "clib.h"

/* cp [-r] SRC DST -- copy a file (or, with -r, a directory tree).
 * Files copy via a plain fd read/write loop; directories recurse using the
 * path-scoped dir_count/dir_name listing + make_dir + clib.h path_join. */

/* 1 if `a` equals `b` or is an ancestor directory of `b` (prefix on a '/'
 * boundary) -- guards against copying a directory into its own subtree. */
static int path_under(const char *a, const char *b)
{
    int la = c_strlen(a);
    while (la > 1 && a[la - 1] == '/') la--;
    int lb = c_strlen(b);
    while (lb > 1 && b[lb - 1] == '/') lb--;
    if (lb < la) return 0;
    if (c_strncmp(a, b, la) != 0) return 0;
    return lb == la || b[la] == '/';
}

#define CP_DEPTH_MAX 32                 /* ring-3 stack guard for deep directory trees */

static int copy_file(const char *src, const char *dst)
{
    int in = sys_open(src, O_RDONLY);
    if (in < 0) { errs("cp: cannot open "); errs(src); errs("\n"); return 1; }
    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { errs("cp: cannot create "); errs(dst); errs("\n"); sys_close(in); return 1; }
    char buf[4096];
    int r, rc = 0;
    while ((r = sys_read(in, buf, sizeof buf)) > 0)
        if (sys_write(out, buf, r) != r) { errs("cp: write error\n"); rc = 1; break; }
    sys_close(in);
    sys_close(out);
    return rc;
}

static int do_cp(const char *src, const char *dst, int recursive, int depth)
{
    if (depth > CP_DEPTH_MAX) { errs("cp: directory tree too deep\n"); return 1; }
    int n = dir_count(src);
    if (n >= 0) {                                   /* src is a directory */
        if (!recursive) { errs("cp: -r required for directory\n"); return 1; }
        if (path_under(src, dst)) { errs("cp: cannot copy a directory into itself\n"); return 1; }
        make_dir(dst);
        int rc = 0;
        for (int i = 0; i < n; i++) {
            char nm[64], cs[128], cd[128];
            if (dir_name(src, i, nm) == -1) continue;
            path_join(cs, src, nm, sizeof cs);
            path_join(cd, dst, nm, sizeof cd);
            if (do_cp(cs, cd, 1, depth + 1)) rc = 1;
        }
        return rc;
    }
    return copy_file(src, dst);
}

int main(int argc, char **argv)
{
    int recursive = 0, i = 1;
    if (i < argc && c_streq(argv[i], "-r")) { recursive = 1; i++; }
    if (argc - i != 2) { errs("usage: cp [-r] <src> <dst>\n"); return 1; }
    return do_cp(argv[i], argv[i + 1], recursive, 0);
}
