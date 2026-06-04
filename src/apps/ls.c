#include "clib.h"

/* ls [dir] -- list a directory (default: cwd). Directories get a trailing '/'. */
int main(int argc, char **argv)
{
    char path[128];
    if (argc > 1) c_strcpy(path, argv[1], sizeof path);
    else sys_getcwd(path, sizeof path);

    int n = dir_count(path);
    if (n < 0) { errs("ls: no such directory: "); errs(path); errs("\n"); return 1; }
    for (int i = 0; i < n; i++) {
        char nm[64];
        int r = dir_name(path, i, nm);
        outs(nm);
        if (r == -2) outc('/');
        outc('\n');
    }
    return 0;
}
