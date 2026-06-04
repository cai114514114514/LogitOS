#include "clib.h"

/* wc [file...] -- lines, words, bytes (stdin if no file). */
static void count(int fd, const char *label)
{
    long lines = 0, words = 0, bytes = 0; int inword = 0;
    char b[512]; int r;
    while ((r = sys_read(fd, b, sizeof b)) > 0)
        for (int i = 0; i < r; i++) {
            char c = b[i]; bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t') inword = 0;
            else if (!inword) { inword = 1; words++; }
        }
    outn(lines); outc(' '); outn(words); outc(' '); outn(bytes);
    if (label) { outc(' '); outs(label); }
    outc('\n');
}

int main(int argc, char **argv)
{
    if (argc < 2) { count(0, 0); return 0; }
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { errs("wc: cannot open "); errs(argv[i]); errs("\n"); continue; }
        count(fd, argv[i]);
        sys_close(fd);
    }
    return 0;
}
