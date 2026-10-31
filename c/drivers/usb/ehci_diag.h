/* Per-endpoint EHCI periodic state, for the hardware panel (hwhud.c).
 * Read without locks: diagnostic values, never decisions. */
#ifndef EHCI_DIAG_H
#define EHCI_DIAG_H
#include <stdint.h>

struct ehci_ep_diag {
    uint8_t controller, dev_addr, ep, armed;
    uint16_t period, phase;
    uint32_t done, errors;          /* interrupt transfers completed / failed */
    uint32_t td_token, qh_token;    /* last completed qTD token; live QH overlay token */
};
/* 0 and *out filled for periodic endpoint i, -1 past the last one. */
int ehci_diag_get(int i, struct ehci_ep_diag *out);

#endif
