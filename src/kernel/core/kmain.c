#include <stdint.h>
#include "serial.h"
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "fb.h"
#include "wm.h"
#include "mouse.h"
#include "vfs.h"
#include "aquafs.h"
#include "net.h"
#include "text.h"
#include "img.h"
#include "smp.h"
#include "proc.h"
#include "file.h"

#define TIMER_HZ 100

#define BOOT_OK_MARKER "AQUA_BOOT_OK"

void kernel_main(uint64_t mb_info)
{
    serial_init();
    serial_puts("\n[aqua] long mode, C kernel running\n");

    idt_init();
    gdt_init();
    pic_remap();
    pit_init(TIMER_HZ);
    pmm_init(mb_info);
    serial_puts("[aqua] interrupts + memory + gdt/tss online\n");

    if (!fb_init(mb_info)) {
        serial_puts("\nAQUA_FB_FAIL\n");
        for (;;) __asm__ volatile ("hlt");
    }

    proc_init();   /* process table */
    file_init();   /* open-file-description pool */

    vfs_register(&aquafs);
    int fs_ok = (vfs_mount() == 0);
    serial_puts(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");

    net_init();   /* NIC + stack (incl. TCP + HTTP); apps drive it at runtime */

    text_init();  /* load the TTF fonts for the anti-aliased Unicode text engine */
    img_init();   /* register the PNG + GIF image codecs (browser <img>) */

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    serial_puts("[aqua] desktop up; mouse + keyboard armed\n");

    smp_init();   /* detect + bring up the other CPUs */

    if (fs_ok)
        serial_puts("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
