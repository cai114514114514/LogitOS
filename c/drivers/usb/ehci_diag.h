/* Per-endpoint EHCI periodic state, for the hardware panel (hwhud.c).
 * Read without locks: diagnostic values, never decisions. */
#ifndef EHCI_DIAG_H
#define EHCI_DIAG_H
#include <stdint.h>

struct ehci_ep_diag {
    uint8_t controller, dev_addr, ep, armed;
    uint16_t period, phase;
    uint32_t done, errors;          /* interrupt transfers completed / failed */
    uint32_t td_token, qh_token;    /* last completed qTD and QH overlay tokens */
    uint8_t phase_uf, smask, cmask, speed, state;
    uint16_t us_s, us_c, us_tt;
    uint32_t rearms, err_halt, err_xact, err_mmf, err_dbe, err_babble, stalls;
    uint32_t overlay_status;       /* copied at completion; never panel DMA */
};
struct ehci_sched_diag {
    uint32_t pse_toggles, rearms, unlinks, relinks, refused;
    uint16_t max_uframe_us, max_tt_frame_us;
    uint8_t eps;
};
int ehci_sched_diag_get(unsigned controller, struct ehci_sched_diag *out);
/* 0 and *out filled for periodic endpoint i, -1 past the last one. */
int ehci_diag_get(int i, struct ehci_ep_diag *out);
/* 0 and USBSTS/USBINTR for controller slot c, -1 if unused or out of range. */
int ehci_ctrl_diag(unsigned c, uint32_t *sts, uint32_t *intr, unsigned long *errors);
/* pci_msi.c: INTx line i as one line of text, -1 past the last. */
int pci_intx_diag(int i, char *buf, int max);

#endif
