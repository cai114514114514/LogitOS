/* 2026-09-10 concurrency correction: USB command callers and IRQ callbacks take active CPU references. Removal closes admission, drains those references, then stops/frees the controller; historical BKL notes below no longer provide exclusion. */
/* USB core: enumeration and driver binding.
 *
 * Enumeration is the sequence USB 2.0 section 9.1.2 and xHCI 4.3 describe, and
 * the order is not negotiable -- a device answers nothing until its port has
 * been reset, has no address until the controller assigns one, and describes
 * nothing useful until its configuration has been read in two passes (you must
 * read the configuration descriptor to learn how long the configuration is).
 *
 * Binding is keyed on the INTERFACE class/subclass/protocol triple, which is the
 * reason this file is worth more than the HID driver it currently serves: a
 * keyboard, a flash drive and a hub all arrive through the same nine steps, and
 * only the last one differs.
 *
 * WHERE THIS RUNS. Nowhere in kmain: this driver is registered declaratively
 * (DRIVER_DECLARE at the bottom) and the device model's probe/bind pass calls
 * xhci_probe() for any PCI function of class 0x0C/0x03/0x30. No file outside
 * c/drivers/usb/ mentions USB at all -- no call in kmain, no line in the
 * Makefile, no vector in interrupts.c.
 *
 * Enumeration happens synchronously inside probe(), which kmain reaches via
 * dev_probe_all() after smp_init(). Two things about that context are worth
 * knowing. It is late enough to do real work -- the AHCI driver reads disks
 * from its own probe() -- but the boot path still has interrupts masked, so
 * timer_ms() would not advance and every timeout in the controller driver would
 * degenerate to a raw spin count. So the bring-up opens an interrupt window
 * around itself and restores the caller's IF afterwards, exactly as
 * c/drivers/virtio/virtio.c's request path does at the same point in the boot,
 * and for the same reason: the poll needs a time base.
 *
 * After that, xHCI is completion-interrupt driven. EHCI additionally uses a
 * bounded backend timer to advance its periodic schedule without waiting in
 * an IRQ. Root-port cable changes queue the existing sleepable kworker; hub
 * downstream changes still require a hub interrupt endpoint and remain absent.
 */

#include <stdint.h>
#include <stddef.h>
#include "usb.h"
#include "../core/io_lock.h"
#include "../core/io_domain.h"
#include "sched.h"
#include "usb_desc.h"
#include "xhci.h"
#include "driver.h"
#include "pci.h"
#include "kprintf.h"
#include "pit.h"
#include "ktime.h"
#include "work.h"

void *memset(void *, int, size_t);

void usb_hid_register(void);
void usb_hub_register(void);
void usb_msc_register(void);

#define USB_MAX_DRIVERS 4

static struct usb_device g_dev[USB_MAX_DEVICES];
static const struct usb_driver *g_drv[USB_MAX_DRIVERS];
static int g_ndrv;
static struct usb_hc *g_hc[USB_MAX_CONTROLLERS];
struct usb_hotplug {
    struct usb_hc *hc;
    struct work work;
    struct ktimer timer;
    int initialized;
};
static struct usb_hotplug g_hotplug[USB_MAX_CONTROLLERS];
static struct io_domain usb_enumeration = IO_DOMAIN_INIT;
static int classes_registered;
/* A reference belongs to its controller. Removing EHCI must neither reject
 * xHCI operations nor free input buffers still borrowed by another CPU. */
struct usb_ref { struct usb_hc *hc; };
static void usb_ref_drop(struct usb_ref *r)
{ if (r->hc) __atomic_fetch_sub(&r->hc->active, 1, __ATOMIC_RELEASE); }
static struct usb_ref usb_ref_take(struct usb_hc *hc)
{
    struct usb_ref r = {0};
    if (!hc) return r;
    IO_GUARD(&hc->admission);
    if (hc->online) {
        __atomic_fetch_add(&hc->active, 1, __ATOMIC_RELAXED);
        r.hc = hc;
    }
    return r;
}
#define USB_REF(hc) struct usb_ref ref __attribute__((cleanup(usb_ref_drop))) = usb_ref_take(hc)


static unsigned long g_found;
unsigned long usb_reports_total;      /* bumped by usb_hid.c */
unsigned long usb_keys_total;
unsigned long usb_motion_total;

unsigned long usb_devices_found(void)    { return g_found; }
unsigned long usb_reports_delivered(void){ return usb_reports_total; }
unsigned long usb_keys_posted(void)      { return usb_keys_total; }
unsigned long usb_motion_posted(void)    { return usb_motion_total; }
int usb_present(void)
{
    for (unsigned i=0;i<USB_MAX_CONTROLLERS;i++)
        if (g_hc[i] && __atomic_load_n(&g_hc[i]->online, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

void usb_register_driver(const struct usb_driver *drv)
{
    if (g_ndrv < USB_MAX_DRIVERS) g_drv[g_ndrv++] = drv;
}

int usb_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                uint16_t idx, void *data, uint16_t len)
{
    USB_REF(d ? d->hc : NULL);
    if (!ref.hc || !d->used || (len && !data) || !d->hc->ops->control) return -1;
    return d->hc->ops->control(d,rt,req,val,idx,data,len);
}
int usb_configure_interface(struct usb_device *d,const struct usb_interface *it)
{
    USB_REF(d ? d->hc : NULL);
    return ref.hc && d->used && d->hc->ops->configure ? d->hc->ops->configure(d,it) : -1;
}
int usb_int_in_arm(struct usb_device *d,uint8_t ep)
{
    USB_REF(d ? d->hc : NULL);
    return ref.hc && d->used && d->hc->ops->int_in_arm ? d->hc->ops->int_in_arm(d,ep) : -1;
}
int usb_int_in_poll(struct usb_device *d,uint8_t ep,uint8_t **buf)
{
    USB_REF(d ? d->hc : NULL);
    return ref.hc && d->used && d->hc->ops->int_in_poll ? d->hc->ops->int_in_poll(d,ep,buf) : -1;
}
int usb_bulk(struct usb_device *d,uint8_t ep,void *data,uint32_t len)
{
    USB_REF(d ? d->hc : NULL);
    if (!ref.hc || !d->used || !USB_EP_NUM(ep) || (len && !data) ||
        len > 0x7fffffffu || !d->hc->ops->bulk) return -1;
    return d->hc->ops->bulk(d,ep,data,len);
}
int usb_clear_halt(struct usb_device *d,uint8_t ep)
{
    USB_REF(d ? d->hc : NULL);
    if (!ref.hc || !d->used || !USB_EP_NUM(ep) || !d->hc->ops->clear_halt) return -1;
    if (usb_control(d,USB_RT_RECIP_EP,USB_REQ_CLEAR_FEATURE,0,ep,NULL,0)<0) return -1;
    return d->hc->ops->clear_halt(d,ep);
}
int usb_clear_tt_buffer(struct usb_device *d,uint8_t ep,unsigned type)
{
    if (!d) return -1;
    if (!d->tt_hub) return 0;
    if (type!=USB_XFER_CONTROL && type!=USB_XFER_BULK) return -1;
    /* USB2 11.24.2.3 encodes device USB address (not xHCI slot) in wValue.
     * A single-TT hub always uses TT selector 1, even behind downstream port 6. */
    uint16_t value=(uint16_t)(USB_EP_NUM(ep)|((unsigned)d->addr<<4)|(type<<11)|
                             (USB_EP_IS_IN(ep)?0x8000:0));
    uint16_t tt=d->tt_multi?d->tt_port:1;
    /* Control pipes can leave both TT directions occupied (USB2 11.24.2.3). */
#ifndef USB_HUB_NEGCTL_CONTROL_ONE_DIRECTION
    if (type==USB_XFER_CONTROL && usb_control(d->tt_hub,USB_RT_TYPE_CLASS|3,8,
        (uint16_t)(value^0x8000),tt,NULL,0)<0) return -1;
#endif
    return usb_control(d->tt_hub,USB_RT_TYPE_CLASS|3,8,value,tt,NULL,0)<0?-1:0;
}
int usb_delay_ms(unsigned ms)
{
    if (!time_ready()) return -1;
    uint64_t start=time_mono_ns(), duration=(uint64_t)ms*1000000;
    unsigned long spins=0;
    while (time_mono_ns()-start < duration) {
        if (++spins==200000000ul) return -1;
        io_relax();
    }
    return 0;
}

static struct usb_device *dev_alloc(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (!g_dev[i].used) {
            struct usb_device *d=&g_dev[i];
            size_t ready=offsetof(struct usb_device,callback_ready);
            /* IRQ reads callback_ready atomically before any ordinary field.
             * Do not let a whole-struct memset race that atomic read, even
             * while the byte is already zero. */
            __atomic_store_n(&d->callback_ready,0,__ATOMIC_RELAXED);
            memset(d,0,ready);
            memset((uint8_t *)d+ready+sizeof d->callback_ready,0,
                   sizeof *d-ready-sizeof d->callback_ready);
            g_dev[i].used = 1;
            return &g_dev[i];
        }
    return NULL;
}

static const char *speed_name(int s)
{
    switch (s) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:           return "?";
    }
}

static int get_descriptor(struct usb_device *d, uint8_t type, uint8_t index,
                          void *buf, uint16_t len)
{
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_STD | USB_RT_RECIP_DEV,
                       USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)((type << 8) | index), 0, buf, len);
}

/* A device that answers a request wrong once and right on retry is the normal
 * case, not the exceptional one: the 10 ms recovery after a port reset is a
 * minimum, not a guarantee, and low-speed devices in particular NAK for a
 * while. Three attempts, and say so when it took more than one. */
static int get_descriptor_retry(struct usb_device *d, uint8_t type, uint8_t index,
                                void *buf, uint16_t len)
{
    for (int try = 0; try < 3; try++) {
        int n = get_descriptor(d, type, index, buf, len);
        if (n >= 0) {
            if (try) kprintf("[usb] descriptor %02x took %d attempts\n", type, try + 1);
            return n;
        }
    }
    return -1;
}

static int enumerate_device(struct usb_device *d)
{
    struct usb_hc *hc=d->hc;
    int port=d->port, speed=d->speed;
    if (hc->ops->device_open(d) != 0) goto fail;

    /* Pass 1 of the device descriptor: 8 bytes, which is all any device is
     * required to answer with the default max packet size. Byte 7 is
     * bMaxPacketSize0 -- the thing we needed in order to ask correctly. */
    uint8_t buf[4096];
    int n = get_descriptor_retry(d, USB_DT_DEVICE, 0, buf, 8);
    if (n < 8) { kprintf("[usb] port %d: no device descriptor\n", port); goto fail; }
    int mp0 = buf[7];
    if (speed == USB_SPEED_FULL && mp0 != 8) {
        if (mp0 != 16 && mp0 != 32 && mp0 != 64) {
            kprintf("[usb] port %d: bMaxPacketSize0 %d is not legal\n", port, mp0);
            goto fail;
        }
        if (hc->ops->set_ep0_packet(d, mp0) != 0) goto fail;
    }

    n = get_descriptor_retry(d, USB_DT_DEVICE, 0, buf, 18);
    if (n < 18 || usb_parse_device_desc(buf, n, &d->dd) != 0) {
        kprintf("[usb] port %d: device descriptor rejected (%d bytes)\n", port, n);
        goto fail;
    }

    /* Pass 1 of the configuration: 9 bytes, to learn wTotalLength. */
    n = get_descriptor_retry(d, USB_DT_CONFIG, 0, buf, 9);
    if (n < 9) { kprintf("[usb] port %d: no config descriptor\n", port); goto fail; }
    uint16_t total = usb_config_total_len(buf, n);
    if (total < 9 || total > sizeof buf) {
        kprintf("[usb] port %d: wTotalLength %d out of range\n", port, total);
        goto fail;
    }
    n = get_descriptor_retry(d, USB_DT_CONFIG, 0, buf, total);
    if (n < (int)total || usb_parse_config(buf, n, &d->cfg) != 0) {
        kprintf("[usb] port %d: configuration rejected (%d of %d bytes)\n", port, n, total);
        goto fail;
    }

    if (usb_control(d, USB_RT_TYPE_STD | USB_RT_RECIP_DEV, USB_REQ_SET_CONFIGURATION,
                    d->cfg.value, 0, NULL, 0) < 0) {
        kprintf("[usb] port %d: SET_CONFIGURATION failed\n", port);
        goto fail;
    }

    g_found++;
    kprintf("USB_DEV port=%d addr=%d speed=%s vid=%04x pid=%04x class=%02x ifs=%d hc=%d parent=%d parent_port=%d depth=%d\n",
            port, d->addr, speed_name(speed), d->dd.vendor, d->dd.product,
            d->dd.dev_class, d->cfg.n_if, hc->index, d->parent?d->parent->addr:0,d->parent_port,d->depth);

    for (int i = 0; i < d->cfg.n_if; i++) {
        struct usb_interface *it = &d->cfg.iface[i];
        kprintf("USB_IF port=%d if=%d class=%02x sub=%02x proto=%02x eps=%d\n",
                port, it->num, it->if_class, it->if_subclass, it->if_proto, it->n_ep);
    }
    if (!usb_bind_interfaces(d, g_drv, g_ndrv))
        kprintf("[usb] port %d: no driver for this device (it is enumerated, just idle)\n", port);
    __atomic_store_n(&d->callback_ready,1,__ATOMIC_RELEASE);
    return 0;

fail:
    hc->ops->device_close(d);
    d->used = 0;
    return -1;
}

/* Device routes are built before address assignment. For a FS hub beneath a
 * HS hub, all its descendants use the SAME TT downstream port, not their own
 * final port; EHCI splits otherwise reach the wrong transaction translator. */
int usb_enumerate_child(struct usb_device *hub,unsigned port,unsigned speed)
{
    if (!hub || !hub->used || !port || port > hub->hub_ports ||
        hub->depth >= USB_MAX_DEPTH || speed < USB_SPEED_FULL || speed > USB_SPEED_HIGH) return -1;
    struct usb_device *d=dev_alloc();
    if (!d) return -1;
    d->hc=hub->hc; d->parent=hub; d->parent_port=(uint8_t)port;
    d->port=hub->port; d->speed=(uint8_t)speed; d->depth=hub->depth+1;
    d->route=hub->route | ((port>15?15:port) << (hub->depth*4));
#ifndef USB_HUB_NEGCTL_NO_TT
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        if (hub->speed == USB_SPEED_HIGH) {
            d->tt_hub=hub; d->tt_port=(uint8_t)port; d->tt_multi=hub->hub_multi_tt;
        } else {
            d->tt_hub=hub->tt_hub; d->tt_port=hub->tt_port; d->tt_multi=hub->tt_multi;
        }
    }
#endif
    return enumerate_device(d);
}
static int enumerate_port(struct usb_hc *hc,int port)
{
    int speed;
    if (hc->ops->root_port_reset(hc,port,&speed)) return -1;
    struct usb_device *d=dev_alloc();
    if (!d) return -1;
    d->hc=hc; d->port=(uint8_t)port; d->speed=(uint8_t)speed;
    return enumerate_device(d);
}

static struct usb_device *root_device(struct usb_hc *hc,int port)
{
    for (int i=0;i<USB_MAX_DEVICES;i++) {
        struct usb_device *d=&g_dev[i];
        if (d->used && d->hc==hc && !d->parent && d->port==port) return d;
    }
    return NULL;
}

/* Wait for the current IRQ/timer callback, then make the entire root subtree
 * invisible to every future class poll.  The lock is intentionally released
 * before class remove, block drain, endpoint close, or re-enumeration: those
 * operations can sleep and hc->callbacks is a non-sleeping IRQ lock. */
static void quiesce_root_callbacks(struct usb_hc *hc,int port)
{
#ifndef USB_HOTPLUG_NEGCTL_UNLOCKED_DETACH
    IO_GUARD(&hc->callbacks);
#endif
    for (int i=0;i<USB_MAX_DEVICES;i++) {
        struct usb_device *d=&g_dev[i];
        if (d->used && d->hc==hc && d->port==port)
            __atomic_store_n(&d->callback_ready,0,__ATOMIC_RELEASE);
    }
}

/* Detach descendants before their hub/TT ancestor.  All descendants retain
 * the same root-port number, so this also catches a whole hub tree without
 * relying on parent pointers after the first child has been cleared. */
static int detach_root_tree(struct usb_hc *hc,int port)
{
    int removed=0;
    for (int depth=USB_MAX_DEPTH;depth>=0;depth--)
        for (int i=0;i<USB_MAX_DEVICES;i++) {
            struct usb_device *d=&g_dev[i];
            if (!d->used || d->hc!=hc || d->port!=port || d->depth!=depth) continue;
            usb_remove_interfaces(d);
            hc->ops->device_close(d);
            size_t ready=offsetof(struct usb_device,callback_ready);
            memset(d,0,ready);
            memset((uint8_t *)d+ready+sizeof d->callback_ready,0,
                   sizeof *d-ready-sizeof d->callback_ready);
            __atomic_store_n(&d->callback_ready,0,__ATOMIC_RELAXED);
            removed++;
            if (g_found) g_found--;
        }
    return removed;
}

static void hotplug_work(void *arg)
{
    struct usb_hotplug *hp=arg;
    struct usb_hc *hc=hp?hp->hc:NULL;
    /* Lock order matters: unregister holds usb_enumeration before it closes
     * admission.  Taking an active reference first would let this worker wait
     * for enumeration while unregister waited for this reference forever. */
    IO_DOMAIN_GUARD(&usb_enumeration);
    USB_REF(hc);
    if (!ref.hc || !hc->ops->root_port_changed) return;
    int n=hc->ops->root_port_count(hc);
    for (int p=1;p<=n;p++) {
        int connected=0;
        int changed=hc->ops->root_port_changed(hc,p,&connected);
#ifndef USB_HOTPLUG_NEGCTL_IGNORE_CSC
        if (changed<=0) continue;
#else
        if (changed<0) continue;
#endif
        struct usb_device *old=root_device(hc,p);
        if (!connected) {
            if (old) quiesce_root_callbacks(hc,p);
            int removed=old?detach_root_tree(hc,p):0;
            kprintf("USB_HOTPLUG hc=%u port=%d disconnected removed=%d\n",hc->index,p,removed);
            continue;
        }
        /* A real CSC while CCS stayed high is a replacement/bounce, not a
         * reset-completion event: reset sets PRC, and each HCD reports CSC
         * separately.  Retire stale endpoints before assigning a fresh USB
         * address so the new device cannot inherit old class state. */
        if (old) {
            quiesce_root_callbacks(hc,p);
            (void)detach_root_tree(hc,p);
        }
        int rc=hc->ops->root_port_connected(hc,p)?enumerate_port(hc,p):-1;
        kprintf("USB_HOTPLUG hc=%u port=%d connected result=%s\n",hc->index,p,rc?"failed":"online");
    }
}

/* Port Status Change interrupts are advisory: firmware routing mistakes and
 * an already-asserted MSI edge can lose one.  A bounded timer reuses the same
 * IRQ path so xHCI retires the corresponding event TRB/EHB before a deferred
 * command is submitted.  It never enumerates, waits, or touches descriptors
 * in interrupt context. */
static void hotplug_watch(struct ktimer *timer)
{
    struct usb_hotplug *hp=timer?timer->arg:NULL;
    if (hp && hp->hc) usb_hc_irq(hp->hc);
}

/* The reference spans class decoding of borrowed interrupt-IN DMA buffers.
 * events only records completions; no enumeration or synchronous bulk runs in
 * this IRQ entry. Hub discovery is a boot-only probe operation. */
void usb_hc_irq(void *arg)
{
    struct usb_hc *hc=arg;
    USB_REF(hc);
    if (!ref.hc) return;
    /* EHCI may also enter from its periodic progress timer. Pinning protects
     * lifetime, but does not serialize two CPUs decoding the same HID state. */
    IO_GUARD(&hc->callbacks);
    hc->ops->events(hc);
    for (int i=0;i<USB_MAX_DEVICES;i++)
        if (__atomic_load_n(&g_dev[i].callback_ready,__ATOMIC_ACQUIRE) &&
            g_dev[i].used && g_dev[i].hc==hc)
            usb_poll_interfaces(&g_dev[i]);
    if (hc->ops->root_change_pending && hc->ops->root_change_pending(hc))
        work_queue(&g_hotplug[hc->index].work);
}
int usb_hc_register(struct usb_hc *hc,struct device *pci,const struct usb_hc_ops *ops,void *priv)
{
    if (!hc || !pci || !ops || !ops->device_open || !ops->device_close ||
        !ops->root_port_reset || !ops->root_port_count || !ops->root_port_connected ||
        !ops->control || !ops->set_ep0_packet || !ops->events || !ops->shutdown) return -1;
    IO_DOMAIN_GUARD(&usb_enumeration);
    unsigned index;
    for (index=0;index<USB_MAX_CONTROLLERS && g_hc[index];index++);
    if (index==USB_MAX_CONTROLLERS) return -1;
    hc->ops=ops; hc->pci=pci; hc->priv=priv; hc->index=index;
    hc->active=0;
    { IO_GUARD(&hc->admission); hc->online=1; }
    g_hc[index]=hc;
    struct usb_hotplug *hp=&g_hotplug[index];
    hp->hc=hc;
    if (!hp->initialized) {
        work_item_init(&hp->work,hotplug_work,hp);
        hp->timer.heap_idx=-1;
        hp->initialized=1;
    }
    dev_set_drvdata(pci,hc);
    if (!classes_registered) {
        classes_registered=1;
        usb_hub_register(); usb_hid_register(); usb_msc_register();
    }
    int n=ops->root_port_count(hc);
    kprintf("[usb] %s hc=%d scanning %d root ports\n",ops->name,index,n);
    for (int p=1;p<=n;p++) if (ops->root_port_connected(hc,p)) enumerate_port(hc,p);
    /* Controller reset/power-on can leave CSC set even on empty ports. Consume
     * that startup state before enabling IRQ/watch, while still honoring a
     * cable transition that raced the first connected snapshot. */
    if (ops->root_port_changed) for (int p=1;p<=n;p++) {
        int connected=0;
        if (ops->root_port_changed(hc,p,&connected)>0) {
            struct usb_device *old=root_device(hc,p);
            if (connected && !old) (void)enumerate_port(hc,p);
            else if (!connected && old) (void)detach_root_tree(hc,p);
        }
    }
    int vec=dev_irq_request(pci,usb_hc_irq,hc,ops->name);
    if (vec>=0 && ops->irq_enable) {
        ops->irq_enable(hc);
        kprintf("USB_IRQ vec=%d mode=%d\n",vec,pci->irq_mode);
    } else kprintf("[usb] %s has no IRQ; interrupt input unavailable\n",ops->name);
    if (ops->root_change_pending && ops->root_port_changed &&
        ktimer_add(&hp->timer,10*NS_PER_MS,10*NS_PER_MS,hotplug_watch,hp,"usb-root-watch"))
        kprintf("[usb] %s root-port watcher unavailable; hotplug requires IRQ delivery\n",ops->name);
    kprintf("USB_READY devices=%d drivers=%d\n",(int)g_found,g_ndrv);
    return 0;
}
void usb_hc_unregister(struct usb_hc *hc)
{
    if (!hc) return;
    IO_DOMAIN_GUARD(&usb_enumeration);
    struct usb_hotplug *hp=hc->index<USB_MAX_CONTROLLERS?&g_hotplug[hc->index]:NULL;
    if (hp) (void)ktimer_cancel(&hp->timer);
    { IO_GUARD(&hc->admission); hc->online=0; }
    dev_irq_release(hc->pci);
    while (__atomic_load_n(&hc->active,__ATOMIC_ACQUIRE)) sched_poll_wait();
    /* Children lose their class state before any ancestor's TT/context. */
    for (int depth=USB_MAX_DEPTH;depth>=0;depth--)
        for (int i=0;i<USB_MAX_DEVICES;i++) {
            struct usb_device *d=&g_dev[i];
            if (!d->used || d->hc!=hc || d->depth!=depth) continue;
            usb_remove_interfaces(d);
            hc->ops->device_close(d);
            memset(d,0,sizeof *d);
            if (g_found) g_found--;
        }
    hc->ops->shutdown(hc);
    if (hc->index<USB_MAX_CONTROLLERS && g_hc[hc->index]==hc) g_hc[hc->index]=NULL;
    if (hp && hp->hc==hc) hp->hc=NULL;
    dev_set_drvdata(hc->pci,NULL);
}

/* xHCI adapter. EHCI registers its own PCI driver and uses the same core. */
static struct usb_hc xhci_hc;
static int x_open(struct usb_device *d)
{ int slot=xhci_enable_slot(); if(slot<0)return -1; d->slot=(uint8_t)slot; return xhci_address_device(d,0); }
static void x_close(struct usb_device *d) { xhci_free_slot(d->slot); }
static int x_ports(struct usb_hc *h) { (void)h; return xhci_port_count(); }
static int x_connected(struct usb_hc *h,int p) { (void)h; return xhci_port_connected(p); }
static int x_reset(struct usb_hc *h,int p,int *s) { (void)h; return xhci_port_reset(p,s); }
static int x_change_pending(struct usb_hc *h) { (void)h;return xhci_root_change_pending(); }
static int x_changed(struct usb_hc *h,int p,int *c) { (void)h;return xhci_port_changed(p,c); }
static void x_events(struct usb_hc *h) { (void)h; xhci_events(); }
static void x_irq(struct usb_hc *h) { (void)h; xhci_irq_enable(); }
static int x_stop(struct usb_hc *h) { (void)h; return xhci_shutdown(); }
static const struct usb_hc_ops x_ops={
    .name="xhci",.root_port_count=x_ports,.root_port_connected=x_connected,.root_port_reset=x_reset,
    .root_change_pending=x_change_pending,.root_port_changed=x_changed,
    .device_open=x_open,.device_close=x_close,.set_ep0_packet=xhci_set_ep0_packet,
    .configure=xhci_configure_ep,.configure_hub=xhci_configure_hub,.control=xhci_control,
    .bulk=xhci_bulk,.clear_halt=xhci_clear_halt,.int_in_arm=xhci_int_in_arm,.int_in_poll=xhci_int_in_poll,
    .events=x_events,.irq_enable=x_irq,.shutdown=x_stop,
};
static int xhci_probe(struct device *dev)
{
    if (xhci_hc.pci) return -1;
    uint64_t fl=io_irq_save();
#if !__STDC_HOSTED__
    __asm__ volatile("sti");
#endif
    int rc=xhci_init(dev);
    if (!rc) {
        rc=usb_hc_register(&xhci_hc,dev,&x_ops,&g_xhci);
        /* xhci_init has already enabled BME.  A full USB controller table must
         * therefore run the same stop/isolate proof as unbind; xhci_shutdown
         * quarantines its DMA ledger if either acknowledgement is missing. */
        if (rc) (void)xhci_shutdown();
    }
    io_irq_restore(fl);
#if !__STDC_HOSTED__
    if (!(fl&0x200)) __asm__ volatile("cli");
#endif
    return rc;
}
static void xhci_remove(struct device *dev)
{
    if (xhci_hc.pci!=dev) return;
    usb_hc_unregister(&xhci_hc);
    xhci_hc.pci=NULL;
}
static const struct dev_match xhci_ids[]={ DEV_MATCH_PROGIF(PCI_CLASS_SERIAL,0x03,0x30), DEV_MATCH_END };
static struct driver xhci_driver={ .name="xhci",.bus_type=DEV_BUS_PCI,.match=xhci_ids,.probe=xhci_probe,.remove=xhci_remove };
DRIVER_DECLARE(xhci_driver);
