/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Derived reverse map: rebuilding it loses no identity. A returned path is
 * discovery, never authority to skip a subsequent VFS permission check. */
static int logitfs_refpath_locked(const struct logit_file_id *id,char *out,int cap)
{
    if (!inodes || sb.version!=LFS_ID_VERSION) return VFS_ENOSYS;
    if (id->volume[0]!=identity_super.volume[0] ||
        id->volume[1]!=identity_super.volume[1] || !id->object) return VFS_ENOENT;
    uint32_t target=NOINO;
    for (uint32_t i=0;i<sb.inode_count;i++)
        if (inodes[i].type && inodes[i].object_id==id->object) { target=i; break; }
    if (target==NOINO) return VFS_ENOENT;
    if (!id_parents || id_parents_epoch!=identity_epoch) {
        if (!id_parents) id_parents=kmalloc((size_t)sb.inode_count*sizeof *id_parents);
        uint8_t *data=kmalloc(BS);
        if (!id_parents || !data) { kfree(data); return VFS_ENOSPC; }
        for (uint32_t i=0;i<sb.inode_count;i++) id_parents[i].parent=NOINO;
        for (uint32_t p=0;p<sb.inode_count;p++) {
            struct dinode *d=&inodes[p]; if(d->type!=T_DIR)continue;
            for (uint32_t off=0;off<d->size;off+=BS) {
                int n=inode_pread(d,data,BS,off);
                if(n<0 || n%LFS_DIRENT_SZ){kfree(data);return VFS_EIO;}
                for (int at=0;at<n;at+=LFS_DIRENT_SZ) {
                    struct lfs_dirent e;memcpy(&e,data+at,sizeof e);
                    if(!e.name[0])continue;
                    if(e.ino>=sb.inode_count){kfree(data);return VFS_EIO;}
                    id_parents[e.ino].parent=p;
                    memcpy(id_parents[e.ino].name,e.name,LFS_NAME_MAX);
                }
            }
        }
        kfree(data);id_parents_epoch=identity_epoch;
    }
    char suffix[LOGIT_FSREF_PATH];int pos=sizeof suffix-1;suffix[pos]=0;
    unsigned depth=0;
    while(target!=sb.root_ino) {
        if(++depth>sb.inode_count || id_parents[target].parent==NOINO)return VFS_ENOENT;
        struct id_parent *p=&id_parents[target];int n=0;
        while(n<LFS_NAME_MAX&&p->name[n])n++;
        if(!n||n==LFS_NAME_MAX)return VFS_EIO;
        if(n+1>pos)return VFS_ENAMETOOLONG;
        pos-=n;memcpy(suffix+pos,p->name,n);suffix[--pos]='/';target=p->parent;
    }
    if(!suffix[pos])suffix[--pos]='/';
    int n=(int)sizeof suffix-pos;if(n>cap)return VFS_ENAMETOOLONG;
    memcpy(out,suffix+pos,n);return 0;
}
LFS_OP(int, logitfs_refpath, (const struct logit_file_id *id,char *p,int n), (id,p,n))
