/* SPDX-License-Identifier: MIT
 * The terminal test selector removes root/storage and never returns. */
#include "clib.h"
#include "wide_memory_verify.h"
int main(void) {
    outs("DMA_SHUTDOWN_REQUEST\n");
    (void)_sys(SYS_MEMINFO,0,MMCTL_DMA_SHUTDOWN_VERIFY,0);
    outs("DMA_SHUTDOWN_FAIL selector returned\n");
    return 1;
}
