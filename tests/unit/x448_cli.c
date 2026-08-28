/* Thin CLI wrapper around x448()/x448_base() for the shell-driven OpenSSL
 * differential (run-x448-openssl.sh). Same role as mlkem_cli.c plays for
 * test-mlkem-openssl -- all the C stays in one small, obviously-correct
 * translation layer and the interesting comparisons live in the shell
 * script, where they can call openssl too. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x448.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex56(uint8_t *out, const char *h)
{
    if (strlen(h) != 112) return -1;
    for (int i = 0; i < 56; i++) {
        int a = hexval(h[2*i]), b = hexval(h[2*i+1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static void printhex(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "base") == 0) {
        uint8_t priv[56], pub[56];
        if (unhex56(priv, argv[2])) { fprintf(stderr, "bad hex\n"); return 2; }
        x448_base(pub, priv);
        printhex(pub, 56);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "shared") == 0) {
        uint8_t priv[56], pub[56], shared[56];
        if (unhex56(priv, argv[2]) || unhex56(pub, argv[3])) {
            fprintf(stderr, "bad hex\n"); return 2;
        }
        x448(shared, priv, pub);
        printhex(shared, 56);
        return 0;
    }
    fprintf(stderr, "usage: x448_cli base <priv56hex>\n"
                     "       x448_cli shared <priv56hex> <pub56hex>\n");
    return 2;
}
