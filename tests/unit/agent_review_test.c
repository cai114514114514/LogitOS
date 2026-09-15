/* SPDX-License-Identifier: MIT */
#include "../../c/lib/agent/review.h"
#include "../../c/apps/gui/textedit/textedit_document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(label,expr) do {if(!(expr)){fprintf(stderr,"FAIL: %s\n",label);return 1;}checks++;}while(0)
int main(int argc,char **argv)
{
    if(argc!=2)return 2;int checks=0;struct ag_task t;struct ag_review r={0},loaded={0};
    ag_task_init(&t,1,"Review a document","/docs/report.md");
    CHECK("enable review",ag_review_enable(argv[1],&t,&r,"/docs/source.md")==0&&r.enabled);
    CHECK("no invented proposal",!ag_review_pending(&t,&r));
    CHECK("save proposal",ag_review_propose(argv[1],&t,&r,42,1,"候选稿",9)==0);
    t.phase=AG_CONFLICT;
    CHECK("proposal survives restart",ag_review_load(argv[1],&loaded)==0&&loaded.candidate.length==9&&!strcmp(loaded.candidate.bytes,"候选稿"));
    CHECK("matching review accepted",ag_review_validate(&t,&loaded,1,r.record.generation)==0);
    CHECK("review version: stale document refused",ag_review_validate(&t,&loaded,2,r.record.generation)==AG_E_CONFLICT);
    CHECK("review version: stale proposal refused",ag_review_validate(&t,&loaded,1,r.record.generation-1)==AG_E_CONFLICT);
    CHECK("new proposal",ag_review_propose(argv[1],&t,&r,43,1,"另一份稿",12)==0);
    /* Tear the newest slot. The previous fsynced proposal remains usable. */
    char file[256];snprintf(file,sizeof file,"%s/review/slot%u",argv[1],(unsigned)(r.record.generation&1));
    FILE *f=fopen(file,"wb");CHECK("tear newest snapshot",f&&fwrite("torn",1,4,f)==4&&fclose(f)==0);
    ag_review_free(&loaded);
    CHECK("torn proposal preserves last durable version",ag_review_load(argv[1],&loaded)==0&&!strcmp(loaded.candidate.bytes,"候选稿"));
    CHECK("document binding survives restart",!strcmp(loaded.record.artifact,"/docs/source.md"));
    t.phase=AG_CANCELLED;CHECK("cancel refuses late apply",ag_review_validate(&t,&loaded,1,loaded.record.generation)==AG_E_STATE);
    t.phase=AG_CONFLICT;t.workflow++;CHECK("old goal cannot supply a candidate",!ag_review_pending(&t,&loaded));t.workflow--;
    t.receipt_count=1;t.receipts[0].operation=42;CHECK("committed proposal cannot replay after crash",!ag_review_pending(&t,&loaded));
    ag_review_free(&r);ag_review_free(&loaded);
    char buf[32]="你好 world";int n=(int)strlen(buf),caret=6,anchor=3;
    CHECK("UTF8 selected replacement",!ted_replace(buf,&n,31,&caret,&anchor,"们",3)&&!strcmp(buf,"你们 world"));
    CHECK("UTF8 caret previous",ted_prev(buf,6)==3&&ted_next(buf,3,n)==6);
    char before[32];strcpy(before,buf);int len=n;
    CHECK("capacity refusal is atomic",ted_replace(buf,&n,n,&caret,&anchor,"extra",5)<0&&n==len&&!strcmp(buf,before));
    int prefix,ae,be;ted_difference("我保留批注",15,"我保留批注并补充",24,&prefix,&ae,&be);
    CHECK("diff preserves UTF8 boundaries",prefix==15&&ae==15&&be==24);
    ted_difference("中",3,"串",3,&prefix,&ae,&be);
    CHECK("diff shared lead byte is not a boundary",prefix==0&&ae==3&&be==3);
    printf("TEXTEDIT_REVIEW_HOST_PASS checks=%d\n",checks);return 0;
}
