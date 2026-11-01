# Audio controller drivers

The controller driver owns PCI resources, DMA rings, codec programming and IRQ
acknowledgement. The existing `c/kernel/audio` mixer owns PCM conversion,
resampling and stream mixing; applications keep using the same sound syscalls.

| Driver | Controller path | Playback and capture format |
| --- | --- | --- |
| `hda.c` | High Definition Audio controller and codec widget graph | 48 kHz stereo s16 |
| `ac97/` | Intel ICH / ICH0 / ICH2 / ICH3 AC'97 PCM output and PCM input | 48 kHz stereo s16 |
| `es1370/` | Ensoniq AudioPCI ES1370 DAC2 / ADC and AK4531 codec | Hardware divider rate, stereo s16 |

AC'97 and ES1370 are separate implementations. ES1371 and later AudioPCI chips
have a different codec/rate interface and must not be bound by the ES1370 ID
table. USB Audio and VirtIO Sound are not supplied by these drivers.

The mixer currently selects the first successfully registered playback device,
and capture selects its first registered input independently. Another controller
cannot overwrite either selection. There is no multiple-output or input-selector
UI. The first AC'97/ES1370 implementation was playback-only; both now supply
independent capture DMA rings and microphone routes through the existing `rec`
application. Normal capture close/reopen and simultaneous playback are supported.
Existing HDA capture remains its own path. The earlier shared mixer did not
restart DMA after unregister/re-register, because one latch represented both
the worker and the current device. It now retains one worker and initializes
each new device's buffers and DMA state separately. Unregister releases old
streams and scratch under the worker lock; driver teardown still owns stopping
hardware and freeing DMA. This supports framework detach/re-register, without
claiming a physical hotplug notification path or recovery from quarantine.

An AC'97 or ES1370 input can register independently when another card already
owns playback. `test-audio-multicard-os` validates an output-only HDA codec with
each of these input cards, including ES1370 input and HDA output at different
native clock rates. Runtime input/output selection UI is still absent.

ES1370 DAC2 uses an integer divider. Its nearest nominal 48 kHz setting is
reported as 48662 Hz, rather than claiming exact 48000 Hz. The shared mixer
resamples application streams to the rate actually programmed into hardware.

Each new controller family has its own directory and register definitions.
AC'97 separates portable controller operations, codec/input routing, streams,
PCI integration and native port I/O. ES1370 separates codec programming, shared
engine transitions, PCM callbacks and PCI lifecycle. Public headers retain chip-specific names because every
kernel source directory is currently on the global include path; a private
`driver.h` here would shadow the device-model header in unrelated modules.

## HDA on a real board: every analog output, auto-mute, GPU audio, test tone

The speakers of a desktop board may sit on the rear green jack, the front
headphone jack or the monitor, and nobody can tell from outside the machine. So
`hda.c` no longer picks one output pin. `hda_output_paths.c` (pure, host
tested) reads every pin's Configuration Default the way Linux's
`snd_hda_parse_pin_defcfg()` does: pins whose port connectivity is "no
connection" are skipped, line-out, speaker and headphone pins are collected
(a fixed-function "line-out" becomes a speaker), ordered by association and
sequence, and the middle two of three or four line-outs are swapped as Linux
does. Each pin gets a path to an analog DAC, directly or through one mixer or
selector. Unused DACs come first, in NID order (`hda_generic.c`
`look_for_dac()`); when the codec runs out, the pin shares the first path's DAC
through its output mixer, so on an ALC887 the front headphone jack rides the
front DAC while four rear jacks keep DACs 2 to 5. Every path gets power D0 (each
confirmed), the pin's connection select, every amplifier unmuted at its 0 dB
step (never the maximum), OUT_EN on the pin (plus HP_EN on a headphone) and the
converter format and stream. All converters listen to the one stereo stream, so
a speaker on any connected jack is audible; Linux would leave the surround
converters silent when playing two channels. If no pin is classified, the
earlier single-route search (`hda_route_find`) still decides.

Auto-mute follows `hda_generic.c`: a plugged headphone switches the line-out and
speaker pins off (OUT_EN cleared), a plugged line-out switches the speakers
off, and a jack that cannot detect (Pin Capabilities without presence detect,
or the Configuration Default's "Jack Detect Override" set) never mutes
anything. Unsolicited responses are not enabled, because `codec_cmd` takes every
RIRB entry for the answer to its own verb. Instead the jacks are re-read at most
every 250 ms by the audio worker (`snd_device.housekeeping`, thread context)
and whenever the hardware panel refreshes. A floating presence line reads as
"plugged" exactly as it does under Linux; the panel shows it
(`hp@1b:plugged:on line@14:empty:off`).

A GPU's HDMI/DP audio function is a second HD Audio controller. The
`hda-display` driver (same file) binds it only far enough to read its codec's
vendor ID and count its pins and sinks, then resets and releases it. It never
sets up a stream: HDMI audio needs the display engine to packetise it and
publish the sink's ELD, which is a later graphics stage ("GPU stage A").

With `HWHUD=1` the panel carries one set of rows per HD Audio function:
`hda 00:1b.0 codec 10ec:0887 pins line@14:empty:on ...`, a stream row (running,
LPIB advancing, bytes played, interrupts, last codec error, tone state) and
`hda 01:00.1 hdmi codec 1002:aa01 present, output needs GPU stage A`. Also only
in `HWHUD=1` builds, a one-second 440 Hz tone at one quarter of full scale plays
once, five seconds after the first panel tick, through the ordinary stream and
mixer path (`c/kernel/audio/boot_test_tone.c`); the log lines start with
`[hda] test tone`. A boot module named `NOSOUND` (a file in `\LOGIT\FW\` listed in
`INDEX.TXT`) silences it.

## Validation

`make BUILD=build/drivers/audio/check test-audio-cards-host` runs production
controller tests and the existing HDA/PCM regressions. Negative controls are
prerequisites, not optional companion targets. `make test-hw-hda-host` adds the
ALC887 board model, the planner, the panel rows and the tone, and
`make test-hw-hda-guest` plays the tone through QEMU's emulated card and checks
the captured WAV (`tests/hw-hda.mk`).

The capture framework gate also exercises registration, close/reopen, stale
interrupt rejection and skipping an in-progress DMA slot after a ring overrun.
The capture guest harness under `tests/drivers/audio/capture/` injects synthetic
PCM through QEMU's external D-Bus audio backend and compares the WAV files made
by the guest `rec` application with the supplied samples. It does not use the
host microphone or write fake samples into the kernel's DMA buffers.

`test-audio-cards-oracle` verifies the captured-audio checker independently.
`test-audio-cards-os` boots HDA, AC'97 and ES1370 separately, drives the existing
ring-3 `sndtest` program, and checks the WAV output produced by QEMU. Device
binding, an advancing position register, and successful writes alone are not
proof of sound output.

QEMU documents its [PC sound device models](https://www.qemu.org/docs/master/system/i386/pc.html).
Captured samples validate those emulated controllers; physical cards still need
boot, interrupt and audible-output validation on the target machine.
