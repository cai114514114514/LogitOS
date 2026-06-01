#include <stdint.h>

/* Built-in trusted root CA public keys (EC points X||Y). A small set; the
 * SSL.com TLS ECC Root CA 2022 (P-384) is example.com's trust anchor. Add more
 * roots here to cover more sites. */

struct root_ca { int curve; const uint8_t *pub; int publen; };

/* SSL.com TLS ECC Root CA 2022 — P-384 public key (X||Y, 96 bytes). */
static const uint8_t sslcom_ecc_root_2022[96] = {
#include "roots_sslcom.inc"
};

const struct root_ca aqua_roots[] = {
    { 384, sslcom_ecc_root_2022, 96 },
};
const int aqua_nroots = (int)(sizeof aqua_roots / sizeof aqua_roots[0]);
