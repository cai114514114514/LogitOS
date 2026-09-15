/* SPDX-License-Identifier: MIT */
/* Real VFS dispatch on pthreads; backend barriers make lifetime/parallelism
 * assertions deterministic instead of relying on a lucky scheduler race. */
#define main vfs_sequential_main
#include "vfs_mount_test.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

struct probe_fs {
    struct filesystem fs;
    atomic_int alive, entered, release, removed, bad_lifetime;
    char scratch[64];
};
static int probe_mount(struct filesystem *f)
{ atomic_store(&((struct probe_fs *)f->priv)->alive, 1); return 0; }
static void probe_umount(struct filesystem *f)
{
    struct probe_fs *p = f->priv;
    atomic_store(&p->alive, 0); atomic_store(&p->removed, 1);
}
static int probe_count(struct filesystem *f, const char *p)
{ (void)f; return strcmp(p, "/") ? -1 : 4; }
static int probe_size(struct filesystem *f, const char *p)
{ (void)f; return strcmp(p, "/") ? 1 : -1; }
static int probe_read(struct filesystem *f, const char *path, void *out, int cap)
{
    (void)path;
    struct probe_fs *p = f->priv;
    atomic_store(&p->entered, 1);
    while (!atomic_load(&p->release)) sched_yield();
    /* The caller retains a mount reference even after removal admission closes.
     * A nested stat must still resolve this same backend and may recurse. */
    struct vattr a;
    if (vfs_statx(f->name, &a, 1) || !atomic_load(&p->alive))
        atomic_store(&p->bad_lifetime, 1);
    if (cap) ((char *)out)[0] = 'x';
    return cap ? 1 : 0;
}
static const char *probe_name(struct filesystem *f, const char *d, int i)
{
    (void)d;
    struct probe_fs *p = f->priv;
    snprintf(p->scratch, sizeof p->scratch, "entry-%d", i);
    sched_yield();
    return p->scratch;
}
static const struct fs_iops probe_ops = {
    .mount=probe_mount, .umount=probe_umount, .size=probe_size,
    .read=probe_read, .count=probe_count, .ent_name=probe_name
};
static int await_flag(atomic_int *p, int value)
{
    for (int i=0;i<1000;i++) {
        if (atomic_load(p)==value) return 1;
        nanosleep(&(struct timespec){0,1000000},0);
    }
    return 0;
}
static void *reader(void *path)
{ char out; return (void *)(intptr_t)(vfs_read(path,&out,1)==1 && out=='x'); }
static void *remover(void *path)
{ return (void *)(intptr_t)(vfs_umount(path)==0); }
struct names_arg { int id; atomic_int *errors; };
static void *names(void *arg)
{
    struct names_arg *a=arg; char expected[64];
    snprintf(expected,sizeof expected,"entry-%d",a->id);
    for (int i=0;i<2000;i++) {
        const char *got=vfs_ent_name("/a",a->id);
        sched_yield(); /* another task may reuse BACKEND scratch after return */
        if (strcmp(got,expected)) atomic_fetch_add(a->errors,1);
    }
    return 0;
}
static atomic_int drain_ready, drain_release;
static void *drainer(void *arg)
{
    (void)arg; struct vfs_drain token;
    int rc=vfs_drain_begin(&token);
    atomic_store(&drain_ready,rc==0?1:-1);
    while(!atomic_load(&drain_release))sched_yield();
    if(rc==0)vfs_drain_end(&token);
    return (void *)(intptr_t)(rc==0);
}
int main(void)
{
    if (vfs_sequential_main()) return 1;
    fixture();
    eqi(vfs_mkdir("/a"),0,"parallel fixture a");
    eqi(vfs_mkdir("/b"),0,"parallel fixture b");
    struct probe_fs a={0},b={0};
    a.fs=(struct filesystem){.name="/a/value",.iops=&probe_ops,.priv=&a};
    b.fs=(struct filesystem){.name="/b/value",.iops=&probe_ops,.priv=&b};
    eqi(vfs_mount_at("/a",&a.fs),0,"probe mount a");
    eqi(vfs_mount_at("/b",&b.fs),0,"probe mount b");
    pthread_t ta,tb;
    pthread_create(&ta,0,reader,"/a/value");
    pthread_create(&tb,0,reader,"/b/value");
    int parallel=await_flag(&a.entered,1)&&await_flag(&b.entered,1);
    atomic_store(&a.release,1); atomic_store(&b.release,1);
    void *ra,*rb;pthread_join(ta,&ra);pthread_join(tb,&rb);
    ok(parallel,"two mounted backends run concurrently");
    ok(ra&&rb,"both concurrent reads return data");
    atomic_int errors=0;
    pthread_t nt[4];struct names_arg args[4];
    for(int i=0;i<4;i++){args[i]=(struct names_arg){i,&errors};pthread_create(&nt[i],0,names,&args[i]);}
    for(int i=0;i<4;i++)pthread_join(nt[i],0);
    eqi(atomic_load(&errors),0,"8000 names survive backend and cross-task scratch reuse");
    atomic_store(&a.entered,0);atomic_store(&a.release,0);
    pthread_create(&ta,0,reader,"/a/value");
    ok(await_flag(&a.entered,1),"reader has entered before unmount");
    pthread_create(&tb,0,remover,"/a");
    nanosleep(&(struct timespec){0,20000000},0);
    ok(!atomic_load(&a.removed),"unmount must wait for active mount borrower");
    atomic_store(&a.release,1);
    pthread_join(ta,&ra);pthread_join(tb,&rb);
    ok(ra&&rb,"borrower and unmount both finish");
    eqi(atomic_load(&a.bad_lifetime),0,"nested lookup stays on pinned draining mount");
    atomic_store(&b.entered,0);atomic_store(&b.release,0);
    pthread_create(&ta,0,reader,"/b/value");
    ok(await_flag(&b.entered,1),"borrower entered before reversible drain");
    pthread_create(&tb,0,drainer,0);
    nanosleep(&(struct timespec){0,20000000},0);
    eqi(atomic_load(&drain_ready),0,"drain waits for admitted transaction");
    atomic_store(&b.release,1);pthread_join(ta,&ra);
    ok(await_flag(&drain_ready,1),"drain completes after borrower exits");
    ok(vfs_write("/resume","x",1)<0,"paused mount refuses new writes");
    eqi(vfs_umount("/b"),VFS_EBUSY,"paused namespace refuses unmount");
    atomic_store(&drain_release,1);pthread_join(tb,&rb);
    ok(ra&&rb,"borrower recursion and reversible drain finish");
    eqi(vfs_write("/resume","x",1),1,"failed power action restores writes");
    eqs(slurp("/resume"),"x","restored mount returns exact bytes");
    eqi(vfs_umount("/b"),0,"last probe released");
    printf("BKL VFS: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
