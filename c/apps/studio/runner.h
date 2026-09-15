/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_RUNNER_H
#define STUDIO_RUNNER_H
#include "document.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define ST_OUTPUT_LIMIT (128*1024)
typedef struct {
    pid_t pid;
    int input,output,mode,finished,status,cancelled,truncated,io_error,sent;
    int source_length,length,tab;
    uint64_t revision;
    char *source,*text;
} StRunner;
void st_runner_init(StRunner *r);
int st_runner_busy(const StRunner *r);
void st_runner_stop(StRunner *r);
void st_runner_dispose(StRunner *r);
int st_runner_start(StRunner *r,const StDocument *d,int tab,int check,const char *compiler,const char *root);
int st_runner_poll(StRunner *r);
int st_runner_current(const StRunner *r,const StDocument *d,int tab);
#endif
