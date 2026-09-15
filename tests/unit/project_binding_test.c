/* SPDX-License-Identifier: MIT */
#include "../../c/lib/agent/files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int ok,const char *s){if(!ok){fprintf(stderr,"BINDING_FAIL %s\n",s);exit(1);}}
int main(int argc,char **argv)
{
    check(argc==2,"private test directory");struct ag_binding a={0},b={0};
    for(unsigned i=0;i<AG_BIND_COUNT;i++){
        a.refs[i]=(struct logit_file_id){{0xfffffffffffffff1ull,0x8000000000000001ull},i+1};a.revisions[i]=i+7;
    }
    for(unsigned i=0;i<6;i++){a.revisions[3]++;check(!ag_binding_save(argv[1],1,&a),"durable generation write");}
    check(!ag_binding_load(argv[1],&b),"load durable binding");
    check(!memcmp(a.refs,b.refs,sizeof a.refs)&&!memcmp(a.revisions,b.revisions,sizeof a.revisions),"full-width identities and revisions preserved");
    struct ag_binding retry={0};memcpy(retry.refs,a.refs,sizeof a.refs);memcpy(retry.revisions,a.revisions,sizeof a.revisions);retry.refs[2].object=999;
    check(!ag_binding_save(argv[1],1,&retry)&&retry.journal.generation>a.journal.generation,"retry does not reset durable generation");
    check(!ag_binding_load(argv[1],&b)&&b.refs[2].object==999,"reboot selects retried binding");
    char path[256];snprintf(path,sizeof path,"%s/bindings/slot%u",argv[1],(unsigned)(retry.journal.generation&1));FILE *f=fopen(path,"wb");check(f!=0,"open torn slot");fputs("torn",f);fclose(f);
    check(!ag_binding_load(argv[1],&b)&&b.refs[2].object==a.refs[2].object,"torn slot restores last complete binding");
    snprintf(path,sizeof path,"%s/bindings/slot%u",argv[1],(unsigned)(a.journal.generation&1));f=fopen(path,"wb");check(f!=0,"open other torn slot");fputs("torn",f);fclose(f);
    check(ag_binding_load(argv[1],&b)==AG_E_IO,"two damaged slots cannot invent an identity");
    puts("PROJECT_BINDING_PASS full-width roundtrip, generation retry and damaged-slot recovery");return 0;
}
