/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB 2.0 chapter 11 hub class. X79/C600 exposes its USB2 ports through
 * integrated rate-matching hubs: a working EHCI root-port reset alone cannot
 * reach the keyboard. This driver powers and resets downstream ports once at
 * boot and supplies topology/TT state before the child gets an address.
 * Runtime connect/disconnect and SuperSpeed hub requests are not implemented. */
#include "usb.h"
#include "kprintf.h"
#define HUB_DESC 0x29
#define HUB_CLASS 9
#define HUB_RECIP_PORT 3
#define HUB_PORT_POWER 8
#define HUB_PORT_RESET 4
#define HUB_CONNECTION 1
#define HUB_ENABLE 2
#define HUB_OVERCURRENT 8
#define HUB_RESET 16
#define HUB_LOW_SPEED (1u<<9)
#define HUB_HIGH_SPEED (1u<<10)

static int port_feature(struct usb_device *d,unsigned port,unsigned feature,int set)
{
    return usb_control(d,USB_RT_TYPE_CLASS|HUB_RECIP_PORT,
        set?USB_REQ_SET_FEATURE:USB_REQ_CLEAR_FEATURE,(uint16_t)feature,(uint16_t)port,0,0);
}
static int port_status(struct usb_device *d,unsigned port,uint16_t *status,uint16_t *change)
{
    uint8_t b[4];
    if (usb_control(d,USB_RT_DIR_IN|USB_RT_TYPE_CLASS|HUB_RECIP_PORT,
        USB_REQ_GET_STATUS,0,(uint16_t)port,b,4)!=4) return -1;
    *status=(uint16_t)(b[0]|b[1]<<8); *change=(uint16_t)(b[2]|b[3]<<8);
    return 0;
}
static int hub_probe(struct usb_device *d,int ifno)
{
    if (d->speed==USB_SPEED_SUPER || d->depth>=USB_MAX_DEPTH) return -1;
    uint8_t desc[71]; /* USB2's maximum 255 ports: 7 + 2*32 bitmap bytes. */
    int n=usb_control(d,USB_RT_DIR_IN|USB_RT_TYPE_CLASS,USB_REQ_GET_DESCRIPTOR,
                      HUB_DESC<<8,0,desc,sizeof desc);
    if (n<7 || desc[1]!=HUB_DESC || desc[0]<7 || desc[0]>n || !desc[2]) {
        kprintf("[usb-hub] addr=%d descriptor rejected length=%d\n",d->addr,n);return -1;
    }
    unsigned ports=desc[2];
    /* We do not consume PortPwrCtrlMask: USB2 made that USB1.x compatibility
     * bitmap obsolete. Some USB1.1 hubs expose a shorter trailing mask (the
     * installed QEMU eight-port hub returns 10 bytes). Requiring two complete
     * bitmaps rejected that usable hub despite all operational fields being
     * present. DeviceRemovable, if consulted later, still has a bounded map. */
    if (desc[0] < 7+(ports+8)/8) {
        kprintf("[usb-hub] addr=%d short bitmap length=%d ports=%d\n",d->addr,desc[0],ports);return -1;
    }
    unsigned characteristics=desc[3]|desc[4]<<8;
    /* Multi-TT is the SELECTED interface protocol, not bDeviceProtocol=2
     * alone. A multi-TT-capable hub's alternate 0 normally selects one TT. */
    d->hub_multi_tt=d->speed==USB_SPEED_HIGH && d->cfg.iface[ifno].if_proto==2;
    d->hub_tt_think=(characteristics>>5)&3;
    d->hub_ports=(uint8_t)ports;
    if (!d->hc->ops->configure_hub || d->hc->ops->configure_hub(d,(int)ports,
        d->hub_multi_tt,d->hub_tt_think)) {
        kprintf("[usb-hub] addr=%d controller hub context rejected\n",d->addr);return -1;
    }
    for (unsigned p=1;p<=ports;p++)
        if (port_feature(d,p,HUB_PORT_POWER,1)<0 && (characteristics&3)!=2) return -1;
    if (usb_delay_ms((unsigned)desc[5]*2+100)) return -1; /* power-good plus debounce */
    int children=0;
    for (unsigned p=1;p<=ports;p++) {
        uint16_t status,change;
        if (port_status(d,p,&status,&change) || !(status&HUB_CONNECTION) ||
            (status&HUB_OVERCURRENT)) continue;
        if (port_feature(d,p,HUB_PORT_RESET,1)<0) continue;
        int enabled=0;
        for (unsigned wait=0;wait<50;wait++) {
            if (usb_delay_ms(10)) break;
            if (port_status(d,p,&status,&change)) break;
            if (!(status&HUB_CONNECTION) || (status&HUB_OVERCURRENT)) break;
            if (!(status&HUB_RESET) && (status&HUB_ENABLE)) { enabled=1; break; }
        }
        if (!enabled) { kprintf("[usb-hub] addr=%d port=%d reset failed\n",d->addr,p); continue; }
        for (unsigned bit=0;bit<5;bit++) if (change&(1u<<bit)) port_feature(d,p,16+bit,0);
        if (usb_delay_ms(20)) return -1; /* USB2 reset recovery is at least 10ms */
        unsigned speed=(status&HUB_HIGH_SPEED)?USB_SPEED_HIGH:
                       (status&HUB_LOW_SPEED)?USB_SPEED_LOW:USB_SPEED_FULL;
#ifndef USB_HUB_NEGCTL_NO_CHILDREN
        if (!usb_enumerate_child(d,p,speed)) children++;
#else
        (void)speed;
#endif
    }
    kprintf("USB_HUB hc=%d addr=%d ports=%d children=%d tt=%d\n",
            d->hc->index,d->addr,ports,children,d->hub_multi_tt);
    return 0;
}
static const struct usb_match hub_ids[]={ { HUB_CLASS,USB_ANY,USB_ANY },{0,0,0} };
static const struct usb_driver hub_driver={ .name="usb-hub",.match=hub_ids,.probe=hub_probe };
void usb_hub_register(void) { usb_register_driver(&hub_driver); }
