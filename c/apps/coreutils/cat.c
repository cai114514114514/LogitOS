#include "clib.h"

/* cat [file...] -- copy files (or stdin if none) to stdout. */
static void copy_fd(int fd)
{
    char b[512]; int r;
    while ((r = sys_read(fd, b, sizeof b)) > 0) {
        int o = 0;
        while (o < r) {                     /* pipe short writes: retry, don't drop data */
            int w = sys_write(1, b + o, r - o);
            if (w <= 0) return;
            o += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { copy_fd(0); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { errs("cat: cannot open "); errs(argv[i]); errs("\n"); rc = 1; continue; }
        copy_fd(fd);
        sys_close(fd);
    }
    return rc;
}
