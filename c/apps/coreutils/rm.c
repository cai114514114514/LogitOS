#include "clib.h"

int main(int argc, char **argv)
{
    if (argc < 2) { errs("rm: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (delete_file(argv[i]) < 0) { errs("rm: cannot remove "); errs(argv[i]); errs("\n"); rc = 1; }
    return rc;
}
