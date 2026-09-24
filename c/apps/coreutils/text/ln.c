/* ln [-sf] TARGET LINK      ln [-sf] TARGET... DIRECTORY
 *
 * A hard link (link(2)) or, with -s, a symbolic one (symlink(2)); -f removes
 * an existing LINK first. Into a directory, each link takes the target's last
 * path component as its name. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    int o_s = 0, o_f = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 's') o_s = 1; else if (*f == 'f') o_f = 1;
            else { fprintf(stderr, "ln: invalid option -- '%c'\n", *f); return 1; }
        }
    }
    int n = argc - i;
    if (n < 1) { fputs("usage: ln [-sf] TARGET LINK | ln [-sf] TARGET... DIRECTORY\n", stderr); return 1; }
    const char *last = argv[argc - 1];
    struct stat st;
    int todir = n >= 2 && stat(last, &st) == 0 && S_ISDIR(st.st_mode);
    if (n > 2 && !todir) { fprintf(stderr, "ln: target '%s' is not a directory\n", last); return 1; }
    int rc = 0, ntargets = n == 1 ? 1 : n - 1;
    for (int k = 0; k < ntargets; k++) {
        const char *target = argv[i + k];
        char link_[4096];
        if (n == 1 || todir) {
            const char *base = strrchr(target, '/');
            base = base ? base + 1 : target;
            if (n == 1) snprintf(link_, sizeof link_, "%s", base);
            else snprintf(link_, sizeof link_, "%s/%s", last, base);
        } else snprintf(link_, sizeof link_, "%s", last);
        if (o_f) unlink(link_);
        if ((o_s ? symlink(target, link_) : link(target, link_)) < 0) {
            fprintf(stderr, "ln: failed to create %s link '%s'\n", o_s ? "symbolic" : "hard", link_);
            rc = 1;
        }
    }
    return rc;
}
