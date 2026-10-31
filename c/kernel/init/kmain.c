#include "dma.h"
#include <stdint.h>
#include "serial.h"
#include "kprintf.h"
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "pit.h"
#include "ktime.h"
#include "pmm.h"
#include "acpi.h"
#include "fb.h"
#include "wm.h"
#include "mouse.h"
#include "vfs.h"
#include "settings.h"
#include "logitfs.h"
#include "net.h"
#include "roots.h"        /* trust_banner -- the compiled-in CA set, named */
#include "text.h"
#include "img.h"
#include "smp.h"
#include "proc.h"
#include "file.h"
#include "virtio_blk.h"
#include "nvme.h"
#include "klog.h"
#include "kdiag.h"
#include "hwreport.h"
#include "smbios.h"
#include "board.h"
#include "lapic.h"
#include "pit.h"
#include "ktime.h"
#include "hwscreen.h"
#include "bootmark.h"
#include "../cpu/power/pat.h"
#include "fb.h"
#include "pcache.h"
#include "pcache_vfs.h"
#include "procfs.h"       /* /proc -- mounted below, right after the root */
#include "cpu_report.h"   /* cpu_simd_selftest -- see the call below pit_init */
#include "blkdev.h"
#include "pci.h"
#include "driver.h"
#include "amd/bootfb.h"

/* TIMER_HZ now lives in pit.h: SYS_MONOTONIC_MS divides by it, so the tick rate
 * is part of a userland-visible answer and cannot be a private constant here. */

#define BOOT_OK_MARKER "LOGIT_BOOT_OK"

int rust_inflate_selftest(void);   /* rust/src/inflate.rs -- DEFLATE port self-check */
int rust_png_selftest(void);       /* rust/src/png.rs -- PNG decoder self-check (needs heap) */

void kernel_main(uint64_t mb_info)
{
    serial_init();
    bootmark_init(mb_info);
    bootmark(3);
#ifdef BOOT_MB2_DUMP
    extern void mb2_dump(uint64_t); mb2_dump(mb_info);
#endif
    /* Boot narration goes through kprintf, not serial_puts. Same bytes on the
     * same wire -- but serial_puts writes ONLY to COM1 and keeps nothing, so
     * every line below used to be invisible to anyone who was not watching the
     * port at the time. Through kprintf they also land in the log ring, which
     * is the whole point: on a machine that fails to bring up a disk you want
     * to read the boot log AFTER it happened, and on a laptop there is no
     * serial port to have been watching. */
    kprintf("\n[logitos] long mode, C kernel running\n");
    hwreport_milestone("efi", "NOT_OBSERVED kernel entered; loader mode not recorded here");
    kprintf(rust_inflate_selftest() == 0 ? "[rust] inflate selftest OK\n"
                                             : "[rust] inflate selftest FAIL\n");

    idt_init();
    gdt_init();
    pic_remap();
    bootmark(4);
    pit_init(TIMER_HZ);
    bootmark(5);

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
    bootmark(6);
    bootmark_early_end();   /* pmm_init edits the tables the early squares used */

    pmm_init(mb_info);
    /* The 8259 reaches the CPU only through LAPIC LINT0 in virtual-wire mode,
     * which UEFI firmware need not leave set up; the PIT has been ticking
     * into nothing until here otherwise. Needs pmm for the LAPIC mapping. */
    lapic_virtual_wire();
    /* Before anything can ask for an ACPI table: under UEFI the RSDP is only
     * reachable through this block (c/boot/efi/loader.c forwards it as tag 15),
     * and the BIOS-area scan acpi.c falls back to finds nothing there. */
    acpi_set_mb2_info(mb_info);
    smbios_capture_boot_info(mb_info);   /* UEFI: the board report reads it later */
    /* Before storage probing and the desktop: a TCO watchdog firmware left
     * counting would reset the machine a few minutes into boot. */
    board_early();
    /* ACPI-backed clocks cannot be discovered in pit_init(): firmware tables
     * are not safely mapped until pmm_init.  This keeps early boot on PIT/TSC,
     * then upgrades a PIT-only machine to HPET continuously when available. */
    time_platform_init();
    /* After time_platform_init: until the HPET legacy route is disarmed the
     * PIT cannot tick, and a PIT-only clock would never reach 50 ms. Bounded
     * by iterations too, so no clock can hang it (Astra round 5). */
    {
        uint64_t t0 = time_mono_ns(), k0 = timer_ticks();
        for (unsigned long spins = 0; spins < 200000000ul && time_mono_ns() - t0 < 50000000ull; spins++)
            __asm__ volatile ("sti; pause");
        kprintf("[time] PIT ticks in 50 ms after virtual wire: %lu (expect ~5)\n", (unsigned long)(timer_ticks() - k0));
    }
    kprintf("[logitos] interrupts + memory + gdt/tss online\n");

    /* The page cache's frame table (pc_of_frame[], pcache.c) comes out of the
     * PMM exactly like rmap's does, and for the same reason argued at
     * pmm.c's rmap_init() call: reclaim must be able to run when memory is
     * exhausted, so every structure it consults -- and reclaim.h's
     * eligibility test now has a THIRD term, pcache_holds() -- has to exist
     * before anything can exhaust memory. rmap_init() runs INSIDE pmm_init()
     * because c/kernel/mm/phys/pmm.c already owns that call; pcache_init() cannot
     * move in beside it without editing a file this unit does not own, so it
     * runs here instead, one line later, which preserves the one invariant
     * that actually matters: nothing between here and the first possible
     * reclaim pass (the first ring-3 page fault -- reclaim_late_init(), see
     * fault.c) can observe pcache_holds() answering out of a table that does
     * not exist yet.
     *
     * This is NOT the backend (pcache_set_ops(), below, after the filesystem
     * is mounted) -- pcache_init() only needs pmm_alloc_contig(), which is
     * live the moment pmm_init() returns. A cache with a frame table and no
     * backend is well-defined: pcache_ready() is already 1, but
     * pcache_file_open() returns -1 until pcache_set_ops() lands (pc_ops is
     * still NULL), so nothing downstream has to know the difference. */
    pcache_init(pmm_total_frames());

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

    /* PA1 becomes write-combining before the framebuffer is first mapped, so
     * the scanout memory is never mapped with two cache types. */
    pat_init();
    kprintf("[pat] cpu0 PA1=%s\n", pat_wc_available() ? "WC (IA32_PAT 0x0007040600070106)" : "unchanged (no PAT)");

    if (!fb_init(mb_info)) {
        hwreport_milestone("fb", "FAIL");
        kprintf("\nLOGIT_FB_FAIL\n");
        for (;;) __asm__ volatile ("hlt");
    }
    hwreport_milestone("fb", "OK framebuffer initialized");
    bootmark_resume();
    bootmark(7);

    proc_init();   /* process table */
    file_init();   /* open-file-description pool */

    nvme_init();         /* prefer NVMe (M24 bare-metal target) when present */
    virtio_blk_init();   /* else virtio-blk; logitfs falls back to ATA */
    blk_init();          /* + AHCI/SATA, partition tables, and pick the root device */
    bootmark(8);

    vfs_register(&logitfs);
    int fs_ok = (vfs_mount() == 0);
    bootmark(9);
    hwreport_milestone("root", fs_ok ? "OK filesystem mounted" : "FAIL filesystem mount failed");
    if (!fs_ok) hwreport_dump();
    kprintf(fs_ok ? "[fs] mounted\n" : "[fs] mount FAILED\n");
    bootmark(10);

    /* /proc, on top of the root that just arrived.
     *
     * THE MKDIR IS NOT A WORKAROUND, it is what vfs_mount_at() requires and
     * says so: "a mount point that does not exist is a filesystem you can only
     * reach by knowing it is there". The alternative was to pack an empty
     * /proc into the disk image, which costs a line in the $(DISK) recipe --
     * in the Makefile, which several lines are editing at once -- and costs
     * the same ONE inode out of the 256 tools/mkfs.py formats. So the mkdir is
     * the same price with no shared-file edit. It is idempotent: on every boot
     * after the first the directory is already there, logitfs refuses to make
     * it again, and the mount happens regardless -- which is why the return
     * value of the mkdir is deliberately ignored and the MOUNT's is not.
     *
     * Failure here is not fatal and must not be. A machine with no /proc is
     * the machine this tree had yesterday; every one of its consumers reports
     * a missing file, which is a better outcome than refusing to boot. */
    if (fs_ok) {
        vfs_mkdir("/proc");
        int prc = procfs_mount("/proc");
        /* The success line names the mount point through procfs_mountpoint()
         * rather than repeating the literal: what that returns is the string
         * procfs_owns_path() will compare against, so a boot log saying
         * "/proc mounted" is evidence about the ownership test and not just
         * about the mount table. */
        if (prc == 0) kprintf("[fs] %s mounted\n", procfs_mountpoint());
        else          kprintf("[fs] /proc mount FAILED (%d)\n", prc);
    }

    /* The page cache's backend (pcache.h's struct pcache_ops, built over
     * c/fs/vfs.h in pcache_vfs.c). Installed here, after vfs_mount(), rather
     * than beside pcache_init() above, because the ops are only USED when a
     * file gets opened through the cache -- the earliest caller is a syscall,
     * long after kernel_main returns -- so there is no correctness reason to
     * install them earlier, and a real one to wait: it puts "the filesystem
     * is mounted" and "the cache can read through it" next to each other in
     * the boot log instead of one implying the other across sixty lines.
     * Unconditional on fs_ok on purpose: every vfs_* call already fails
     * cleanly (a negative return) against an unmounted root, and pcv_stat()
     * turns that into pcache_file_open()'s ordinary -1 -- there is no third
     * state to special-case here that vfs.c does not already handle. */
    pcache_vfs_install();
    bootmark(11);

    /* Load what this machine remembers about its user. Between the mount and
     * net_init/wm_init on purpose: the network configuration and the theme are
     * both chosen from it, and both happen below. A damaged or absent file is
     * NOT an error here -- settings_init() reports what it rejected and leaves
     * every key at its default, which is why nothing after this line has to
     * care whether it worked. */
    if (fs_ok) settings_init();

    /* Enumerate the trust store on serial before the network exists. It used
     * to be printed from tls_start(), which meant a machine that never opened
     * a TLS connection never said what it trusts -- and "what it trusts" is
     * not a runtime property, it is compiled in, so there was never a reason
     * to wait for a handshake to state it. Beside net_init because this is the
     * anchor set every https fetch net_init makes possible will be checked
     * against. trust_banner() is idempotent; tls_start still calls it, and on
     * an ordinary boot finds it already said. */
    trust_banner();
    bootmark(12);

    net_init();   /* NIC + stack (incl. TCP + HTTP); apps drive it at runtime */
    bootmark(14);

    text_init();  /* load the TTF fonts for the anti-aliased Unicode text engine */
    bootmark(15);
    img_init();   /* register the PNG + GIF image codecs (browser <img>) */
    bootmark(16);

    wm_init();
    bootmark(17);
    wm_render();                 /* first frame -> desktop visible */
    bootcon_stop();              /* the desktop owns the screen from here */
    bootmark(18);
    fb_present_bench();          /* the back buffer exists now: time one full frame */
    mouse_init();
    kprintf("[logitos] desktop up; mouse + keyboard armed\n");
    hwreport_milestone("input", "ARMED keyboard/mouse initialized; interaction unverified");
    hwreport_milestone("desktop", "READY first frame submitted; scanout unverified");

    smp_init();   /* detect + bring up the other CPUs */
    bootmark(19);
    hwreport_milestone("smp", "OBSERVED see CPU topology summary");

    /* Now that the LAPIC and I/O APIC are live, run the probe/bind pass and
     * print the device table -- one line per PCI function, with its class name
     * and the driver that claimed it. On unfamiliar hardware this log is the
     * first debugging tool anyone has. */
    dev_probe_all();
    bootmark(20);
    /* Resource discovery needs both a mounted root and the boot-display
     * driver's successful binding; at root mount time no GPU is bound yet. */
    if (fs_ok)
        amd_bootfb_probe_resources();
    dev_dump();
    dma_report("boot");
    hwreport_dump();

    if (fs_ok)
        kprintf("\n" BOOT_OK_MARKER "\n");
    else {
        /* No root: no desktop, no apps and nowhere to keep the log, and on a
         * machine without a serial port nothing else would show it. The board
         * report normally runs on the acpi-init thread wm_run() starts; run it
         * here so its lines reach the screen too. */
        board_init();
        bootmark(21);
        hwscreen_report();
    }

    wm_run();                    /* becomes the scheduler main thread; never returns */
}
