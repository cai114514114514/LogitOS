#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

/* Linux's 64-bit dev_t layout.  Matching it matters for ports that serialize
 * st_dev/st_rdev or compare values constructed with makedev(). */
static inline unsigned int major(dev_t d)
{ return (unsigned int)(((d >> 8) & 0xfffu) | ((d >> 32) & ~0xfffu)); }
static inline unsigned int minor(dev_t d)
{ return (unsigned int)((d & 0xffu) | ((d >> 12) & ~0xffu)); }
static inline dev_t makedev(unsigned int ma, unsigned int mi)
{
    return (dev_t)((mi & 0xffu) | ((ma & 0xfffu) << 8) |
                   ((dev_t)(mi & ~0xffu) << 12) |
                   ((dev_t)(ma & ~0xfffu) << 32));
}

#endif /* _SYS_SYSMACROS_H */
