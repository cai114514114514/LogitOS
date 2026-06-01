#include <stdint.h>
#include "roots.h"

/* Built-in trusted root CA public keys. The table (and the key byte arrays it
 * points at) is generated from tools/roots/*.pem by tools/genroots.py. Add a
 * PEM there and re-run the generator to trust another root. */
#include "roots_bundle.inc"
