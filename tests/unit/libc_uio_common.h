/* Included by the portable branch of libc_uio_test.c. No fake preadv or
 * pwritev reference: macOS 11+ declares both with _DARWIN_C_SOURCE enabled. */
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "libc_uio_host.h"

static int checks, failures;

static void check(const char *label, int condition)
{
    checks++;
    if (!condition) failures++;
    printf("%s: %s\n", label, condition ? "ok" : "FAIL");
}

static void note(const char *message)
{
    write(2, message, strlen(message));
}

static void pipe_cases(void)
{
    int descriptors[2];
    int ready = pipe(descriptors) == 0;
    check("pipe fixture", ready);
    if (!ready) return;
    struct iovec send[] = {{"Hello", 5}, {NULL, 0}, {" World!", 7}};
    check("pipe vector write", writev(descriptors[1], send, 3) == 12);
    close(descriptors[1]);
    char first[5] = {0}, second[7] = {0};
    struct iovec receive[] = {{first, 5}, {NULL, 0}, {second, 7}};
    check("pipe vector read", readv(descriptors[0], receive, 3) == 12);
    check("pipe vector bytes", memcmp(first, "Hello", 5) == 0 && memcmp(second, " World!", 7) == 0);
    check("pipe EOF", readv(descriptors[0], receive, 3) == 0);
    close(descriptors[0]);

    ready = host_full_pipe(descriptors) == 0;
    check("full nonblocking pipe fixture", ready);
    if (!ready) return;
    struct iovec one = {"x", 1};
    check("full pipe refuses write before progress", writev(descriptors[1], &one, 1) == -1);
    close(descriptors[0]);
    close(descriptors[1]);
}

static void eof_cases(void)
{
    int fd = host_temp_file("abcde", 5, 0);
    check("EOF file fixture", fd >= 0);
    if (fd < 0) return;
    char first[8], second[4];
    memset(first, '!', sizeof first);
    memset(second, '?', sizeof second);
    struct iovec vector[] = {{first, 8}, {second, 4}};
    check("EOF short vector read", readv(fd, vector, 2) == 5);
    check("EOF first buffer bytes", memcmp(first, "abcde!!!", 8) == 0);
    check("EOF later buffer untouched", memcmp(second, "????", 4) == 0);
    check("EOF advances only actual bytes", lseek(fd, 0, SEEK_CUR) == 5);
    close(fd);
}

static void positioned_cases(void)
{
    int fd = host_temp_file("0123456789", 10, 1);
    check("positioned file fixture", fd >= 0);
    if (fd < 0) return;
    check("set ordinary cursor", lseek(fd, 4, SEEK_SET) == 4);
    char first[2] = {0}, second[3] = {0};
    struct iovec receive[] = {{first, 2}, {second, 3}};
    check("preadv count", preadv(fd, receive, 2, 1) == 5);
    check("preadv bytes", memcmp(first, "12", 2) == 0 && memcmp(second, "345", 3) == 0);
    check("preadv preserves cursor", lseek(fd, 0, SEEK_CUR) == 4);
    struct iovec send[] = {{"AB", 2}, {"C", 1}};
    check("pwritev count", pwritev(fd, send, 2, 6) == 3);
    check("pwritev preserves cursor", lseek(fd, 0, SEEK_CUR) == 4);
    char next = 0;
    check("ordinary read resumes at cursor", read(fd, &next, 1) == 1 && next == '4');
    char contents[10] = {0};
    check("rewind for content check", lseek(fd, 0, SEEK_SET) == 0);
    check("positioned write bytes", read(fd, contents, 10) == 10 && memcmp(contents, "012345ABC9", 10) == 0);
    check("negative preadv offset", preadv(fd, receive, 2, -1) == -1);
    check("negative pwritev offset", pwritev(fd, send, 2, -1) == -1);
    check("bad offsets preserve cursor", lseek(fd, 0, SEEK_CUR) == 10);

    errno = 0;
    check("too many preadv vectors", preadv(fd, receive, IOV_MAX + 1, 0) == -1 && errno == EINVAL);
    check("invalid vector preserves cursor", lseek(fd, 0, SEEK_CUR) == 10);
    close(fd);

    int descriptors[2];
    if (pipe(descriptors) < 0) { check("positioned pipe fixture", 0); return; }
    check("preadv refuses pipe", preadv(descriptors[0], receive, 2, 0) == -1);
    check("pwritev refuses pipe", pwritev(descriptors[1], send, 2, 0) == -1);
    close(descriptors[0]);
    close(descriptors[1]);
}

static void invalid_cases(void)
{
    /* Only validation performed by uio.c compares errno. Failures delegated
     * to host read/write/lseek set host TLS errno, not our plain errno shim;
     * compare their real return values rather than a disconnected variable. */
    int fd = host_temp_file("data", 4, 1);
    check("invalid-vector fixture", fd >= 0);
    if (fd < 0) return;
    char data[4];
    struct iovec vector = {data, sizeof data};
    errno = 0;
    check("negative readv count", readv(fd, &vector, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("negative writev count", writev(fd, &vector, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("too many readv vectors", readv(fd, &vector, IOV_MAX + 1) == -1 && errno == EINVAL);
    check("readv invalid descriptor", readv(-1, &vector, 1) == -1);
    check("writev invalid descriptor", writev(-1, &vector, 1) == -1);
    check("preadv invalid descriptor", preadv(-1, &vector, 1, 0) == -1);
    check("pwritev invalid descriptor", pwritev(-1, &vector, 1, 0) == -1);
    close(fd);
}

#ifdef LIBC_OURS_HOST_TEST
static int contract_checks, contract_failures;

static void contract(const char *label, int condition)
{
    contract_checks++;
    if (!condition) {
        contract_failures++;
        printf("FAIL uio contract: %s\n", label);
    }
}

static void project_contracts(void)
{
    /* Darwin rejects iovcnt==0, while our documented Linux-style API accepts
     * it after checking descriptor access. Partial EFAULT and ioctl policy
     * are also kept as named project assertions rather than normalized away. */
    int fd = host_temp_file("0123456789", 10, 0);
    contract("contract file fixture", fd >= 0);
    if (fd < 0) return;
    contract("empty read vector", readv(fd, NULL, 0) == 0);
    contract("empty write validates access", writev(fd, NULL, 0) == -1);
    contract("empty preadv preserves position", preadv(fd, NULL, 0, 5) == 0 && lseek(fd, 0, SEEK_CUR) == 0);
    char data[5] = {0};
    struct iovec partial[] = {{data, 5}, {NULL, 2}};
    contract("read error after progress returns count", readv(fd, partial, 2) == 5);
    contract("read before error delivered bytes", memcmp(data, "01234", 5) == 0);
    /* These ioctl rejection paths never call host fstat into a LogitOS-sized
     * struct stat. Its successful FIONREAD path needs a guest ABI test. */
    errno = 0;
    contract("FIONREAD null argument", ioctl(fd, FIONREAD, NULL) == -1 && errno == EFAULT);
    int untouched = 42;
    errno = 0;
    contract("unknown ioctl reports ENOTTY", ioctl(fd, 0x9999UL, &untouched) == -1 && errno == ENOTTY && untouched == 42);
    close(fd);
    contract("empty read validates descriptor", readv(-1, NULL, 0) == -1);
    char message[96];
    snprintf(message, sizeof message, "uio project contracts: %d checks, %d failures\n", contract_checks, contract_failures);
    note(message);
}
#endif

int main(void)
{
    pipe_cases();
    eof_cases();
    positioned_cases();
    invalid_cases();
    printf("uio common: %d checks, %d failures\n", checks, failures);
#ifdef LIBC_OURS_HOST_TEST
    project_contracts();
    failures += contract_failures;
#else
    note("SKIP Linux-specific uio reference cases in common mode: zero-vector, partial EFAULT, ioctl policy and F_SETPIPE_SZ setup differ on Darwin; run make test-libc-uio on a Linux glibc host for the original full differential. preadv/pwritev are exercised against the real host APIs.\n");
#endif
    return failures ? 1 : 0;
}
