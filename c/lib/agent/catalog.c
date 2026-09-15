/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "catalog.h"
#include <string.h>
const struct ag_app ag_apps[]={
#define AG_APP(name,id,path,domain) {name,id,path,domain},
#include "agent_catalog.inc"
#undef AG_APP
};
const unsigned ag_app_count=sizeof ag_apps/sizeof *ag_apps;
const struct ag_app *ag_app_find(const char *id)
{for(unsigned i=0;i<ag_app_count;i++)if(!strcmp(id,ag_apps[i].id))return &ag_apps[i];return 0;}
