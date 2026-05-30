#ifndef AQUA_VFS_H
#define AQUA_VFS_H

/* A deliberately tiny virtual filesystem layer: one registered backend that
 * implements these operations. Enough to abstract the on-disk filesystem from
 * its callers (the ELF loader, demos, future shell). */
struct filesystem {
    const char *name;
    int  (*mount)(void);
    void (*list)(void);
    int  (*size)(const char *path);                 /* bytes, or -1 */
    int  (*read)(const char *path, void *buf, int max);  /* bytes read, or -1 */
    int  (*count)(void);                            /* number of files */
    const char *(*ent_name)(int i);                 /* name of file i */
    int  (*ent_size)(int i);                         /* size of file i, bytes */
};

void vfs_register(struct filesystem *fs);
int  vfs_mount(void);
void vfs_list(void);
int  vfs_size(const char *path);
int  vfs_read(const char *path, void *buf, int max);

/* Directory enumeration. */
int         vfs_count(void);
const char *vfs_ent_name(int i);
int         vfs_ent_size(int i);

#endif /* AQUA_VFS_H */
