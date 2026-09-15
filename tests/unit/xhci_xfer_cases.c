/* SPDX-License-Identifier: MIT */
/* Included after the common real-driver DMA fixture. This model substitutes
 * controller register side effects only; every transfer/recovery is production. */
static unsigned model_offset,model_chunks,model_bad_out,model_stops,model_dequeues;
static unsigned model_toggle_resets;
int usb_clear_tt_buffer(struct usb_device *d,uint8_t ep,unsigned type)
{ (void)d;(void)ep;(void)type;return 0; }
static uint64_t model_last_dequeue;
static int model_mode;
static void xhci_model_write(size_t off,uint64_t value)
{
    struct xhci *x=&g_xhci;
    if (off==0x2000 && x->cmd_want && stuck!=2) {
        unsigned i=x->cmd.enq?x->cmd.enq-1:x->cmd.n-2;
        struct trb *cmd=&x->cmd.trb[i];
        unsigned type=TRB_TYPE(cmd->control),slot=cmd->control>>24,dci=(cmd->control>>16)&31;
        if(slot && slot<=XHCI_MAX_SLOTS && x->slot[slot].in_ctx) {
            struct xhci_slot *s=&x->slot[slot];
            if(type==TRB_ADDRESS_DEVICE || type==TRB_CONFIG_EP || type==TRB_EVAL_CONTEXT) {
                uint32_t add=input_ctrl_ctx(s->in_ctx)[1];
                if (type==TRB_CONFIG_EP && (add&input_ctrl_ctx(s->in_ctx)[0]))model_toggle_resets++;
                for(unsigned c=0;c<32;c++)if(add&(1u<<c)) {
                    memcpy(s->dev_ctx+c*x->ctxsize,s->in_ctx+(c+1)*x->ctxsize,x->ctxsize);
                    if(c)((uint32_t *)(s->dev_ctx+c*x->ctxsize))[0]|=1;
                }
                slot_ctx(x,s->dev_ctx,0)[3]=slot;
            }
            if(type==15 || type==TRB_RESET_EP) {
                ep_ctx(x,s->dev_ctx,(int)dci,0)[0]=3;model_stops++;
            }
            if(type==16) { model_last_dequeue=cmd->param;model_dequeues++; }
        }
    }
    if(off!=0x2004 || !x->up || value>31 || !value) return;
    struct xhci_ep *ep=x->slot[1].ep[value];
    if(!ep)return;
    unsigned i=ep->ring.enq?ep->ring.enq-1:ep->ring.n-2;
    struct trb *trb=&ep->ring.trb[i];
    struct trb event={.param=ep->ring.dma_base+i*16,
        .status=CC_SUCCESS<<24,.control=TRB_SET_TYPE(TRB_TRANSFER_EVENT)|(1u<<24)|((uint32_t)value<<16)|x->ev.cycle};
    if(model_mode==4)return; /* timeout: no DMA completion */
    if(model_mode==2) {
        event.status=CC_STALL<<24;
        if(value==1)event.param-=16; /* failed DATA stage, not awaited status */
        ep_ctx(x,x->slot[1].dev_ctx,(int)value,0)[0]=2;
    } else if(value!=1) {
        unsigned n=trb->status&0x1ffff,got=n;
        if(model_mode==3)got=0;
        if(model_mode==1 && model_chunks==1)got=31;
        if(got<n)event.status=(CC_SHORT_PACKET<<24)|(n-got);
        if(value&1)for(unsigned j=0;j<got;j++)ep->buf[j]=(uint8_t)(model_offset+j);
        else for(unsigned j=0;j<got;j++)if(ep->buf[j]!=(uint8_t)(model_offset+j))model_bad_out++;
        model_offset+=got;model_chunks++;
        ep_ctx(x,x->slot[1].dev_ctx,(int)value,0)[0]=1;
    }
    x->ev.trb[x->ev.deq]=event;
}
static void model_transfer(int mode)
{ model_mode=mode;model_offset=model_chunks=model_bad_out=0; }
int main(void)
{
    device.res[0].flags=DEV_RES_MEM;device.res[0].size=65536;
    *(uint32_t*)(test_regs+XCAP_CAPLENGTH)=0x40;
    *(uint32_t*)(test_regs+XCAP_HCSPARAMS1)=8;
    *(uint32_t*)(test_regs+XCAP_HCCPARAMS1)=1;
    *(uint32_t*)(test_regs+XCAP_RTSOFF)=0x1000;
    *(uint32_t*)(test_regs+XCAP_DBOFF)=0x2000;
    *(uint32_t*)(test_regs+0x44)=1;
    check(xhci_init(&device)==0,"controller setup");
    struct usb_device d={.slot=1,.speed=USB_SPEED_HIGH,.port=2,.route=0x21};
    struct usb_device tt={.slot=7};d.tt_hub=&tt;d.tt_port=3;
    check(xhci_address_device(&d,0)==0,"address device behind hub");
    uint32_t *sc=slot_ctx(&g_xhci,g_xhci.slot[1].dev_ctx,0);
    check((sc[0]&0xfffff)==0x21 && (sc[1]>>16&255)==2,"xHCI preserves downstream route and root port");
    check((sc[2]&0xffff)==0x307,"xHCI uses TT slot ID and TT downstream port");
    check(xhci_configure_hub(&d,8,1,2)==0 && (sc[0]&(1u<<26)) && (sc[0]&(1u<<25)) && (sc[1]>>24)==8,"hub and multi-TT slot context is configured");
    struct usb_interface it={.n_ep=2,.ep={{.addr=0x81,.attr=USB_XFER_BULK,.max_packet=512},{.addr=0x02,.attr=USB_XFER_BULK,.max_packet=512}}};
    check(xhci_configure_ep(&d,&it)==0,"bulk endpoints configure");
    check((ep_ctx(&g_xhci,g_xhci.slot[1].dev_ctx,3,0)[0]>>16&255)==0,"bulk does not inherit interrupt polling interval");
    uint8_t data[32768];memset(data,0,sizeof data);
    model_transfer(0);
    check(xhci_bulk(&d,0x81,data,sizeof data)==sizeof data && model_chunks==8,"bulk reads 32KiB through eight owned DMA pages");
    int good=1;for(unsigned i=0;i<sizeof data;i++)if(data[i]!=(uint8_t)i)good=0;
    check(good,"bulk copies every completed byte in order");
    model_transfer(0);
    check(xhci_bulk(&d,0x02,data,sizeof data)==sizeof data && !model_bad_out,"bulk OUT preserves payload across chunk boundaries");
    model_transfer(1);
    check(xhci_bulk(&d,0x81,data,sizeof data)==4096+31 && model_chunks==2,"short packet stops bulk at its actual byte count");
    model_transfer(3);
    check(xhci_bulk(&d,0x81,data,512)==0,"zero-length short packet is successful zero bytes");
    check(xhci_clear_halt(&d,0x81)==0 && model_toggle_resets==1,"healthy endpoint clear-halt resets DATA0 with drop/add context");
    model_transfer(0);
    int success=1;for(int i=0;i<100;i++)if(xhci_control(&d,0x80,6,0,0,data,18)!=18)success=0;
    check(success && !g_xhci.slot[1].ep[1]->ring.pending,"control status completion retires the entire TD over repeated ring wraps");
    unsigned before=model_dequeues;model_transfer(2);
    check(xhci_control(&d,0x80,0xfe,0,0,data,1)<0,"control DATA-stage STALL is terminal without a status completion");
    struct xhci_ep *ep=g_xhci.slot[1].ep[1];
    check(model_dequeues==before+1 && model_last_dequeue==((ep->ring.dma_base+ep->ring.enq*16)|ep->ring.cycle),"STALL recovery advances dequeue past the failed control TD");
    model_transfer(0);check(xhci_control(&d,0x80,6,0,0,data,18)==18,"next control works after stalled GET_MAX_LUN style request");
    model_transfer(4);before=model_stops;
    check(xhci_bulk(&d,0x81,data,512)<0 && model_stops>before && g_xhci.up,"bulk timeout confirms endpoint stop before page reuse");
    model_transfer(0);check(xhci_bulk(&d,0x81,data,512)==512,"next bulk succeeds after confirmed timeout retirement");
    model_transfer(4);stuck=2;int retained=live;
    check(xhci_bulk(&d,0x81,data,512)<0 && !g_xhci.up && g_xhci.dma.blocked && live==retained,"unconfirmed endpoint stop quarantines DMA and refuses reuse");
    stuck=0;check(xhci_shutdown()==0 && !live,"halt acknowledgement releases quarantined endpoint pages");
    printf("xHCI transfers: %d checks, %d failures\n",checks,failures);return failures!=0;
}
