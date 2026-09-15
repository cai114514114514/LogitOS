/* A real PTY byte/line discipline, shared by SSH and future GUI terminals.
 * Every queue predicate and its mutation use the waitq lock. A blocked I/O
 * owns a file reference; last-close serializes both ends before freeing the
 * backing, so poll/read/close cannot observe a recycled terminal. */
#include "pty.h"
#include "kpoll.h"
#include "kheap.h"
#include "ksignal.h"
#include "proc.h"
#include "usercopy.h"
#include "logit_abi.h"
#include <stddef.h>
void *memset(void *, int, size_t);
#define PQ 32768
#define EDIT 4096
struct pq { unsigned char b[PQ]; unsigned head, tail, n; };
struct pty {
    struct waitq wq;
    struct pq in, out;
    unsigned char edit[EDIT]; unsigned editing;
    int master, slave, eof;
    uint64_t id;
    int session, fg_pgid;
    unsigned signals;
    struct logit_termios attr;
    struct logit_winsize win;
};
static spinlock_t life = SPINLOCK_INIT;
static uint64_t next_pty_id = 1;
static void push(struct pq *q,unsigned char c) { q->b[q->head]=c;q->head=(q->head+1)%PQ;q->n++; }
static int pop(struct pq *q) { int c=q->b[q->tail];q->tail=(q->tail+1)%PQ;q->n--;return c; }
static void nap(struct pty *p,uint64_t *flags)
{
    struct waiter w;waitq_enqueue(&p->wq,&w);
    sched_block_self_unlock(&p->wq.lock,*flags);
    *flags=spin_lock_irqsave(&p->wq.lock);waitq_dequeue(&p->wq,&w);
}
int pty_open(struct file **master,struct file **slave)
{
    struct pty *p=kmalloc(sizeof *p);if(!p)return -1;memset(p,0,sizeof *p);waitq_init(&p->wq);
    /* Do not key a controlling terminal by pid or pointer: both are reused.
     * IDs are monotonic for this boot; after uint64_t exhaustion allocation
     * refuses instead of wrapping onto a live or historical identity. */
    uint64_t life_flags=spin_lock_irqsave(&life);
    p->id=next_pty_id;
    if(next_pty_id)next_pty_id++;
    spin_unlock_irqrestore(&life,life_flags);
    if(!p->id){kfree(p);return -1;}
    struct file *m=file_alloc(),*s=file_alloc();
    if(!m||!s){if(m)file_close(m);if(s)file_close(s);kfree(p);return -1;}
    p->master=p->slave=1;p->win.rows=24;p->win.cols=80;
    p->attr.iflag=LPTY_ICRNL;p->attr.oflag=LPTY_OPOST|LPTY_ONLCR;
    p->attr.lflag=LPTY_ISIG|LPTY_ICANON|LPTY_ECHO|LPTY_ECHOE;
    p->attr.cc[LPTY_VINTR]=3;p->attr.cc[LPTY_VQUIT]=28;p->attr.cc[LPTY_VERASE]=127;
    p->attr.cc[LPTY_VKILL]=21;p->attr.cc[LPTY_VEOF]=4;p->attr.cc[LPTY_VMIN]=1;
    /* Both endpoints are the existing terminal fd kind. A NULL F_TTY backing
     * is the serial console; this non-NULL backing selects the PTY discipline
     * inside file.c. A sixth kind made fstat/isatty grow a parallel path for
     * bytes that are still, semantically, a terminal. */
    m->type=s->type=F_TTY;m->backing=s->backing=p;m->is_write=1;s->is_write=0;
    m->amode=s->amode=O_RDWR;*master=m;*slave=s;return 0;
}
long pty_read(struct file *f,void *data,long len)
{
    if(len<=0)return 0;struct pty *p=f->backing;int master=f->is_write;
    struct pq *q=master?&p->out:&p->in;uint64_t flags=spin_lock_irqsave(&p->wq.lock);
    while(!q->n && (master?p->slave:p->master) && !(p->eof&&!master) && !ksig_interrupted()){
        if(f->flags&O_NONBLOCK)break;nap(p,&flags);
    }
    long n=0;
    if(q->n) { unsigned char *b=data;while(n<len&&q->n){int c=pop(q);b[n++]=c;if(!master&&(p->attr.lflag&LPTY_ICANON)&&c=='\n')break;} }
    else if(!master&&p->eof)p->eof--;
    else if(master?p->slave:p->master)n=ksig_interrupted()?SIG_E_INTR:LSK_E_AGAIN;
    spin_unlock_irqrestore(&p->wq.lock,flags);waitq_wake_all(&p->wq);return n;
}
long pty_write(struct file *f,const void *data,long len)
{
    struct pty *p=f->backing;const unsigned char *b=data;long n=0;
    while(n<len){
        uint64_t flags=spin_lock_irqsave(&p->wq.lock);int master=f->is_write;
        /* Reserve enough room before consuming a byte, including a complete
         * edited line and erase/newline echo. This never drops accepted input. */
        while((master?p->slave:p->master) &&
              (master ? (PQ-p->in.n<p->editing+1 || PQ-p->out.n<3) : PQ-p->out.n<2) && !ksig_interrupted()){
            if(f->flags&O_NONBLOCK)break;nap(p,&flags);
        }
        int alive=master?p->slave:p->master;
        int room=master?(PQ-p->in.n>=p->editing+1&&PQ-p->out.n>=3):(PQ-p->out.n>=2);
        if(!alive||!room||ksig_interrupted()){
            spin_unlock_irqrestore(&p->wq.lock,flags);
            return n?n:!alive?-1:ksig_interrupted()?SIG_E_INTR:LSK_E_AGAIN;
        }
        unsigned char c=b[n++];
        if(!master){
            if(c=='\n'&&(p->attr.oflag&LPTY_OPOST)&&(p->attr.oflag&LPTY_ONLCR))push(&p->out,'\r');
            push(&p->out,c);
        }else{
            if(c=='\r'&&(p->attr.iflag&LPTY_ICRNL))c='\n';
            int isig=(p->attr.lflag&LPTY_ISIG) && c && (c==p->attr.cc[LPTY_VINTR]||c==p->attr.cc[LPTY_VQUIT]);
            if(isig){
                int signal=c==p->attr.cc[LPTY_VINTR]?LOGIT_SIGINT:LOGIT_SIGQUIT;
                p->signals|=1u<<signal;p->editing=0;p->in.n=p->in.head=p->in.tail=0;
            }else if((p->attr.lflag&LPTY_ICANON)&&c&&c==p->attr.cc[LPTY_VERASE]){
                if(p->editing){p->editing--;if((p->attr.lflag&(LPTY_ECHO|LPTY_ECHOE))==(LPTY_ECHO|LPTY_ECHOE)){push(&p->out,8);push(&p->out,' ');push(&p->out,8);}}
            }else if((p->attr.lflag&LPTY_ICANON)&&c&&c==p->attr.cc[LPTY_VKILL]){
                p->editing=0;if(p->attr.lflag&LPTY_ECHO){push(&p->out,'\r');push(&p->out,'\n');}
            }else{
                int eof=(p->attr.lflag&LPTY_ICANON)&&c&&c==p->attr.cc[LPTY_VEOF];
                if(p->attr.lflag&LPTY_ICANON){
                    if(!eof && p->editing<EDIT)p->edit[p->editing++]=c;
                    if(c=='\n'||eof||p->editing==EDIT){
                        if(eof&&!p->editing)p->eof++;
                        for(unsigned i=0;i<p->editing;i++)push(&p->in,p->edit[i]);p->editing=0;
                    }
                }else push(&p->in,c);
                if(!eof&&(p->attr.lflag&LPTY_ECHO)){if(c=='\n')push(&p->out,'\r');push(&p->out,c);}
            }
        }
        spin_unlock_irqrestore(&p->wq.lock,flags);waitq_wake_all(&p->wq);
    }
    return n;
}
short pty_poll(struct file *f,struct poll_table *pt)
{
    struct pty *p=f->backing;poll_wait(pt,&p->wq);uint64_t fl=spin_lock_irqsave(&p->wq.lock);
    int master=f->is_write;short r=0;
    if((master?p->out.n:p->in.n)||(!master&&p->eof))r|=LPOLLIN;
    if(!(master?p->slave:p->master))r|=LPOLLHUP|LPOLLIN;
    else if(master?(PQ-p->in.n>=p->editing+1&&PQ-p->out.n>=3):(PQ-p->out.n>=2))r|=LPOLLOUT;
    spin_unlock_irqrestore(&p->wq.lock,fl);return r;
}
void pty_release(void *backing,int master)
{
    struct pty *p=backing;if(!p)return;
    uint64_t life_flags=spin_lock_irqsave(&life),fl=spin_lock_irqsave(&p->wq.lock);
    if(master)p->master=0;else p->slave=0;int dead=!p->master&&!p->slave;
    spin_unlock_irqrestore(&p->wq.lock,fl);if(!dead)waitq_wake_all(&p->wq);
    spin_unlock_irqrestore(&life,life_flags);
    if(dead){proc_clear_ctty(p->id);kfree(p);}
}
long pty_syscall(long num,long a,long b,long c)
{
    struct proc *owner=proc_current();if(!owner)return -1;
    if(num==SYS_PTY_OPEN){
        int fds[2];if(!user_range_ok((void *)a,sizeof fds,1))return -1;
        struct file *m=0,*s=0;if(pty_open(&m,&s)<0)return -1;
        struct file *hold_m FILE_REF=m,*hold_s FILE_REF=s;file_dup(m);file_dup(s);
        if(proc_fd_pair(owner,m,s,fds)<0){file_close(m);file_close(s);return -1;}
        if(user_copy_to((void *)a,fds,sizeof fds)<0){proc_fd_close_if(owner,fds[0],hold_m);proc_fd_close_if(owner,fds[1],hold_s);return -1;}
        return 0;
    }
    struct file *f FILE_REF=proc_fd_acquire(owner,a);
    if(!f||f->type!=F_TTY||!f->backing)return -1;
    struct pty *p=f->backing;struct logit_termios attr;struct logit_winsize win;
    int caller_sid=0,caller_pgid=0;uint64_t caller_ctty=0;
    if((b==LPTY_SETCTTY||b==LPTY_GETPGRP||b==LPTY_SETPGRP) &&
       !proc_terminal_ids(&caller_sid,&caller_pgid,&caller_ctty))return -1;
    if(b==LPTY_SETPGRP && !proc_group_in_session((int)c,caller_sid))return -1;
    if(b==LPTY_DRAIN){
        uint64_t fl=spin_lock_irqsave(&p->wq.lock);
        while(p->out.n&&p->master&&!ksig_interrupted())nap(p,&fl);
        int r=ksig_interrupted()?SIG_E_INTR:!p->master?-1:0;
        spin_unlock_irqrestore(&p->wq.lock,fl);return r;
    }
    if(b==LPTY_SETATTR){
        if(user_copy_from(&attr,(void *)c,sizeof attr)<0)return -1;
        /* VMIN/VTIME timers and software XON/XOFF are still absent; do not
         * accept settings that would make reads behave differently. */
        if(attr.iflag&~LPTY_ICRNL || attr.oflag&~(LPTY_OPOST|LPTY_ONLCR) ||
           attr.lflag&~(LPTY_ISIG|LPTY_ICANON|LPTY_ECHO|LPTY_ECHOE) ||
           attr.cc[LPTY_VMIN]!=1 || attr.cc[LPTY_VTIME]!=0)return -1;
    }
    if(b==LPTY_SETWIN && user_copy_from(&win,(void *)c,sizeof win)<0)return -1;
    long r=0;uint64_t fl=spin_lock_irqsave(&p->wq.lock);
    switch(b){
    case LPTY_GETATTR:attr=p->attr;break;
    case LPTY_SETATTR:
        if((p->attr.lflag&LPTY_ICANON)&&!(attr.lflag&LPTY_ICANON)){
            if(PQ-p->in.n<p->editing){r=-1;break;}for(unsigned i=0;i<p->editing;i++)push(&p->in,p->edit[i]);p->editing=0;
        }
#ifdef PTY_NEGCTL_ECHO_STUCK
        /* test-pty's real-kernel control removes the ECHO transition only.
         * The syscall still returns success, so the observed master byte --
         * not a return code -- is what makes the control go red. */
        attr.lflag=(attr.lflag&~LPTY_ECHO)|(p->attr.lflag&LPTY_ECHO);
#endif
        p->attr=attr;break;
    case LPTY_GETWIN:win=p->win;break;
    case LPTY_SETWIN:p->win=win;p->signals|=1u<<LOGIT_SIGWINCH;break;
    case LPTY_SIGNAL:
        if(!f->is_write){r=-1;break;}
        for(int i=1;i<32;i++)if(p->signals&(1u<<i)){p->signals&=~(1u<<i);r=i;break;}break;
    case LPTY_FLUSH:
        if(c<0||c>2){r=-1;break;}
        if(c!=1){p->in.n=p->in.head=p->in.tail=p->editing=p->eof=0;}
        if(c!=0)p->out.n=p->out.head=p->out.tail=0;break;
    case LPTY_SETCTTY:
        /* TIOCSCTTY is slave-only and session-leader-only. proc_attach_ctty
         * also refuses a second terminal for this session; the PTY side
         * refuses a second live session for this terminal. Force-stealing a
         * terminal is deliberately absent because there is no privilege model
         * for that Linux extension to consult. */
        if(f->is_write||(p->session&&p->session!=caller_sid)||
           proc_attach_ctty(p->id)<0){r=-1;break;}
        p->session=caller_sid;p->fg_pgid=caller_pgid;break;
    case LPTY_GETPGRP:
        if(p->session!=caller_sid||caller_ctty!=p->id||p->fg_pgid<=0){r=-1;break;}
        r=p->fg_pgid;break;
    case LPTY_SETPGRP:
        if(p->session!=caller_sid||caller_ctty!=p->id){r=-1;break;}
        p->fg_pgid=(int)c;break;
    default:r=-1;break;
    }
    spin_unlock_irqrestore(&p->wq.lock,fl);waitq_wake_all(&p->wq);
    if(r<0)return r;
    if(b==LPTY_GETATTR)return user_copy_to((void *)c,&attr,sizeof attr)<0?-1:0;
    if(b==LPTY_GETWIN)return user_copy_to((void *)c,&win,sizeof win)<0?-1:0;
    return r;
}
