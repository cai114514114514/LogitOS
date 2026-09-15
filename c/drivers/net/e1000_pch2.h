/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_E1000_PCH2_H
#define LOGIT_E1000_PCH2_H
struct device;
int e1000_pch2_probe(struct device *dev);
void e1000_pch2_remove(struct device *dev);
#endif
