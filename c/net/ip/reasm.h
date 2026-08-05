#ifndef LOGIT_REASM_H
#define LOGIT_REASM_H

#include <stdint.h>

/* IPv4 fragment reassembly (RFC 791 receive side; overlap/timeout policy in
 * the spirit of RFC 5722). ip.c feeds every MF/offset fragment here; a
 * completed datagram is dispatched from the slot buffer and then released. */

struct reasm_dgram {
    const uint8_t *iph;      /* offset-0 fragment's IP header (slot storage) */
    const uint8_t *l4;       /* reassembled L4 payload (slot storage) */
    uint16_t       l4len;
    int            slot;     /* private: hand back to reasm_release() */
};

/* Feed one fragment. src/dst are host order; iph/ihl describe this fragment's
 * own IP header; off is the payload byte offset (frag field << 3); more is
 * the MF flag. Returns 1 and fills `out` when the datagram is complete --
 * the caller must dispatch it immediately and call reasm_release(). Returns
 * 0 while incomplete, and drops the whole datagram (returns 0) on overlap or
 * oversize. */
int  reasm_input(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                 const uint8_t *iph, uint8_t ihl,
                 uint16_t off, int more, const uint8_t *data, uint16_t dlen,
                 struct reasm_dgram *out);

/* Free the slot a completed datagram occupied. */
void reasm_release(struct reasm_dgram *g);

/* Timeout sweep (~30 s), hooked from net_poll() via a weak reference. */
void ip_poll(void);

#endif /* LOGIT_REASM_H */
