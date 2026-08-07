#ifndef LOGIT_KPRINTF_H
#define LOGIT_KPRINTF_H
/* Host stub: the kernel's dual VGA/serial printf becomes printf. The unit tests
 * mostly care that the device model does not crash while formatting; dev_dump()
 * output is checked by the QEMU boot tests, not here. */
#include <stdio.h>
#define kprintf printf
#endif
