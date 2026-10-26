/* The libc's NAME_MAX / PATH_MAX are promises about what LogitFS accepts. They
 * are spelled in two places that cannot include each other (the libc header and
 * the kernel's), so this ties them together at compile time. NAME_MAX said 63
 * while the directory entry holds 60 bytes (59 characters): a 60-character name
 * passed a `strlen(name) <= NAME_MAX` check and was then refused with ENAMETOOLONG. */
#include <limits.h>
#include "vfs_path.h"

_Static_assert(NAME_MAX == VFS_NAME_MAX, "libc NAME_MAX differs from what the filesystem accepts (VFS_NAME_MAX)");
_Static_assert(PATH_MAX == VFS_PATH_MAX, "libc PATH_MAX differs from VFS_PATH_MAX");

int libc_name_max_ok;
