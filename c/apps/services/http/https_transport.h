#ifndef LOGIT_HTTPS_TRANSPORT_H
#define LOGIT_HTTPS_TRANSPORT_H
#include "tls_server.h"
#include "x509.h"
#include "crypto.h"
static struct tls_ident https_identity;
static unsigned char https_der[TLSS_CHAIN_MAX][4096];
static int https_session = -1;
static long long https_now(void)
{ struct logit_timespec t;if(clock_gettime_ns(LOGIT_CLOCK_REALTIME,&t)<0)return 0;return t.tv_sec; }
static int identity_file(const char *p,void *b,int max,int secret)
{
    struct logit_stat s;
    if(st_lstat(p,&s)<0 || (s.mode&LST_IFMT)!=LST_IFREG || s.size>(unsigned)max ||
       (secret && ((s.mode&077) || s.uid!=(unsigned)sys_getuid()))) return -1;
    int fd=sys_open(p,O_RDONLY);if(fd<0)return -1;
    int n=0;while(n<(int)s.size){int r=sys_read(fd,(char *)b+n,s.size-n);if(r<=0){sys_close(fd);return -1;}n+=r;}
    char extra;int r=sys_read(fd,&extra,1);sys_close(fd);return r==0?n:-1;
}
static int identity_write(const char *p,const void *b,int n,int mode)
{
    int mask=st_umask(077),fd=sys_open(p,O_WRONLY|O_CREAT|O_TRUNC);st_umask(mask);
    if(fd<0)return -1;int used=0,ok=0;
    while(used<n){int r=sys_write(fd,(const char *)b+used,n-used);if(r<=0){ok=-1;break;}used+=r;}
    if(sys_close(fd)<0)ok=-1;
    if(st_chmod(p,mode)<0)ok=-1;
    return ok;
}
static int https_init(const char *prefix,const char *name)
{
    char keypath[256],certpath[256];int n=c_strlen(prefix);
    if(n<1 || n>230 || !getrandom_strong())return -1;
    c_strcpy(keypath,prefix,sizeof keypath);c_strcpy(keypath+n,".key",sizeof keypath-n);
    c_strcpy(certpath,prefix,sizeof certpath);c_strcpy(certpath+n,".der",sizeof certpath-n);
    struct logit_stat ks,cs;int havekey=st_lstat(keypath,&ks)==0,havecert=st_lstat(certpath,&cs)==0;
    if(havekey||havecert){
        if(!havekey||!havecert || identity_file(keypath,https_identity.key,32,1)!=32)return -1;
        int size=identity_file(certpath,https_der[0],sizeof https_der[0],0);if(size<=0)return -1;
        struct cert cert;unsigned char pub[65];
        if(x509_parse(https_der[0],size,&cert)<0 || cert.key_type!=KEY_EC || cert.key_curve!=256 || cert.publen!=65 ||
           ecdh_keygen(256,https_identity.key,1,pub)<0)return -1;
        for(int i=0;i<65;i++)if(pub[i]!=cert.pub[i])return -1;
        https_identity.chain[0]=https_der[0];https_identity.chainlen[0]=size;https_identity.nchain=1;https_identity.key_curve=256;
    }else{
        long long now=https_now();if(now<=0)return -1;
        int size=tlss_self_signed(&https_identity,name,now-300,365,https_der[0],sizeof https_der[0]);
        if(size<=0 || identity_write(keypath,https_identity.key,32,0600)<0 || identity_write(certpath,https_der[0],size,0644)<0)return -1;
        errs("httpsd: created self-signed identity; trust its certificate explicitly\n");
    }
    /* Optional DER intermediates let an administrator use a CA-issued leaf.
     * No built-in test identity, trust exception, or automatic replacement. */
    for(int i=1;i<TLSS_CHAIN_MAX;i++){
        c_strcpy(certpath,prefix,sizeof certpath);char suffix[]=".chain1.der";suffix[6]='0'+i;
        c_strcpy(certpath+n,suffix,sizeof certpath-n);
        if(st_lstat(certpath,&cs)<0)break;
        int size=identity_file(certpath,https_der[i],sizeof https_der[i],0);if(size<=0)return -1;
        https_identity.chain[i]=https_der[i];https_identity.chainlen[i]=size;https_identity.nchain++;
    }
    return 0;
}
static int https_accept(int fd)
{
    sys_set_nonblock(fd);https_session=tlss_start(fd,&https_identity,"http/1.1",https_now());
    if(https_session<0)return -1;
    for(;;){int r=tlss_step(https_session);if(r==TLS_DONE)return 0;if(r<0)return -1;sys_sleep_ms(1);}
}
static int client_read(int fd,void *b,int n)
{(void)fd;int r=tlss_recv(https_session,b,n);return r==0?LSK_E_AGAIN:r;}
static int client_write(int fd,const void *b,int n)
{(void)fd;int r=tlss_send(https_session,b,n);return r==0?LSK_E_AGAIN:r;}
static void client_close(int fd)
{
    if(https_session>=0){unsigned long long deadline=monotonic_ns()+15000000000ull;
        while(tlss_flush(https_session)==0 && monotonic_ns()<deadline)sys_sleep_ms(1);
        tlss_close(https_session);https_session=-1;}
    sys_shutdown(fd,LOGIT_SHUT_WR);sys_close(fd);
}
#endif
