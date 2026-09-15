/* SPDX-License-Identifier: MIT */
#include "../../c/lib/agent/task.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static int fail_sync,fail_close,short_write;
static int tested_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return fsync(fd);}
static int tested_close(int fd){int r=close(fd);if(fail_close){errno=EIO;return -1;}return r;}
static ssize_t tested_write(int fd,const void *p,size_t n){return write(fd,p,short_write&&n>3?3:n);}
#define fsync tested_fsync
#define close tested_close
#define write tested_write
#ifndef AG_STORE_SOURCE
#define AG_STORE_SOURCE "../../c/lib/agent/store.c"
#endif
#include AG_STORE_SOURCE
#undef fsync
#undef close
#undef write
#define CHECK(x,label) do{if(!(x)){fprintf(stderr,"FAIL: %s\n",label);return 1;}checks++;}while(0)
int main(int argc,char **argv)
{
    if(argc!=2)return 2;unsigned checks=0;struct ag_task t;struct ag_document d={0};
    ag_task_init(&t,1,"store failure","/docs/report.md");CHECK(!ag_document_set(&d,"",0,1),"initial document");
    CHECK(!ag_task_enter(&t)&&!ag_task_commit(&t,&d,1,1,"confirmed",9),"confirmed operation");
    short_write=1;CHECK(!ag_store_save(argv[1],&t,&d)&&t.generation==1,"short writes completed");short_write=0;
    fail_sync=1;CHECK(ag_store_save(argv[1],&t,&d)==AG_E_IO&&t.generation==1,"fsync failure cannot be acknowledged");fail_sync=0;
    fail_close=1;CHECK(ag_store_save(argv[1],&t,&d)==AG_E_IO&&t.generation==1,"close failure cannot be acknowledged");fail_close=0;
    CHECK(!ag_store_save(argv[1],&t,&d)&&t.generation==2,"retry advances generation once");
    struct ag_task restored;struct ag_document doc={0};CHECK(!ag_store_load(argv[1],&restored,&doc)&&doc.length==9&&!memcmp(doc.bytes,"confirmed",9)&&restored.receipt_count==1,"retry preserves one effect and receipt");
    ag_document_free(&d);ag_document_free(&doc);printf("AGENT_STORE_IO_PASS checks=%u\n",checks);return 0;
}
