#include "clib.h"

int main(void) { char b[128]; sys_getcwd(b, sizeof b); outs(b); outc('\n'); return 0; }
