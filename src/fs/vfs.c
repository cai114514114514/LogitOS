#include <stddef.h>
#include "vfs.h"

static struct filesystem *root;

void vfs_register(struct filesystem *fs) { root = fs; }

int vfs_mount(void)
{
    return (root && root->mount) ? root->mount() : -1;
}

void vfs_list(void)
{
    if (root && root->list)
        root->list();
}

int vfs_size(const char *path)
{
    return (root && root->size) ? root->size(path) : -1;
}

int vfs_read(const char *path, void *buf, int max)
{
    return (root && root->read) ? root->read(path, buf, max) : -1;
}

int vfs_count(const char *dir)
{
    return (root && root->count) ? root->count(dir) : 0;
}

const char *vfs_ent_name(const char *dir, int i)
{
    return (root && root->ent_name) ? root->ent_name(dir, i) : "";
}

int vfs_ent_size(const char *dir, int i)
{
    return (root && root->ent_size) ? root->ent_size(dir, i) : 0;
}

int vfs_ent_is_dir(const char *dir, int i)
{
    return (root && root->ent_is_dir) ? root->ent_is_dir(dir, i) : 0;
}

int vfs_write(const char *path, const void *buf, int size)
{
    return (root && root->write) ? root->write(path, buf, size) : -1;
}

int vfs_delete(const char *path)
{
    return (root && root->del) ? root->del(path) : -1;
}

int vfs_mkdir(const char *path)
{
    return (root && root->mkdir) ? root->mkdir(path) : -1;
}
