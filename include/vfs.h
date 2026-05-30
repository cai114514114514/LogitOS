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
};

void vfs_register(struct filesystem *fs);
int  vfs_mount(void);
void vfs_list(void);
int  vfs_size(const char *path);
int  vfs_read(const char *path, void *buf, int max);

#endif /* AQUA_VFS_H */
