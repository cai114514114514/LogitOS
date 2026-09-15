/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_FS_REF_H
#define LOGIT_FS_REF_H
#define SYS_FSREF 197
#define LOGIT_FSREF_VERSION 1u
#define LOGIT_FSREF_PATH 128
/* A reference names an object, never a capability. Resolve checks current
 * VFS credentials and may fail after a permission change or unlink. */
struct logit_file_id { unsigned long long volume[2], object; };
struct logit_fsref {
    unsigned int version, reserved;
    struct logit_file_id id;
    char path[LOGIT_FSREF_PATH];
};
#endif
