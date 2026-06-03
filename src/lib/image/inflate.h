#ifndef AQUA_INFLATE_H
#define AQUA_INFLATE_H

#include <stdint.h>

/* RFC 1951 raw DEFLATE decompress into out (capacity outcap). On success returns
 * 0 and sets *outlen to the decompressed length; -1 on malformed input or if the
 * output would exceed outcap. Integer-only. */
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen);

/* RFC 1950 zlib: skip the 2-byte header (+ optional dict), inflate the DEFLATE
 * body, ignore the trailing adler32. Same return contract. */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen);

#endif /* AQUA_INFLATE_H */
