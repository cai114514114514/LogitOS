/* USB 2.0 section 9.6.5: interface numbers, not device addresses, are the
 * binding boundary. Keep this callback dispatch independent of enumeration
 * so a composite-device fixture can exercise the exact guest binder. */
#include "usb.h"
#include "usb_hc.h"
#include "kprintf.h"

int usb_bind_interfaces(struct usb_device *d, const struct usb_driver *const *drivers, int count)
{
    int bound = 0;
    for (int ifno = 0; ifno < d->cfg.n_if; ifno++) {
        const struct usb_interface *it = &d->cfg.iface[ifno];
        if (d->binding[ifno].drv) { bound++; continue; }
#ifdef USB_INPUT_NEGCTL_FIRST_INTERFACE
        if (bound) break;
#endif
        int configured = 0;
        for (int i = 0; i < count; i++) {
            const struct usb_driver *drv = drivers[i];
            const struct usb_match *m = drv->match;
            int matched = 0;
            for (; m && (m->if_class || m->if_subclass || m->if_proto); m++) {
                if (m->if_class != USB_ANY && m->if_class != it->if_class) continue;
                if (m->if_subclass != USB_ANY && m->if_subclass != it->if_subclass) continue;
                if (m->if_proto != USB_ANY && m->if_proto != it->if_proto) continue;
                matched = 1; break;
            }
            if (!matched) continue;
            /* A declining class driver must not make the next candidate
             * allocate/configure the same transport endpoint a second time. */
            if (!configured && usb_configure_interface(d, it)) break;
            configured = 1;
            if (!drv->probe(d, ifno)) {
                d->binding[ifno].drv = drv;
                kprintf("[usb] if%d bound to driver '%s'\n", it->num, drv->name);
                bound++; break;
            }
        }
    }
    return bound;
}

void usb_poll_interfaces(struct usb_device *d)
{
    for (int i = 0; i < d->cfg.n_if; i++) {
        const struct usb_driver *drv = d->binding[i].drv;
        if (drv && drv->poll) drv->poll(d, i);
    }
}

void usb_remove_interfaces(struct usb_device *d)
{
    /* Core removal closes admission and drains IRQ references before this
     * call; no poll may retain a class-private pointer while it is freed. */
    for (int i = 0; i < d->cfg.n_if; i++) {
        const struct usb_driver *drv = d->binding[i].drv;
        if (drv && drv->remove) drv->remove(d, i);
        d->binding[i].drv = 0;
        d->binding[i].drvdata = 0;
    }
}
