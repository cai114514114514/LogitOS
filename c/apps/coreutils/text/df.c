/* df [-h] [-i] [PATH...] -- how big each filesystem is and how much is free.
 *
 * With no PATH, every mounted filesystem: the mount table is read from
 * /dev/vfsmounts ("name mountpoint" per line, c/fs/vfs/vfs.c
 * vfs_mounts_render). A filesystem that cannot report its capacity is listed
 * with dashes rather than left out, so the table never pretends a mount is not
 * there. Numbers come from statvfs(), which is SYS_STATVFS.
 *
 * Output is POSIX `df -k`'s shape (1024-byte blocks), or -h's human sizes. */
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

static int human, inodes;

static void size_str(char *out, int n, unsigned long long bytes)
{
    if (!human) { snprintf(out, (size_t)n, "%llu", bytes / 1024); return; }
    const char *u = "BKMGTP";
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && u[i + 1]) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, (size_t)n, "%lluB", bytes);
    else if (v < 10.0) snprintf(out, (size_t)n, "%.1f%c", v, u[i]);
    else snprintf(out, (size_t)n, "%.0f%c", v, u[i]);
}

static int show(const char *fsname, const char *mnt, const char *path)
{
    struct statvfs s;
    char a[24], b[24], c[24];
    if (statvfs(path, &s) < 0) {
        printf("%-12s %10s %10s %10s %4s %s\n", fsname, "-", "-", "-", "-", mnt);
        return 1;
    }
    if (inodes) {
        unsigned long long t = s.f_files, f = s.f_ffree, u = t - f;
        int pct = t ? (int)((u * 100 + t - 1) / t) : 0;
        printf("%-12s %10llu %10llu %10llu %3d%% %s\n", fsname, t, u, f, pct, mnt);
        return 0;
    }
    unsigned long long bs = s.f_frsize ? s.f_frsize : s.f_bsize;
    unsigned long long total = s.f_blocks * bs, fr = s.f_bfree * bs, av = s.f_bavail * bs;
    unsigned long long used = total - fr;
    /* POSIX rounds the percentage UP: a disk with one block used is not 0% full. */
    int pct = (used + av) ? (int)((used * 100 + used + av - 1) / (used + av)) : 0;
    size_str(a, sizeof a, total); size_str(b, sizeof b, used); size_str(c, sizeof c, av);
    printf("%-12s %10s %10s %10s %3d%% %s\n", fsname, a, b, c, pct, mnt);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'h') human = 1;
            else if (*f == 'i') inodes = 1;
            else if (*f == 'k') human = 0;
            else { fprintf(stderr, "df: unknown option -%c\n", *f); return 2; }
        }
    }
    if (inodes) printf("%-12s %10s %10s %10s %4s %s\n", "Filesystem", "Inodes", "IUsed", "IFree", "IUse%", "Mounted on");
    else printf("%-12s %10s %10s %10s %4s %s\n", "Filesystem", human ? "Size" : "1K-blocks", "Used", "Available", "Use%", "Mounted on");
    int rc = 0;
    if (i < argc) {
        for (; i < argc; i++) rc |= show(argv[i], argv[i], argv[i]);
        return rc;
    }
    FILE *m = fopen("/dev/vfsmounts", "r");
    if (!m) { fprintf(stderr, "df: cannot read the mount table (/dev/vfsmounts)\n"); return 1; }
    char line[256];
    while (fgets(line, sizeof line, m)) {
        char name[64], at[192];
        if (sscanf(line, "%63s %191s", name, at) == 2) rc |= show(name, at, at);
    }
    fclose(m);
    return rc;
}
