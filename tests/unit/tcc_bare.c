/* A program with NO headers and NO libc, for tcc to compile and link ON THE
 * DEVICE before the sysroot line has put /usr/include and /usr/lib/libc.a on
 * the disk: `tcc -nostdlib -nostdinc -o /bare /src/bare.c`, then `/bare`.
 *
 * What it proves if it prints: the device tcc's preprocessor, code generator,
 * inline-assembler (the int 0x80 below is tcc's own asm parser), ELF writer
 * and the chmod patch in tccelf.c all work under mini-libc, and the kernel
 * then loads the bare ELF tcc wrote. The syscall shape is c/apps/logit.h's:
 * rax = number, rdi/rsi/rdx = arguments. SYS_WRITE is 1 and SYS_EXIT is 2
 * (include/abi/logit_abi.h); spelled as numbers because this file must need
 * nothing but itself. */
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static const char msg[] = "BARE-ELF ok: compiled, linked and run on the device\n";

void _start(void)
{
    sys3(1, 1, (long)msg, sizeof msg - 1);
    sys3(2, 0, 0, 0);
    for (;;) ;
}
