/* Probe the same process architecture and sanitizer configuration as the test.
 * A host with 16 KiB pages cannot measure four independent 4 KiB permissions;
 * recording four PTE flags would fabricate a passing hardware-backed gate. */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf fault;
static volatile sig_atomic_t trapped;

static void on_fault(int signal_number)
{
    (void)signal_number;
    trapped = 1;
    siglongjmp(fault, 1);
}

static int writable(volatile unsigned char *address)
{
    trapped = 0;
    if (sigsetjmp(fault, 1) == 0) {
        unsigned char value = *address;
        *address = (unsigned char)(value ^ 1);
        *address = value;
    }
    return !trapped;
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size != 4096) {
        printf("host page size is %ld; independent 4096-byte permissions required\n",
               page_size);
        return 77;
    }

    /* Use a hint first: replacing some unrelated mapping with MAP_FIXED would
     * make an address-space collision look like successful host support. The
     * Darwin link flags shrink __PAGEZERO so this guest window is available. */
    void *wanted = (void *)0x60000000;
    unsigned char *pages = mmap(wanted, 8192, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        printf("cannot reserve the guest mapping window: %s\n", strerror(errno));
        return 77;
    }
    if ((void *)pages != wanted) {
        munmap(pages, 8192);
        puts("the guest mapping window is occupied");
        return 77;
    }
    void *fixed = mmap(pages, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (fixed == MAP_FAILED || mprotect(pages, 4096, PROT_READ) != 0) {
        printf("fixed mapping or page protection unavailable: %s\n", strerror(errno));
        munmap(pages, 8192);
        return 77;
    }

    struct sigaction handler = {0}, old_segv, old_bus;
    handler.sa_handler = on_fault;
    handler.sa_flags = SA_NODEFER;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGSEGV, &handler, &old_segv);
    sigaction(SIGBUS, &handler, &old_bus);
    int independent = !writable(pages) && writable(pages + 4096);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);

    /* NX-disabled loader cases also request executable writable mappings.
     * Probe permission support without executing any generated instructions. */
    int executable = mprotect(pages, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
    munmap(pages, 8192);
    if (!independent || !executable) {
        puts("host cannot enforce the fixture's independent R/RW/RWX mappings");
        return 77;
    }
    puts("host supports fixed mappings and independent 4096-byte permissions");
    return 0;
}
