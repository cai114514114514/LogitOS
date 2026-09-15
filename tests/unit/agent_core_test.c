/* SPDX-License-Identifier: MIT */
#include "../../c/lib/agent/task.h"
#include "../../c/lib/agent/json.h"
#include "../../c/lib/agent/model.h"
#include "../../include/abi/agent_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define CHECK(x,label) do {if(!(x)){fprintf(stderr,"FAIL: %s\n",label);return 1;}checks++;}while(0)
static unsigned checks;
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    struct ag_task t;struct ag_document d={0};ag_task_init(&t,1,"write report","/docs/report.md");
    CHECK(!ag_document_set(&d,"",0,1),"initial document");
    CHECK(ag_manual_preserved("report","report\nhuman note","new report\nhuman note\nnext steps"),"manual addition retained across model rewrite");
    CHECK(!ag_manual_preserved("report","report\nhuman note","new report\nnext steps"),"manual addition loss requires review");
    CHECK(!ag_manual_preserved("old phrase","new phrase","old phrase"),"manual replacement cannot silently revert");
    CHECK(ag_manual_preserved("unchanged","unchanged","rewritten"),"model may revise unedited text");
    struct ag_object o={.id=1,.revision=1,.rights=AG_READ,.bytes=3,.kind=1,.path="/docs/source.md"};
    CHECK(!ag_task_grant(&t,&o),"grant explicit object");
    CHECK(ag_citations_valid(&t,"Fact [S1:0-3]"),"source citation range");
    CHECK(ag_citations_valid(&t,"Facts [S1:0-1, 1-3]"),"grouped source ranges");
    CHECK(!ag_citations_valid(&t,"Facts [S1:0-1, 1-4]")&&!ag_citations_valid(&t,"[S2:0-1]"),"each cited span must name real bytes");
    CHECK(!ag_citations_valid(&t,"[S1:0-18446744073709551616]")&&!ag_citations_valid(&t,"[S1:-1-3]"),"citation integer bounds");
    CHECK(ag_task_authorize(&t,AG_FINDER,2,AG_READ)==AG_E_SCOPE,"permission: outside object rejected");
    CHECK(ag_task_authorize(&t,AG_FINDER,1,AG_RIGHT_EDIT)==AG_E_SCOPE,"permission: finder cannot write");
    CHECK(!ag_task_enter(&t)&&!ag_task_enter(&t)&&ag_task_enter(&t)==AG_E_LIMIT,"two worker limit");
    ag_task_leave(&t);ag_task_leave(&t);CHECK(!t.active,"worker baseline");
    for(int i=0;i<32;i++)CHECK(!ag_task_call(&t),"model budget available");
    CHECK(ag_task_call(&t)==AG_E_BUDGET&&t.phase==AG_WAIT_BUDGET,"budget retains progress");
    CHECK(!ag_task_control(&t,AG_QUEUED,32)&&!ag_task_enter(&t),"explicit budget extension");
    CHECK(ag_task_commit(&t,&d,9,2,"new",3)==AG_E_CONFLICT,"revision: stale candidate cannot replace text");
    CHECK(!ag_task_commit(&t,&d,9,1,"new",3),"commit document");
    CHECK(ag_task_commit(&t,&d,9,1,"new",3)==1&&t.revision==2,"dedup: repeat keeps one effect");
    CHECK(ag_task_commit(&t,&d,9,2,"BAD",3)==AG_E_CONFLICT,"dedup binds payload");
    CHECK(!ag_store_save(argv[1],&t,&d)&&t.generation==1,"checkpoint: acknowledged write is durable");
    struct ag_task restored;struct ag_document rd={0};
    CHECK(!ag_store_load(argv[1],&restored,&rd)&&restored.receipt_count==1&&rd.length==3&&!memcmp(rd.bytes,"new",3),"checkpoint: restore receipt and document together");
    CHECK(!restored.active&&restored.phase==AG_QUEUED,"restart drops process state");ag_document_free(&rd);
    CHECK(!ag_store_save(argv[1],&t,&d)&&t.generation==2,"second slot");
    char p[200];snprintf(p,sizeof p,"%s/slot0",argv[1]);int fd=open(p,O_WRONLY);CHECK(fd>=0,"open newer slot");
    CHECK(write(fd,"broken",6)==6&&close(fd)==0,"inject interrupted newest slot");
    CHECK(!ag_store_load(argv[1],&restored,&rd)&&restored.generation==1,"torn snapshot recovers last confirmed generation");ag_document_free(&rd);
    char *large=malloc(AEX_AGENT_DOCUMENT_MAX+1);memset(large,'x',AEX_AGENT_DOCUMENT_MAX+1);
    CHECK(!ag_document_set(&d,large,AEX_AGENT_DOCUMENT_MAX,3),"one MiB accepted");
    CHECK(ag_document_set(&d,large,AEX_AGENT_DOCUMENT_MAX+1,4)==AG_E_LIMIT&&d.length==AEX_AGENT_DOCUMENT_MAX,"oversize never truncates existing document");free(large);
    CHECK(ag_utf8("中文\xf0\x9f\x98\x80",10)&&!ag_utf8("\xc0\x80",2)&&!ag_utf8("\xed\xa0\x80",3),"strict UTF-8");
    struct ag_json j;const char *js="{\"a\":\"\\ud83d\\ude00\",\"b\":[1,2]}";
    CHECK(!ag_json_parse(&j,js,strlen(js)),"JSON parse");char *str=ag_json_string(&j,ag_json_get(&j,0,"a"));
    CHECK(str&&!strcmp(str,"\xf0\x9f\x98\x80"),"JSON surrogate pair");free(str);ag_json_free(&j);
    const char badnul[]={'{','}',0};CHECK(ag_json_parse(&j,badnul,3)<0,"JSON embedded NUL rejected");
    CHECK(ag_json_parse(&j,"[1,]",4)<0,"JSON trailing comma rejected");
    const char *response="{\"choices\":[{\"message\":{\"content\":\"真实报告\"},\"finish_reason\":\"stop\"}]}";
    char *answer=0;CHECK(!ag_model_decode(response,strlen(response),&answer)&&!strcmp(answer,"真实报告"),"model final content decoded");free(answer);
    response="{\"choices\":[{\"message\":{\"content\":\"truncated\"},\"finish_reason\":\"length\"}]}";
    CHECK(ag_model_decode(response,strlen(response),&answer)==AG_E_MODEL&&!answer,"truncated model output rejected");
    struct ag_model_config config={.model="deepseek-flash",.max_tokens=8192};char *body=ag_model_body(&config,"system","context\nquote\"");
    CHECK(body&&!ag_json_parse(&j,body,strlen(body)),"provider request is valid JSON");ag_json_free(&j);free(body);
    for(unsigned n=0;n<256;n++)if(n==SYS_OPEN||n==SYS_FORK||n==SYS_EXECVE||n==SYS_SOCK_OPEN||n==SYS_SIGRETURN)
        CHECK(!aex_agent_syscall_allowed(n,3),"worker ambient authority rejected");
    CHECK(aex_agent_syscall_allowed(SYS_WRITE,3)&&!aex_agent_syscall_allowed(SYS_WRITE,1),"channel-only descriptor inheritance");
    CHECK(!ag_task_control(&t,AG_CANCELLED,0)&&ag_task_control(&t,AG_QUEUED,0)==AG_E_STATE,"cancel is terminal");
    ag_document_free(&d);printf("AGENT_CORE_OK checks=%u\n",checks);return 0;
}
