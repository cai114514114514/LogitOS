#include <stdint.h>
#include "serial.h"
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "fb.h"
#include "desktop.h"
#include "vfs.h"
#include "aquafs.h"
#include "elf.h"

#define TIMER_HZ 100
#define USER_STACK_TOP   0x48000000ULL
#define USER_STACK_PAGES 4

/* Visible to syscall.c: gates the success marker on the storage stack too. */
int aqua_fs_ok = 0;

extern void enter_user(uint64_t entry, uint64_t user_rsp);

/* Mount the disk, list it, read a file, and verify its contents. */
static int test_filesystem(void)
{
    vfs_register(&aquafs);
    if (vfs_mount()) {
        serial_puts("[fs] mount failed (no AquaFS volume?)\n");
        return 0;
    }
    vfs_list();

    static uint8_t buf[2048];
    int n = vfs_read("readme.txt", buf, sizeof buf);
    if (n <= 0) {
        serial_puts("[fs] read of readme.txt failed\n");
        return 0;
    }
    buf[n] = '\0';
    serial_puts("[fs] readme.txt:\n");
    serial_puts((const char *)buf);

    const char *expect = "Aqua OS - AquaFS volume";
    for (int i = 0; expect[i]; i++)
        if (buf[i] != (uint8_t)expect[i])
            return 0;
    return 1;
}

/* Load hello.elf from the disk and run it in ring 3. Does not return on
 * success — the user program's SYS_EXIT takes over from the syscall handler. */
static void run_user_program(void)
{
    int sz = vfs_size("hello.elf");
    if (sz <= 0) {
        serial_puts("[user] hello.elf not found on disk\n");
        return;
    }

    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img || vfs_read("hello.elf", img, bytes) <= 0) {
        serial_puts("[user] failed to read hello.elf\n");
        return;
    }

    uint64_t entry = elf_load(img);
    if (!entry) {
        serial_puts("[user] not a valid ELF64 image\n");
        return;
    }

    for (int i = 1; i <= USER_STACK_PAGES; i++)
        vmm_map_page(USER_STACK_TOP - (uint64_t)i * 0x1000, pmm_alloc(),
                     VMM_WRITABLE | VMM_USER);

    serial_puts("[user] entering ring 3...\n");
    enter_user(entry, USER_STACK_TOP);
}

void kernel_main(uint64_t mb_info)
{
    serial_init();
    serial_puts("\n[aqua] long mode, C kernel running\n");

    idt_init();
    gdt_init();                 /* kernel/user segments + TSS for ring 3 */
    pic_remap();
    pit_init(TIMER_HZ);
    pmm_init(mb_info);
    serial_puts("[aqua] interrupts + memory + gdt/tss online\n");

    if (fb_init(mb_info)) {
        desktop_draw();
        serial_puts("[aqua] desktop composited\n");
    }

    aqua_fs_ok = test_filesystem();
    serial_puts(aqua_fs_ok ? "[fs] verified OK\n" : "[fs] FAILED\n");

    run_user_program();         /* normally never returns */

    serial_puts("\nAQUA_USER_FAIL: userland did not start\n");
    __asm__ volatile ("sti");
    for (;;)
        __asm__ volatile ("hlt");
}
