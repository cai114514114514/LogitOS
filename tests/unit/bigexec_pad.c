/* bigexec_pad -- a LOAD-PATH RULER, not a program.
 *
 * It exists to answer one question: did the whole image arrive? So every
 * decision here is about making a partial load impossible to mistake for a
 * whole one.
 *
 *  - The blob is .rodata (bigexec_pad.S's .incbin), so it is a REAL PT_LOAD
 *    with p_filesz == p_memsz. .bss would measure nothing at all: elf.c commits
 *    p_memsz and the pages are never read from the file, so a .bss pad would
 *    "load" at any size on any loader and prove nothing about either.
 *  - main() touches the LAST byte before it prints ok. A header that parsed and
 *    a first page that mapped are not the claim.
 *  - It also touches the middle and the first byte and SUMS all three, so the
 *    compiler cannot drop the loads, and it prints the sum -- three known
 *    values planted by the generator, so the check is on the CONTENTS and not
 *    merely on the absence of a fault. A loader that maps the right number of
 *    pages holding the wrong bytes fails here.
 *
 * The blob is incompressible (a fixed PRNG seed) because nothing between the
 * generator and the loader may be able to elide it, and deterministic because a
 * corpus that changes every run cannot be bisected.
 */
#include "clib.h"

extern const unsigned char pad_blob[];
extern const unsigned char pad_end[];

static void putn(unsigned long n)
{
    char b[24];
    int i = 0;
    if (!n) b[i++] = '0';
    while (n) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i) outc(b[--i]);
}

int main(void)
{
    unsigned long n = (unsigned long)(pad_end - pad_blob);
    unsigned s = pad_blob[n - 1];
    s += pad_blob[n / 2];
    s += pad_blob[0];
    outs("BIGEXEC ok bytes=");
    putn(n);
    outs(" sum=");
    putn(s);
    outc('\n');
    return 0;
}
