/* SPDX-License-Identifier: MIT */
#include "internal.h"

static int json_int(const struct ag_json *j,int object,const char *name,int *value)
{int t=ag_json_get(j,object,name);if(t<0)return -1;const struct ag_jtoken *k=&j->tokens[t];int v=0;if(k->end<=k->start||k->end-k->start>9)return -1;
 for(int i=k->start;i<k->end;i++){char c=j->text[i];if(c<'0'||c>'9'||v>100000000)return -1;v=v*10+c-'0';}*value=v;return 0;}

void st_finished_job(StEngine *e)
{
    if(e->runner_seen||!e->state.runner.finished)return;e->runner_seen=1;
    if(e->state.runner.cancelled){st_engine_notice(e,"Process stopped");return;}if(e->state.runner.io_error){st_engine_notice(e,"Process I/O failed; output may be incomplete");return;}
    int ec=WIFEXITED(e->state.runner.status)?WEXITSTATUS(e->state.runner.status):-1;
    if(!e->state.runner.mode){snprintf(e->state.notice,sizeof e->state.notice,"Process finished: %s %d%s",ec<0?"signal":"exit",ec<0?WTERMSIG(e->state.runner.status):ec,e->state.runner.truncated?"; output limit reached":"");return;}
    if(e->state.runner.tab<0||e->state.runner.tab>=e->state.count||!st_runner_current(&e->state.runner,&e->state.docs[e->state.runner.tab],e->state.runner.tab)){st_engine_notice(e,"Check finished for an older version; current edits are kept.");return;}
    if(e->state.runner.truncated){st_engine_notice(e,"Compiler diagnostics exceeded the output limit; result rejected.");return;}
    struct ag_json j;if(ag_json_parse(&j,e->state.runner.text,(size_t)e->state.runner.length)<0){st_engine_notice(e,"Compiler did not return structured diagnostics. Check the installed /bin/as.");return;}
    int version=0,bytes=0;char *file=ag_json_string(&j,ag_json_get(&j,0,"file"));
    int valid=json_int(&j,0,"version",&version)==0&&version==1&&json_int(&j,0,"source_bytes",&bytes)==0&&bytes==e->state.runner.source_length&&file&&!strcmp(file,e->state.docs[e->state.runner.tab].path);
    free(file);int array=ag_json_get(&j,0,"diagnostics");if(array<0||j.tokens[array].type!='[')valid=0;
    int n=0;StProblem parsed[32];
    while(valid&&n<32){int item=ag_json_at(&j,array,(unsigned)n);if(item<0)break;StProblem *p=&parsed[n];
        if(json_int(&j,item,"start",&p->start)<0||json_int(&j,item,"end",&p->end)<0||json_int(&j,item,"line",&p->line)<0||json_int(&j,item,"column",&p->column)<0||p->end<p->start||
           p->line<1||p->column<1||!st_boundary(e->state.runner.source,e->state.runner.source_length,p->start)||!st_boundary(e->state.runner.source,e->state.runner.source_length,p->end)){valid=0;break;}
        char *msg=ag_json_string(&j,ag_json_get(&j,item,"message")),*code=ag_json_string(&j,ag_json_get(&j,item,"code"));
        if(!msg||!code){valid=0;free(msg);free(code);break;}snprintf(p->message,sizeof p->message,"%s",msg);snprintf(p->code,sizeof p->code,"%s",code);free(msg);free(code);n++;
    }
    int ok=ag_json_get(&j,0,"ok");
    if(ok<0||j.tokens[ok].type!=(n?'f':'t')||ag_json_at(&j,array,32)>=0||ec!=(n?1:0))valid=0;
    if(valid){memcpy(e->state.problems,parsed,(size_t)n*sizeof *e->state.problems);e->state.problem_count=n;e->state.problem_tab=e->state.runner.tab;e->state.problem_revision=e->state.runner.revision;snprintf(e->state.notice,sizeof e->state.notice,n?"%d compiler problems":"Syntax check passed",n);}
    else st_engine_notice(e,"Invalid compiler result; diagnostics were not attached.");ag_json_free(&j);
}
