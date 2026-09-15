/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "model.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONFIG_LIMIT 2047u

static int copy_field(char *out,size_t capacity,const char *value)
{
    size_t length=strlen(value);
    if(length>=capacity)return -1;
    memcpy(out,value,length+1);return 0;
}
static int decimal(const char *text,unsigned maximum,unsigned *out)
{
    /* Former strtoul(...,0,10) ignored suffixes and overflow before the cast
     * to unsigned: a large number could become an allowed port or TLS flag.
     * Check every digit against the field's bound before arithmetic. */
    if(!*text)return -1;
    unsigned value=0;
    for(;*text;text++) {
        unsigned ch=(unsigned char)*text;
        if(ch<'0'||ch>'9')return -1;
        unsigned digit=ch-'0';
        if(value>maximum/10||(value==maximum/10&&digit>maximum%10))return -1;
        value=value*10+digit;
    }
    *out=value;return 0;
}
static int flag(const char *text,unsigned *out)
{
    if((text[0]!='0'&&text[0]!='1')||text[1])return -1;
    *out=(unsigned)(text[0]-'0');return 0;
}
static int visible(const char *text)
{
    if(!*text)return 0;
    for(;*text;text++)if((unsigned char)*text<33||(unsigned char)*text>126)return 0;
    return 1;
}
static int valid_host(const char *host)
{
    /* SYS_SOCK_OPEN accepts a DNS/IPv4 host separately from the port. Do not
     * accept URL authority syntax, whitespace or HTTP header separators here. */
    size_t label=0;unsigned previous=0;
    if(!*host)return 0;
    for(;*host;host++) {
        unsigned ch=(unsigned char)*host;
        if(ch=='.') {
            if(!label||previous=='-')return 0;
            label=0;
        } else {
            if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
                 (ch>='0'&&ch<='9')||ch=='-'))return 0;
            if((!label&&ch=='-')||++label>63)return 0;
        }
        previous=ch;
    }
    return previous!='-'; /* A terminal dot is a valid absolute DNS name. */
}
static int valid_key_path(const char *path)
{
    if(path[0]!='/')return 0;
    for(;*path;path++)if((unsigned char)*path<32||(unsigned char)*path>126)return 0;
    return 1;
}
int ag_model_config_parse(struct ag_model_config *out,char *key_file,size_t key_capacity,
                          const char *text,size_t length)
{
    if(out)memset(out,0,sizeof *out);
    if(key_file&&key_capacity)key_file[0]=0;
    if(!out||!key_file||!key_capacity||(!text&&length)||length>CONFIG_LIMIT||
       (length&&memchr(text,0,length)))return AG_E_ARGUMENT;
    struct ag_model_config config={0};
    config.tls=1;config.port=443;config.max_tokens=8192;
    strcpy(config.host,"api.deepseek.com");strcpy(config.path,"/chat/completions");
    strcpy(config.model,"deepseek-flash");
    char keypath[AG_PATH]="/etc/agent.key",buffer[CONFIG_LIMIT+1];
    if(length)memcpy(buffer,text,length);buffer[length]=0;
    unsigned seen=0;
    for(char *line=buffer;line;) {
        char *next=strchr(line,'\n');if(next)*next++=0;
        size_t n=strlen(line);if(n&&line[n-1]=='\r')line[n-1]=0;
        if(*line&&*line!='#') {
            char *value=strchr(line,'=');if(!value)return AG_E_ARGUMENT;
            *value++=0;unsigned bit=0;int result=0;
            if(!strcmp(line,"host")){bit=1;result=copy_field(config.host,sizeof config.host,value);}
            else if(!strcmp(line,"path")){bit=2;result=copy_field(config.path,sizeof config.path,value);}
            else if(!strcmp(line,"model")){bit=4;result=copy_field(config.model,sizeof config.model,value);}
            else if(!strcmp(line,"key_file")){bit=8;result=copy_field(keypath,sizeof keypath,value);}
            else if(!strcmp(line,"port")){bit=16;result=decimal(value,65535,&config.port);}
            else if(!strcmp(line,"tls")){bit=32;result=flag(value,&config.tls);}
            else if(!strcmp(line,"thinking")){bit=64;result=flag(value,&config.thinking);}
            else if(!strcmp(line,"max_tokens")){bit=128;result=decimal(value,32768,&config.max_tokens);}
            else return AG_E_ARGUMENT;
            if(result<0||(seen&bit))return AG_E_ARGUMENT;
            seen|=bit;
        }
        line=next;
    }
    if(!valid_host(config.host)||config.path[0]!='/'||!visible(config.path)||
       strchr(config.path,'#')||!visible(config.model)||!valid_key_path(keypath)||
       !config.port||config.port>65535||!config.max_tokens||config.max_tokens>32768)
        return AG_E_ARGUMENT;
    /* Preserve the existing fixture exception. Check before opening a key:
     * plaintext is only allowed to exact loopback/QEMU-host numeric addresses. */
    if(!config.tls&&strcmp(config.host,"10.0.2.2")&&strcmp(config.host,"127.0.0.1"))
        return AG_E_ARGUMENT;
    if(strlen(keypath)>=key_capacity)return AG_E_ARGUMENT;
    memcpy(key_file,keypath,strlen(keypath)+1);*out=config;return 0;
}
static int read_file(const char *path,char *buffer,size_t capacity,size_t *length,int credential)
{
    int fd=open(path,O_RDONLY);if(fd<0)return AG_E_NOTFOUND;
    size_t used=0;int result=0;
    /* Startup also works without a key. Before reading a subsequently installed
     * or separately configured key, require its permissions to be private;
     * protection failure must never publish credential bytes. */
    if(credential&&chmod(path,0600)<0){(void)close(fd);return AG_E_IO;}
    /* A short positive read is not EOF. Keep reading within the same bound;
     * reject an extra byte, and observe read/close failures before publishing. */
    while(used<capacity) {
        long n=read(fd,buffer+used,capacity-used);
        if(n<0||(size_t)n>capacity-used){result=AG_E_IO;break;}
        if(!n)break;used+=(size_t)n;
    }
    if(!result&&used==capacity){char extra;if(read(fd,&extra,1)!=0)result=AG_E_IO;}
    if(close(fd)<0)result=AG_E_IO;
    if(!result)*length=used;
    return result;
}
int ag_model_config_load(struct ag_model_config *out,const char *path)
{
    if(!out)return AG_E_ARGUMENT;
    memset(out,0,sizeof *out);if(!path||!*path)return AG_E_ARGUMENT;
    char buffer[CONFIG_LIMIT+1],keypath[AG_PATH];size_t length=0;
    int result=read_file(path,buffer,CONFIG_LIMIT,&length,0);if(result)return result;
    struct ag_model_config config;
    result=ag_model_config_parse(&config,keypath,sizeof keypath,buffer,length);
    if(result)return result;
    result=read_file(keypath,config.key,sizeof config.key-1,&length,1);if(result)return result;
    if(!length)return AG_E_IO;
    while(length&&(config.key[length-1]=='\r'||config.key[length-1]=='\n'))length--;
    config.key[length]=0;
    if(!length)return AG_E_ARGUMENT;
    for(size_t i=0;i<length;i++)
        if((unsigned char)config.key[i]<33||(unsigned char)config.key[i]>126)return AG_E_ARGUMENT;
    *out=config;return 0;
}
