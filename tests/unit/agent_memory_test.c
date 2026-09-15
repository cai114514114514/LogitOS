/* SPDX-License-Identifier: MIT
 * Actual memory.c + task.c + store.c, using independent real directories. */
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned checks,failures;
#define CHECK(ok,label) do{checks++;if(!(ok)){failures++;printf("FAIL: %s\n",label);}}while(0)
static const char app_a[]="os.logit.alpha",app_b[]="os.logit.beta";
static void child(char *out,const char *root,const char *name)
{
    int n=snprintf(out,AG_PATH,"%s/%s",root,name);
    if(n<0||n>=AG_PATH||mkdir(out,0700))exit(2);
}
static int absent(const char *directory)
{
    char path[AG_PATH];struct stat st;
    for(unsigned i=0;i<2;i++){snprintf(path,sizeof path,"%s/slot%u",directory,i);if(!stat(path,&st))return 0;}
    return 1;
}
static int text_is(const char *directory,const char *id,const char *want,uint64_t revision)
{
    struct ag_document d={0};int r=ag_memory_load(directory,id,&d);
    int ok=!r&&d.revision==revision&&d.length==strlen(want)&&!memcmp(d.bytes,want,d.length);
    ag_document_free(&d);return ok;
}
static uint64_t generation(const char *directory,unsigned *receipts)
{
    struct ag_task task;struct ag_document d={0};int r=ag_store_load(directory,&task,&d);
    ag_document_free(&d);if(r)return 0;if(receipts)*receipts=task.receipt_count;return task.generation;
}
static uint32_t disk_checksum(const char *directory)
{
    uint32_t result=0;
    for(unsigned i=0;i<2;i++) {
        char path[AG_PATH];snprintf(path,sizeof path,"%s/slot%u",directory,i);
        FILE *f=fopen(path,"rb");if(!f)continue;
        if(fseek(f,0,SEEK_END))exit(2);long size=ftell(f);if(size<0||fseek(f,0,SEEK_SET))exit(2);
        char *bytes=malloc((size_t)size);if(!bytes)exit(2);
        if(fread(bytes,1,(size_t)size,f)!=(size_t)size||fclose(f))exit(2);
        result^=ag_checksum(bytes,(size_t)size)+(i+1)*0x12345u;free(bytes);
    }
    return result;
}
static void corrupt(const char *directory,unsigned slot)
{
    char path[AG_PATH];snprintf(path,sizeof path,"%s/slot%u",directory,slot);
    FILE *f=fopen(path,"r+b");if(!f)exit(2);
    if(fseek(f,-1,SEEK_END))exit(2);int c=fgetc(f);if(c==EOF||fseek(f,-1,SEEK_END))exit(2);
    if(fputc(c^1,f)==EOF||fclose(f))exit(2);
}
static void incompatible(const char *directory,unsigned slot)
{
    /* Existing store wire header, altered only in this private fixture. Fix
     * its checksum so a version error cannot pass as ordinary corruption. */
    struct header {uint32_t magic,version,state_bytes,doc_bytes;uint64_t generation;
        uint32_t state_crc,doc_crc,header_crc,reserved;} h;
    char path[AG_PATH];snprintf(path,sizeof path,"%s/slot%u",directory,slot);
    FILE *f=fopen(path,"r+b");if(!f||fread(&h,1,sizeof h,f)!=sizeof h)exit(2);
    h.version=99;h.header_crc=0;h.header_crc=ag_checksum(&h,sizeof h);
    if(fseek(f,0,SEEK_SET)||fwrite(&h,1,sizeof h,f)!=sizeof h||fclose(f))exit(2);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    char a[AG_PATH],b[AG_PATH],bad[AG_PATH],version[AG_PATH],full[AG_PATH],large[AG_PATH],other[AG_PATH],kv[AG_PATH];
    child(a,argv[1],"app-a");child(b,argv[1],"app-b");child(bad,argv[1],"bad");child(version,argv[1],"version");
    child(full,argv[1],"full");child(large,argv[1],"large");child(other,argv[1],"other");child(kv,argv[1],"kv");
    CHECK(text_is(a,app_a,"",1),"initial absent memory is empty revision one");
    CHECK(absent(a),"initial load does not create a checkpoint");
    uint64_t revision=0;
    CHECK(ag_memory_commit(a,app_a,11,1,"领域甲",9,&revision)==0&&revision==2,"first explicit text commit succeeds");
    CHECK(text_is(a,app_a,"领域甲",2),"restart: explicit memory survives fresh load");
    /* These prerequisites need real persisted fixtures for the later corrupt
     * slot tests. Report a semantic failure before touching a missing file. */
    if(failures){printf("AGENT_MEMORY checks=%u failures=%u\n",checks,failures);return 1;}
    uint64_t gen=generation(a,0);
    CHECK(ag_memory_commit(a,app_a,11,1,"领域甲",9,&revision)==1&&revision==2,
          "dedup: same operation and payload has one effect after restart");
    CHECK(generation(a,0)==gen,"duplicate operation performs no checkpoint write");
    CHECK(ag_memory_commit(a,app_a,11,2,"领域乙",9,&revision)==AG_E_CONFLICT,
          "dedup: same operation with different payload is refused");
    CHECK(ag_memory_commit(a,app_a,12,1,"stale",5,&revision)==AG_E_CONFLICT,
          "CAS: stale revision cannot overwrite durable memory");
    CHECK(text_is(a,app_a,"领域甲",2),"conflicts preserve original memory");
    CHECK(ag_memory_commit(a,app_a,12,2,"new",3,&revision)==0&&revision==3,"fresh CAS commit succeeds");
    CHECK(ag_memory_commit(a,app_a,11,1,"领域甲",9,&revision)==1&&revision==3,
          "old matching receipt stays idempotent after later edits");
    CHECK(text_is(a,app_a,"new",3),"old matching receipt cannot restore an old payload");
    CHECK(text_is(b,app_b,"",1),"second application starts independently");
    CHECK(ag_memory_commit(b,app_b,11,1,"beta",4,&revision)==0,"operation IDs are private to each app directory");
    CHECK(text_is(a,app_a,"new",3)&&text_is(b,app_b,"beta",2),"two application documents remain isolated");
    uint32_t before=disk_checksum(a);struct ag_document d={0};
    CHECK(ag_memory_load(a,app_b,&d)==AG_E_SCOPE,"identity: cross-AppID directory load refused");ag_document_free(&d);
    CHECK(ag_memory_commit(a,app_b,20,3,"wrong",5,&revision)==AG_E_SCOPE,"identity: cross-AppID commit refused");
    CHECK(disk_checksum(a)==before,"identity refusal leaves both slot bytes unchanged");

    CHECK(!ag_memory_commit(bad,app_a,1,1,"older",5,&revision),"fallback fixture first checkpoint");
    CHECK(!ag_memory_commit(bad,app_a,2,2,"newer",5,&revision),"fallback fixture second checkpoint");
    corrupt(bad,0);CHECK(text_is(bad,app_a,"older",2),"bad newest slot falls back to last valid checkpoint");
    CHECK(!ag_memory_commit(bad,app_a,3,2,"repair",6,&revision)&&text_is(bad,app_a,"repair",3),
          "explicit write replaces only failed inactive slot after valid fallback");
    corrupt(bad,0);corrupt(bad,1);before=disk_checksum(bad);
    CHECK(ag_memory_load(bad,app_a,&d)==AG_E_IO,"both corrupt slots are not initial empty memory");ag_document_free(&d);
    CHECK(ag_memory_commit(bad,app_a,4,1,"reset",5,&revision)==AG_E_IO,"corrupt memory refuses implicit overwrite");
    CHECK(disk_checksum(bad)==before,"corrupt slot bytes remain untouched on refusal");
    CHECK(!ag_memory_commit(version,app_a,1,1,"old",3,&revision),"version fixture old checkpoint");
    CHECK(!ag_memory_commit(version,app_a,2,2,"new",3,&revision),"version fixture new checkpoint");
    incompatible(version,0);before=disk_checksum(version);
    CHECK(ag_memory_load(version,app_a,&d)==AG_E_VERSION,"incompatible newest slot does not fall back into overwrite");ag_document_free(&d);
    CHECK(ag_memory_commit(version,app_a,3,3,"reset",5,&revision)==AG_E_VERSION,"incompatible memory refuses commit");
    CHECK(disk_checksum(version)==before,"incompatible state remains byte-for-byte retained");

    unsigned receipts=0;
    for(unsigned i=1;i<=AG_RECEIPTS;i++) {
        char value[40];int n=snprintf(value,sizeof value,"memory %u",i);
        CHECK(!ag_memory_commit(full,app_a,i,i,value,(uint32_t)n,&revision)&&revision==i+1,
              "receipt capacity accepts each of sixty-four distinct operations");
    }
    gen=generation(full,&receipts);CHECK(receipts==64&&gen==64,"all sixty-four receipts persist without compaction");
    CHECK(ag_memory_commit(full,app_a,65,65,"full",4,&revision)==AG_E_LIMIT,"receipt limit is explicit and does not silently prune history");
    CHECK(ag_memory_commit(full,app_a,1,1,"memory 1",8,&revision)==1&&revision==65,
          "matching retry still succeeds when receipt history is full");
    CHECK(generation(full,&receipts)==gen&&receipts==64,"full-table retry and refusal keep durable generation unchanged");
    CHECK(text_is(full,app_a,"memory 64",65),"restart preserves full-table memory and revision");

    char *payload=malloc(AG_MEMORY_MAX+1);if(!payload)return 2;memset(payload,'x',AG_MEMORY_MAX+1);
    CHECK(!ag_memory_commit(large,app_a,1,1,payload,AG_MEMORY_MAX,&revision),"capacity: exactly 64 KiB UTF-8 accepted");
    CHECK(!ag_memory_load(large,app_a,&d)&&d.length==AG_MEMORY_MAX&&!memcmp(d.bytes,payload,AG_MEMORY_MAX),
          "capacity: maximum text survives actual checkpoint roundtrip");ag_document_free(&d);
    before=disk_checksum(large);
    CHECK(ag_memory_commit(large,app_a,2,2,payload,AG_MEMORY_MAX+1,&revision)==AG_E_LIMIT,
          "capacity: sixty-four KiB plus one is refused before write");
    CHECK(disk_checksum(large)==before,"over-capacity input leaves durable state unchanged");free(payload);
    CHECK(ag_memory_commit(large,app_a,2,2,"\xc0\x80",2,&revision)==AG_E_ARGUMENT,"invalid UTF-8 rejected");
    CHECK(ag_memory_commit(large,app_a,2,2,"a\0b",3,&revision)==AG_E_ARGUMENT,"binary or NUL-containing model state rejected");
    CHECK(!ag_memory_commit(large,app_a,2,2,0,0,&revision)&&text_is(large,app_a,"",3),"explicit empty text clears memory durably");
    CHECK(ag_memory_commit(a,app_a,0,3,"x",1,&revision)==AG_E_ARGUMENT,"zero operation ID refused");
    CHECK(ag_memory_commit(a,app_a,UINT64_MAX,3,"x",1,&revision)==AG_E_LIMIT,"operation counter exhaustion refused");
    CHECK(ag_memory_load(a,"../wrong",&d)==AG_E_ARGUMENT,"invalid AppID spelling refused");
    struct ag_task task;ag_task_init(&task,1,"ordinary task","/report");task.phase=AG_DONE;
    strcpy(task.app_id,app_a);CHECK(!ag_document_set(&d,"",0,1)&&!ag_store_save(other,&task,&d),"ordinary task fixture is a valid store checkpoint");ag_document_free(&d);
    before=disk_checksum(other);
    CHECK(ag_memory_load(other,app_a,&d)==AG_E_VERSION,"schema: ordinary task checkpoint rejected as memory");ag_document_free(&d);
    CHECK(ag_memory_commit(other,app_a,1,1,"replace",7,&revision)==AG_E_VERSION,"schema: memory does not overwrite an ordinary task");
    CHECK(disk_checksum(other)==before,"schema mismatch preserves task bytes");
    CHECK(!ag_memory_commit(kv,app_a,1,1,"text",4,&revision),"model-state fixture has genuine memory checkpoint");
    CHECK(!ag_store_load(kv,&task,&d),"load model-state fixture through real store");task.model_bytes=1;
    CHECK(!ag_store_save(kv,&task,&d),"persist private incompatible model-state fixture");ag_document_free(&d);
    CHECK(ag_memory_load(kv,app_a,&d)==AG_E_IO,"schema: model cache state is not memory");ag_document_free(&d);
    printf("AGENT_MEMORY checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
