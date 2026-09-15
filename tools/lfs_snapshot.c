/* SPDX-License-Identifier: MIT
 * Validate a private copy with the SAME journal recovery and fsck as mount.
 * Never write the input file. Output is a new, mode-0600 temporary image used
 * only by the packer; diagnostics contain counts, never file data or names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "fsck.h"

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
struct image { unsigned char *data; size_t size; };
static int read_block(void *cx, uint32_t n, void *out)
{ struct image *i=cx; if ((uint64_t)n*LFS_BS+LFS_BS>i->size)return -1;memcpy(out,i->data+(size_t)n*LFS_BS,LFS_BS);return 0; }
static int write_block(void *cx, uint32_t n, const void *in)
{ struct image *i=cx; if ((uint64_t)n*LFS_BS+LFS_BS>i->size)return -1;memcpy(i->data+(size_t)n*LFS_BS,in,LFS_BS);return 0; }
static int sync_copy(void *cx) { (void)cx;return 0; }
int main(int argc,char **argv)
{
    if(argc!=3){fputs("usage: lfs_snapshot <input> <new-output>\n",stderr);return 2;}
    int fd=open(argv[1],O_RDONLY),out=-1,rc=1;struct stat st;struct image im={0};
    if(fd<0||fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<LFS_BS||st.st_size%LFS_BS)goto done;
    im.size=(size_t)st.st_size;
    im.data=mmap(NULL,im.size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    if(im.data==MAP_FAILED){im.data=NULL;goto done;}
    struct lfs_super sb;memcpy(&sb,im.data,sizeof sb);
    if(fsck_super_valid(&sb)<0||(uint64_t)sb.total_blocks*LFS_BS!=im.size)goto done;
    struct fsck_dev d={read_block,write_block,sync_copy,&im,sb.total_blocks};
    int discarded=0,replayed=fsck_log_recover(&d,&sb,&discarded);
    if(replayed<0)goto done;
    d.write=NULL;struct fsck_report report;
    if(fsck_run(&d,0,&report,NULL,NULL)<0){fprintf(stderr,"profile snapshot: filesystem refused (%d problems)\n",report.problems);goto done;}
    out=open(argv[2],O_WRONLY|O_CREAT|O_EXCL,0600);
    if(out<0)goto done;
    size_t at=0;
    while(at<im.size){ssize_t n=write(out,im.data+at,im.size-at);if(n<=0)goto done;at+=(size_t)n;}
    if(fsync(out)<0)goto done;
    printf("profile snapshot: checked; journal replayed=%d discarded=%d\n",replayed,discarded);rc=0;
done:
    if(out>=0){if(close(out)<0)rc=1;if(rc)unlink(argv[2]);}
    if(im.data)munmap(im.data,im.size);
    if(fd>=0)close(fd);
    if(rc)fputs("profile snapshot: refused; original image untouched\n",stderr);
    return rc;
}
