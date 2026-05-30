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
