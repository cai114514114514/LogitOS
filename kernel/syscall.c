#include <stdint.h>
#include "syscall.h"
#include "serial.h"
#include "kprintf.h"

/* Set by the kernel before launching userland; the success marker is gated on
 * the storage stack having verified too, so a green test means the whole
 * boot -> fs -> ring3 path worked. */
extern int aqua_fs_ok;

static int user_wrote = 0;

void syscall_dispatch(struct registers *r)
{
    switch (r->rax) {
    case SYS_WRITE: {
        const char *buf = (const char *)r->rsi;     /* user pointer, mapped */
        long len = (long)r->rdx;
        for (long i = 0; i < len; i++)
            serial_putc(buf[i]);
        user_wrote = 1;
        r->rax = (uint64_t)len;
        break;
    }
    case SYS_EXIT:
        kprintf("\n[sys] user process exited, code=%d\n", (int)r->rdi);
        if (aqua_fs_ok && user_wrote)
            serial_puts("\nAQUA_BOOT_OK\n");
        else
            serial_puts("\nAQUA_USER_FAIL\n");
        __asm__ volatile ("sti");
        for (;;)
            __asm__ volatile ("hlt");
    default:
        r->rax = (uint64_t)-1;                       /* ENOSYS */
        break;
    }
}
