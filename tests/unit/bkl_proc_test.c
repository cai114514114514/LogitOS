/* SPDX-License-Identifier: MIT */
/* Real file.c + wait.c and extracted, unmodified proc_fd_* production bodies.
 * Host scheduling uses the existing pollhost park/unpark model. This proves
 * object ownership and parallel record integrity, not guest performance. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "file.h"
#include "proc.h"
#include "logit_abi.h"

void hostsched_init(void);
void stor_vfs_reset(void);
static int checks, failures;
#define CHECK(c, text) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n", text); } } while(0)
struct worker { struct file *f; unsigned id, n; int failed; };
struct rec { uint32_t id, index, guard, inverse; };
static void *writer(void *v)
{
    struct worker *w=v;
    for (unsigned i=0;i<w->n;i++) {
        struct rec r={w->id,i,0xbad00cab,~i};
        if(file_write(w->f,&r,sizeof r)!=sizeof r) { w->failed=1; break; }
    }
    file_close(w->f); return NULL;
}
static void *event_writer(void *v)
{
    struct worker *w=v; uint64_t one=1;
    for(unsigned i=0;i<w->n;i++) if(file_write(w->f,&one,8)!=8)w->failed=1;
    file_close(w->f); return NULL;
}
static void check_records(struct rec *r,unsigned count,unsigned each)
{
    unsigned char *seen=calloc(count,1); int intact=1;
    for(unsigned i=0;i<count;i++) {
        if(r[i].id>=4||r[i].index>=each||r[i].guard!=0xbad00cab||r[i].inverse!=~r[i].index) {intact=0;continue;}
        unsigned k=r[i].id*each+r[i].index;if(seen[k]++)intact=0;
    }
    for(unsigned i=0;i<count;i++) if(seen[i]!=1)intact=0;
    CHECK(intact,"all concurrent records arrive once, intact, without interleaving");free(seen);
}
static void fd_lifetime(void)
{
    struct proc p={0};p.fd_lock=(spinlock_t)SPINLOCK_INIT;
    struct file *f=file_alloc();CHECK(f!=NULL,"fresh description");f->type=F_TTY;
    int fd=proc_fd_alloc(&p,f);CHECK(fd==0,"lowest descriptor claimed");
    struct file *held=proc_fd_acquire(&p,fd);CHECK(held==f&&held->refcount==2,"acquire owns an independent reference");
    CHECK(proc_fd_close(&p,fd)==0,"close detaches table before teardown");
    struct file *next=file_alloc();CHECK(next!=held,"held description cannot be reused after descriptor close");
    CHECK(held->type==F_TTY,"held description retains its original backend");
    if(next!=held)file_close(next);
    file_close(held);
    struct file *a=file_alloc(),*b=file_alloc();int fds[2];
    CHECK(proc_fd_pair(&p,a,b,fds)==0&&fds[0]==0&&fds[1]==1,"pair published atomically");
    CHECK(proc_fd_dup2(&p,0,1)==1&&a->refcount==2,"dup2 exchanges target under fd-table lock");
    struct proc child={0};child.fd_lock=(spinlock_t)SPINLOCK_INIT;
    proc_fd_clone(&child,&p);CHECK(a->refcount==4,"fork snapshot owns each inherited reference");
    proc_fd_close_all(&p);CHECK(a->refcount==2,"closing parent preserves child descriptors");
    proc_fd_close_all(&child);CHECK(a->refcount==0,"all references return to baseline");
    struct file *old=file_alloc();fd=proc_fd_alloc(&p,old);file_dup(old);
    proc_fd_close(&p,fd);struct file *replacement=file_alloc();
    CHECK(proc_fd_alloc(&p,replacement)==fd,"sibling reuses descriptor during failed copyout");
    CHECK(proc_fd_close_if(&p,fd,old)==0&&p.fd[fd]==replacement,"failed copyout rollback cannot close replacement descriptor");
    CHECK(old->refcount==1,"rollback identity reference remains alive across close and reopen");
    file_close(old);proc_fd_close_all(&p);
}
static void pipe_parallel(void)
{
    enum { EACH=1000, N=4*EACH };
    struct file *r,*w;CHECK(file_pipe(&r,&w)==0,"pipe allocation");
    struct worker ws[4];pthread_t t[4];
    for(unsigned i=0;i<4;i++){ws[i]=(struct worker){w,i,EACH,0};file_dup(w);pthread_create(&t[i],NULL,writer,&ws[i]);}
    file_close(w);
    struct rec *all=calloc(N,sizeof *all);size_t got=0;
    while(got<N*sizeof *all){long n=file_read(r,(char *)all+got,N*sizeof *all-got);if(n<=0)break;got+=(size_t)n;}
    CHECK(got==N*sizeof *all,"pipe producer and consumer make progress while readers park");
    for(int i=0;i<4;i++){pthread_join(t[i],NULL);CHECK(!ws[i].failed,"parallel pipe writer completes");}
    CHECK(file_read(r,all,1)==0,"last writer close wakes EOF");
    if(got==N*sizeof *all)check_records(all,N,EACH);file_close(r);free(all);
}
static void vfs_parallel(void)
{
    enum { EACH=500,N=4*EACH };
    stor_vfs_reset();struct file *f=file_open_vfs("/store",O_CREAT|O_TRUNC|O_RDWR);
    CHECK(f!=NULL,"shared writable description");
    struct worker ws[4];pthread_t t[4];
    for(unsigned i=0;i<4;i++){ws[i]=(struct worker){f,i,EACH,0};file_dup(f);pthread_create(&t[i],NULL,writer,&ws[i]);}
    for(int i=0;i<4;i++){pthread_join(t[i],NULL);CHECK(!ws[i].failed,"shared cursor writer completes");}
    CHECK(file_lseek(f,0,SEEK_CUR)==N*sizeof(struct rec),"shared file cursor has no lost updates");
    CHECK(file_fsync(f)==0,"concurrent writes flush to VFS");
    CHECK(file_lseek(f,0,SEEK_SET)==0,"rewind shared description");
    struct rec *all=calloc(N,sizeof *all);CHECK(file_read(f,all,N*sizeof *all)==N*sizeof *all,"read entire serialized VFS content");
    check_records(all,N,EACH);free(all);file_close(f);
}
static void events_parallel(void)
{
    struct file *f=file_eventfd(0,O_NONBLOCK);CHECK(f!=NULL,"eventfd allocation");
    struct worker ws[4];pthread_t t[4];
    for(unsigned i=0;i<4;i++){ws[i]=(struct worker){f,i,5000,0};file_dup(f);pthread_create(&t[i],NULL,event_writer,&ws[i]);}
    for(int i=0;i<4;i++){pthread_join(t[i],NULL);CHECK(!ws[i].failed,"parallel eventfd writer completes");}
    uint64_t n=0;CHECK(file_read(f,&n,8)==8&&n==20000,"eventfd additions are neither lost nor duplicated");file_close(f);
}
int main(int argc,char **argv)
{
    (void)argv;hostsched_init();file_init();fd_lifetime();
    if(argc==1){pipe_parallel();vfs_parallel();events_parallel();}
    printf("BKL_PROC checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
