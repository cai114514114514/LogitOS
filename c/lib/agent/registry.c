/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdk.h"
#include "../../crypto/crypto.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
static uint32_t le32(const unsigned char *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int ag_registry(const char *path,struct aex_agent_identity *id,struct aex_agent_manifest *out)
{
    int fd=open(path,O_RDONLY);if(fd<0)return AG_E_IO;
    unsigned char header[64];long n=read(fd,header,sizeof header);
    if(n!=64||memcmp(header,"AEX1",4)||header[4]!=3||header[5]){close(fd);return AG_E_VERSION;}
    unsigned size=(unsigned)header[52]|((unsigned)header[53]<<8);
    if(size<64||size>16384||(size&7)){close(fd);return AG_E_ARGUMENT;}
    unsigned char *meta=malloc(size);if(!meta){close(fd);return AG_E_IO;}
    memcpy(meta,header,64);size_t used=64;
    while(used<size){n=read(fd,meta+used,size-used);if(n<=0)break;used+=(size_t)n;}
    struct sha256 hash;sha256_init(&hash);sha256_update(&hash,meta,used);
    memset(id,0,sizeof *id);int got_id=0,got_agent=0;struct aex_agent_manifest a={0};
    int r=used==size?0:AG_E_IO;
    for(unsigned p=64;!r&&p+8<=size;){unsigned tag=le32(meta+p),len=le32(meta+p+4);
        if(len>size-p-8){r=AG_E_ARGUMENT;break;}const unsigned char *v=meta+p+8;
        if(tag==0x44495841u){if(got_id++||len<2||len>sizeof id->app_id||v[len-1]){r=AG_E_ARGUMENT;break;}memcpy(id->app_id,v,len);}
        if(tag==AEX_T_AGENT){if(got_agent++||len!=sizeof a){r=AG_E_ARGUMENT;break;}memcpy(&a,v,len);}
        p+=(8+len+7)&~7u;}
    free(meta);unsigned char buf[4096];
    while(!r&&(n=read(fd,buf,sizeof buf))>0)sha256_update(&hash,buf,(size_t)n);
    int closed=close(fd);if(n<0||closed<0)r=AG_E_IO;
    if(r<0)return r;
    if(!got_id||!got_agent||a.abi!=AEX_AGENT_ABI||a.reserved)return AG_E_VERSION;
    sha256_final(&hash,id->image_hash);id->abi=AEX_AGENT_ABI;id->size=sizeof *id;
    id->state_version=a.state_version;id->capability_version=a.capability_version;
    if(out)*out=a;return 0;
}
