/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "model.h"
#include "../../apps/logit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Configuration parsing and bounded file reads live in model_config.c so host
 * validation and non-model SDK links do not pull in socket/HTTP transport. */
void ag_model_close(struct ag_model *m)
{
    if(m->active&&m->socket>=0)_sys(SYS_SOCK_CLOSE,m->socket,0,0);
    h1_response_free(&m->response);free(m->request);memset(m,0,sizeof *m);m->socket=-1;
}
int ag_model_start(struct ag_model *m,const struct ag_model_config *c,const char *system,const char *input)
{
    memset(m,0,sizeof *m);m->socket=-1;h1_response_init(&m->response);h1_response_limit(&m->response,4*1024*1024);
    char *body=ag_model_body(c,system,input);if(!body)return AG_E_IO;
    size_t bn=strlen(body),cap=bn+strlen(c->key)+strlen(c->host)+strlen(c->path)+512;
    m->request=malloc(cap);if(!m->request){free(body);return AG_E_IO;}
    int n=snprintf(m->request,cap,"POST %s HTTP/1.1\r\nHost: %s:%u\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\nContent-Length: %lu\r\n\r\n%s",
        c->path,c->host,c->port,c->key,(unsigned long)bn,body);free(body);
    if(n<0||(size_t)n>=cap){ag_model_close(m);return AG_E_LIMIT;}m->length=(size_t)n;
    m->socket=(int)_sys(SYS_SOCK_OPEN,(long)c->host,((long)c->port<<16)|(c->tls?SOCK_F_TLS:0)|SOCK_F_ALPN_HTTP11,0);
    if(m->socket<0){ag_model_close(m);return AG_E_IO;}
    m->active=1;m->started=monotonic_ms();return 0;
}
int ag_model_pump(struct ag_model *m,char **result)
{
    *result=0;if(!m->active)return AG_E_STATE;
    if(monotonic_ms()-m->started>120000)return AG_E_MODEL;
    long flags=_sys(SYS_SOCK_POLL,m->socket,0,0);
    if(flags<0||(flags&SOCK_P_ERROR))return AG_E_MODEL;
    if(flags&SOCK_P_CONNECTED){
        if(m->sent<m->length){size_t n=m->length-m->sent;if(n>16384)n=16384;
            long wrote=_sys(SYS_SOCK_SEND,m->socket,(long)(m->request+m->sent),n);
            if(wrote<0)return AG_E_MODEL;m->sent+=(size_t)wrote;}
        char buf[16384];long n=_sys(SYS_SOCK_RECV,m->socket,(long)buf,sizeof buf);
        if(n>0){if(h1_response_feed(&m->response,buf,(int)n)<0)return AG_E_MODEL;}
        else if(n<0||(flags&SOCK_P_EOF)){if(h1_response_eof(&m->response)<0)return AG_E_MODEL;}
    }
    if(!h1_response_done(&m->response))return 0;
    m->status=m->response.code;
    if(m->response.code!=200)return AG_E_MODEL;
    int r=ag_model_decode((const char *)m->response.body,m->response.body_len,result);
    return r<0?r:1;
}
