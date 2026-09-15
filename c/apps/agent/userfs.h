/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_AGENT_USERFS_H
#define LOGIT_AGENT_USERFS_H
#include <stddef.h>
#include <sys/stat.h>
/* Broker-only helpers. A short-lived child drops to the granting user before
 * doing VFS work; the daemon's root credential never reads an authorized path. */
enum ag_userfs_op { AG_U_STAT, AG_U_READ, AG_U_LIST, AG_U_CREATE, AG_U_DIRECTORY,
    AG_U_REF,AG_U_RESOLVE };
int ag_userfs(unsigned uid,unsigned gid,unsigned op,const char *path,
              const void *input,size_t size,void **output,size_t *length,struct stat *st);
#endif
