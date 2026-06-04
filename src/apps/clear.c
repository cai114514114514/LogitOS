#include "clib.h"

/* clear -- ANSI clear screen + home cursor (works on the serial console and any
 * terminal that interprets escapes). */
int main(void) { outs("\033[2J\033[H"); return 0; }
