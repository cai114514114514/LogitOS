/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "model.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
char *ag_model_body(const struct ag_model_config *c,const char *system,const char *input)
{
    char *s=ag_json_quote(system),*u=ag_json_quote(input),*model=ag_json_quote(c->model);
    if(!s||!u||!model){free(s);free(u);free(model);return 0;}
    size_t cap=strlen(s)+strlen(u)+strlen(model)+1024;char *out=malloc(cap);
    if(out)snprintf(out,cap,"{\"model\":%s,\"messages\":[{\"role\":\"system\",\"content\":%s},"
        "{\"role\":\"user\",\"content\":%s}],\"stream\":false,\"thinking\":{\"type\":\"%s\"},"
        "\"max_tokens\":%u,\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"complete_work\","
        "\"description\":\"Return the completed document or research notes to the requesting application.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},"
        "\"required\":[\"content\"],\"additionalProperties\":false}}}],\"tool_choice\":\"auto\"}",
        model,s,u,c->thinking?"enabled":"disabled",c->max_tokens);
    free(s);free(u);free(model);return out;
}
int ag_model_decode(const char *s,size_t n,char **out)
{
    *out=0;struct ag_json j;if(ag_json_parse(&j,s,n)<0)return AG_E_MODEL;
    int choice=ag_json_at(&j,ag_json_get(&j,0,"choices"),0);
    int msg=ag_json_get(&j,choice,"message");
    char *finish=ag_json_string(&j,ag_json_get(&j,choice,"finish_reason"));
    if(finish&&!strcmp(finish,"stop"))*out=ag_json_string(&j,ag_json_get(&j,msg,"content"));
    else if(finish&&!strcmp(finish,"tool_calls")){
        int calls=ag_json_get(&j,msg,"tool_calls"),call=ag_json_at(&j,calls,0);
        if(ag_json_at(&j,calls,1)<0){
            int fn=ag_json_get(&j,call,"function");char *name=ag_json_string(&j,ag_json_get(&j,fn,"name"));
            char *args=ag_json_string(&j,ag_json_get(&j,fn,"arguments"));
            if(name&&args&&!strcmp(name,"complete_work")){
                struct ag_json a;if(ag_json_parse(&a,args,strlen(args))==0){
                    /* Exactly one argument: unknown actions or parameters never
                     * become shell commands or unchecked object operations. */
                    int key=a.tokens[0].child, val=key>=0?a.tokens[key].next:-1;
                    if(val>=0&&a.tokens[val].next<0)*out=ag_json_string(&a,ag_json_get(&a,0,"content"));
                    ag_json_free(&a);}}
            free(name);free(args);
        }
    }
    free(finish);ag_json_free(&j);
    if(!*out||!**out||strlen(*out)>AEX_AGENT_DOCUMENT_MAX||!ag_utf8(*out,(uint32_t)strlen(*out))){free(*out);*out=0;return AG_E_MODEL;}
    return 0;
}
