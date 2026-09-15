#ifndef LOGIT_FFMPEG_AACSBR_CONFIG_H
#define LOGIT_FFMPEG_AACSBR_CONFIG_H

/* LogitOS deliberately starts with the scalar C path.  The upstream x86
 * dispatch reaches FFmpeg's private CPU feature framework, while this port is
 * linked into a freestanding guest.  Once the scalar differential is green,
 * individual kernels can be replaced behind SBRDSPContext without changing
 * the bitstream or state machinery. */
#define ARCH_ARM 0
#define ARCH_AARCH64 0
#define ARCH_X86 0
#define ARCH_MIPS 0

#endif
