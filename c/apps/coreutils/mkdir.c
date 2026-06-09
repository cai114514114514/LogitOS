#include "clib.h"

int main(int argc, char **argv)
{
    if (argc < 2) { errs("mkdir: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (make_dir(argv[i]) < 0) { errs("mkdir: cannot create "); errs(argv[i]); errs("\n"); rc = 1; }
    return rc;
}
