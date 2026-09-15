/* SPDX-License-Identifier: GPL-3.0-or-later */
#define LOGIT_WEAK_LOCAL_ag_gui_dispatch 1
#include "gui.h"
#include "catalog.h"
#include <string.h>
__attribute__((weak)) const char *ag_gui_context(unsigned *bytes)
{*bytes=0;return "";}
int ag_gui_dispatch(const struct logit_event *e)
{
    if(e->type!=EV_KEY||e->a!=12)return 0;
    struct aex_agent_identity id;if(ag_self(&id)<0)return 1;
    const struct ag_app *app=ag_app_find(id.app_id);if(!app)return 1;
    unsigned n;const char *s=ag_gui_context(&n);uint64_t context;
    if(ag_publish_state(app->name,s,n,monotonic_ms()+1,&context)==0)ag_show_assistant(context);
    else notify("Tasks","Context could not be published",0);
    return 1;
}
