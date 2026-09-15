/* Protocol fixture, not a hardware claim: real usb_bind.c + usb_hid.c +
 * hid_report.c run through fake control/interrupt transport into WM spies.
 * Reuse the established HID descriptor fixtures and their regression suite. */
#define main usb_existing_regressions
#include "usb_hid_test.c"
#undef main
#include <stdlib.h>
#include <stdarg.h>
#include "usb.h"

static const struct usb_driver *registered;
static const uint8_t *descriptors[USB_MAX_IF];
static int lengths[USB_MAX_IF], pending[16], allocated, configured, protocols;
static int refuse_protocol;
static uint8_t packets[16][128];
static int keys_seen, last_key, last_key_mods, mouse_seen, mx, my, ml, mm;
unsigned long usb_reports_total, usb_keys_total, usb_motion_total;

void usb_hid_register(void);
void usb_register_driver(const struct usb_driver *drv) { registered = drv; }
void *kmalloc(size_t n) { void *p = calloc(1, n); if (p) allocated++; return p; }
void kfree(void *p) { if (p) { allocated--; free(p); } }
void kprintf(const char *fmt, ...) { (void)fmt; }
uint32_t fb_width(void) { return 1280; }
uint32_t fb_height(void) { return 800; }
void wm_key(int k) { keys_seen++; last_key = k; last_key_mods = usb_hid_mods(); }
void wm_mouse_event(int x, int y, int l, int r, int m, int wheel)
{ (void)r; (void)m; (void)wheel; mouse_seen++; mx=x; my=y; ml=l; mm=usb_hid_mods(); }
int usb_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                uint16_t idx, void *buf, uint16_t len)
{
    (void)d; (void)rt;
    if (req == USB_REQ_GET_DESCRIPTOR && val == (USB_DT_HID_REPORT << 8)) {
        if (idx >= USB_MAX_IF || !descriptors[idx] || len < lengths[idx]) return -1;
        memcpy(buf, descriptors[idx], lengths[idx]); return lengths[idx];
    }
    if (req == HID_REQ_SET_PROTOCOL) {
        protocols++;
        if (refuse_protocol) return -1;
        checki(val, HID_PROTO_REPORT, "boot-capable parsed HID explicitly selects report protocol");
    }
    return 0;
}
int usb_configure_interface(struct usb_device *d, const struct usb_interface *it)
{ (void)d; (void)it; configured++; return 0; }
int usb_int_in_arm(struct usb_device *d, uint8_t ep)
{ (void)d; (void)ep; return 0; }
int usb_int_in_poll(struct usb_device *d, uint8_t ep, uint8_t **buf)
{
    (void)d; ep &= 15;
    if (!pending[ep]) return -1;
    int n = pending[ep]; pending[ep] = 0; *buf = packets[ep]; return n;
}
static void iface(struct usb_device *d, int i, const uint8_t *rd, int len, int boot, int proto)
{
    descriptors[i] = rd; lengths[i] = len;
    struct usb_interface *it = &d->cfg.iface[i];
    it->num=i; it->if_class=USB_CLASS_HID; it->if_subclass=boot;
    it->if_proto=proto; it->n_ep=1; it->has_hid=1; it->hid_report_len=len;
    it->ep[0]=(struct usb_endpoint){ .addr=0x81+i, .attr=USB_XFER_INT,
                                   .max_packet=64, .interval=10 };
    d->cfg.n_if=i+1;
}
static void inject(struct usb_device *d, int endpoint, const uint8_t *p, int len)
{
    memcpy(packets[endpoint],p,len); pending[endpoint]=len; usb_poll_interfaces(d);
}
static void composite(void)
{
    struct usb_device d = { .used=1 };
    iface(&d,0,kbd_rd,sizeof kbd_rd,1,1);
    iface(&d,1,mouse_rd,sizeof mouse_rd,1,2);
    int c0=configured,p0=protocols,k0=keys_seen,m0=mouse_seen;
    checki(usb_bind_interfaces(&d,&registered,1),2,"both composite interfaces bind");
    checki(configured-c0,2,"both transport endpoints configured");
    checki(protocols-p0,2,"both boot-capable interfaces select report mode");
    check(d.binding[0].drvdata != d.binding[1].drvdata,"interface private states differ");
    uint8_t k[8]={2,0,4}, m[4]={1,3,0xfe,0};
    inject(&d,1,k,sizeof k); inject(&d,2,m,sizeof m);
    checki(keys_seen-k0,1,"composite keyboard types once");
    checki(last_key,'A',"USB shift changes character");
    checki(last_key_mods,EV_MOD_SHIFT,"first shifted key carries modifier");
    checki(mouse_seen-m0,1,"second composite interface delivers motion");
    checki(mx,643,"relative mouse X"); checki(my,398,"relative mouse Y");
    checki(ml,1,"composite mouse left button");
    checki(mm,EV_MOD_SHIFT,"USB keyboard modifier crosses to mouse event");
    usb_remove_interfaces(&d);
    checki(usb_hid_mods(),0,"removal releases held modifiers");
    checki(allocated,0,"all composite interface states freed");
}
static void multiplexed(void)
{
    struct usb_device d={ .used=1 };
    iface(&d,0,combo_rd,sizeof combo_rd,0,0);
    checki(usb_bind_interfaces(&d,&registered,1),1,"report-only combo interface binds");
    int k0=keys_seen,m0=mouse_seen;
    uint8_t k[9]={1,1,0,0x16}, m[6]={2,1,0xff,0x1f,0,0};
    inject(&d,1,k,sizeof k); inject(&d,1,m,sizeof m);
    checki(keys_seen-k0,1,"combo key report delivered exactly once");
    checki(last_key,19,"Ctrl+S keeps common control-code convention");
    checki(usb_hid_key_held('s'),1,"held query sees physical S despite Ctrl text translation");
    checki(usb_hid_key_held('w'),0,"unpressed game key is not held");
    checki(mouse_seen-m0,1,"same interface mouse report reaches second role");
    checki(mx,639,"12-bit combo relative X"); checki(my,401,"12-bit combo relative Y");
    checki(mm,EV_MOD_CTRL,"mouse report does not clear keyboard report modifiers");
    usb_remove_interfaces(&d);
    checki(usb_hid_key_held('s'),0,"device removal clears held game keys");
}
static void tablet_and_nkro(void)
{
    /* A real generic absolute HID layout, with nonzero and independent axis
     * ranges. Repeating the same sample must not integrate or jump to edges. */
    static const uint8_t rd[]={
        5,1,9,2,0xa1,1, 5,9,0x19,1,0x29,3,0x15,0,0x25,1,
        0x75,1,0x95,3,0x81,2,0x75,5,0x95,1,0x81,1,
        5,1,9,0x30,0x16,0xe8,3,0x26,0xb8,0x0b,0x75,16,0x95,1,0x81,2,
        9,0x31,0x16,0x18,0xfc,0x26,0xe8,3,0x81,2,0xc0
    };
    struct hid_desc hd; struct hid_mouse_state ms;
    checki(hid_parse_report_desc(rd,sizeof rd,&hd),0,"absolute tablet descriptor parses");
    uint8_t report[]={1,0xd0,7,0,0}; /* X=2000 of 1000..3000, Y=0 of -1000..1000 */
    checki(hid_decode_mouse(&hd,report,sizeof report,&ms),1,"absolute report decodes");
    int x=17,y=28; uint32_t b=0;
    hid_mouse_apply(&ms,1280,800,&x,&y,&b);
    checki(x,639,"tablet X range normalized"); checki(y,399,"tablet Y range normalized");
    hid_mouse_apply(&ms,1280,800,&x,&y,&b);
    checki(x,639,"identical tablet report does not drift X");
    checki(y,399,"identical tablet report does not drift Y");
    struct hid_mouse_state axes={.dx=5,.present=1};
    hid_mouse_apply(&axes,1280,800,&x,&y,&b);
    checki(b,1,"axis-only report preserves other report's held buttons");
    static const uint8_t nkro[]={5,1,9,6,0xa1,1,5,7,0x19,4,0x29,29,
        0x15,0,0x25,1,0x75,1,0x95,26,0x81,2,0xc0};
    struct hid_kbd_state ks; uint8_t keys[]={5,0,0,0};
    checki(hid_parse_report_desc(nkro,sizeof nkro,&hd),0,"NKRO keyboard descriptor parses");
    check(hid_looks_like_keyboard(&hd),"NKRO keyboard accepted without array/modifier fields");
    checki(hid_decode_keyboard(&hd,keys,sizeof keys,&ks),1,"NKRO bitmap decodes");
    checki(ks.nkeys,2,"NKRO bitmap reports two keys");
    checki(ks.keys[0],4,"NKRO A"); checki(ks.keys[1],6,"NKRO C");
}
static void protocol_refusal(void)
{
    struct usb_device d={.used=1}; iface(&d,0,kbd_rd,sizeof kbd_rd,1,1);
    refuse_protocol=1;
    checki(usb_bind_interfaces(&d,&registered,1),0,"refused wire protocol leaves interface unbound");
    refuse_protocol=0; checki(allocated,0,"failed protocol probe frees private state");
}
int main(void)
{
    if (usb_existing_regressions()) return 1;
    usb_hid_register(); composite(); multiplexed(); tablet_and_nkro(); protocol_refusal();
    if (failures) { printf("usb_input_extensions: %d FAILURE(S)\n",failures); return 1; }
    puts("usb_input_extensions: PASS composite interfaces, multiplexed roles, modifier snapshots, absolute tablet, NKRO, protocol refusal");
    return 0;
}
