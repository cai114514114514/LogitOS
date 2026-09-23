/* An argz length is authoritative even when its final element lacks NUL.
 * argz_count and argz_extract already accept that shape.  Searching it with
 * strstr instead of a length-bounded scan reads past the allocation; ASan
 * must see the one-byte no-match case, not merely a familiar NUL string. */
#include <argz.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int checks;

static void check(int good, const char *name)
{
    checks++;
    if (!good) {
        printf("FAIL argz bounds: %s\n", name);
        exit(1);
    }
}

int main(void)
{
    char *z = malloc(1);
    check(z != NULL, "allocate no-match input");
    z[0] = 'a';
    size_t len = 1;
    unsigned int replaced = 0;
    check(argz_replace(&z, &len, "z", "x", &replaced) == 0,
          "no-match result");
    check(len == 1 && z[0] == 'a' && replaced == 0,
          "no-match preserves unterminated entry");
    free(z);

    z = malloc(3);
    check(z != NULL, "allocate match input");
    memcpy(z, "aba", 3);
    len = 3;
    check(argz_replace(&z, &len, "b", "XY", &replaced) == 0,
          "match result");
    check(len == 4 && memcmp(z, "aXYa", 4) == 0 && replaced == 1,
          "replace within unterminated entry");
    free(z);

    printf("argz bounds: %d checks, 0 failed\n", checks);
    return 0;
}
