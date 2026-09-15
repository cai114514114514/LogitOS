/* SPDX-License-Identifier: MIT
 * Actual configuration parser/loader. Only POSIX I/O calls in the loader TU
 * are renamed to these wrappers, so short reads and failures are observable. */
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
static unsigned checks,failures;
#define CHECK(ok,label) do{checks++;if(!(ok)){failures++;printf("FAIL: %s\n",label);}}while(0)
static int opens,reads,closes,live,fail_open,fail_read,fail_close;
static size_t read_chunk;
static int protections,fail_protection;
int ag_config_test_chmod(const char *path,mode_t mode)
{
    if(++protections==fail_protection){errno=EPERM;return -1;}
    return chmod(path,mode);
}
int ag_config_test_open(const char *path,int flags,...)
{
    if(++opens==fail_open){errno=ENOENT;return -1;}
    int fd=open(path,flags);if(fd>=0)live++;return fd;
}
ssize_t ag_config_test_read(int fd,void *buffer,size_t size)
{
    if(++reads==fail_read){errno=EIO;return -1;}
    if(read_chunk&&size>read_chunk)size=read_chunk;
    return read(fd,buffer,size);
}
int ag_config_test_close(int fd)
{
    int result=close(fd);if(!result)live--;
    if(++closes==fail_close){errno=EIO;return -1;}
    return result;
}
static void io_reset(void)
{opens=reads=closes=live=fail_open=fail_read=fail_close=0;read_chunk=0;protections=fail_protection=0;}
static int empty(const struct ag_model_config *c)
{
    const unsigned char *bytes=(const unsigned char *)c;
    for(size_t n=0;n<sizeof *c;n++)if(bytes[n])return 0;
    return 1;
}
static int parse(const char *text,struct ag_model_config *out)
{char path[AG_PATH];return ag_model_config_parse(out,path,sizeof path,text,strlen(text));}
static void reject(const char *text,const char *label)
{
    struct ag_model_config c;memset(&c,0x5a,sizeof c);
    CHECK(parse(text,&c)==AG_E_ARGUMENT,label);
    CHECK(empty(&c),"failure publishes no partial configuration");
}
static void write_bytes(const char *path,const void *data,size_t size)
{
    FILE *f=fopen(path,"wb");if(!f||fwrite(data,1,size,f)!=size||fclose(f))exit(2);
}
static char *read_bytes(const char *path,size_t *length)
{
    FILE *f=fopen(path,"rb");if(!f)exit(2);
    char *data=malloc(4096);*length=fread(data,1,4095,f);
    if(ferror(f)||fclose(f))exit(2);data[*length]=0;return data;
}
static void load_check(const char *path,int expected,const char *label)
{
    struct ag_model_config c;memset(&c,0x5a,sizeof c);
    int result=ag_model_config_load(&c,path);CHECK(result==expected,label);
    if(result)CHECK(empty(&c),"loader failure publishes no credential or partial config");
    CHECK(live==0,"loader closes every opened descriptor");
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    struct ag_model_config c;char keypath[AG_PATH];
    size_t size;char *shipped=read_bytes(argv[2],&size);
    CHECK(!ag_model_config_parse(&c,keypath,sizeof keypath,shipped,size),"shipped DeepSeek configuration accepted");
    CHECK(!strcmp(c.host,"api.deepseek.com")&&!strcmp(c.model,"deepseek-flash")&&
          !strcmp(c.path,"/chat/completions")&&c.port==443&&c.tls==1&&
          c.thinking==0&&c.max_tokens==8192&&!strcmp(keypath,"/etc/agent.key"),
          "shipped DeepSeek values preserved");free(shipped);
    CHECK(!parse("",&c)&&c.port==443&&c.max_tokens==8192,"empty text retains explicit defaults");
    CHECK(!parse("# fixture\r\nhost=127.0.0.1\r\nport=8080\r\ntls=0\r\nthinking=1\r\n",&c)&&
          c.port==8080&&!c.tls&&c.thinking,"CRLF loopback fixture accepted");
    CHECK(!parse("host=10.0.2.2\ntls=0\nport=1\nmax_tokens=32768\n",&c)&&c.port==1&&c.max_tokens==32768,
          "QEMU-host plaintext fixture and numeric lower/upper bounds accepted");
    CHECK(!parse("port=65535\nmax_tokens=1\n",&c)&&c.port==65535&&c.max_tokens==1,"other numeric endpoints accepted");
    CHECK(!parse("port=00443\n",&c)&&c.port==443,"port remains decimal with leading zeroes");
    const char *bad_ports[]={"","0","65536","-1","+443"," 443","443 ","443tail","443.0","0x1bb","4294967739","18446744073709552059"};
    for(size_t n=0;n<sizeof bad_ports/sizeof *bad_ports;n++){
        char text[160];snprintf(text,sizeof text,"port=%s\n",bad_ports[n]);
        reject(text,"numeric: entire bounded decimal port required");
    }
    const char *bad_tokens[]={"0","32769","8192tail","4294975488","18446744073709551616"};
    for(size_t n=0;n<sizeof bad_tokens/sizeof *bad_tokens;n++){
        char text[160];snprintf(text,sizeof text,"max_tokens=%s\n",bad_tokens[n]);
        reject(text,"numeric: token bound and overflow enforced");
    }
    for(unsigned field=0;field<2;field++) {
        const char *values[]={"","2","01","-1","true","1tail","4294967297"};
        for(size_t n=0;n<sizeof values/sizeof *values;n++){
            char text[100];snprintf(text,sizeof text,"%s=%s\n",field?"thinking":"tls",values[n]);
            reject(text,"flags: exact zero or one required");
        }
    }
    reject("model=\n","model: empty model refused");reject("model= \n","model: whitespace model refused");
    reject("model=a\tb\n","model: control character refused");
    reject("host=bad host\n","header: host separator refused");
    reject("host=bad\thost\n","header: host tab refused");
    reject("host=api.deepseek.com:443\n","header: host cannot contain port");
    reject("host=https://api.deepseek.com\n","header: host cannot be a URL");
    reject("host=api.deepseek.com/path\n","header: host cannot contain path");
    reject("host=bad\rhost\n","header: embedded CR refused");
    reject("host=.example\n","header: empty DNS label refused");
    reject("host=-example\n","header: invalid DNS label refused");
    reject("path=\n","header: empty request target refused");
    reject("path=chat/completions\n","header: absolute request target required");
    reject("path=/bad path\n","header: path space refused");
    reject("path=/bad\tpath\n","header: path tab refused");
    reject("path=/bad\177path\n","header: path DEL refused");
    reject("path=/chat#fragment\n","header: HTTP target excludes fragment");
    reject("key_file=\n","key: empty key path refused");
    reject("key_file=relative.key\n","key: absolute key path required");
    reject("key_file=/tmp/bad\tkey\n","key: path control character refused");
    reject("tls=0\n","plaintext: remote endpoint refused");
    reject("host=localhost\ntls=0\n","plaintext: hostname alias does not widen numeric exception");
    reject("host=127.0.0.2\ntls=0\n","plaintext: numeric exception remains exact");
    reject("port=443\nport=443\n","duplicate fields refused");
    reject("unknown=yes\n","unknown field refused");reject("bad-line\n","missing equals refused");
    char oversized[2100];memset(oversized,'x',sizeof oversized);oversized[sizeof oversized-1]=0;
    CHECK(ag_model_config_parse(&c,keypath,sizeof keypath,oversized,sizeof oversized-1)==AG_E_ARGUMENT,"configuration size is bounded");
    char long_host[200];memset(long_host,'a',sizeof long_host);memcpy(long_host,"host=",5);long_host[sizeof long_host-1]=0;
    reject(long_host,"field size is bounded");
    const char nul[]="port=443\n\0thinking=2";
    CHECK(ag_model_config_parse(&c,keypath,sizeof keypath,nul,sizeof nul-1)==AG_E_ARGUMENT,"embedded NUL cannot hide trailing data");
    CHECK(ag_model_config_parse(&c,keypath,1,"",0)==AG_E_ARGUMENT,"key path output capacity checked");

    char config_path[512],key_path[512],text[800];
    snprintf(config_path,sizeof config_path,"%s/config",argv[1]);snprintf(key_path,sizeof key_path,"%s/key file",argv[1]);
    snprintf(text,sizeof text,"key_file=%s\n",key_path);
    write_bytes(config_path,text,strlen(text));write_bytes(key_path,"fixture-token\r\n",15);
    io_reset();CHECK(!ag_model_config_load(&c,config_path)&&!strcmp(c.key,"fixture-token"),"load actual files and trim terminal key newline");
    CHECK(opens==2&&closes==2&&!live,"normal load uses and closes exactly two descriptors");
    struct stat key_stat;
    CHECK(protections==1&&!stat(key_path,&key_stat)&&(key_stat.st_mode&0777)==0600,
          "credential: installed key becomes private before use");
    io_reset();fail_protection=1;load_check(config_path,AG_E_IO,"credential: protection failure refuses key");
    CHECK(opens==2&&closes==2&&reads==2,"credential: protection failure reads no key bytes");
    io_reset();read_chunk=1;load_check(config_path,0,"short positive reads are not mistaken for EOF");
    CHECK(reads>20,"short-read test actually uses repeated reads");
    io_reset();fail_open=1;load_check(config_path,AG_E_NOTFOUND,"config open failure reported");
    io_reset();fail_open=2;load_check(config_path,AG_E_NOTFOUND,"key open failure reported");
    io_reset();fail_read=1;load_check(config_path,AG_E_IO,"config read failure reported");
    io_reset();fail_read=3;load_check(config_path,AG_E_IO,"key read failure reported");
    io_reset();fail_close=1;load_check(config_path,AG_E_IO,"config close failure reported");
    CHECK(opens==1,"config close failure prevents key access");
    io_reset();fail_close=2;load_check(config_path,AG_E_IO,"key close failure reported");
    write_bytes(key_path,"fixture token",13);io_reset();load_check(config_path,AG_E_ARGUMENT,"credential: embedded whitespace refused");
    write_bytes(key_path,"fixture\ttoken",13);io_reset();load_check(config_path,AG_E_ARGUMENT,"credential: tab refused");
    const char bad_key[]={'f','i','x',0,'t','u','r','e'};
    write_bytes(key_path,bad_key,sizeof bad_key);io_reset();load_check(config_path,AG_E_ARGUMENT,"credential: embedded NUL refused");
    write_bytes(key_path,"\r\n",2);io_reset();load_check(config_path,AG_E_ARGUMENT,"credential: newline-only key refused");
    write_bytes(key_path,"",0);io_reset();load_check(config_path,AG_E_IO,"empty key file reported");
    char key[256];memset(key,'x',sizeof key);
    write_bytes(key_path,key,255);io_reset();load_check(config_path,0,"maximum key length accepted");
    write_bytes(key_path,key,256);io_reset();load_check(config_path,AG_E_IO,"oversized key file rejected");
    memset(oversized,'#',2048);write_bytes(config_path,oversized,2048);io_reset();load_check(config_path,AG_E_IO,"oversized config file rejected");
    write_bytes(config_path,"tls=0\n",6);io_reset();load_check(config_path,AG_E_ARGUMENT,"invalid plaintext config rejected before credential read");
    CHECK(opens==1,"rejected remote plaintext never opens credential file");
    printf("AGENT_MODEL_CONFIG checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
