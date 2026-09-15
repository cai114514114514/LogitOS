/* SPDX-License-Identifier: MIT */
/* Production lsock/unix/poll/wait code; only file allocation, credentials and
 * the scheduler's privileged leaves are hosted. INET calls abort, never fake
 * readiness. Timed-out parks are observed directly instead of benchmarking host
 * elapsed time, and injection happens after the real readiness query. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kpoll.h"
#include "file.h"
#include "lsock.h"
#include "unix.h"
#include "vfs_meta.h"


void hostsched_init(void);
int unix_host_block_until(spinlock_t *,uint64_t,uint64_t);
static atomic_int blocks,timeouts,allocations;
static int checks,failures;
static void check(int ok,const char *name)
{checks++;if(!ok){failures++;printf("FAIL: %s\n",name);}}
int sched_block_self_unlock_until(spinlock_t *lock,uint64_t flags,uint64_t deadline)
{atomic_fetch_add(&blocks,1);int awake=unix_host_block_until(lock,flags,deadline);if(!awake)atomic_fetch_add(&timeouts,1);return awake;}
void *kmalloc(size_t n){void *p=calloc(1,n);if(p)atomic_fetch_add(&allocations,1);return p;}
void kfree(void *p){if(p)atomic_fetch_sub(&allocations,1);free(p);}
struct file *file_alloc(void){struct file *f=calloc(1,sizeof *f);if(f)f->refcount=1;return f;}
int file_close(struct file *f){if(f){lsock_file_release_backing(f->backing);free(f);}return 0;}
int vfs_may_create(const char *p){(void)p;return 0;}
int vfs_cred_get(int pid,struct vcred *c){(void)pid;memset(c,0,sizeof *c);return 0;}
uint32_t vmeta_umask(int pid,int set){(void)pid;(void)set;return 0;}
int ksig_interrupted(void){return 0;}
int ksig_post_current(int signo){(void)signo;return 0;}
int net_up(void){return 1;}

static struct file *socket_new(int type)
{int e=0;struct file *s=lsock_create(LOGIT_AF_UNIX,type,0,1,&e);if(!s){fprintf(stderr,"socket setup failed %d\n",e);exit(2);}s->flags=O_NONBLOCK;return s;}
static void pair(int type,struct file **a,struct file **b)
{int e=0;if(lsock_socketpair(LOGIT_AF_UNIX,type,0,1,a,b,&e)){fprintf(stderr,"pair setup failed %d\n",e);exit(2);}(*a)->flags=(*b)->flags=O_NONBLOCK;}
static short ready(void *f,struct poll_table *pt){return lsock_file_poll(f,pt);}
static int pollone(struct file *f,short events,int ms,short *mask)
{struct pollsrc p={f,ready,events,0};int r=poll_core(&p,1,ms);if(mask)*mask=p.revents;return r;}
static void put(struct file *f,const void *p,int n)
{if(lsock_file_write(f,p,n)!=n){fprintf(stderr,"write setup failed\n");exit(2);}}
static void take(struct file *f,void *p,int n)
{if(lsock_file_read(f,p,n)!=n){fprintf(stderr,"read setup failed\n");exit(2);}}

struct injected {struct file *reader,*writer;int once;};
static short inject_ready(void *v,struct poll_table *pt)
{struct injected *i=v;short m=lsock_file_poll(i->reader,pt);if(pt&&i->once){i->once=0;put(i->writer,"x",1);}return m;}
struct action {struct file *socket;int read;int observed_blocks;};
static void *act_after_park(void *v)
{struct action *a=v;while(atomic_load(&blocks)==a->observed_blocks){}char b;
 if(a->read)take(a->socket,&b,1);else put(a->socket,"x",1);return 0;}
static void test_stream(void)
{
 struct file *a,*b;short m;char data[4096];memset(data,42,sizeof data);pair(LOGIT_SOCK_STREAM,&a,&b);
 check(lsock_file_poll(a,0)==LPOLLOUT,"stream: empty connected socket is writable only");
 int before=atomic_load(&timeouts);
 check(pollone(a,LPOLLIN,30,&m)==0&&m==0,"stream: empty input poll times out");
 check(atomic_load(&timeouts)>before,"stream: idle poll actually parks");
 put(a,data,sizeof data);check(!(lsock_file_poll(a,0)&LPOLLOUT),"stream: full output removes writable");
 check(lsock_file_poll(b,0)&LPOLLIN,"stream: peer data readable");
 struct action action={b,1,atomic_load(&blocks)};pthread_t actor;pthread_create(&actor,0,act_after_park,&action);before=atomic_load(&timeouts);
 int r=pollone(a,LPOLLOUT,400,&m);pthread_join(actor,0);
 check(r==1&&(m&LPOLLOUT)&&atomic_load(&timeouts)==before,"stream: draining peer wakes writer without timeout");
 take(b,data,4095);
 struct injected injection={b,a,1};struct pollsrc p={&injection,inject_ready,LPOLLIN,0};before=atomic_load(&blocks);
 check(poll_core(&p,1,100)==1&&(p.revents&LPOLLIN)&&atomic_load(&blocks)==before,"registration: query-to-sleep event cannot be lost");take(b,data,1);
 action=(struct action){a,0,atomic_load(&blocks)};pthread_create(&actor,0,act_after_park,&action);before=atomic_load(&timeouts);
 r=pollone(b,LPOLLIN,400,&m);pthread_join(actor,0);
 check(r==1&&(m&LPOLLIN)&&atomic_load(&timeouts)==before,"stream: data wakes parked reader");take(b,data,1);
 check(lsock_shutdown(a,LOGIT_SHUT_WR)==0,"stream: half-close setup");
 check((lsock_file_poll(b,0)&(LPOLLIN|LPOLLHUP))==LPOLLIN,"stream: peer write shutdown gives EOF without full HUP");
 check(lsock_file_read(b,data,1)==0,"stream: read confirms half-close EOF");
 check(pollone(a,LPOLLIN,20,&m)==0,"stream: own write shutdown still permits read wait");
 put(b,"tail",4);file_close(b);
 check((lsock_file_poll(a,0)&(LPOLLIN|LPOLLHUP))==(LPOLLIN|LPOLLHUP),"stream: buffered close gives readable and HUP");
 take(a,data,4);check(!memcmp(data,"tail",4),"stream: close retains queued bytes");
 check(lsock_file_read(a,data,1)==0,"stream: drained close EOF");file_close(a);
 /* A peer can terminate both directions without releasing its descriptor.
  * Consumers waiting with events=0 still need the unconditional HUP. */
 pair(LOGIT_SOCK_STREAM,&a,&b);
 check(lsock_shutdown(b,LOGIT_SHUT_RDWR)==0,"stream: full peer shutdown setup");
 check(pollone(a,0,20,&m)==1&&(m&LPOLLHUP),"stream: full peer shutdown wakes HUP-only poll");
 check(lsock_file_read(a,data,1)==0,"stream: full peer shutdown confirms EOF");
 file_close(a);file_close(b);
}
static void test_listener(void)
{
 struct file *l=socket_new(LOGIT_SOCK_STREAM),*c=socket_new(LOGIT_SOCK_STREAM);short m;int e=0;
 check(lsock_bind_unix(l,"/poll-listener")==0&&lsock_listen(l,4)==0,"listener: bind/listen setup");
 check(lsock_file_poll(l,0)==0,"listener: valid empty listener is not NVAL");
 check(pollone(l,LPOLLIN,20,&m)==0,"listener: idle waits until timeout");
 check(lsock_connect_unix(c,"/poll-listener")==0,"listener: connect queues accept");
 check(lsock_file_poll(l,0)==LPOLLIN,"listener: pending accept readable");
 struct file *a=lsock_accept(l,0,0,&e);check(a!=0,"listener: actual accept succeeds");
 check(lsock_file_poll(l,0)==0,"listener: accept drains readiness");
 file_close(a);file_close(c);file_close(l);
}
static void test_records(int type)
{
 struct file *a,*b;pair(type,&a,&b);char c;
 for(int i=0;i<32;i++)put(a,"r",1);
 check(!(lsock_file_poll(a,0)&LPOLLOUT),"records: full descriptor ring blocks output with byte room");
 take(b,&c,1);check(lsock_file_poll(a,0)&LPOLLOUT,"records: draining record restores writable");
 file_close(a);file_close(b);
}
static void test_shutdown_states(void)
{
 /* Explicit contract table, cross-checked by real nonblocking reads/writes.
  * Run every state with no data and with one buffered byte. A ready OUT can
  * report an immediate write error; it promises no sleep, not successful I/O. */
 static const struct {
  const char *name;int side,how;short empty,buffered;long read_empty,read_buffer,write;
 } cases[]={
  {"open",-1,0,LPOLLOUT,LPOLLIN|LPOLLOUT,EAGAIN_RC,1,1},
  {"local RD",0,LOGIT_SHUT_RD,LPOLLIN|LPOLLOUT,LPOLLIN|LPOLLOUT,0,0,1},
  {"local WR",0,LOGIT_SHUT_WR,LPOLLOUT,LPOLLIN|LPOLLOUT,EAGAIN_RC,1,-1},
  {"local full",0,LOGIT_SHUT_RDWR,LPOLLIN|LPOLLOUT|LPOLLHUP,LPOLLIN|LPOLLOUT|LPOLLHUP,0,0,-1},
  {"peer RD",1,LOGIT_SHUT_RD,LPOLLOUT,LPOLLIN|LPOLLOUT,EAGAIN_RC,1,-1},
  {"peer WR",1,LOGIT_SHUT_WR,LPOLLIN|LPOLLOUT,LPOLLIN|LPOLLOUT,0,1,1},
  {"peer full",1,LOGIT_SHUT_RDWR,LPOLLIN|LPOLLOUT|LPOLLHUP,LPOLLIN|LPOLLOUT|LPOLLHUP,0,1,-1},
  {"peer close",2,0,LPOLLIN|LPOLLOUT|LPOLLHUP,LPOLLIN|LPOLLOUT|LPOLLHUP,0,1,-1},
 };
 for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++)for(int buffered=0;buffered<2;buffered++){
  struct file *a,*b;pair(LOGIT_SOCK_STREAM,&a,&b);char byte,name[100];
  if(buffered)put(b,"s",1);
  if(cases[i].side==2){file_close(b);b=0;}
  else if(cases[i].side>=0&&lsock_shutdown(cases[i].side?b:a,cases[i].how))exit(2);
  snprintf(name,sizeof name,"states: %s buffered=%d mask",cases[i].name,buffered);
  check(lsock_file_poll(a,0)==(buffered?cases[i].buffered:cases[i].empty),name);
  snprintf(name,sizeof name,"states: %s buffered=%d actual read",cases[i].name,buffered);
  check(lsock_file_read(a,&byte,1)==(buffered?cases[i].read_buffer:cases[i].read_empty),name);
  snprintf(name,sizeof name,"states: %s buffered=%d actual write",cases[i].name,buffered);
  check(lsock_file_write(a,"w",1)==cases[i].write,name);
  file_close(a);file_close(b);
 }
}
static void test_datagram(void)
{
 struct file *a=socket_new(LOGIT_SOCK_DGRAM),*b=socket_new(LOGIT_SOCK_DGRAM);char data[4096];short m;
 check(lsock_bind_unix(a,"/poll-dgram-a")==0&&lsock_bind_unix(b,"/poll-dgram-b")==0,"datagram: names setup");
 check(lsock_connect_unix(a,"/poll-dgram-b")==0&&lsock_connect_unix(b,"/poll-dgram-a")==0,"datagram: destinations setup");
 check(lsock_file_poll(a,0)==LPOLLOUT,"datagram: empty local RX and writable peer");
 memset(data,1,sizeof data);put(a,data,sizeof data);
 check(!(lsock_file_poll(a,0)&LPOLLOUT),"datagram: full peer inbox removes writable");
 /* A one-byte receive discards the rest of this record and frees peer room. */
 struct action action={b,1,atomic_load(&blocks)};pthread_t actor;pthread_create(&actor,0,act_after_park,&action);int before=atomic_load(&timeouts);
 int r=pollone(a,LPOLLOUT,400,&m);pthread_join(actor,0);
 check(r==1&&(m&LPOLLOUT)&&atomic_load(&timeouts)==before,"datagram: peer drain wakes aggregate poll queue");
 action=(struct action){b,0,atomic_load(&blocks)};pthread_create(&actor,0,act_after_park,&action);before=atomic_load(&timeouts);
 r=pollone(a,LPOLLIN,400,&m);pthread_join(actor,0);
 check(r==1&&(m&LPOLLIN)&&atomic_load(&timeouts)==before,"datagram: local inbox wakes aggregate poll queue");take(a,data,1);
 file_close(b);check(lsock_file_poll(a,0)&LPOLLERR,"datagram: disappeared destination reports error");
 b=socket_new(LOGIT_SOCK_DGRAM);check(lsock_bind_unix(b,"/poll-dgram-b")==0,"datagram: destination name reused");
 check((lsock_file_poll(a,0)&(LPOLLOUT|LPOLLERR))==LPOLLOUT,"datagram: readiness re-resolves reused destination");
 file_close(a);file_close(b);
}
static void test_capacity(void)
{
 struct file *f[32];struct pollsrc p[32];
 for(int i=0;i<32;i++){f[i]=socket_new(LOGIT_SOCK_DGRAM);p[i]=(struct pollsrc){f[i],ready,LPOLLIN,0};}
 check(poll_core(p,32,20)==0,"capacity: 32 datagram fds register within poll table limit");
 for(int i=0;i<32;i++)file_close(f[i]);
}
int main(void)
{
 hostsched_init();test_listener();
 /* The old missing-hook control must fail by assertion before actor threads
  * whose handshake requires poll to park; a timeout would be inconclusive. */
 if(failures)return 1;
 test_stream();test_shutdown_states();test_records(LOGIT_SOCK_SEQPACKET);test_records(LOGIT_SOCK_DGRAM);test_datagram();test_capacity();
 check(lsock_file_poll(0,0)==LPOLLNVAL,"invalid: missing file remains NVAL");
 int e=0;struct file *inet=lsock_create(LOGIT_AF_INET,LOGIT_SOCK_STREAM,0,1,&e);
 check(inet&&lsock_file_poll(inet,0)==LPOLLNVAL,"INET: unsupported readiness remains explicit NVAL");file_close(inet);
 check(unix_stat(UNIXSTAT_SOCKS)==0&&atomic_load(&allocations)==0,"lifetime: socket queues released after poll unregister");
 printf("Unix poll: %d checks, %d failures, %d parks, %d timeouts\n",checks,failures,atomic_load(&blocks),atomic_load(&timeouts));return failures?1:0;
}
