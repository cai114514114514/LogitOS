#include "clib.h"

/* head [-n N] [file] -- first N lines (default 10) of a file or stdin. */
int main(int argc, char **argv)
{
    int limit = 10, ai = 1;
    if (argc > 2 && c_streq(argv[1], "-n")) { limit = c_atoi(argv[2]); ai = 3; }
    int fd = (ai < argc) ? sys_open(argv[ai], O_RDONLY) : 0;
    if (fd < 0) { errs("head: cannot open\n"); return 1; }
    int lines = 0; char c;
    while (lines < limit && sys_read(fd, &c, 1) > 0) { sys_write(1, &c, 1); if (c == '\n') lines++; }
    if (fd) sys_close(fd);
    return 0;
}
