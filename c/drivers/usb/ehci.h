/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_EHCI_H
#define LOGIT_EHCI_H
struct device;
int ehci_probe(struct device *);
void ehci_remove(struct device *);
#endif
