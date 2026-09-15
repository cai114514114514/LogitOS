/* SPDX-License-Identifier: MIT */
#include "tabs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static char root[256];static int checks,failures,short_write,corrupt_read;
#define CHECK(x,n) do{checks++;if(!(x)){failures++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
static void path(char *out,const char *p){snprintf(out,512,"%s%s",root,p);}
static int rd(const char *p,void *buf,int cap){char full[512];path(full,p);FILE*f=fopen(full,"rb");if(!f)return -1;fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);if(length>cap){fclose(f);return -1;}int n=(int)fread(buf,1,(size_t)cap,f);fclose(f);if(corrupt_read&&n&&strstr(p,"/body"))((char*)buf)[0]^=1;return n;}
static int wr(const char *p,const void *buf,int len){char full[512];path(full,p);FILE*f=fopen(full,"wb");if(!f)return -1;int n=(int)fwrite(buf,1,(size_t)(short_write&&len?len-1:len),f);fclose(f);return n;}
static int mk(const char *p){char full[512];path(full,p);return mkdir(full,0700);}
static int rn(const char *a,const char *b){char x[512],y[512];path(x,a);path(y,b);if(link(x,y))return -1;return unlink(x);}
static int rm(const char *p){char full[512];path(full,p);return unlink(full)==0?0:rmdir(full);}
static int exists(const char *p){char full[512];path(full,p);return access(full,F_OK)==0;}
int main(void){char temp[]="/tmp/logit-download-XXXXXX";char *r=mkdtemp(temp);if(!r)return 2;strcpy(root,r);struct bstore_ops store={rd,wr,mk,rn,rm,exists};tabs_set_store(&store);
 char name[96];download_name("https://example.test/",name,sizeof name);CHECK(!strcmp(name,"download"),"empty basename");
 dl_filename("https://x/a","attachment; filename=\"fallback.txt\"; filename*=UTF-8''%E4%B8%8B%E8%BD%BD.txt",NULL,name,sizeof name);CHECK(!strcmp(name,"下载.txt"),"UTF-8 disposition wins");
 dl_filename("https://x/a","attachment; filename=\"../folder\\\\bad.txt\"",NULL,name,sizeof name);CHECK(!strchr(name,'/')&&!strchr(name,'\\')&&name[0]!='.',"basename only");
 dl_filename("https://x/%2e%2e",NULL,NULL,name,sizeof name);CHECK(!strcmp(name,"download"),"dot basename fallback");
 CHECK(download_should_save("https://x/api","attachment; filename=a.txt","text/plain",0),"extensionless attachment");
 CHECK(download_should_save("https://x/api",NULL,"application/octet-stream",0),"binary response");
 CHECK(!download_should_save("https://x/file.zip",NULL,"text/html",0),"HTML response beats URL extension");
 CHECK(download_should_save("https://x/report",NULL,"text/plain",1),"download attribute wins");
 int id=download_record_as("https://x/a",NULL,"same.txt",(const unsigned char*)"first",5);const struct download *d=download_at(id);CHECK(d&&d->ok&&!strcmp(d->path,DOWNLOAD_DIR "/same.txt"),"first committed download");
 id=download_record_as("https://x/a",NULL,"same.txt",(const unsigned char*)"second",6);d=download_at(id);CHECK(d&&d->ok&&!strcmp(d->path,DOWNLOAD_DIR "/same (2).txt"),"collision suffix");
 char body[16];CHECK(rd(DOWNLOAD_DIR "/same.txt",body,sizeof body)==5&&!memcmp(body,"first",5),"first file preserved");
 id=download_record_as("https://x/empty",NULL,"empty.bin",NULL,0);d=download_at(id);CHECK(d&&d->ok&&rd(d->path,body,sizeof body)==0,"empty download committed");
 short_write=1;id=download_record_as("https://x/a",NULL,"partial.bin",(const unsigned char*)"wrong",5);short_write=0;d=download_at(id);CHECK(d&&!d->ok&&rd(d->path,body,sizeof body)<0,"short write never published");
 corrupt_read=1;id=download_record_as("https://x/a",NULL,"corrupt.bin",(const unsigned char*)"wrong",5);corrupt_read=0;d=download_at(id);CHECK(d&&!d->ok&&rd(d->path,body,sizeof body)<0,"bad readback never published");
 for(int i=0;i<36;i++){id=download_record_as("https://x/a",NULL,"repeat.txt",(const unsigned char*)"x",1);d=download_at(id);CHECK(d&&d->ok,"downloads continue beyond history capacity");}
 printf("browser-downloads: %d checks, %d failures\n",checks,failures);
 /* Only the private temporary namespace created above belongs to this test. */
 char command[320];snprintf(command,sizeof command,"rm -rf '%s'",root);system(command);
 return failures?1:0;
}
