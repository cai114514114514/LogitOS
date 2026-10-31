/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_HWREPORT_LOG_H
#define LOGIT_HWREPORT_LOG_H

/* Internal whole-transcript sink. 0 = saved, -1 = disabled after a filesystem
 * refusal; no retry loop. All later calls update the SAME boot's file. */
int hwreport_save(const char *text, int length);

#endif
