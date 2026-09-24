/* du [-s] [-a] [-h] [-k] [PATH...] -- disk usage, in 1024-byte blocks.
 *
 * Usage is what the filesystem ALLOCATED (st_blocks, 512-byte units), not the
 * byte length: a 1-byte file on 4 KiB blocks uses 4 KiB, which is the number a
 * person freeing space needs. Symbolic links are not followed. A hard-linked
 * file is counted once per (dev, ino), as POSIX asks. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

static int summarize, all, human;

#define SEEN_MAX 4096
static struct { unsigned long long dev, ino; } seen[SEEN_MAX];
static int nseen;

static int already(const struct stat *st)
{
    if (st->st_nlink < 2) return 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i].dev == (unsigned long long)st->st_dev && seen[i].ino == (unsigned long long)st->st_ino) return 1;
    if (nseen < SEEN_MAX) { seen[nseen].dev = st->st_dev; seen[nseen].ino = st->st_ino; nseen++; }
    return 0;
}

static void print(unsigned long long kb, const char *path)
{
    if (!human) { printf("%llu\t%s\n", kb, path); return; }
    const char *u = "KMGTP";
    double v = (double)kb;
    int i = 0;
    while (v >= 1024.0 && u[i + 1]) { v /= 1024.0; i++; }
    if (v < 10.0 && i) printf("%.1f%c\t%s\n", v, u[i], path);
    else printf("%.0f%c\t%s\n", v, u[i], path);
}

/* Returns KiB used under `path`; prints per the flags. depth 0 is an operand. */
static unsigned long long walk(const char *path, int depth, int *err)
{
    struct stat st;
    if (lstat(path, &st) < 0) { fprintf(stderr, "du: cannot access '%s'\n", path); *err = 1; return 0; }
#ifdef TEXTUTIL_NEGCTL
    /* THE CONTROL: bytes, not allocation -- the classic du mistake. */
    unsigned long long kb = already(&st) ? 0 : ((unsigned long long)st.st_size + 1023) / 1024;
#else
    unsigned long long kb = already(&st) ? 0 : ((unsigned long long)st.st_blocks * 512 + 1023) / 1024;
#endif
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "du: cannot read directory '%s'\n", path); *err = 1; }
        else {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[1024];
                size_t n = strlen(path);
                snprintf(sub, sizeof sub, "%s%s%s", path, (n && path[n - 1] == '/') ? "" : "/", e->d_name);
                kb += walk(sub, depth + 1, err);
            }
            closedir(d);
        }
        if (!summarize || depth == 0) print(kb, path);
    } else if (depth == 0 || (all && !summarize)) {
        print(kb, path);
    }
    return kb;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 's') summarize = 1;
            else if (*f == 'a') all = 1;
            else if (*f == 'h') human = 1;
            else if (*f == 'k') human = 0;
            else { fprintf(stderr, "du: unknown option -%c\n", *f); return 2; }
        }
    }
    int err = 0;
    if (i == argc) walk(".", 0, &err);
    for (; i < argc; i++) walk(argv[i], 0, &err);
    return err;
}
