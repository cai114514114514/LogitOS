/* ASan fuzz for the image decoders (lib/image: png, gif, inflate). Decodes each
 * file at full length AND at truncated prefixes -- matching the kernel res_fetch
 * truncation (images >64KB arrive cut). Untrusted/truncated input into the
 * hand-written decoders is the prime heap-overflow suspect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "img.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

int main(int argc, char **argv)
{
    img_init();   /* register PNG + GIF */
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); if (!f) { printf("skip %s\n", argv[a]); continue; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(len); fread(buf, 1, len, f); fclose(f);
        int cuts[] = { (int)len, 65536, (int)len/2, (int)len/3, 1000, 256, 64, 33, 16, 8, 1 };
        for (unsigned ci = 0; ci < sizeof cuts/sizeof cuts[0]; ci++) {
            int n = cuts[ci];
            if (n <= 0 || n > len) continue;
            struct image im;
            int r = img_decode(buf, n, &im);
            printf("%s n=%d -> rc=%d %dx%d\n", argv[a], n, r, r==0?im.w:0, r==0?im.h:0);
            if (r == 0) img_free(&im);
        }
        free(buf);
    }
    printf("IMG FUZZ DONE\n");
    return 0;
}
