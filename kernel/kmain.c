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
#include "eth.h"
#include "arp.h"
#include "kprintf.h"

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

    vfs_register(&aquafs);
    int fs_ok = (vfs_mount() == 0);
    serial_puts(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");

    net_init();

    /* TEMP L2 self-test: ARP-resolve the gateway (10.0.2.2). Pump net_poll()
     * here since the WM loop isn't running yet. Use real PIT ticks (100 Hz):
     * QEMU's e1000 defers its RX flush by up to 1s, so poll for a few seconds
     * and re-send the request periodically. */
    if (net_up()) {
        __asm__ volatile ("sti");                    /* let the PIT advance (time base) */
        uint8_t gw_mac[6];
        int resolved = 0;
        uint64_t start = timer_ticks();
        while (timer_ticks() - start < 500) {        /* ~5 s at 100 Hz */
            net_poll();
            if (arp_resolve(net_cfg.gw, gw_mac) == 0) { resolved = 1; break; }
            for (volatile int d = 0; d < 500000; d++) ;
        }
        __asm__ volatile ("cli");                    /* restore (wm_run re-enables) */
        if (resolved)
            kprintf("[net] L2 ARP_OK gw mac %x:%x:%x:%x:%x:%x\n",
                    gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);
        else
            serial_puts("[net] L2 ARP_FAIL\n");
    }

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    serial_puts("[aqua] desktop up; mouse + keyboard armed\n");

    if (fs_ok)
        serial_puts("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
