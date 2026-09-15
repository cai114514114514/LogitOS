/* c/lib/audio/aac_sbr.h -- HE-AAC v1 Spectral Band Replication adapter.
 *
 * The bitstream/state/QMF implementation is FFmpeg's LGPL-2.1-or-later SBR
 * decoder at tag n4.4, kept under third_party/ffmpeg-aacsbr with its original
 * notices.  This header is the small project-authored boundary: AAC-LC hands
 * it one normalized 1024-sample core frame and receives one 2048-sample frame
 * at the extension sampling rate.
 */
#ifndef LOGIT_AAC_SBR_H
#define LOGIT_AAC_SBR_H

#include <stdint.h>

typedef struct aac_sbr_ctx aac_sbr_ctx;

/* HE-AAC v1 is one SCE or one CPE.  Parametric Stereo (AOT 29) remains a
 * distinct unsupported profile; accepting it here would duplicate a mono
 * channel instead of decoding PS and would be the old core-only lie again. */
aac_sbr_ctx *aac_sbr_open(int core_rate, int output_rate, int channels);
void aac_sbr_close(aac_sbr_ctx *s);

/* Parse one EXT_SBR_DATA/EXT_SBR_DATA_CRC payload. bitpos points immediately
 * after the four-bit extension_type, while count is the whole FIL payload in
 * bytes (including that nibble), matching ISO table 4.55. element_type is the
 * AAC syntactic id: SCE=0, CPE=1, LFE=3. */
int aac_sbr_parse(aac_sbr_ctx *s, const uint8_t *data, long nbits,
                  long bitpos, int count, int crc, int element_type);

/* core is interleaved 1024-sample normalized float PCM; output is interleaved
 * 2048-sample PCM. The buffers must not alias. */
int aac_sbr_synthesize(aac_sbr_ctx *s, const float *core, float *output,
                       int channels);

#endif
