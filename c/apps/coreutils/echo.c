#include "clib.h"

/* echo [args...] -- arguments separated by spaces, then a newline. */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) { outs(argv[i]); if (i + 1 < argc) outc(' '); }
    outc('\n');
    return 0;
}
