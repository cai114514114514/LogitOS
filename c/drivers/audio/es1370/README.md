# AudioPCI ES1370 playback and capture

The driver matches **Ensoniq 1274:5000 only**. ES1371 and Creative CT5880 use a
different codec/sample-rate interface and are not claimed. `DRIVER_DECLARE`
registers it with the PCI device model. Successful probe registers a real
`snd_device` and calls `snd_init`; start publishes a DMA ring to DAC2, and its
IRQ handler acknowledges the hardware and notifies `snd_period_elapsed`.
ADC capture registers `es1370-in` through `snd_register_capture_device` and
publishes completed input periods through `snd_capture_period_elapsed`. If
another card owns playback, ES1370 can register capture alone without
reinitializing that card's mixer. Capture allocation or registration failure
does not discard usable ES1370 playback.

## Format and resources

- DAC2 and ADC each have an independent signed 16-bit stereo DMA ring of eight
  4096-byte periods. The input ring never aliases the output or phantom ring.
- The hardware divider is 27. Its actual integer rate is **48,662 Hz**, derived
  from 1,411,200 / (27 + 2). The sound framework sees this actual rate and
  resamples a 48 kHz playback application. ADC and DAC2 share this divider;
  recorded PCM is reported at 48,662 Hz. Reporting 48,000 would cause drift.
- Coherent DMA uses an explicit 32-bit mask; CPU pointers are never published
  as device addresses. The whole ring must fit below 4 GiB. A separate page is
  retained for the ES1370 phantom-channel DMA workaround used by Linux.
- The I/O BAR must cover all 64 used register bytes; the complete enumerated
  power-of-two aperture is validated. QEMU's aperture is 256 bytes, not 64.
- The AK4531 codec is reset through its 16-bit write interface, clocked from
  LRCLK2 and routed to stereo voice output. Capture selects the microphone
  input at 0 dB, with the 30 dB microphone boost disabled and microphone
  sidetone excluded from playback. Other analogue inputs remain muted.
  Its register interface is not AC97. MIDI, gameport and runtime hardware
  mixer/source selection are not exposed.

The physical AK4531 microphone source is mono and is routed into both ADC
channels. This does not claim independent stereo line-in support. QEMU does
not apply the analogue codec's routing or gain to its audio backend: stereo
backend input at 48,662 Hz reaches the ADC ring unchanged, allowing independent
left/right DMA validation without proving the physical analogue route.

## Interrupt and teardown contract

The banked DAC2/ADC frame-address/size window is always selected under the card's
IRQ-safe lock. IRQ acknowledgement toggles each pending SERIAL interrupt enable;
STATUS is not a write-one-to-clear register. A single pending status bit may
represent multiple periods: the ISR reads the current frame counter, tracks
its bounded progress and reports each completed period. The public position
is a coarse monotonic completed-frame count, not a wrapping ring offset.

Zero progress with a pending interrupt, invalid counters, a backwards clock,
or a service gap of at least one complete ring duration cannot establish how
many laps occurred. These cases stop publication and quarantine the card
rather than advancing the mixer by a guessed number of periods.

Normal stop disables only the selected engine and its interrupt, confirms the
control readback, and completes that ring's DMA token. The other engine keeps
its clock, ring position, interrupt and PCI bus mastering. Only the final
engine's stop disables bus mastering and completes the shared phantom token.
A fault, including a failed second engine start, immediately quarantines the
whole card because both directions share the interrupt handler and PCI state.
Remove detaches playback and capture consumers, stops both engines, drains the
IRQ and disables PCI decode before freeing buffers. Any uncertain stop or IRQ release
retains the context and allocations. It suppresses PCI INTx and attempts to
clear bus mastering; a still-delivered fault IRQ repeats the mask attempt.

Quarantine is retained separately by PCI BDF because the device framework
clears `drvdata` after failed probe/remove. Sixteen static tombstones prevent
the same function from being rebound over unresolved DMA/IRQ state; if that
table fills, additional ES1370 bindings are refused. There is no reset or
quarantine-clear API. Even a new `struct device` at the same BDF is refused.

## Evidence and primary references

Register semantics, codec sequencing and the phantom buffer follow Linux
v6.12 [`ens1370.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ens1370.c),
[`ak4531_codec.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ak4531_codec.c)
and [`ak4531_codec.h`](https://github.com/torvalds/linux/blob/v6.12/include/sound/ak4531_codec.h).
The independent emulator reference is QEMU v9.2.0
[`hw/audio/es1370.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/audio/es1370.c).
No source from those drivers is compiled into LogitOS.

The implementation separates PCI lifetime (`driver.c`), register/DMA safety
(`common.c`), AK4531 setup (`codec.c`), shared engine/IRQ transitions (`engine.c`)
and sound-framework callbacks (`pcm.c`). All use the private
`es1370_internal.h`; `es1370.h` remains the PCI probe/remove interface.

`tests/drivers/audio/es1370/test.mk` links these production files with only
kernel service/port-I/O substitutes. Its literal hardware model consumes the
programmed DAC2 physical address and writes external literal input samples
through the programmed ADC physical address. The capture consumer checks
every byte across three ring laps. Cases cover coalesced/simultaneous IRQs,
independent stop/restart in full duplex, capture-only registration, resource
failures, ambiguous progress and failed-remove/failed-probe rebinding.
The prerequisite playback mutation removes mixer period notifications and must
fail six assertions; a second mutation removes capture notifications and must
fail four assertions despite valid ADC DMA writes. Address and undefined-behavior
sanitizers are enabled for both negative controls and the positive run.

The integrated QEMU guest gate additionally captures the real emulator's WAV
output from a 48 kHz application and checks its resampled duration, 200 Hz
signal, channel direction and antiphase samples. The capture guest gate supplies
external deterministic PCM through QEMU's D-Bus AudioInListener and compares
the recorded PCM against the injected bytes, including recording while playback
runs. QEMU validates the implemented digital transport and framework paths;
analogue input/output on a physical ES1370/AK4531 board remains unverified.

```sh
make -f tests/drivers/audio/es1370/test.mk \
  BUILD=build/drivers/audio/capture-es1370 CC=clang test-es1370-host
```
