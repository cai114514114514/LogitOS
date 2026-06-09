/* ASan/UBSan fuzz for the untrusted-input parsers url_parse/url_resolve (redirect
 * Location: headers are attacker-controlled) and utf8_next (runs on all page
 * text). Buffers are sized tightly so any out-of-bounds read/write is caught. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "url.h"
#include "utf8.h"

int main(void)
{
    srand(7);

    /* url_parse: random bytes, mutated valid URL, and overlong host/path */
    for (int it = 0; it < 200000; it++) {
        int n = rand() % 280; char *s = malloc(n + 1);
        for (int i = 0; i < n; i++) s[i] = 1 + rand() % 255; s[n] = 0;
        struct url u; url_parse(s, &u); free(s);
    }
    const char *base = "http://example.com:8080/a/b/c?q=1";
    for (int it = 0; it < 200000; it++) {
        int n = (int)strlen(base); char *s = malloc(n + 1); memcpy(s, base, n + 1);
        int k = 1 + rand() % 5; for (int j = 0; j < k; j++) s[rand() % n] ^= 1 + rand() % 255;
        struct url u; url_parse(s, &u); free(s);
    }
    { char *s = malloc(4000); memcpy(s, "http://", 7);
      for (int i = 7; i < 3998; i++) s[i] = 'a'; s[3998] = 0; struct url u; url_parse(s, &u); free(s); }
    { char *s = malloc(4000); memcpy(s, "http://h/", 9);
      for (int i = 9; i < 3998; i++) s[i] = 'p'; s[3998] = 0; struct url u; url_parse(s, &u); free(s); }

    /* url_resolve: random refs against a base, into both roomy and tiny buffers */
    struct url b; url_parse(base, &b);
    for (int it = 0; it < 200000; it++) {
        int n = rand() % 280; char *ref = malloc(n + 1);
        for (int i = 0; i < n; i++) ref[i] = 1 + rand() % 255; ref[n] = 0;
        char out[700], tiny[16];
        url_resolve(&b, ref, out, sizeof out);
        url_resolve(&b, ref, tiny, sizeof tiny);
        free(ref);
    }

    /* utf8_next: walk tightly-sized (no slack past the NUL) random byte buffers */
    for (int it = 0; it < 300000; it++) {
        int n = 1 + rand() % 32; unsigned char *bbuf = malloc(n + 1);
        for (int i = 0; i < n; i++) bbuf[i] = rand() % 256; bbuf[n] = 0;
        const char *p = (const char *)bbuf; uint32_t cp; int guard = 0;
        while (*p && guard++ < 200) p = utf8_next(p, &cp);
        free(bbuf);
    }

    printf("PARSE FUZZ DONE\n");
    return 0;
}
