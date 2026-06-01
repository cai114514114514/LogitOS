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
#include "icmp.h"
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

    /* TEMP L3 self-test: ping the gateway (10.0.2.2). Pump net_poll() here with
     * interrupts on (PIT time base); QEMU defers its e1000 RX flush up to 1s, so
     * poll for a few seconds and re-issue the ping periodically. */
    if (net_up()) {
        __asm__ volatile ("sti");
        int got = 0;
        uint64_t start = timer_ticks(), last_ping = 0;
        while (timer_ticks() - start < 500) {        /* ~5 s at 100 Hz */
            net_poll();
            if (icmp_last_rtt() >= 0) { got = 1; break; }
            if (timer_ticks() - last_ping >= 50 || last_ping == 0) {
                last_ping = timer_ticks();
                icmp_ping(net_cfg.gw);               /* (re)send; ARP resolves first try */
            }
            for (volatile int d = 0; d < 300000; d++) ;
        }
        __asm__ volatile ("cli");
        if (got)
            kprintf("[net] L3 ping gw: %d ticks RTT\n[net] AQUA_NET_OK\n", icmp_last_rtt());
        else
            serial_puts("[net] L3 PING_FAIL\n");
    }

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    serial_puts("[aqua] desktop up; mouse + keyboard armed\n");

    if (fs_ok)
        serial_puts("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
