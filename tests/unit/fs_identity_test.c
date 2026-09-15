/* SPDX-License-Identifier: MIT */
#include "fs_sim.h"
#include "logitfs.h"
#include "bcache.h"
#include "fsck.h"
#include "crc32.h"
static unsigned checks;
static void check(int ok,const char *name)
{if(!ok){fprintf(stderr,"FS_IDENTITY_FAIL %s\n",name);exit(1);}checks++;}
static struct vattr attr(const char *p)
{struct vattr a;check(logitfs.getattr(p,&a)==0,"stat object");return a;}
static struct logit_file_id key(struct vattr a)
{return (struct logit_file_id){{a.volume[0],a.volume[1]},a.object_id};}
int main(void)
{
    sim_open();struct lfs_super *sb=(void *)sim_media;sb->version=LFS_ID_VERSION;
    struct lfs_identity_super e={LFS_ID_MAGIC,{12345,67890},0};e.checksum=crc32(&e,20);
    memcpy(sim_media+sizeof *sb,&e,sizeof e);
    struct lfs_dinode *root=(void *)(sim_media+(size_t)sb->inode_start*LFS_BS);
    root->object_id=1;root->revision=1;root->next_id=1;
    check(!logitfs.mount(),"mount identity volume");
    check(!logitfs.mkdir("/project"),"mkdir is a Project");
    check(logitfs.write("/project/report.md","first",5)==5,"write document");
    struct vattr original=attr("/project/report.md"),project=attr("/project");
    check(original.flags&VA_ID,"identity is advertised");
    check(!logitfs.rename("/project","/renamed"),"rename Project");
    struct logit_file_id id=key(original);char p[128];
    check(!logitfs.refpath(&id,p,sizeof p)&&!strcmp(p,"/renamed/report.md"),"reference follows parent rename");
    struct vattr moved=attr(p);
    check(moved.object_id==original.object_id,"rename preserves file identity");
    check(logitfs.write(p,"second",6)==6,"write after rename");
    check(attr(p).revision>moved.revision,"content generation advances");
    logitfs_unmount();sim_power_cut(1,0);
    check(!logitfs.mount(),"remount");
    check(!logitfs.refpath(&id,p,sizeof p)&&!strcmp(p,"/renamed/report.md"),"reference survives reboot");
    check(attr("/renamed").object_id==project.object_id,"Project identity survives reboot");
    check(!logitfs.del(p),"unlink document");
    check(logitfs.write(p,"new",3)==3,"reuse directory name and inode");
    check(attr(p).object_id!=original.object_id,"deleted identity is never reused");
    check(logitfs.refpath(&id,p,sizeof p)<0,"old reference cannot name replacement");
    /* Full mount-time fsck already exercises the shipping checker. */
    check(!logitfs_fsck(0),"identity volume remains fsck clean");
    logitfs_unmount();
    struct lfs_dinode *table=(void *)(sim_media+(size_t)sb->inode_start*LFS_BS);
    struct lfs_identity_super *ext=(void *)(sim_media+sizeof *sb);
    check(!fsck_identity_valid(sb,ext,table),"checked durable identities");
    ext->volume[0]^=1;check(fsck_identity_valid(sb,ext,table)<0,"damaged volume identity refused");ext->volume[0]^=1;
    uint64_t count=table[sb->root_ino].next_id;
    table[sb->root_ino].next_id=1;check(fsck_identity_valid(sb,ext,table)<0,"counter rollback refused");table[sb->root_ino].next_id=count;
    uint64_t saved_id=table[project.ino].object_id;table[project.ino].object_id=table[sb->root_ino].object_id;
    check(fsck_identity_valid(sb,ext,table)<0,"duplicate identities refused");table[project.ino].object_id=saved_id;
    table[sb->root_ino].next_id=UINT64_MAX;
    check(!logitfs.mount(),"mount exhausted identity counter");
    check(logitfs.mkdir("/overflow")<0,"counter exhaustion refuses new identity");
    check(!logitfs_fsck(0),"counter exhaustion preserves live data");
    logitfs_unmount();sim_close();printf("FS_IDENTITY_PASS %u checks\n",checks);return 0;
}
