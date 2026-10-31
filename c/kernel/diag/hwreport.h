/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_HWREPORT_H
#define LOGIT_HWREPORT_H

/* Boot/BSP context only, never from an IRQ or a concurrent driver callback.
 * detail describes evidence, not a boolean: ARMED is not tested input and a
 * bound audio driver is not evidence that a person heard sound. Unknown IDs
 * are ignored. The fixed names are the L0-L9 bring-up ladder. */
void hwreport_milestone(const char *id, const char *detail);
void hwreport_dump(void);

#endif
