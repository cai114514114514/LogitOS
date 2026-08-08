#include <stdint.h>
#include "serial.h"
#include "kprintf.h"
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
#include "klog.h"
#include "kdiag.h"
#include "cpu_report.h"   /* cpu_simd_selftest -- see the call below pit_init */
#include "blkdev.h"
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
    /* Boot narration goes through kprintf, not serial_puts. Same bytes on the
     * same wire -- but serial_puts writes ONLY to COM1 and keeps nothing, so
     * every line below used to be invisible to anyone who was not watching the
     * port at the time. Through kprintf they also land in the log ring, which
     * is the whole point: on a machine that fails to bring up a disk you want
     * to read the boot log AFTER it happened, and on a laptop there is no
     * serial port to have been watching. */
    kprintf("\n[logitos] long mode, C kernel running\n");
    kprintf(rust_inflate_selftest() == 0 ? "[rust] inflate selftest OK\n"
                                             : "[rust] inflate selftest FAIL\n");

    idt_init();
    gdt_init();
    pic_remap();
    pit_init(TIMER_HZ);

    /* The SIMD self-test. It was written, documented down to its call order
     * ("once the PIT is ticking, needs IF=0 on entry") and then never called
     * from anywhere -- so nothing it checks was being checked on any machine.
     *
     * What it checks cannot be checked host-side, which is why it exists: that
     * the XMM register file survives a real timer interrupt (the FXSAVE/FXRSTOR
     * in isr_common), and that the selected AES backend agrees with the
     * portable one WHILE interrupts are landing inside the instruction stream.
     * A host test runs neither of those.
     *
     * Here, unconditionally, for the reason the mount-time fsck is: it makes
     * every boot harness in the tree assert it, including the ones written for
     * something else entirely. It costs ~120 ms and it manages its own IF
     * (xmm_probe brackets its spin with sti/cli under pushfq/popfq), so IF is
     * 0 on both sides of this call. */
    cpu_simd_selftest();

    pmm_init(mb_info);
    kprintf("[logitos] interrupts + memory + gdt/tss online\n");

    /* Diagnostics come up early and deliberately: everything printed from here
     * on is also retained in the log ring, so a failure further down this
     * function can be read back afterwards instead of only being visible to
     * whoever was watching the serial port. kdiag_init installs the two
     * diagnostic IDT gates (panic's stop-the-other-cores IPI, and the
     * interrupt-context log probe). */
    kdiag_init();
    kinfo("[klog] ring %u lines x %u bytes; `cat /dev/kmsg`, `cat /dev/kstat`",
          (unsigned)KLOG_SLOTS, (unsigned)KLOG_TEXT);

    /* Enumerate PCI before any driver runs: everything below -- including the
     * legacy pci_find() the NVMe/e1000/virtio drivers still call -- answers out
     * of the device registry this builds. Binding happens later (dev_probe_all,
     * after smp_init), because wiring an interrupt needs a live LAPIC. */
    pci_init();
    kprintf(rust_png_selftest() == 0 ? "[rust] png selftest OK\n"
                                          : "[rust] png selftest FAIL\n");

    if (!fb_init(mb_info)) {
        kprintf("\nLOGIT_FB_FAIL\n");
        for (;;) __asm__ volatile ("hlt");
    }

    proc_init();   /* process table */
    file_init();   /* open-file-description pool */

    nvme_init();         /* prefer NVMe (M24 bare-metal target) when present */
    virtio_blk_init();   /* else virtio-blk; logitfs falls back to ATA */
    blk_init();          /* + AHCI/SATA, partition tables, and pick the root device */

    vfs_register(&logitfs);
    int fs_ok = (vfs_mount() == 0);
    kprintf(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");

    net_init();   /* NIC + stack (incl. TCP + HTTP); apps drive it at runtime */

    text_init();  /* load the TTF fonts for the anti-aliased Unicode text engine */
    img_init();   /* register the PNG + GIF image codecs (browser <img>) */

    wm_init();
    wm_render();                 /* first frame -> desktop visible */
    mouse_init();
    kprintf("[logitos] desktop up; mouse + keyboard armed\n");

    smp_init();   /* detect + bring up the other CPUs */

    /* Now that the LAPIC and I/O APIC are live, run the probe/bind pass and
     * print the device table -- one line per PCI function, with its class name
     * and the driver that claimed it. On unfamiliar hardware this log is the
     * first debugging tool anyone has. */
    dev_probe_all();
    dev_dump();

    if (fs_ok)
        kprintf("\n" BOOT_OK_MARKER "\n");

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
