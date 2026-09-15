#ifndef LOGIT_ES1370_REGISTERS_H
#define LOGIT_ES1370_REGISTERS_H

/* Byte I/O offsets and fields from Linux v6.12 sound/pci/ens1370.c (CHIP1370).
 * The 0x30..0x3f window is banked by MEM_PAGE. Register names alone do not
 * identify the active DMA engine; every window transaction selects its page. */
#define ES_CONTROL 0x00u
#define ES_STATUS 0x04u
#define ES_MEM_PAGE 0x0cu
#define ES_CODEC 0x10u
#define ES_SERIAL 0x20u
#define ES_DAC2_COUNT 0x28u
#define ES_ADC_COUNT 0x2cu
#define ES_ADC_FRAME 0x30u
#define ES_ADC_SIZE 0x34u
#define ES_DAC2_FRAME 0x38u
#define ES_DAC2_SIZE 0x3cu
#define ES_PHANTOM_FRAME 0x38u
#define ES_PHANTOM_SIZE 0x3cu
#define ES_PAGE_DAC 0x0cu
#define ES_PAGE_ADC 0x0du
#define ES_IO_BYTES 64u

#define ES_CONTROL_CODEC_ENABLE (1u << 1)
#define ES_CONTROL_DAC2_ENABLE (1u << 5)
#define ES_CONTROL_ADC_ENABLE (1u << 4)
#define ES_CONTROL_DMA_ENABLES ((1u << 4) | (1u << 5) | (1u << 6))
#define ES_STATUS_INTERRUPT (1u << 31)
#define ES_STATUS_DAC2 (1u << 1)
#define ES_STATUS_ADC (1u << 0)
#define ES_STATUS_CODEC_BUSY (1u << 10)
#define ES_SERIAL_DAC2_IRQ (1u << 9)
#define ES_SERIAL_ADC_IRQ (1u << 10)
#define ES_SERIAL_ADC_S16_STEREO (3u << 4)
#define ES_SERIAL_DAC2_S16_STEREO (3u << 2)
#define ES_SERIAL_DAC2_END_INCREMENT (2u << 19)

/* The DAC2 integer divisor cannot produce 48000 exactly. Divider 27 is the
 * closest setting: 1411200 / (27 + 2) = 48662 Hz (integer reporting). The
 * mixer must see this rate so a 48 kHz application is resampled correctly. */
#define ES_DAC2_DIVIDER 27u
#define ES_RATE (1411200u / (ES_DAC2_DIVIDER + 2u))
#define ES_CONTROL_IDLE ((ES_DAC2_DIVIDER << 16) | ES_CONTROL_CODEC_ENABLE)
#define ES_SERIAL_FORMAT (ES_SERIAL_DAC2_S16_STEREO | ES_SERIAL_DAC2_END_INCREMENT)

/* AK4531 is not AC97: writes are register<<8 | value on a 16-bit port. */
#define AK_MASTER_LEFT 0x00u
#define AK_MASTER_RIGHT 0x01u
#define AK_VOICE_LEFT 0x02u
#define AK_VOICE_RIGHT 0x03u
#define AK_MIC_VOLUME 0x0eu
#define AK_MONO_OUTPUT 0x0fu
#define AK_OUTPUT_SWITCH1 0x10u
#define AK_OUTPUT_SWITCH2 0x11u
#define AK_INPUT_SWITCH_FIRST 0x12u
#define AK_INPUT_SWITCH_LAST 0x15u
#define AK_INPUT_LEFT_SWITCH1 0x12u
#define AK_INPUT_RIGHT_SWITCH1 0x13u
#define AK_INPUT_MIC_ROUTE 0x01u
#define AK_RESET 0x16u
#define AK_CLOCK 0x17u
#define AK_ADC_INPUT 0x18u
#define AK_MIC_GAIN 0x19u
#define AK_MUTE_VOLUME 0x9fu
#define AK_MUTE_MONO 0x87u
#define AK_POWERED_RESET 0x02u
#define AK_POWERED_RUNNING 0x03u
#define AK_VOICE_OUTPUT_BOTH 0x0cu
#define AK_VOICE_UNITY_GAIN 0x06u

#endif
