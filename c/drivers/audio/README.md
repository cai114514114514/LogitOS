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

## Validation

`make BUILD=build/drivers/audio/check test-audio-cards-host` runs production
controller tests and the existing HDA/PCM regressions. Negative controls are
prerequisites, not optional companion targets.

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
