#include "clib.h"

/* touch FILE -- create an empty file if it doesn't exist. */
int main(int argc, char **argv)
{
    if (argc < 2) { errs("touch: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { errs("touch: cannot create "); errs(argv[i]); errs("\n"); rc = 1; }
        else sys_close(fd);
    }
    return rc;
}
