/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_GUI_H
#define LOGIT_AGENT_GUI_H
#include "sdk.h"
#include "../../apps/logit.h"
/* The application supplies a semantic snapshot only after this explicit
 * gesture. Hidden fields, passwords and surrounding application memory are
 * never scraped by the broker. */
const char *ag_gui_context(unsigned *bytes);
#include "../../../include/weaksym.h"
int ag_gui_dispatch(const struct logit_event *) LOGIT_WEAK;
LOGIT_WEAK_STUB(ag_gui_dispatch);
static inline int ag_gui_event(const struct logit_event *e)
{return LOGIT_HAVE(ag_gui_dispatch) ? ag_gui_dispatch(e) : 0;}
#endif
