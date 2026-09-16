/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#include "logit_abi.h"

static int scoped_child(int argc, char **argv)
{
    if (argc < 5) {
        return 126;
    }
    struct logit_capreq request = {0};
    char *end;
    request.caps = strtoul(argv[2], &end, 10);
    if (!argv[2][0] || *end || strlen(argv[3]) >= sizeof request.prefix) {
        return 126;
    }
    strcpy(request.prefix, argv[3]);
    long child;
    /* A kernel-created restricted child is the proof boundary. Passing a
     * fake capability mask as an environment variable would test only the
     * language runtime's willingness to believe its launcher. */
    __asm__ volatile("int $0x80"
                     : "=a"(child)
                     : "a"((long)SYS_CAP_SPAWN), "D"(argv[4]), "S"(argv + 4), "d"(&request)
                     : "memory");
    if (child < 0) {
        fprintf(stderr, "native capability spawn failed: %ld\n", child);
        return 126;
    }
    int status;
    pid_t result;
    do {
        result = waitpid((pid_t)child, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != child) {
        return 126;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* The guest shell currently redirects stdout only. Redirect in a real guest
 * process before exec, so the harness observes native stderr as bytes in the
 * same closed output file and need not reconstruct it from serial chatter. */
int main(int argc, char **argv)
{
    if (argc < 2 || dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        return 126;
    }
    if (!strcmp(argv[1], "--caps")) {
        return scoped_child(argc, argv);
    }
    execv(argv[1], argv + 1);
    perror("native test exec");
    return 127;
}
