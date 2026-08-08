#include "clib.h"
#include "hidden.h"

/* ls [-a] [dir] -- list a directory (default: cwd). Directories get a '/'.
 *
 * -a shows dot-prefixed entries. The system directories are NOT filtered here:
 * see c/apps/hidden.h -- a shell that hides /bin is lying to the person who
 * typed the command. */
int main(int argc, char **argv)
{
    char path[128];
    int all = 0, have_path = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !have_path) {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'a') all = 1;
                else { errs("ls: unknown flag: "); errs(argv[i]); errs("\n"); return 1; }
            }
        } else { c_strcpy(path, argv[i], sizeof path); have_path = 1; }
    }
    if (!have_path) sys_getcwd(path, sizeof path);

    int n = dir_count(path);
    if (n < 0) { errs("ls: no such directory: "); errs(path); errs("\n"); return 1; }
    for (int i = 0; i < n; i++) {
        char nm[64]; nm[0] = 0;
        int r = dir_name(path, i, nm);
        if (r == -1 || !nm[0]) continue;
        if (!all && hidden_dotfile(nm)) continue;
        outs(nm);
        if (r == -2) outc('/');
        outc('\n');
    }
    return 0;
}
