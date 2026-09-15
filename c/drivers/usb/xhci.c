#include "../core/io_domain.h"
#include "../core/io_lock.h"
/* Command/context transactions may wait while IRQ keeps draining events.
 * IRQ never takes the transaction owner, only the short event/ring gate. */
static struct io_domain xhci_owner = IO_DOMAIN_INIT;
static io_lock_t xhci_events_gate = IO_LOCK_INIT;
/* xHCI 1.2 host controller driver.
 *
 * Structure follows the spec's own order: 4.2 "Host Controller Initialization",
 * 4.3 "USB Device Initialization", 4.9 "TRB Ring", 4.11 "TRB Types".
 *
 * TWO THINGS WORTH KNOWING BEFORE READING.
 *
 * 1. THIS DRIVER IS INTERRUPT-DRIVEN, NOT POLLED. The event ring raises MSI-X,
 *    wired by the device model's dev_irq_request() (c/drivers/core/irq.c), and
 *    xhci_events() runs as the interrupt handler. That is worth stating because
 *    every other device in this kernel is polled -- the e1000 has no NIC IRQ at
 *    all -- and because polling was the fallback plan here: it is not what
 *    shipped.
 *
 *    It works because a HID report needs nothing that an interrupt handler
 *    cannot do. Draining the event ring is bounded, allocates nothing and
 *    sleeps never; decoding a report is arithmetic; and delivering it calls
 *    wm_key()/wm_mouse_event(), which are documented IRQ-context entry points
 *    that only enqueue -- the PS/2 handlers have called them from IRQ 1 and
 *    IRQ 12 all along.
 *
 *    What is NOT done in the handler: enumeration. Addressing a device and
 *    reading its descriptors means control transfers with real waits, so
 *    usb_core.c defers root-port changes to its kworker.  Its bounded root
 *    watcher also recovers a missed Port Status Change interrupt without
 *    doing descriptor or transfer work in timer context.
 *
 * 2. WAS: DMA IS IDENTITY-MAPPED PHYSICAL MEMORY.
 *    NOW: coherent DMA allocations keep a physmap CPU pointer and an independent
 *    device address, negotiated from AC64. The page geometry below still applies.
 *    Historical allocation description: c/drivers/virtio/virtio.c hands
 *    pmm_alloc() frames straight to the device, which works because boot.asm
 *    identity-maps the first 1 GiB and every frame the PMM hands out lives
 *    there. This does the same. It matters more here than for virtio: xHCI's
 *    contexts and rings have alignment AND boundary requirements (Table 6-1) --
 *    64-byte alignment for contexts and the ERST, 16-byte for TRBs, and a ring
 *    segment may not cross a 64 KiB boundary. A 4 KiB page satisfies all of them
 *    by construction, so every structure here gets its own page (or a quarter of
 *    one, at a 1 KiB offset, which is still 64-byte aligned and still inside the
 *    same 64 KiB). Nothing is kmalloc'd for the hardware to touch, because
 *    kmalloc makes no alignment promise at all.
 */

#include <stdint.h>
#include <stddef.h>
#include "xhci.h"
#include "usb.h"
#include "driver.h"
#include "pci.h"
#include "vmm.h"
#include "dma.h"
#include "kheap.h"
#include "kprintf.h"
#include "pit.h"

void *memset(void *, int, size_t);

struct xhci g_xhci;
static uint8_t *ring_pool;
static int ring_pool_used;

/* ------------------------------------------------------------- MMIO --- */

/* Every xHCI register is read and written as a DWORD. The narrower accessors
 * that used to be here are gone on purpose: CAPLENGTH and HCIVERSION share a
 * dword, and controllers exist that only decode the 32-bit access. */
static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline void w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
/* Written as two dwords, low first. The spec permits a 64-bit access only when
 * HCCPARAMS1.AC64 says so, and every controller accepts the dword pair; the
 * ORDER is the part that matters, because a controller latches the register on
 * the high write. */
static inline void w64(volatile uint8_t *b, int o, uint64_t v)
{
    w32(b, o, (uint32_t)v);
    w32(b, o + 4, (uint32_t)(v >> 32));
}
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* Rings and contexts are persistent controller-owned allocations. The DMA
 * ledger retains every page, including shared ring segments and scratchpads.
 * They can be released only after HCHalted is observed. */
static void *dma_page(void)
{
    struct dma_buffer *b = dma_alloc_coherent(&g_xhci.dma, 4096, 4096, 65536);
    if (!b) return NULL;
    if (!dma_buffer_submit(b)) { dma_free_coherent(b); return NULL; }
    return b->cpu;
}

/* Setup-only translation within our allocation ledger. Runtime endpoints and
 * rings retain device addresses, so the interrupt path never walks this list. */
static uint64_t dma_address(const void *cpu)
{
    uintptr_t p = (uintptr_t)cpu;
    for (struct dma_buffer *b = g_xhci.dma.buffers; b; b = b->next) {
        uintptr_t base = (uintptr_t)b->cpu;
        if (p >= base && p - base < b->size)
            return dma_addr_value(dma_addr_add(b->dma, p - base));
    }
    return 0;
}

/* Wait with a real time base when there is one. During bring-up the caller runs
 * on a kernel thread with interrupts on and the PIT ticking, so timer_ms()
 * moves; the spin cap is the backstop for the case where it does not (an early
 * call, a wedged timer) so a dead controller cannot hang the boot. */
static int wait_ms_expired(uint64_t t0, unsigned ms, long *spins)
{
    if (++*spins > 200000000L) return 1;
    return (timer_ms() - t0) > ms;
}

/* ------------------------------------------------ PCI/DMA ownership --- */

/* PCI Command is a 16-bit register adjacent to write-one-to-clear Status, so
 * every transition uses the native 16-bit accessor and is read back exactly.
 * The quiet value keeps BAR0 decodable while suppressing both DMA and legacy
 * INTx.  Bus mastering is a separate, final transition after our rings exist. */
static int pci_command_set(struct xhci *x, uint16_t wanted)
{
    if (!x->pci) return -1;
    pci_cfg_write16(x->pci->bus, x->pci->slot, x->pci->func,
                    PCI_CFG_COMMAND, wanted);
    return pci_cfg_read16(x->pci->bus, x->pci->slot, x->pci->func,
                          PCI_CFG_COMMAND) == wanted ? 0 : -1;
}

static void xhci_forget(struct xhci *x)
{
    ring_pool = NULL;
    ring_pool_used = 0;
    memset(x, 0, sizeof *x);
}

static void xhci_quarantine(struct xhci *x, const char *why)
{
    x->up = 0;
    x->quarantined = 1;
    if (x->dma.buffers) {
        dma_device_quarantine(&x->dma);
    } else {
        x->dma.blocked = 1;
    }
    kprintf("[xhci] %s: controller/DMA quarantined\n", why);
}

/* Used only before MMIO ownership has changed.  If the original Command value
 * cannot be restored, retain the controller identity permanently.  A best
 * effort quiet write contains DMA, but an unverifiable config transaction is
 * never grounds for another probe. */
static int restore_command_or_quarantine(struct xhci *x)
{
    if (pci_command_set(x, x->pci_command_old) == 0) {
        xhci_forget(x);
        return -1;
    }
    (void)pci_command_set(x, x->pci_command_quiet);
    xhci_quarantine(x, "PCI Command restore was not acknowledged");
    return -1;
}

/* The three observations are deliberately independent.  HCHalted alone does
 * not prove that software's Run/Stop request stuck, and CRCR.CRR exposes a
 * command ring that hardware still considers running. */
static int controller_stop_confirmed(struct xhci *x)
{
    if (!x->op) return -1;
    if (x->rt) w32(x->rt, XRT_IR0 + XIR_IMAN, 0);
    uint32_t cmd = r32(x->op, XOP_USBCMD);
    w32(x->op, XOP_USBCMD, cmd & ~(CMD_RS | CMD_INTE));
    uint64_t t0 = timer_ms(); long spins = 0;
    for (;;) {
        uint32_t live_cmd = r32(x->op, XOP_USBCMD);
        uint32_t sts = r32(x->op, XOP_USBSTS);
        uint32_t crcr = r32(x->op, XOP_CRCR);
        if (!(live_cmd & CMD_RS) && (sts & STS_HCH) && !(crcr & CRCR_CRR))
            return 0;
        if (wait_ms_expired(t0, 200, &spins)) return -1;
    }
}

static void release_dma(struct xhci *x)
{
    dma_device_quiesced(&x->dma);
    while (x->dma.buffers) {
        if (dma_free_coherent(x->dma.buffers) != 0) break;
    }
}

/* Once our DMA addresses have been published, memory is returned only after
 * both controller stop and PCI bus-master isolation are read back. */
static int stop_isolate_or_quarantine(struct xhci *x, const char *why)
{
    int stopped = controller_stop_confirmed(x) == 0;
    int isolated = pci_command_set(x, x->pci_command_quiet) == 0;
    if (!stopped || !isolated) {
        xhci_quarantine(x, why);
        return -1;
    }
    release_dma(x);
    xhci_forget(x);
    return 0;
}

/* --------------------------------------------- extended capabilities --- */

/* 7.1: the xHCI Extended Capabilities are a linked list in MMIO, NOT in PCI
 * config space, rooted at HCCPARAMS1[31:16] as a DWORD offset from the
 * capability base. Two of them matter here.
 *
 * ID 1, USB Legacy Support (7.1.1), is THE reason a USB keyboard and a PS/2
 * keyboard do not both deliver the same keystroke on real hardware. Before an
 * OS driver exists, firmware makes a USB keyboard look like a PS/2 one by
 * trapping xHCI accesses into SMM and feeding the 8042. If we start driving the
 * controller without taking ownership, the firmware keeps doing that and every
 * key arrives twice -- once through this driver and once through
 * c/drivers/char/keyboard.c. The handoff below is the protocol that stops it:
 * set the OS-owned semaphore, wait for firmware to drop the BIOS-owned one,
 * then disable every SMI it had enabled. */
static int legacy_handoff(struct xhci *x, uint64_t bar_size)
{
    uint32_t hcc = r32(x->cap, XCAP_HCCPARAMS1);
    uint32_t off = HCC1_XECP(hcc) * 4;
    if (!off) return 0;

    uint32_t seen[64];
    int nseen = 0;

    for (int guard = 0; guard < 64 && off; guard++) {
        if ((off & 3u) || (bar_size && ((uint64_t)off + 8 > bar_size))) {
            kprintf("[xhci] malformed extended capability offset; refusing controller\n");
            return -1;
        }
        for (int i = 0; i < nseen; i++) if (seen[i] == off) {
            kprintf("[xhci] cyclic extended capability list; refusing controller\n");
            return -1;
        }
        seen[nseen++] = off;
        volatile uint8_t *c = x->cap + off;
        uint32_t hdr = r32(c, 0);
        uint8_t id = hdr & 0xFF;
        uint8_t next = (hdr >> 8) & 0xFF;

        if (id == 1) {
            /* Claim OS ownership even if BIOS already dropped its bit: the
             * semaphore is also our persistent statement to SMM. */
            w32(c, 0, hdr | (1u << 24));            /* HC OS Owned Semaphore */
            if (hdr & (1u << 16)) {                 /* HC BIOS Owned Semaphore */
                uint64_t t0 = timer_ms(); long spins = 0;
                while ((r32(c, 0) & (1u << 16)) && !wait_ms_expired(t0, 1000, &spins))
                    ;
            }
            uint32_t owned = r32(c, 0);
            if ((owned & (1u << 16)) || !(owned & (1u << 24))) {
                kprintf("[xhci] BIOS ownership did not release; refusing controller\n");
                return -1;
            }
            kprintf("[xhci] BIOS handoff complete (USB legacy emulation off)\n");
            /* USBLEGCTLSTS: clear every SMI enable, and write the RW1C status
             * bits back to acknowledge whatever is already pending. */
            uint32_t ctl = r32(c, 4);
            w32(c, 4, ctl & 0xE0000000u);
        }
        if (id == 2) {
            /* Supported Protocol (7.2). Purely informational here, but knowing
             * which root ports are USB2 and which are USB3 is the difference
             * between "port 5 is dead" and "port 5 is the SuperSpeed half of
             * port 1 and nothing is plugged into it". */
            uint32_t d2 = r32(c, 8);
            kprintf("[xhci] protocol USB %d.%d on ports %d..%d\n",
                    (int)((hdr >> 24) & 0xFF), (int)((hdr >> 16) & 0xFF),
                    (int)(d2 & 0xFF), (int)((d2 & 0xFF) + ((d2 >> 8) & 0xFF) - 1));
        }
        if (!next) return 0;
        if (guard == 63) {
            kprintf("[xhci] extended capability chain is too long; refusing controller\n");
            return -1;
        }
        if (off > UINT32_MAX - (uint32_t)next * 4u) return -1;
        off += (uint32_t)next * 4u;
    }
    return -1;
}

/* -------------------------------------------------------- contexts --- */

/* 6.2.2 / 6.2.3: a context is 32 or 64 bytes depending on HCCPARAMS1.CSZ, and
 * getting that wrong puts every field at half or double its offset -- the
 * controller then reads the Slot Context's speed out of the middle of the
 * Endpoint Context and rejects Address Device with a Parameter Error. */
static uint32_t *slot_ctx(struct xhci *x, uint8_t *ctx_base, int input)
{
    return (uint32_t *)(ctx_base + (input ? x->ctxsize : 0));
}
static uint32_t *ep_ctx(struct xhci *x, uint8_t *ctx_base, int dci, int input)
{
    /* Device Context: [slot][ep1][ep2]...  Input Context: [control][slot][ep1]... */
    int index = input ? dci + 1 : dci;
    return (uint32_t *)(ctx_base + (size_t)index * x->ctxsize);
}
static uint32_t *input_ctrl_ctx(uint8_t *ctx_base) { return (uint32_t *)ctx_base; }

/* --------------------------------------------------------- the rings --- */

/* One page holds four 64-TRB ring segments. Handing out quarter pages keeps a
 * device with several endpoints from burning a frame each while still meeting
 * the 16-byte alignment and the "may not cross a 64 KiB boundary" rule. */
static struct trb *ring_segment(void)
{
    if (!ring_pool || ring_pool_used == 4) {
        ring_pool = dma_page();
        ring_pool_used = 0;
        if (!ring_pool) return NULL;
    }
    return (struct trb *)(ring_pool + (size_t)(ring_pool_used++) * (XHCI_RING_TRBS * 16));
}

static void ring_doorbell(struct xhci *x, int slot, uint32_t target)
{
    dma_wmb();
    x->db[slot] = target;
    barrier();
}

/* ------------------------------------------------ event ring drain --- */

static void ep_push_event(struct xhci_ep *ep, const struct trb *e)
{
    uint8_t nt = (uint8_t)((ep->ev_tail + 1) % XHCI_EVQ);
    if (nt == ep->ev_head) return;      /* nobody is draining; drop the oldest news */
    ep->ev[ep->ev_tail] = *e;
    ep->ev_tail = nt;
}

static int ep_pop_event(struct xhci_ep *ep, struct trb *out)
{
    if (ep->ev_head == ep->ev_tail) return 0;
    *out = ep->ev[ep->ev_head];
    ep->ev_head = (uint8_t)((ep->ev_head + 1) % XHCI_EVQ);
    return 1;
}

void xhci_events(void)
{
    IO_GUARD(&xhci_events_gate);
    struct xhci *x = &g_xhci;
    if (!x->up) return;

    /* 4.17.5: acknowledge the interrupt BEFORE draining, not after. Both bits
     * are write-1-to-clear, and clearing them after the drain loses any event
     * the controller posted while we were in it -- which on a keyboard is a
     * keystroke that never arrives and never will, because the next interrupt
     * only comes when something else happens. */
    uint32_t sts = r32(x->op, XOP_USBSTS);
    if (sts & STS_EINT) w32(x->op, XOP_USBSTS, STS_EINT);
    uint32_t iman = r32(x->rt, XRT_IR0 + XIR_IMAN);
    if (iman & 1) w32(x->rt, XRT_IR0 + XIR_IMAN, iman);

    struct trb e;
    int drained = 0;
    while (xring_pop(&x->ev, &e)) {
        x->events++;
        drained++;
        switch (TRB_TYPE(e.control)) {
        case TRB_TRANSFER_EVENT: {
            int slot = (int)TRB_EV_SLOT(e.control);
            int dci  = (int)TRB_EV_EPID(e.control);
            x->transfers++;
            if (slot >= 1 && slot <= XHCI_MAX_SLOTS && dci >= 1 && dci < 32) {
                struct xhci_ep *ep = x->slot[slot].ep[dci];
                if (ep) {
                    /* A transfer event retires exactly one TRB of the ring. */
                    xring_complete(&ep->ring, 1);
                    ep_push_event(ep, &e);
                }
            }
            if (TRB_CC(e.status) != CC_SUCCESS && TRB_CC(e.status) != CC_SHORT_PACKET)
                x->errors++;
            break;
        }
        case TRB_CMD_COMPLETION:
            xring_complete(&x->cmd, 1);
            if (x->cmd_want && e.param == x->cmd_want) {
                x->cmd_cc = (int)TRB_CC(e.status);
                x->cmd_slot = (int)TRB_EV_SLOT(e.control);
                __atomic_store_n(&x->cmd_ready, 1, __ATOMIC_RELEASE);
            }
            break;
        case TRB_PORT_STATUS:
            x->port_changes++;
            break;
        default:
            break;
        }
        if (drained > 256) break;    /* bounded: never spin here forever */
    }

    if (drained) {
        /* 4.9.4: hand the dequeue pointer back and clear Event Handler Busy.
         * Skipping this is survivable for a while and then is not: the
         * controller stops raising events once it thinks the ring is full. */
        w64(x->rt, XRT_IR0 + XIR_ERDP, xring_deq_ptr(&x->ev) | ERDP_EHB);
    }
}

/* ------------------------------------------------------- commands --- */

static int xhci_cmd(struct xhci *x, uint64_t param, uint32_t status, uint32_t control,
                    int *slot_out)
{
    if (!x->up || x->dma.blocked) return -1;
    uint64_t flags = io_lock_enter(&xhci_events_gate);
    x->cmd_ready = 0;
    x->cmd_cc = 0;
    x->cmd_slot = 0;
    uint64_t trb = xring_push(&x->cmd, param, status, control);
    if (!trb) { io_lock_leave(&xhci_events_gate, flags); kprintf("[xhci] command ring full\n"); return -1; }
    x->cmd_want = trb;
    io_lock_leave(&xhci_events_gate, flags);

    ring_doorbell(x, 0, 0);            /* doorbell 0, target 0 = command ring */

    uint64_t t0 = timer_ms(); long spins = 0;
    while (!__atomic_load_n(&x->cmd_ready, __ATOMIC_ACQUIRE)) {
        xhci_events();
        uint32_t sts=r32(x->op,XOP_USBSTS);
        if (sts & (STS_HSE|STS_HCE)) {
            kprintf("[xhci] controller error during command usbsts=%x\n",sts);
            { IO_GUARD(&xhci_events_gate); x->cmd_want = 0; }
            return -1;
        }
        if (wait_ms_expired(t0, 1000, &spins)) {
            kprintf("[xhci] command %d timed out usbcmd=%x usbsts=%x iman=%x crcr=%x "
                    "events=%lu ports=%lu evdeq=%u evcycle=%u cmdpending=%u\n",
                    (int)TRB_TYPE(control),r32(x->op,XOP_USBCMD),r32(x->op,XOP_USBSTS),
                    r32(x->rt,XRT_IR0+XIR_IMAN),r32(x->op,XOP_CRCR),x->events,
                    x->port_changes,x->ev.deq,x->ev.cycle,x->cmd.pending);
            { IO_GUARD(&xhci_events_gate); x->cmd_want = 0; }
            return -1;
        }
    }
    IO_GUARD(&xhci_events_gate);
    x->cmd_want = 0;
    if (slot_out) *slot_out = x->cmd_slot;
    return x->cmd_cc;
}

/* ----------------------------------------------------------- init --- */

static void portsc_write(struct xhci *x, int port, uint32_t set);

int xhci_init(struct device *dev)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (x->pci || x->cap) return -1; /* includes a quarantined controller */

    /* The device model found this controller by CLASS -- 0x0C/0x03/0x30, "USB
     * controller, xHCI programming interface" -- so no vendor:device ID appears
     * anywhere in this driver and it binds to any xHCI, not only to the two
     * QEMU emulates.  BAR decoding is enabled first with bus mastering forced
     * clear; dev_bar_map() sizes and identity-maps BAR0 uncached. */
    x->pci = dev;
    x->pci_command_old = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                        PCI_CFG_COMMAND);
    if (x->pci_command_old == UINT16_MAX) {
        kprintf("[xhci] PCI Command is unreadable\n");
        xhci_forget(x);
        return -1;
    }
    x->pci_command_quiet = (uint16_t)((x->pci_command_old | PCI_CMD_MEM |
                                      PCI_CMD_INTX_DIS) & ~PCI_CMD_MASTER);
    if (pci_command_set(x, x->pci_command_quiet) != 0) {
        kprintf("[xhci] MEM-only PCI Command was not acknowledged\n");
        return restore_command_or_quarantine(x);
    }
    uint64_t base = dev_bar_map(dev, 0);
    if (!base) {
        kprintf("[xhci] BAR0 is absent or not memory-mapped\n");
        return restore_command_or_quarantine(x);
    }

    x->cap = (volatile uint8_t *)(uintptr_t)base;
    /* CAPLENGTH and HCIVERSION share one dword and some controllers only decode
     * the 32-bit access, so read the dword and split it rather than issuing a
     * byte and a word read (5.3.1). */
    uint32_t cap0 = r32(x->cap, 0);
    uint8_t caplen = (uint8_t)(cap0 & 0xFF);
    uint16_t ver = (uint16_t)(cap0 >> 16);
    x->op = x->cap + caplen;
    x->rt = x->cap + (r32(x->cap, XCAP_RTSOFF) & ~0x1Fu);
    x->db = (volatile uint32_t *)(x->cap + (r32(x->cap, XCAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = r32(x->cap, XCAP_HCSPARAMS1);
    uint32_t hcs2 = r32(x->cap, XCAP_HCSPARAMS2);
    uint32_t hcc1 = r32(x->cap, XCAP_HCCPARAMS1);
    x->maxslots = HCS1_MAXSLOTS(hcs1);
    x->maxports = HCS1_MAXPORTS(hcs1);
    x->ctxsize  = HCC1_CSZ(hcc1) ? 64 : 32;
    dma_device_init(&x->dma, "xhci", HCC1_AC64(hcc1) ? DMA_MASK_64 : DMA_MASK_32);
    if (x->maxslots > XHCI_MAX_SLOTS) x->maxslots = XHCI_MAX_SLOTS;
    if (x->maxports > 64) x->maxports = 64;

    kprintf("[xhci] %s %04x:%04x at %p: v%d.%d%d, %d ports, %d slots, %d-byte contexts\n",
            dev->name, dev->vendor, dev->device, (void *)(uintptr_t)base,
            (ver >> 8) & 0xFF, (ver >> 4) & 0xF, ver & 0xF,
            x->maxports, x->maxslots, x->ctxsize);

    if (legacy_handoff(x, dev->res[0].size) != 0) {
        /* We may have raised the OS-owned semaphore.  Firmware ownership is
         * now ambiguous, so keep BME clear and refuse every later probe. */
        (void)pci_command_set(x, x->pci_command_quiet);
        xhci_quarantine(x, "firmware ownership handoff failed");
        return -1;
    }

    /* 4.2 step 1: halt, then reset. A controller left running by firmware with
     * its own rings still programmed will DMA into memory we have since handed
     * to something else. */
    if (controller_stop_confirmed(x) != 0) {
        (void)pci_command_set(x, x->pci_command_quiet);
        xhci_quarantine(x, "firmware controller halt was not acknowledged");
        return -1;
    }
    w32(x->op, XOP_USBCMD, CMD_HCRST);
    {
        uint64_t t0 = timer_ms(); long spins = 0;
        /* Both conditions, not just HCRST: 4.2 says the controller may clear
         * HCRST before it is actually usable and signals that with CNR. */
        while ((r32(x->op, XOP_USBCMD) & CMD_HCRST) || (r32(x->op, XOP_USBSTS) & STS_CNR))
            if (wait_ms_expired(t0, 1000, &spins)) {
                (void)pci_command_set(x, x->pci_command_quiet);
                xhci_quarantine(x, "controller reset was not acknowledged");
                return -1;
            }
    }
    if (controller_stop_confirmed(x) != 0) {
        (void)pci_command_set(x, x->pci_command_quiet);
        xhci_quarantine(x, "post-reset halt state was not stable");
        return -1;
    }

    /* 4.2 step 3: Device Context Base Address Array. Entry 0 is not a device --
     * it is the scratchpad buffer array pointer (4.20). */
    x->dcbaa = dma_page();
    if (!x->dcbaa) goto dma_fail;

    int nscratch = (int)HCS2_SPB(hcs2);
    if (nscratch > 0) {
        if (nscratch > 512) nscratch = 512;
        x->scratch_arr = dma_page();
        if (!x->scratch_arr) goto dma_fail;
        for (int i = 0; i < nscratch; i++) {
            void *p = dma_page();
            if (!p) goto dma_fail;
            x->scratch_arr[i] = dma_address(p);
        }
        x->dcbaa[0] = dma_address(x->scratch_arr);
        kprintf("[xhci] %d scratchpad buffers\n", nscratch);
    }

    w32(x->op, XOP_CONFIG, (uint32_t)x->maxslots);       /* MaxSlotsEn */
    w64(x->op, XOP_DCBAAP, dma_address(x->dcbaa));

    /* 4.2 step 5: the command ring. CRCR carries the Ring Cycle State in bit 0,
     * which must equal the producer cycle we start at. */
    struct trb *cmdseg = ring_segment();
    if (!cmdseg) goto dma_fail;
    xring_init(&x->cmd, cmdseg, dma_address(cmdseg), XHCI_RING_TRBS, 1);
    w64(x->op, XOP_CRCR, xring_base_dcs(&x->cmd));

    /* 4.2 step 6: the event ring. One segment, described by a one-entry Event
     * Ring Segment Table. ERSTSZ is published first. ERSTBA's high-dword write
     * may make a controller fetch the table immediately, so PCI bus mastering
     * must be enabled before that latch. */
    struct trb *evseg = ring_segment();
    if (!evseg) goto dma_fail;
    xring_init(&x->ev, evseg, dma_address(evseg), XHCI_RING_TRBS, 0);
    x->erst = dma_page();
    if (!x->erst) goto dma_fail;
    *(volatile uint64_t *)(x->erst + 0) = dma_address(evseg);
    *(volatile uint32_t *)(x->erst + 8) = XHCI_RING_TRBS;
    *(volatile uint32_t *)(x->erst + 12) = 0;

    w32(x->rt, XRT_IR0 + XIR_ERSTSZ, 1);
    w32(x->rt, XRT_IR0 + XIR_IMOD, 0);
    /* Interrupter Enable stays CLEAR until a vector has actually been wired
     * (xhci_irq_enable below). Setting it first would let the controller raise
     * an interrupt on a vector with no IDT gate behind it. */
    w32(x->rt, XRT_IR0 + XIR_IMAN, 0);

    /* BME is enabled only now: firmware released ownership, reset/halt were
     * acknowledged, and every controller-visible DMA object is initialized.
     * ERSTBA is intentionally still unlatched: QEMU and physical controllers
     * are allowed to fetch the segment table on its high-dword write. */
    dma_wmb();
    if (pci_command_set(x, (uint16_t)(x->pci_command_quiet | PCI_CMD_MASTER)) != 0) {
        kprintf("[xhci] bus-master enable was not acknowledged\n");
        goto dma_fail;
    }

    /* Publish the ERST base before its dequeue pointer. This follows 4.2's
     * initialization order and guarantees the immediate table fetch above can
     * actually reach memory. */
    w64(x->rt, XRT_IR0 + XIR_ERSTBA, dma_address(x->erst));
    w64(x->rt, XRT_IR0 + XIR_ERDP, dma_address(evseg));
    /* A same-device USBSTS read is the posted-write flush and HCE/HSE
     * observation point for the event-ring publication above. */
    barrier();
    uint32_t ring_sts=r32(x->op,XOP_USBSTS);
    if (ring_sts & (STS_HSE|STS_HCE)) {
        kprintf("[xhci] controller rejected event-ring publication usbsts=%x\n",
                ring_sts);
        goto dma_fail;
    }

    /* 4.2 step 8: run. INTE follows IMAN.IE, for the same reason. */
    w32(x->op, XOP_USBCMD, CMD_RS);
    {
        uint64_t t0 = timer_ms(); long spins = 0;
        for (;;) {
            uint32_t sts=r32(x->op,XOP_USBSTS);
            if (sts & (STS_HSE|STS_HCE)) {
                kprintf("[xhci] controller error while starting usbsts=%x\n",sts);
                goto dma_fail;
            }
            if (!(sts & STS_HCH)) break;
            if (wait_ms_expired(t0, 200, &spins)) { kprintf("[xhci] did not start\n"); goto dma_fail; }
        }
    }

    x->up = 1;
    kprintf("[xhci] running\n");
    return 0;

dma_fail:
    (void)stop_isolate_or_quarantine(x,
        "failed initialization could not prove stop and bus-master isolation");
    return -1;
}

/* Called once a CPU vector is behind the interrupter. Order matters: the
 * per-interrupter enable, then the global one. */
void xhci_irq_enable(void)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (!x->up) return;
    w32(x->rt, XRT_IR0 + XIR_IMAN, 2);                       /* IE, IP clear */
    w32(x->op, XOP_USBCMD, r32(x->op, XOP_USBCMD) | CMD_INTE);
}

/* ------------------------------------------------------- root hub --- */

int xhci_port_count(void) {
    IO_DOMAIN_GUARD(&xhci_owner); return g_xhci.up ? g_xhci.maxports : 0; }

int xhci_port_connected(int port)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (!x->up || port < 1 || port > x->maxports) return 0;
    return (r32(x->op, XOP_PORTSC(port)) & PORTSC_CCS) ? 1 : 0;
}

/* A Port Status Change Event can also report reset/link changes caused by our
 * own enumeration.  Only CSC authorizes detach/rebind; treating every port
 * event as unplug would make the reset below recursively replace its device.
 * This check runs at IRQ tail and intentionally only reads MMIO. */
int xhci_root_change_pending(void)
{
    struct xhci *x=&g_xhci;
    if (!x->up) return 0;
    for (int p=1;p<=x->maxports;p++)
        if (r32(x->op,XOP_PORTSC(p))&PORTSC_CSC) return 1;
    return 0;
}

int xhci_port_changed(int port,int *connected)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x=&g_xhci;
    if (!connected || !x->up || port<1 || port>x->maxports) return -1;
    uint32_t v=r32(x->op,XOP_PORTSC(port));
    if (!(v&PORTSC_CSC)) return 0;
    *connected=!!(v&PORTSC_CCS);
    /* CSC is RW1C.  portsc_write also masks PED and all unrelated change bits,
     * so acknowledging a cable transition cannot disable the port or consume
     * a reset/link event another path still needs. */
    portsc_write(x,port,PORTSC_CSC);
    return 1;
}

/* Read-modify-write PORTSC without destroying it. PED and the seven change bits
 * are RW1C: writing back what you read DISABLES the port and acknowledges
 * changes you never handled. This mask is the whole trick and every xHCI driver
 * has it. */
static void portsc_write(struct xhci *x, int port, uint32_t set)
{
    uint32_t v = r32(x->op, XOP_PORTSC(port));
    v &= ~(PORTSC_PED | PORTSC_RW1C);
    w32(x->op, XOP_PORTSC(port), v | set);
}

int xhci_port_reset(int port, int *speed)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (!x->up || port < 1 || port > x->maxports) return -1;

    uint32_t v = r32(x->op, XOP_PORTSC(port));
    if (!(v & PORTSC_CCS)) return -1;

    if (!(v & PORTSC_PP)) {                    /* power the port if it is not */
        portsc_write(x, port, PORTSC_PP);
        uint64_t t0 = timer_ms(); long spins = 0;
        while (!wait_ms_expired(t0, 20, &spins)) ;
        v = r32(x->op, XOP_PORTSC(port));
    }

    /* A SuperSpeed port trains and enables itself on connect; PED is already
     * set and asserting PR on it is at best a no-op. A USB2 port is NOT enabled
     * until the host drives a reset, and the device has no address until it is
     * (USB 2.0 9.1.1). Distinguishing the two is what makes one code path work
     * for both, which is the whole reason xHCI exists. */
    if (!(v & PORTSC_PED)) {
        portsc_write(x, port, PORTSC_PR);
        uint64_t t0 = timer_ms(); long spins = 0;
        for (;;) {
            v = r32(x->op, XOP_PORTSC(port));
            if (v & PORTSC_PRC) break;
            if (wait_ms_expired(t0, 500, &spins)) {
                kprintf("[xhci] port %d reset timed out (portsc %x)\n", port, v);
                return -1;
            }
        }
        /* Acknowledge the change bits we just consumed -- they are RW1C, and a
         * PRC left set makes the next reset look like it completed instantly. */
        w32(x->op, XOP_PORTSC(port),
            (v & ~(PORTSC_PED | PORTSC_RW1C)) | PORTSC_PRC | PORTSC_CSC);

        /* USB 2.0 7.1.7.5: a device is not required to respond for 10 ms after
         * reset. Asking earlier is the classic "device descriptor read fails
         * once, works on retry" bug. */
        t0 = timer_ms(); spins = 0;
        while (!wait_ms_expired(t0, 20, &spins)) ;
        v = r32(x->op, XOP_PORTSC(port));
    } else {
        w32(x->op, XOP_PORTSC(port), (v & ~(PORTSC_PED | PORTSC_RW1C)) | PORTSC_CSC);
        v = r32(x->op, XOP_PORTSC(port));
    }

    if (!(v & PORTSC_PED)) {
        kprintf("[xhci] port %d did not enable (portsc %x)\n", port, v);
        return -1;
    }
    if (speed) *speed = (int)PORTSC_SPEED(v);
    return 0;
}

/* ------------------------------------------------ slots + contexts --- */

int xhci_enable_slot(void)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    int slot = 0;
    int cc = xhci_cmd(x, 0, 0, TRB_SET_TYPE(TRB_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS) { kprintf("[xhci] Enable Slot failed cc=%d\n", cc); return -1; }
    if (slot < 1 || slot > XHCI_MAX_SLOTS) {
        kprintf("[xhci] controller returned slot %d, out of our range\n", slot);
        return -1;
    }
    if (x->slot[slot].used) return -1;
    x->slot[slot].used = 1; /* even an allocation failure must disable this slot */
    return slot;
}

static struct xhci_ep *ep_alloc(struct xhci *x, int slot, int dci)
{
    if (x->slot[slot].ep[dci]) return x->slot[slot].ep[dci];
    struct xhci_ep *ep = kmalloc(sizeof *ep);
    if (!ep) return NULL;
    memset(ep, 0, sizeof *ep);
    /* Slot rings get their own pages. Sharing a controller-wide quarter-page
     * pool would prevent reclaiming this slot while other devices stay live. */
    ep->ring_dma = dma_alloc_coherent(&x->dma, 4096, 4096, 65536);
    ep->payload_dma = dma_alloc_coherent(&x->dma, 4096, 4096, 65536);
    if (!ep->ring_dma || !ep->payload_dma) {
        if (ep->ring_dma) dma_free_coherent(ep->ring_dma);
        if (ep->payload_dma) dma_free_coherent(ep->payload_dma);
        kfree(ep); return NULL; /* no address has been published yet */
    }
    xring_init(&ep->ring, ep->ring_dma->cpu, dma_addr_value(ep->ring_dma->dma), XHCI_RING_TRBS, 1);
    ep->buf = ep->payload_dma->cpu;
    ep->buf_dma = dma_addr_value(ep->payload_dma->dma);
    dma_buffer_submit(ep->ring_dma);
    dma_buffer_submit(ep->payload_dma);
    ep->buflen = dci == 1 ? 4096 : 64;
    ep->dci = (uint8_t)dci;
    ep->used = 1;
    { IO_GUARD(&xhci_events_gate); x->slot[slot].ep[dci] = ep; }
    return ep;
}

/* 6.2.2: the default control endpoint's Max Packet Size is fixed by speed --
 * except at full speed, where it is 8, 16, 32 or 64 and the device only tells
 * you in a descriptor you must first read using... the max packet size. The way
 * out (USB 2.0 9.6.1) is to read the first 8 bytes with a guess of 8, which is
 * legal for every full-speed device, then correct it. */
static int default_max_packet(int speed)
{
    switch (speed) {
    case XSPEED_LOW:   return 8;
    case XSPEED_FULL:  return 8;
    case XSPEED_HIGH:  return 64;
    case XSPEED_SUPER: return 512;
    default:           return 8;
    }
}

int xhci_address_device(struct usb_device *d, int bsr)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    struct xhci_slot *s = &x->slot[d->slot];

    if (!s->dev_ctx) {
        s->dev_ctx_dma = dma_alloc_coherent(&x->dma, 4096, 4096, 65536);
        s->in_ctx_dma = dma_alloc_coherent(&x->dma, 4096, 4096, 65536);
        if (!s->dev_ctx_dma || !s->in_ctx_dma) {
            if (s->dev_ctx_dma) dma_free_coherent(s->dev_ctx_dma);
            if (s->in_ctx_dma) dma_free_coherent(s->in_ctx_dma);
            s->dev_ctx_dma = s->in_ctx_dma = NULL;
            return -1;
        }
        s->dev_ctx = s->dev_ctx_dma->cpu;
        s->in_ctx = s->in_ctx_dma->cpu;
        s->used = 1;
        s->dev = d;
        dma_buffer_submit(s->dev_ctx_dma);
        dma_buffer_submit(s->in_ctx_dma);
        x->dcbaa[d->slot] = dma_addr_value(s->dev_ctx_dma->dma);
    }
    struct xhci_ep *ep0 = ep_alloc(x, d->slot, 1);
    if (!ep0) return -1;

    memset(s->in_ctx, 0, 4096);
    uint32_t *icc = input_ctrl_ctx(s->in_ctx);
    icc[0] = 0;                       /* Drop Context flags: none */
    icc[1] = (1u << 0) | (1u << 1);   /* Add A0 (slot) + A1 (EP0) */

    uint32_t *sc = slot_ctx(x, s->in_ctx, 1);
    sc[0] = ((uint32_t)1 << 27)                      /* Context Entries = 1 */
          | ((uint32_t)d->speed << 20) | (d->route & 0xfffffu);
    sc[1] = ((uint32_t)d->port << 16);               /* Root Hub Port Number */
    /* USB2 LS/FS children below a HS hub require its SLOT ID, not USB address.
     * EHCI uses the USB address for the same TT topology; the core retains
     * the hub device so each backend selects the correct namespace. */
    sc[2] = d->tt_hub ? (uint32_t)d->tt_hub->slot | ((uint32_t)d->tt_port<<8) : 0;
    if (d->tt_multi) sc[0] |= 1u<<25;
    sc[3] = 0;

    uint32_t *ec = ep_ctx(x, s->in_ctx, 1, 1);
    ec[0] = 0;
    ec[1] = (3u << 1)                                /* CErr = 3 */
          | ((uint32_t)EP_TYPE_CONTROL << 3)
          | ((uint32_t)default_max_packet(d->speed) << 16);
    uint64_t deq = xring_base_dcs(&ep0->ring);
    ec[2] = (uint32_t)deq;
    ec[3] = (uint32_t)(deq >> 32);
    ec[4] = 8;                                       /* Average TRB Length */

    /* BSR = Block Set Address Request (4.6.5): address the slot without issuing
     * SET_ADDRESS on the wire. Some devices need the two-step form; QEMU does
     * not, and we use bsr=0. */
    uint32_t ctl = TRB_SET_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)d->slot << 24);
    if (bsr) ctl |= (1u << 9);
    int cc = xhci_cmd(x, dma_address(s->in_ctx), 0, ctl, NULL);
    if (cc != CC_SUCCESS) { kprintf("[xhci] Address Device failed cc=%d\n", cc); return -1; }

    /* The controller wrote the assigned address into the OUTPUT slot context. */
    d->addr = (uint8_t)(slot_ctx(x, s->dev_ctx, 0)[3] & 0xFF);
    return 0;
}

int xhci_set_ep0_packet(struct usb_device *d, int max_packet)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    struct xhci_slot *s = &x->slot[d->slot];

    memset(s->in_ctx, 0, 4096);
    uint32_t *icc = input_ctrl_ctx(s->in_ctx);
    icc[0] = 0;
    icc[1] = (1u << 1);                 /* Add A1 (EP0) only */

    uint32_t *ec = ep_ctx(x, s->in_ctx, 1, 1);
    ec[1] = (3u << 1) | ((uint32_t)EP_TYPE_CONTROL << 3) | ((uint32_t)max_packet << 16);

    int cc = xhci_cmd(x, dma_address(s->in_ctx), 0,
                      TRB_SET_TYPE(TRB_EVAL_CONTEXT) | ((uint32_t)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) { kprintf("[xhci] Evaluate Context failed cc=%d\n", cc); return -1; }
    return 0;
}

/* 6.2.3.6: the Interval field is a power of two in 125 us units, but bInterval
 * means different things at different speeds. High/SuperSpeed already give an
 * exponent (1..16, in 125 us units). Low/full speed give FRAMES (1..255 ms), so
 * the exponent is log2(bInterval) + 3. Getting this wrong does not fail loudly;
 * it makes the controller poll a keyboard 128x too slowly, which reads as a
 * keyboard that misses keys. */
static uint32_t interval_field(int speed, const struct usb_endpoint *ep)
{
    int b = ep->interval ? ep->interval : 1;
    if (speed == XSPEED_HIGH || speed == XSPEED_SUPER) {
        int v = b - 1;
        if (v < 0) v = 0;
        if (v > 15) v = 15;
        return (uint32_t)v;
    }
    int e = 0;
    while ((1 << (e + 1)) <= b && e < 10) e++;
    e += 3;
    if (e > 15) e = 15;
    return (uint32_t)e;
}

int xhci_configure_ep(struct usb_device *d, const struct usb_interface *iface)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    struct xhci_slot *s = &x->slot[d->slot];

    memset(s->in_ctx, 0, 4096);
    uint32_t *icc = input_ctrl_ctx(s->in_ctx);
    uint32_t add = 1u;                  /* A0: the slot context always comes along */
    int max_dci = 1;

    for (int i = 0; i < iface->n_ep; i++) {
        const struct usb_endpoint *e = &iface->ep[i];
        int dci = XHCI_DCI(e->addr);
        if (dci < 2 || dci > 31) continue;
        int in = USB_EP_IS_IN(e->addr);
        int xfer = USB_EP_XFER(e->attr);
        int type;
        switch (xfer) {
        case USB_XFER_INT:  type = in ? EP_TYPE_INT_IN : EP_TYPE_INT_OUT; break;
        case USB_XFER_BULK: type = in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT; break;
        default: continue;              /* isochronous: not supported */
        }
        struct xhci_ep *ep = ep_alloc(x, d->slot, dci);
        if (!ep) return -1;
        ep->xfer=(uint8_t)xfer;
        unsigned packet=e->max_packet & 0x7ff;
        if (!packet || packet>1024) return -1;
        ep->buflen=xfer==USB_XFER_BULK ? 4096 : (int)packet;

        uint32_t *ec = ep_ctx(x, s->in_ctx, dci, 1);
        ec[0] = xfer==USB_XFER_INT ? interval_field(d->speed, e) << 16 : 0;
        ec[1] = (3u << 1) | ((uint32_t)type << 3) | (packet << 16);
        uint64_t deq = xring_base_dcs(&ep->ring);
        ec[2] = (uint32_t)deq;
        ec[3] = (uint32_t)(deq >> 32);
        ec[4] = packet | (xfer==USB_XFER_INT ? packet<<16 : 0);

        add |= 1u << dci;
        if (dci > max_dci) max_dci = dci;
    }

    /* The slot context comes from the DEVICE context, not from zero: Configure
     * Endpoint replaces it wholesale, and a zeroed one loses the speed and root
     * port the controller needs to keep talking to the device. */
    uint32_t *dsc = slot_ctx(x, s->dev_ctx, 0);
    uint32_t *isc = slot_ctx(x, s->in_ctx, 1);
    /* Context Entries is the highest VALID DCI on the slot, across every
     * interface configured so far -- not just this one. Lowering it on the
     * second call would tell the controller that endpoints it is already
     * servicing do not exist. */
    int have = (int)(dsc[0] >> 27);
    if (have > max_dci) max_dci = have;
    isc[0] = (dsc[0] & 0x07FFFFFFu) | ((uint32_t)max_dci << 27);
    isc[1] = dsc[1];
    isc[2] = dsc[2];
    isc[3] = 0;

    icc[0] = 0;
    icc[1] = add;

    int cc = xhci_cmd(x, dma_address(s->in_ctx), 0,
                      TRB_SET_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) { kprintf("[xhci] Configure Endpoint failed cc=%d\n", cc); return -1; }
    return 0;
}

int xhci_configure_hub(struct usb_device *d,int ports,int multi_tt,int tt_think)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x=&g_xhci;
    if (!d || d->slot<1 || d->slot>XHCI_MAX_SLOTS || ports<1 || ports>255) return -1;
    struct xhci_slot *s=&x->slot[d->slot];
    if (!s->in_ctx || !s->dev_ctx) return -1;
    memset(s->in_ctx,0,4096);
    uint32_t *out=slot_ctx(x,s->dev_ctx,0), *in=slot_ctx(x,s->in_ctx,1);
    in[0]=out[0] | (1u<<26); /* Hub bit must precede addressing any child. */
    if (multi_tt) in[0]|=1u<<25; else in[0]&=~(1u<<25);
    in[1]=(out[1]&0x00ffffffu)|((uint32_t)ports<<24);
    in[2]=(out[2]&~(3u<<16))|((uint32_t)(tt_think&3)<<16);
    input_ctrl_ctx(s->in_ctx)[1]=1;
    int cc=xhci_cmd(x,dma_address(s->in_ctx),0,
        TRB_SET_TYPE(TRB_CONFIG_EP)|((uint32_t)d->slot<<24),NULL);
    if (cc!=CC_SUCCESS) kprintf("[xhci] configure hub slot=%d cc=%d\n",d->slot,cc);
    return cc==CC_SUCCESS?0:-1;
}

/* Only called after Disable Slot's success completion, which retires this
 * slot's DMA references without stopping the controller's other live slots. */
static void slot_dma_release(struct dma_buffer *b)
{
    if (!b) return;
    if (b->state == DMA_DEVICE_OWNED) dma_buffer_complete(b, b->token);
    dma_free_coherent(b);
}

void xhci_free_slot(int slot)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (slot < 1 || slot > XHCI_MAX_SLOTS || !x->slot[slot].used) return;
    if (xhci_cmd(x, 0, 0, TRB_SET_TYPE(TRB_DISABLE_SLOT) |
                 ((uint32_t)slot << 24), NULL) != CC_SUCCESS) {
        dma_device_quarantine(&x->dma);
        x->up = 0;
        return; /* retain contexts and buffers that hardware may still use */
    }
    struct xhci_slot retired;
    {
        IO_GUARD(&xhci_events_gate);
        x->dcbaa[slot] = 0;
        retired = x->slot[slot];
        memset(&x->slot[slot], 0, sizeof x->slot[slot]);
    }
    struct xhci_slot *s = &retired;
    for (int i = 1; i < 32; i++) {
        struct xhci_ep *ep = s->ep[i];
        if (!ep) continue;
        slot_dma_release(ep->ring_dma);
        slot_dma_release(ep->payload_dma);
        kfree(ep);
    }
    slot_dma_release(s->dev_ctx_dma);
    slot_dma_release(s->in_ctx_dma);
    memset(s, 0, sizeof *s);
}

/* ------------------------------------------------------- transfers --- */

/* Wait for the transfer event belonging to `want`, draining (and remembering)
 * any other completions on the same endpoint that arrive first -- which is
 * exactly what a short data stage produces. */
static int wait_transfer(struct xhci *x, struct xhci_ep *ep, uint64_t want,
                         int *residual, unsigned ms)
{
    uint64_t t0 = timer_ms();
    long spins = 0;
    for (;;) {
        xhci_events();
        struct trb e;
        for (;;) {
            int present;
            { IO_GUARD(&xhci_events_gate); present = ep_pop_event(ep, &e); }
            if (!present) break;
            if (TRB_CC(e.status) == CC_SHORT_PACKET && residual)
                *residual = (int)TRB_RESIDUAL(e.status);
            /* A stalled DATA/SETUP stage never reaches the status TRB whose
             * address the control caller awaits. Its error is terminal for
             * this serialized TD, even when e.param is not that status TRB. */
            if (TRB_CC(e.status)!=CC_SUCCESS && TRB_CC(e.status)!=CC_SHORT_PACKET)
                return (int)TRB_CC(e.status);
            if (e.param == want) {
                if (TRB_CC(e.status) == CC_SUCCESS || TRB_CC(e.status) == CC_SHORT_PACKET) {
                    if (TRB_CC(e.status) == CC_SHORT_PACKET && residual)
                        *residual = (int)TRB_RESIDUAL(e.status);
                    return CC_SUCCESS;
                }
                return (int)TRB_CC(e.status);
            }
        }
        if (r32(x->op, XOP_USBSTS) & STS_HSE) return -1;
        if (wait_ms_expired(t0, ms, &spins)) return -1;
    }
}

/* Stop/reset before changing a dequeue pointer. Reusing the bounce page after
 * a timeout without a confirmed stop lets the old request DMA over a new one.
 * If the controller cannot acknowledge retirement, keep its allocations and
 * refuse further traffic instead of returning a reusable buffer. */
static int recover_endpoint(struct usb_device *d,int dci)
{
    struct xhci *x=&g_xhci;
    struct xhci_slot *s=&x->slot[d->slot];
    struct xhci_ep *ep=s->ep[dci];
    unsigned state=ep_ctx(x,s->dev_ctx,dci,0)[0]&7;
    uint32_t target=((uint32_t)d->slot<<24)|((uint32_t)dci<<16);
    int cc=CC_SUCCESS;
    if (state==1) cc=xhci_cmd(x,0,0,TRB_SET_TYPE(15)|target,NULL); /* Stop Endpoint */
    if (state==2) cc=xhci_cmd(x,0,0,TRB_SET_TYPE(TRB_RESET_EP)|target,NULL);
    if (cc!=CC_SUCCESS || (state!=1 && state!=2 && state!=3)) goto fail;
    if (d->tt_hub && (dci==1 || ep->xfer==USB_XFER_BULK)) {
        uint8_t address=dci==1?0:(uint8_t)((dci/2)|((dci&1)?0x80:0));
        if (usb_clear_tt_buffer(d,address,dci==1?USB_XFER_CONTROL:USB_XFER_BULK)) goto fail;
    }
    uint64_t next=ep->ring.dma_base+(uint64_t)ep->ring.enq*sizeof(struct trb)|ep->ring.cycle;
    if (xhci_cmd(x,next,0,TRB_SET_TYPE(16)|target,NULL)!=CC_SUCCESS) goto fail; /* Set TR Dequeue */
    { IO_GUARD(&xhci_events_gate);
      ep->ring.pending=0; ep->ev_head=ep->ev_tail=0; ep->inflight=0; }
    return 0;
fail:
    { IO_GUARD(&xhci_events_gate); x->up=0; }
    dma_device_quarantine(&x->dma);
    return -1;
}

int xhci_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                 uint16_t idx, void *data, uint16_t len)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (!x->up || d->slot < 1 || d->slot > XHCI_MAX_SLOTS) return -1;
    struct xhci_ep *ep = x->slot[d->slot].ep[1];
    if (!ep) return -1;

    int in = (rt & USB_RT_DIR_IN) != 0;

    /* The data buffer the controller DMAs to is NOT the caller's: a caller's
     * buffer may be a kmalloc'd or stack address with no alignment guarantee,
     * and a control transfer must not straddle whatever the allocator felt like.
     * One page per endpoint, copied in and out. */
    if (!ep->buf) return -1;
    if (len > ep->buflen) return -1;
    if (len && !in && data) {
        const uint8_t *sp = data;
        for (int i = 0; i < len; i++) ep->buf[i] = sp[i];
    }

    /* 6.4.1.2.1 Setup Stage: the 8 setup bytes travel INSIDE the TRB
     * (Immediate Data), little-endian, in the order the wire wants them. */
    uint64_t setup = (uint64_t)rt
                   | ((uint64_t)req << 8)
                   | ((uint64_t)val << 16)
                   | ((uint64_t)idx << 32)
                   | ((uint64_t)len << 48);
    uint32_t trt = len ? (in ? TRT_IN : TRT_OUT) : TRT_NO_DATA;
    uint64_t ring_flags = io_lock_enter(&xhci_events_gate);
    if (xring_space(&ep->ring)<(len?3u:2u)) { io_lock_leave(&xhci_events_gate,ring_flags); return -1; }
    xring_push(&ep->ring, setup, 8, TRB_SET_TYPE(TRB_SETUP_STAGE) | TRB_IDT | (trt << 16));

    if (len)
        xring_push(&ep->ring, ep->buf_dma, len,
                   TRB_SET_TYPE(TRB_DATA_STAGE) | TRB_ISP | (in ? TRB_DIR_IN : 0));

    /* The Status Stage goes the OTHER way from the data, and is the TRB we ask
     * for an interrupt on -- it is the one that means "the transfer is over". */
    uint64_t st = xring_push(&ep->ring, 0, 0,
                             TRB_SET_TYPE(TRB_STATUS_STAGE) | TRB_IOC |
                             ((len && in) ? 0 : TRB_DIR_IN));
    io_lock_leave(&xhci_events_gate, ring_flags);
    if (!st) return -1;

    ring_doorbell(x, d->slot, 1);      /* DCI 1 = the default control endpoint */

    int residual = 0;
    int cc = wait_transfer(x, ep, st, &residual, 1000);
    if (cc != CC_SUCCESS) {
        recover_endpoint(d,1);
        return -1;
    }
    /* Only the final status has IOC. That event retires the entire control TD,
     * including Setup/Data TRBs which did not each request a completion. */
    { IO_GUARD(&xhci_events_gate); ep->ring.pending=0; }

    int got = len - residual;
    if (got < 0) got = 0;
    if (len && in && data) {
        uint8_t *dp = data;
        for (int i = 0; i < got; i++) dp[i] = ep->buf[i];
    }
    return got;
}

int xhci_bulk(struct usb_device *d,uint8_t ep_addr,void *data,uint32_t len)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x=&g_xhci;
    int dci=XHCI_DCI(ep_addr);
    if (!d || !x->up || d->slot<1 || d->slot>XHCI_MAX_SLOTS || dci<2 || dci>31 || (len&&!data)) return -1;
    struct xhci_ep *ep=x->slot[d->slot].ep[dci];
    if (!ep || ep->xfer!=USB_XFER_BULK) return -1;
    uint8_t *p=data;
    uint32_t done=0;
    do {
        uint32_t chunk=len-done;
        if (chunk>4096) chunk=4096; /* one DMA page never crosses a 64KiB boundary */
        if (!USB_EP_IS_IN(ep_addr)) for (uint32_t i=0;i<chunk;i++) ep->buf[i]=p[done+i];
        uint64_t trb;
        { IO_GUARD(&xhci_events_gate);
          trb=xring_push(&ep->ring,ep->buf_dma,chunk,TRB_SET_TYPE(TRB_NORMAL)|TRB_IOC|TRB_ISP); }
        if (!trb) return -1;
        ring_doorbell(x,d->slot,(uint32_t)dci);
        int residual=0;
        int cc=wait_transfer(x,ep,trb,&residual,2000);
        if (cc!=CC_SUCCESS) { recover_endpoint(d,dci); return -1; }
        if (residual<0 || (uint32_t)residual>chunk) return -1;
        uint32_t got=chunk-(uint32_t)residual;
        if (USB_EP_IS_IN(ep_addr)) for (uint32_t i=0;i<got;i++) p[done+i]=ep->buf[i];
        done+=got;
        if (got<chunk) break;
    } while(done<len);
    return (int)done;
}
int xhci_clear_halt(struct usb_device *d,uint8_t ep_addr)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    if (!d || !g_xhci.up || d->slot<1 || d->slot>XHCI_MAX_SLOTS) return -1;
    int dci=XHCI_DCI(ep_addr);
    if (dci<2 || dci>31 || !g_xhci.slot[d->slot].ep[dci]) return -1;
    if (recover_endpoint(d,dci)) return -1;
    /* xHCI 4.6.8.1: Reset Endpoint is legal only in Halted state. BOT reset
     * recovery also clears healthy pipes; Stop + Set Dequeue alone preserves
     * their old DATA toggle. Drop and add that stopped endpoint in one
     * Configure Endpoint command to reset its host-side sequence to DATA0. */
    struct xhci *x=&g_xhci;
    struct xhci_slot *s=&x->slot[d->slot];
    struct xhci_ep *ep=s->ep[dci];
    memset(s->in_ctx,0,4096);
    uint32_t *isc=slot_ctx(x,s->in_ctx,1),*osc=slot_ctx(x,s->dev_ctx,0);
    for(unsigned i=0;i<3;i++)isc[i]=osc[i];
    uint32_t *iec=ep_ctx(x,s->in_ctx,dci,1),*oec=ep_ctx(x,s->dev_ctx,dci,0);
    for(unsigned i=0;i<5;i++)iec[i]=oec[i];
    iec[0]&=~7u; /* Endpoint State is output-only. */
    uint64_t next=(ep->ring.dma_base+(uint64_t)ep->ring.enq*sizeof(struct trb))|ep->ring.cycle;
    iec[2]=(uint32_t)next;iec[3]=(uint32_t)(next>>32);
    input_ctrl_ctx(s->in_ctx)[0]=1u<<dci;
    input_ctrl_ctx(s->in_ctx)[1]=(1u<<dci)|1u;
    return xhci_cmd(x,dma_address(s->in_ctx),0,
        TRB_SET_TYPE(TRB_CONFIG_EP)|((uint32_t)d->slot<<24),NULL)==CC_SUCCESS?0:-1;
}

int xhci_int_in_arm(struct usb_device *d, uint8_t ep_addr)
{
    IO_GUARD(&xhci_events_gate);
    struct xhci *x = &g_xhci;
    int dci = XHCI_DCI(ep_addr);
    if (!x->up || d->slot < 1 || d->slot > XHCI_MAX_SLOTS) return -1;
    struct xhci_ep *ep = x->slot[d->slot].ep[dci];
    if (!ep) return -1;
    if (ep->inflight) return 0;

    if (!ep->buf) return -1; /* allocated by ep_alloc, never in IRQ */
    if (!xring_push(&ep->ring, ep->buf_dma, (uint32_t)ep->buflen,
                    TRB_SET_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP))
        return -1;
    ep->inflight = 1;
    ring_doorbell(x, d->slot, (uint32_t)dci);
    return 0;
}

int xhci_int_in_poll(struct usb_device *d, uint8_t ep_addr, uint8_t **buf)
{
    IO_GUARD(&xhci_events_gate);
    struct xhci *x = &g_xhci;
    int dci = XHCI_DCI(ep_addr);
    if (!x->up || d->slot < 1 || d->slot > XHCI_MAX_SLOTS) return -1;
    struct xhci_ep *ep = x->slot[d->slot].ep[dci];
    if (!ep) return -1;

    struct trb e;
    if (!ep_pop_event(ep, &e)) return -1;
    ep->inflight = 0;

    int cc = (int)TRB_CC(e.status);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        /* Deliberately NOT recovering the endpoint here. This runs in the
         * interrupt handler, and a Reset Endpoint command means blocking on a
         * command completion from inside an interrupt -- the one thing the
         * handler must not do. The error is counted and the endpoint is
         * re-armed by the caller; a persistently halted HID endpoint means a
         * device that has stopped talking, which recovery would not fix. */
        x->errors++;
        return -1;
    }
    *buf = ep->buf;
    int got = ep->buflen - (int)TRB_RESIDUAL(e.status);
    return got < 0 ? 0 : got;
}

int xhci_shutdown(void)
{
    IO_DOMAIN_GUARD(&xhci_owner);
    struct xhci *x = &g_xhci;
    if (!x->pci && !x->cap) return 0;
    if (!x->cap) return -1; /* config-space quarantine cannot be reprobed */
    { IO_GUARD(&xhci_events_gate); x->up = 0; } /* event users have drained */
    int stopped = controller_stop_confirmed(x) == 0;
    int isolated = pci_command_set(x, x->pci_command_quiet) == 0;
    if (!stopped || !isolated) {
        xhci_quarantine(x, "unbind could not prove stop and bus-master isolation");
        return -1;
    }
    dma_device_quiesced(&x->dma);
    for (int slot = 1; slot <= XHCI_MAX_SLOTS; slot++) {
        for (int ep = 1; ep < 32; ep++)
            if (x->slot[slot].ep[ep]) kfree(x->slot[slot].ep[ep]);
        memset(&x->slot[slot], 0, sizeof x->slot[slot]);
    }
    while (x->dma.buffers) {
        if (dma_free_coherent(x->dma.buffers) != 0) return -1;
    }
    xhci_forget(x);
    return 0;
}
