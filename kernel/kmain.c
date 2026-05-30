#include <stdint.h>
#include "serial.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "fb.h"
#include "desktop.h"

#define BOOT_OK_MARKER "AQUA_BOOT_OK"
#define TIMER_HZ 100

void kernel_main(uint64_t mb_info)
{
    serial_init();
    serial_puts("\n[aqua] long mode, C kernel running\n");

    idt_init();
    pic_remap();
    pit_init(TIMER_HZ);
    pmm_init(mb_info);
    serial_puts("[aqua] interrupts + memory online\n");

    if (!fb_init(mb_info)) {
        serial_puts("\nAQUA_FB_FAIL: no 32-bpp linear framebuffer\n");
        for (;;)
            __asm__ volatile ("hlt");
    }
    serial_puts("[aqua] framebuffer mapped; compositing desktop\n");

    desktop_draw();

    serial_puts("\n" BOOT_OK_MARKER "\n");

    __asm__ volatile ("sti");
    for (;;)
        __asm__ volatile ("hlt");
}
