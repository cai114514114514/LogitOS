/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_CATALOG_H
#define LOGIT_AGENT_CATALOG_H
struct ag_app { const char *name,*id,*path,*domain; };
extern const struct ag_app ag_apps[];
extern const unsigned ag_app_count;
const struct ag_app *ag_app_find(const char *id);
#endif
