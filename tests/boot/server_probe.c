/* A normal remote program used to observe SSH's two streams and raw status. */
#include "clib.h"
int main(void) { outs("SERVER_STDOUT\n"); errs("SERVER_STDERR\n"); return 37; }
