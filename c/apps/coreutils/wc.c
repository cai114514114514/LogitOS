#include "clib.h"

/* Match the largest read(2) transfer the kernel accepts.  This buffer is BSS,
 * not a 64 KiB stack frame.  The old 512-byte loop turned one ui.ttf pass into
 * 4,341 syscalls and path/permission checks; sequential counting has no reason
 * to pay that fixed cost 128 times per transfer. */
#define WC_IO_CHUNK (64 * 1024)
static char io_buf[WC_IO_CHUNK];

/* wc [file...] -- lines, words, bytes (stdin if no file). */
static void count(int fd, const char *label)
{
    long lines = 0, words = 0, bytes = 0; int inword = 0;
    int r;
    while ((r = sys_read(fd, io_buf, sizeof io_buf)) > 0)
        for (int i = 0; i < r; i++) {
            char c = io_buf[i]; bytes++;
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
