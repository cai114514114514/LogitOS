#ifndef LOGIT_OBSERVE_HUD_H
#define LOGIT_OBSERVE_HUD_H

/* Format one line of the observation summary for the hardware HUD overlay and
 * return its length. */
int observe_hud_format_summary(char *out, int capacity);

/* Development-machine harnesses that compile the HUD without the observer get
 * a stand-in that formats nothing and reports a length of zero, so the HUD adds
 * no line and the harness links no observer code. */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define observe_hud_format_summary(out, capacity) ((void)(out), (void)(capacity), 0)
#endif

#endif
