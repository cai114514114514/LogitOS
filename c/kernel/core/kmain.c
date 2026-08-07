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
#include "logitfs.h"
#include "net.h"
#include "text.h"
#include "img.h"
#include "smp.h"
#include "proc.h"
#include "file.h"
#include "virtio_blk.h"
#include "nvme.h"
#include "pci.h"
#include "driver.h"

/* TIMER_HZ now lives in pit.h: SYS_MONOTONIC_MS divides by it, so the tick rate
 * is part of a userland-visible answer and cannot be a private constant here. */

#define BOOT_OK_MARKER "LOGIT_BOOT_OK"

int rust_inflate_selftest(void);   /* rust/src/inflate.rs -- DEFLATE port self-check */
int rust_png_selftest(void);       /* rust/src/png.rs -- PNG decoder self-check (needs heap) */

void kernel_main(uint64_t mb_info)
{
    serial_init();
    serial_puts("\n[logitos] long mode, C kernel running\n");
    serial_puts(rust_inflate_selftest() == 0 ? "[rust] inflate selftest OK\n"
                                             : "[rust] inflate selftest FAIL\n");

    idt_init();
    gdt_init();
    pic_remap();
    pit_init(TIMER_HZ);
    pmm_init(mb_info);
    serial_puts("[logitos] interrupts + memory + gdt/tss online\n");

    /* Enumerate PCI before any driver runs: everything below -- including the
     * legacy pci_find() the NVMe/e1000/virtio drivers still call -- answers out
     * of the device registry this builds. Binding happens later (dev_probe_all,
     * after smp_init), because wiring an interrupt needs a live LAPIC. */
    pci_init();
    serial_puts(rust_png_selftest() == 0 ? "[rust] png selftest OK\n"
                                          : "[rust] png selftest FAIL\n");

    if (!fb_init(mb_info)) {
        serial_puts("\nLOGIT_FB_FAIL\n");
        for (;;) __asm__ volatile ("hlt");
    }

    proc_init();   /* process table */
    file_init();   /* open-file-description pool */

    nvme_init();         /* prefer NVMe (M24 bare-metal target) when present */
    virtio_blk_init();   /* else virtio-blk; logitfs falls back to ATA */

    vfs_register(&logitfs);
    int fs_ok = (vfs_mount() == 0);
    serial_puts(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");

    net_init();   /* NIC + stack (incl. TCP + HTTP); apps drive it at runtime */

    text_init();  /* load the TTF fonts for the anti-aliased Unicode text engine */
    img_init();   /* register the PNG + GIF image codecs (browser <img>) */

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    serial_puts("[logitos] desktop up; mouse + keyboard armed\n");

    smp_init();   /* detect + bring up the other CPUs */

    /* Now that the LAPIC and I/O APIC are live, run the probe/bind pass and
     * print the device table -- one line per PCI function, with its class name
     * and the driver that claimed it. On unfamiliar hardware this log is the
     * first debugging tool anyone has. */
    dev_probe_all();
    dev_dump();

    if (fs_ok)
        serial_puts("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
