/* Aqua OS - a minimal ring 3 userland program.
 * It cannot touch kernel memory; it talks to the kernel only via int 0x80. */

#define SYS_WRITE 1
#define SYS_EXIT  2

static long sys_write(long fd, const char *buf, long len)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"((long)SYS_WRITE), "D"(fd), "S"(buf), "d"(len)
                      : "memory");
    return ret;
}

static long slen(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    return n;
}

int umain(void)
{
    const char *msg =
        "hello from ring 3 -- a userland ELF, loaded off the AquaFS disk,\n"
        "running unprivileged, calling the kernel through int 0x80.\n";
    sys_write(1, msg, slen(msg));
    return 0;
}
