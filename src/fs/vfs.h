#ifndef AETHER_VFS_H
#define AETHER_VFS_H

/* A deliberately tiny virtual filesystem layer: one registered backend that
 * implements these operations. Enough to abstract the on-disk filesystem from
 * its callers (the ELF loader, demos, future shell). */
struct filesystem {
    const char *name;
    int  (*mount)(void);
    void (*list)(void);
    int  (*size)(const char *path);                 /* bytes, or -1 */
    int  (*read)(const char *path, void *buf, int max);  /* bytes read, or -1 */
    int  (*count)(const char *dir);                 /* entries in directory `dir` */
    const char *(*ent_name)(const char *dir, int i);/* name of entry i in `dir` */
    int  (*ent_size)(const char *dir, int i);        /* size of entry i, bytes */
    int  (*ent_is_dir)(const char *dir, int i);      /* 1 if entry i is a directory */
    int  (*write)(const char *path, const void *buf, int size);  /* create/overwrite */
    int  (*del)(const char *path);                  /* delete */
    int  (*mkdir)(const char *path);                /* create a directory */
    int  (*rename)(const char *old, const char *new_path); /* re-link a dir entry (move/rename) */
};

void vfs_register(struct filesystem *fs);
int  vfs_mount(void);
void vfs_list(void);
int  vfs_size(const char *path);
int  vfs_read(const char *path, void *buf, int max);

/* Directory enumeration (scoped to a directory path, e.g. "/" or "/docs"). */
int         vfs_count(const char *dir);
const char *vfs_ent_name(const char *dir, int i);
int         vfs_ent_size(const char *dir, int i);
int         vfs_ent_is_dir(const char *dir, int i);

/* Mutating ops. */
int         vfs_write(const char *path, const void *buf, int size);
int         vfs_delete(const char *path);
int         vfs_mkdir(const char *path);
int         vfs_rename(const char *old_path, const char *new_path);

#endif /* AETHER_VFS_H */
