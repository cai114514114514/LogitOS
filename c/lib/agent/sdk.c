/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdk.h"
#include "../../apps/logit.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
static int io(int fd,void *buf,size_t n,int writing)
{
    char *p=buf;unsigned long long end=monotonic_ms()+180000;
    while(n){
        unsigned long long now=monotonic_ms();if(now>=end)return AG_E_IO;
        struct pollfd wait={fd,writing?POLLOUT:POLLIN,0};
        if(poll(&wait,1,(int)(end-now))<=0)return AG_E_IO;
        long r=writing?write(fd,p,n):read(fd,p,n);
        if(r<=0)return AG_E_IO;p+=r;n-=(size_t)r;
    }return 0;
}
int ag_send(int fd,const struct ag_message *m,const void *payload)
{
    if(m->bytes>AG_PAYLOAD_MAX || (m->bytes&&!payload))return AG_E_LIMIT;
    struct ag_message h=*m;h.magic=AG_WIRE_MAGIC;h.version=AEX_AGENT_ABI;
    if(io(fd,&h,sizeof h,1)<0)return AG_E_IO;
    return h.bytes?io(fd,(void *)payload,h.bytes,1):0;
}
int ag_receive(int fd,struct ag_message *m,void **payload)
{
    *payload=0;if(io(fd,m,sizeof *m,0)<0)return AG_E_IO;
    if(m->magic!=AG_WIRE_MAGIC||m->version!=AEX_AGENT_ABI||m->bytes>AG_PAYLOAD_MAX)return AG_E_ARGUMENT;
    if(m->bytes){*payload=malloc((size_t)m->bytes+1);if(!*payload)return AG_E_IO;
        if(io(fd,*payload,m->bytes,0)<0){free(*payload);*payload=0;return AG_E_IO;}
        ((char *)*payload)[m->bytes]=0;}
    return 0;
}
int ag_exchange(int fd,struct ag_message *m,const void *data,void **out)
{
    uint64_t op=m->operation;int r=ag_send(fd,m,data);if(r<0)return r;
    r=ag_receive(fd,m,out);if(r<0)return r;
    if(m->type!=AG_REPLY||m->operation!=op){free(*out);*out=0;return AG_E_ARGUMENT;}
    return m->status;
}
int ag_self(struct aex_agent_identity *i)
{return (int)_sys(SYS_AGENT_SELF,(long)i,sizeof *i,0);}
int ag_peer(int fd,struct aex_agent_identity *i)
{return (int)_sys(SYS_AGENT_PEER,fd,(long)i,sizeof *i);}
int ag_connect(void)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return AG_E_IO;
    struct sockaddr_un addr;memset(&addr,0,sizeof addr);addr.sun_family=AF_UNIX;
    memcpy(addr.sun_path,AG_SOCKET,sizeof AG_SOCKET);
    if(connect(fd,(struct sockaddr *)&addr,sizeof addr)<0){close(fd);return AG_E_IO;}
    /* Authenticate the service before publishing user context. This checks
     * kernel-captured identity and the installed image digest, not its greeting. */
    struct aex_agent_identity peer,installed;
    if(ag_peer(fd,&peer)<0 || ag_registry("/bin/agentd",&installed,0)<0 ||
       strcmp(peer.app_id,installed.app_id)||memcmp(peer.image_hash,installed.image_hash,32)){
        close(fd);return AG_E_SCOPE;}
    return fd;
}
int ag_call(struct ag_message *m,const void *data,void **out)
{*out=0;int fd=ag_connect();if(fd<0)return fd;int r=ag_exchange(fd,m,data,out);close(fd);return r;}
int ag_memory_read(const char *id,char **text,uint32_t *bytes,uint64_t *revision)
{
    struct ag_memory_request req={0};if(id&&strlen(id)>=sizeof req.app_id)return AG_E_LIMIT;
    if(id)strcpy(req.app_id,id);struct ag_message m={.type=AG_MEMORY_GET,.bytes=sizeof req};void *out=0;
    int rc=ag_call(&m,&req,&out);if(rc<0){free(out);return rc;}
    *text=out;*bytes=m.bytes;*revision=m.revision;return 0;
}
int ag_memory_write(const char *id,const char *text,uint32_t bytes,uint64_t *revision,uint64_t operation)
{
    if((id&&strlen(id)>=AEX_AGENT_ID_MAX)||bytes>AG_MEMORY_MAX||!operation)return AG_E_LIMIT;
    struct ag_memory_request *req=calloc(1,sizeof *req+bytes);if(!req)return AG_E_IO;
    if(id)strcpy(req->app_id,id);if(bytes)memcpy(req+1,text,bytes);
    struct ag_message m={.type=AG_MEMORY_SET,.bytes=sizeof *req+bytes,.revision=*revision,.operation=operation};void *out=0;
    int rc=ag_call(&m,req,&out);free(req);free(out);if(!rc)*revision=m.revision;return rc;
}
int ag_publish_selection(const char *const *paths,unsigned n,const char *out,uint64_t *context)
{
    if(!n||n>AG_OBJECTS||strlen(out)>=AG_PATH)return AG_E_LIMIT;
    struct ag_context c;memset(&c,0,sizeof c);c.kind=AEX_AGENT_CONTEXT_SELECTION;c.count=n;
    strcpy(c.output,out);for(unsigned i=0;i<n;i++){if(strlen(paths[i])>=AG_PATH)return AG_E_LIMIT;strcpy(c.paths[i],paths[i]);}
    struct ag_message m={.type=AG_CONTEXT,.bytes=sizeof c};void *reply=0;int r=ag_call(&m,&c,&reply);free(reply);
    if(!r)*context=m.object;return r;
}
int ag_publish_document(const char *path,const char *s,unsigned n,uint64_t rev,uint64_t *context)
{
    if(n>AEX_AGENT_DOCUMENT_MAX||strlen(path)>=AG_PATH)return AG_E_LIMIT;
    struct ag_context *c=calloc(1,sizeof *c+n);if(!c)return AG_E_IO;
    c->kind=AEX_AGENT_CONTEXT_DOCUMENT;c->count=1;c->document_bytes=n;c->revision=rev;
    strcpy(c->paths[0],path);memcpy(c+1,s,n);
    struct ag_message m={.type=AG_CONTEXT,.bytes=sizeof *c+n};void *reply=0;int r=ag_call(&m,c,&reply);free(c);free(reply);
    if(!r)*context=m.object;return r;
}
void ag_show_assistant(uint64_t context)
{(void)context;sys_open_path("/assistant.aex");}
int ag_spawn_worker(const char *path,const struct aex_agent_identity *id,int fd)
{
    struct aex_agent_spawn req;memset(&req,0,sizeof req);req.size=sizeof req;req.abi=AEX_AGENT_ABI;req.channel_fd=fd;
    memcpy(req.app_id,id->app_id,sizeof req.app_id);memcpy(req.image_hash,id->image_hash,32);
    return (int)_sys(SYS_AGENT_SPAWN,(long)path,(long)&req,0);
}

int ag_publish_state(const char *label,const char *s,unsigned n,uint64_t revision,uint64_t *context)
{
    if(n>AEX_AGENT_DOCUMENT_MAX||!ag_utf8(s,n)||!revision)return AG_E_LIMIT;
    struct ag_context *c=calloc(1,sizeof *c+n);if(!c)return AG_E_IO;
    c->kind=AEX_AGENT_CONTEXT_STATE;c->count=1;c->document_bytes=n;c->revision=revision;
    int z=snprintf(c->paths[0],AG_PATH,"/context/%s",label);
    if(z<0||z>=AG_PATH){free(c);return AG_E_LIMIT;}strcpy(c->output,"/docs");
    if(n)memcpy(c+1,s,n);
    struct ag_message m={.type=AG_CONTEXT,.bytes=sizeof *c+n};void *out=0;int r=ag_call(&m,c,&out);
    free(out);free(c);if(!r)*context=m.object;return r;
}
