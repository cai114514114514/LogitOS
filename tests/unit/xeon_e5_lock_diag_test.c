/* Host proof that fatal lock diagnostics preserve CPU indices above seven. */
#include <stdio.h>
#include <string.h>
#include "spinlock.h"

static int host_cpu;
static char serial_text[512];
static unsigned int serial_used;

int logit_lock_host_cpu(void) { return host_cpu; }
void tlb_service(void) { }
void serial_putc(char c)
{
    if (serial_used + 1 < sizeof serial_text) {
        serial_text[serial_used++] = c;
        serial_text[serial_used] = 0;
    }
}

int main(void)
{
    spinlock_t lock = SPINLOCK_INIT;
    host_cpu = 31;
    spin_lock(&lock);
    host_cpu = 30;
    spin_unlock(&lock);
    if (!strstr(serial_text, "cpu 30 released a lock held by 31")) {
        fprintf(stderr, "bad high-CPU diagnostic: %s\n", serial_text);
        return 1;
    }
    puts("xeon_e5_lock_diag: CPU 30/31 preserved");
    return 0;
}
