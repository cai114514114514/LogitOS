/* SPDX-License-Identifier: MIT
 * The production USB core is linked verbatim; only HCD registers, descriptors
 * and the kworker scheduler are fixtures.  Work stays queued until run_work(),
 * proving that enumeration is deferred out of the interrupt callback. */
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "usb.h"
#include "driver.h"
#include "ktime.h"
#include "work.h"

static int checks,failed,connected,changed,event_pending;
static int opened,closed,probed,removed,changed_calls,order_tick;
static int remove_order,close_order,open_order;
static struct work *queued_work;
static struct ktimer *root_timer;
static pthread_mutex_t sync_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cv=PTHREAD_COND_INITIALIZER;
static int block_events,events_entered,release_events,worker_started;
static int block_remove,remove_entered,release_remove;
#define CHECK(c,m) do { checks++;if(!(c)){failed++;printf("FAIL: %s\n",m);} } while(0)

void kprintf(const char *fmt,...) { (void)fmt; }
uint64_t timer_ms(void) { static uint64_t t;return ++t; }
int time_ready(void) { return 1; }
uint64_t time_mono_ns(void) { return timer_ms()*1000000; }
void sched_poll_wait(void) {}
void work_item_init(struct work *w,void (*fn)(void *),void *arg)
{ w->fn=fn;w->arg=arg;w->next=NULL;w->queued=0; }
int work_queue(struct work *w)
{
    if (w->queued) return 0;
    w->queued=1;queued_work=w;return 1;
}
int ktimer_add(struct ktimer *t,uint64_t delay,uint64_t period,ktimer_fn fn,void *arg,const char *name)
{
    t->expires_ns=delay;t->period_ns=period;t->fn=fn;t->arg=arg;t->name=name;
    t->heap_idx=0;root_timer=t;return 0;
}
int ktimer_cancel(struct ktimer *t)
{
    int active=root_timer==t;
    if(active)root_timer=NULL;
    t->heap_idx=-1;t->period_ns=0;return active;
}
static void fire_root_timer(void)
{ struct ktimer *t=root_timer;if(t&&t->fn)t->fn(t); }
static void run_work(void)
{
    struct work *w=queued_work;queued_work=NULL;
    if (!w) return;
    w->queued=0;w->fn(w->arg);
}
int dev_irq_request(struct device *p,irq_handler_t fn,void *arg,const char *name)
{ (void)fn;(void)arg;(void)name;p->irq_vec=97;return 97; }
int dev_irq_release(struct device *p) { p->irq_vec=-1;return 0; }

static int root_count(struct usb_hc *h) { (void)h;return 1; }
static int root_connected(struct usb_hc *h,int p) { (void)h;return p==1&&connected; }
static int root_reset(struct usb_hc *h,int p,int *speed)
{ (void)h;if(p!=1||!connected)return -1;*speed=USB_SPEED_HIGH;return 0; }
static int root_pending(struct usb_hc *h)
{ (void)h;int p=event_pending;event_pending=0;return p; }
static int root_changed(struct usb_hc *h,int p,int *state)
{
    (void)h;changed_calls++;
    if (p!=1) return -1;
    if (!changed) return 0;
    changed=0;*state=connected;return 1;
}
static int device_open(struct usb_device *d)
{ d->addr=(uint8_t)(++opened);d->slot=d->addr;open_order=++order_tick;return 0; }
static void device_close(struct usb_device *d)
{ (void)d;closed++;close_order=++order_tick; }
static int set_ep0(struct usb_device *d,int packet) { (void)d;return packet==64?0:-1; }
static int configure(struct usb_device *d,const struct usb_interface *it)
{ (void)d;(void)it;return 0; }
static int control(struct usb_device *d,uint8_t rt,uint8_t req,uint16_t val,
                   uint16_t idx,void *buf,uint16_t len)
{
    (void)d;(void)rt;(void)idx;
    if (req==USB_REQ_GET_DESCRIPTOR && (val>>8)==USB_DT_DEVICE) {
        uint8_t b[18]={18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
        if(len>sizeof b)len=sizeof b;memcpy(buf,b,len);return len;
    }
    if (req==USB_REQ_GET_DESCRIPTOR && (val>>8)==USB_DT_CONFIG) {
        uint8_t b[18]={9,2,18,0,1,1,0,0x80,50,9,4,0,0,0,0xff,0,0,0};
        if(len>sizeof b)len=sizeof b;memcpy(buf,b,len);return len;
    }
    if (req==USB_REQ_SET_CONFIGURATION) return 0;
    return -1;
}
static void events(struct usb_hc *h)
{
    (void)h;
    pthread_mutex_lock(&sync_mu);
    if(block_events) {
        events_entered=1;pthread_cond_broadcast(&sync_cv);
        while(!release_events)pthread_cond_wait(&sync_cv,&sync_mu);
    }
    pthread_mutex_unlock(&sync_mu);
}
static int stop(struct usb_hc *h) { (void)h;return 0; }
static const struct usb_hc_ops ops={
    .name="hotplug-fixture",.root_port_count=root_count,
    .root_port_connected=root_connected,.root_port_reset=root_reset,
    .root_change_pending=root_pending,.root_port_changed=root_changed,
    .device_open=device_open,.device_close=device_close,.set_ep0_packet=set_ep0,
    .configure=configure,.control=control,.events=events,.shutdown=stop,
};
static int leaf_probe(struct usb_device *d,int ifno)
{ (void)d;(void)ifno;probed++;return 0; }
static void leaf_remove(struct usb_device *d,int ifno)
{
    (void)d;(void)ifno;
    pthread_mutex_lock(&sync_mu);
    if(block_remove) {
        remove_entered=1;pthread_cond_broadcast(&sync_cv);
        while(!release_remove)pthread_cond_wait(&sync_cv,&sync_mu);
    }
    pthread_mutex_unlock(&sync_mu);
    __atomic_add_fetch(&removed,1,__ATOMIC_SEQ_CST);remove_order=++order_tick;
}
static const struct usb_match leaf_ids[]={{0xff,0,0},{0,0,0}};
static const struct usb_driver leaf={.name="hot-leaf",.match=leaf_ids,
    .probe=leaf_probe,.remove=leaf_remove};
void usb_hub_register(void) {}
void usb_hid_register(void) {}
void usb_msc_register(void) { usb_register_driver(&leaf); }

static void signal_change(struct usb_hc *h,int new_connected,int has_csc)
{
    connected=new_connected;changed=has_csc;event_pending=1;usb_hc_irq(h);
}

static void *blocking_irq(void *arg) { usb_hc_irq(arg);return NULL; }
static void *work_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&sync_mu);worker_started=1;pthread_cond_broadcast(&sync_cv);
    pthread_mutex_unlock(&sync_mu);run_work();return NULL;
}

int main(void)
{
    struct usb_hc hc={0};struct device pci={0};
    changed=1; /* controller reset left an empty-port CSC */
    CHECK(!usb_hc_register(&hc,&pci,&ops,NULL),"empty controller registers");
    CHECK(!usb_devices_found()&&!opened&&changed_calls==1,
          "disconnected root port is idle and startup CSC is consumed");

    connected=changed=event_pending=1;fire_root_timer();
    CHECK(queued_work&&opened==0,"lost connect IRQ is recovered by deferred root watcher");
    run_work();
    CHECK(opened==1&&probed==1&&usb_devices_found()==1,"post-boot connect enumerates and binds device");

    int before_open=opened,before_close=closed,before_remove=removed;
    signal_change(&hc,1,0);run_work();
    CHECK(opened==before_open&&closed==before_close&&removed==before_remove,
          "change interrupt without CSC leaves existing device untouched");

    order_tick=remove_order=close_order=open_order=0;
    signal_change(&hc,1,1);run_work();
    CHECK(opened==2&&closed==1&&removed==1&&remove_order<close_order&&close_order<open_order,
          "connected CSC retires stale class and endpoint before replacement");

    signal_change(&hc,0,1);run_work();
    CHECK(closed==2&&removed==2&&!usb_devices_found(),"disconnect removes binding and device");

    signal_change(&hc,1,1);run_work();
    CHECK(opened==3&&probed==3&&usb_devices_found()==1,"same root port reconnects cleanly");

    int calls=changed_calls;
    usb_hc_irq(&hc);run_work();
    CHECK(changed_calls==calls,"ordinary transfer IRQ schedules no hotplug scan");

    /* Queue a disconnect, then hold an IRQ in class polling while another CPU
     * runs the worker.  Remove must wait for the callback lock. */
    signal_change(&hc,0,1);
    pthread_mutex_lock(&sync_mu);
    block_events=1;events_entered=release_events=worker_started=0;
    pthread_mutex_unlock(&sync_mu);
    pthread_t irq_thread,worker_thread;
    pthread_create(&irq_thread,NULL,blocking_irq,&hc);
    pthread_mutex_lock(&sync_mu);
    while(!events_entered)pthread_cond_wait(&sync_cv,&sync_mu);
    pthread_mutex_unlock(&sync_mu);
    int before_concurrent=__atomic_load_n(&removed,__ATOMIC_SEQ_CST);
    pthread_create(&worker_thread,NULL,work_thread,NULL);
    pthread_mutex_lock(&sync_mu);
    while(!worker_started)pthread_cond_wait(&sync_cv,&sync_mu);
    pthread_mutex_unlock(&sync_mu);
    usleep(20000);
    int removed_while_polling=__atomic_load_n(&removed,__ATOMIC_SEQ_CST);
    pthread_mutex_lock(&sync_mu);release_events=1;pthread_cond_broadcast(&sync_cv);
    pthread_mutex_unlock(&sync_mu);
    pthread_join(irq_thread,NULL);pthread_join(worker_thread,NULL);
    CHECK(removed_while_polling==before_concurrent && removed==before_concurrent+1 && !usb_devices_found(),
          "disconnect waits for in-flight class callback before remove");
    pthread_mutex_lock(&sync_mu);block_events=0;pthread_mutex_unlock(&sync_mu);
    signal_change(&hc,1,1);run_work();

    /* A slow class teardown runs after callback_ready was cleared and after
     * the short IRQ lock was released, so unrelated HCD events still flow. */
    signal_change(&hc,0,1);
    pthread_mutex_lock(&sync_mu);
    block_remove=1;remove_entered=release_remove=0;
    block_events=1;events_entered=release_events=0;worker_started=0;
    pthread_mutex_unlock(&sync_mu);
    before_concurrent=__atomic_load_n(&removed,__ATOMIC_SEQ_CST);
    pthread_create(&worker_thread,NULL,work_thread,NULL);
    pthread_mutex_lock(&sync_mu);
    while(!remove_entered)pthread_cond_wait(&sync_cv,&sync_mu);
    pthread_mutex_unlock(&sync_mu);
    pthread_create(&irq_thread,NULL,blocking_irq,&hc);
    pthread_mutex_lock(&sync_mu);
    while(!events_entered)pthread_cond_wait(&sync_cv,&sync_mu);
    release_events=release_remove=1;pthread_cond_broadcast(&sync_cv);
    pthread_mutex_unlock(&sync_mu);
    pthread_join(irq_thread,NULL);pthread_join(worker_thread,NULL);
    CHECK(events_entered && removed==before_concurrent+1,
          "slow class remove runs outside non-sleeping callback lock");
    pthread_mutex_lock(&sync_mu);block_events=block_remove=0;pthread_mutex_unlock(&sync_mu);
    signal_change(&hc,1,1);run_work();

    signal_change(&hc,0,1); /* queued but intentionally not run */
    usb_hc_unregister(&hc);
    int after_unregister=changed_calls;
    run_work();
    CHECK(!usb_present()&&!usb_devices_found()&&changed_calls==after_unregister,
          "queued work after unregister observes closed admission and touches no HCD");
    printf("usb-hotplug: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
