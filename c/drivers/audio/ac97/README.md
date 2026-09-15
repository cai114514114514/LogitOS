# Intel ICH AC'97 playback and capture

The original implementation matched only **8086:2415**, QEMU's `AC97` device.
The I/O driver now accepts the following precisely scoped Intel profiles:

| PCI vendor:device | Intel controller | Initialization difference | Evidence |
| --- | --- | --- | --- |
| 8086:2415 | 82801AA ICH | Fixed stereo; no multichannel field modified | Host DMA model and QEMU audio input/output |
| 8086:2425 | 82801AB ICH0 | Same fixed stereo engines as ICH | Intel manual and host DMA model |
| 8086:2445 | 82801BA/BAM ICH2 | Clear GLOB_CNT bits 21:20 to stereo and confirm readback | Intel datasheet and host DMA model |
| 8086:2485 | 82801CA ICH3 | Same channel-selection field as ICH2; confirm stereo | Intel datasheet and host DMA model |

All four use the three original I/O engines, DMA32 BDL entries and 16-bit
sample counts. ICH2/3 firmware can leave the PCM engine in four- or six-channel
mode, so accepting the ID without changing that state would consume stereo
samples with the wrong channel grouping. A rejected mode write now fails probe
within the bounded reset poll. Bit 22 is reserved on these profiles and is
preserved; the earlier generic code incorrectly treated it as ICH4's 20-bit
selection bit. `ac97_models.h` supplies one model inventory for both matching
and controller initialization, preventing the two lists from drifting.

These are not claims of physical-board validation. In particular, **ICH4 and
later IDs remain rejected**: those require stopping additional engines and
handling SDIN steering and their MMIO-capable register resources. Intel 440MX
also remains rejected because Linux documents an uncached-DMA erratum. VIA,
SiS, NVIDIA and unknown controllers are not admitted by an audio-class match.
A stale PCI identity, a mismatched I/O BAR or an incompatible resource shape
still fails before accessing audio ports.

## File responsibilities

| File | Responsibility |
| --- | --- |
| `ac97_models.h`, `models.c` | Exact Intel identities and supported register profiles |
| `controller.c` | Link initialization, shared engine protocol, descriptors, interrupt progress and frame position |
| `codec.c` | Serialized codec access and playback configuration |
| `capture.c` | ADC readiness, fixed input rate, microphone routing and recording gain |
| `streams.c` | Separate input/output DMA ownership, sound callbacks and shared IRQ dispatch |
| `pci.c` | PCI validation, binding, shared containment and teardown |
| `ports.c` | Native port I/O, bounded delays and the DMA publication barrier |
| `ac97_regs.h` | Controller and codec register definitions |
| `ac97.h` | Controller state, bus interface and public entry points |

The controller's bus interface permits host tests to exercise the production
code against a separate register model. Hardware access uses absolute I/O
ports. Calls are serialized by the PCI owner's IRQ-safe gate.

## Playback path

Probe validates the PCI identity and two I/O BARs, disables inherited bus
mastering, stops firmware-owned engines, resets the AC-link, and waits for the
primary codec and analogue output to become ready. Variable-rate codecs have
VRA disabled for fixed 48 kHz output. Master and PCM output volume are unmuted.
The driver must obtain an IRQ before registering with the existing sound mixer.

Playback is signed 16-bit stereo at **48,000 Hz**. Eight 4096-byte PCM periods
are represented by 32 buffer-list descriptors, repeating the ring four times.
Descriptor length counts individual 16-bit samples: one period contains 2048
samples or 1024 stereo frames. Coherent allocations and descriptor addresses
are constrained to DMA32; a CPU pointer is not used as a bus address.

Interrupt handling acknowledges status and uses the hardware's current index
to count completed periods, including coalesced interrupts. Extending the last
valid index resumes a halted RUN engine. Rewriting RUN at that point can skip
a prefetched descriptor. Position sampling also handles the interval where the
terminal descriptor has been counted but its successor has not been fetched.

The sound mixer fills PCM and converts application formats. The earlier
playback-only implementation did not expose capture; the input path described
below now feeds the existing kernel recording interface. No new application
ABI is introduced. One statically addressed card slot keeps callback addresses
stable. Failed teardown retains
the slot and any unresolved DMA allocations rather than permitting re-probe
to overwrite them. Runtime device switching is not supplied by this driver.

## Capture and simultaneous playback

A second DMA32 ring and descriptor list feed the ICH **PCM input** engine at
bus-master offset `0x00`; the playback engine remains at `0x10`. The dedicated
mono MIC engine at `0x20` stays stopped. Both directions deliver signed 16-bit
stereo at 48 kHz, with eight 4096-byte ring periods, but each has its own current
index, completion counters, DMA cookies and start/stop callbacks.

The primary codec must report ADC and analogue readiness. The disabled VRA
mode is checked against the LR ADC rate on variable-rate codecs. Record Select
routes microphone to both ADC channels; Record Gain is `0x0808`, AC'97's +12 dB
per channel. Microphone monitoring into the output mixer stays muted to avoid
speaker feedback. QEMU interprets this gain as volume 136/255, rather than
physical analogue gain; using zero here would discard all emulator input.
No runtime source selector, microphone boost control or jack detection is
provided. A physical board still needs verification of its codec wiring.

The driver registers `ac97-in` through `snd_register_capture_device`, and only
starts input DMA when the kernel capture consumer calls its start callback.
A shared interrupt checks both independently running engines. A DMA read
barrier precedes each batch of capture notifications; the device retains
ownership while the kernel copies completed periods. The kernel capture layer
handles falling behind the ring, rather than the driver inventing old samples.

Stopping one healthy direction leaves the other running. If either stop or
DMA completion fails, both states become poisoned, PCI bus mastering is
contained, and all unresolved allocations remain quarantined. A late IRQ
retries PCI-only containment without accessing disabled I/O ports. Removal
unregisters both consumers and drains the shared IRQ before freeing either
ring. ADC initialization failure leaves playback usable and capture absent.

## Capture when another card owns playback

The first registered playback device and the first registered capture device
are independent choices. If HDA or another card already owns playback, AC'97
still attempts to register its ADC, binds successfully when capture succeeds,
and leaves its own output engine stopped. It does not call `snd_init` in this
case: that routine belongs to the successful playback registration and must
not reset the other card's mixer. Capture registration initializes its own
framework through the existing capture API.

If both directions are rejected, probe releases this card's resources and
leaves both existing owners untouched. If only capture is accepted, removal
unregisters only that consumer. The unused AC'97 playback allocation remains
owned by this card until teardown; it is never submitted to hardware.

## Validation

`make BUILD=build/drivers/audio/expand-ac97 CC=clang test-ac97-host` exercises the actual
controller and PCI integration with host substitutes for hardware and kernel
services. Required descriptor-length, terminal-position, capture-notification,
stereo-mode-readback and other-playback-reinitialization mutation controls must fail before the positive suite can pass. The literal
host ADC writes distinct signed samples into DMA-addressed input buffers; the
capture notification consumer checks every sample across ring wraps and shared
interrupts. Host cases also cover independent stops, absent input routing,
registration rejection and shared containment after capture failures. The integrated `tests/drivers/audio/guest.py --device AC97` gate
additionally checks QEMU's captured PCM from the existing ring-3 `sndtest`.
Each accepted Intel profile also runs a complete modeled playback/capture
cycle. The capture-only test installs a preexisting playback-owner fixture,
records real modeled ADC bytes and verifies that no playback initialization or
foreign-device unregister occurred. Host positives are saved as `host.log` and
`host-complete.log` under `$(BUILD)/audio/ac97/`; every mutation has its own raw
`*-negative.log` (the original length control uses `negative.log`). None of
these host results proves analogue output on a physical motherboard.

Primary sources for model differences and register behavior:

- [Intel ICH/ICH0 AC'97 Programmer's Reference Manual 298028-001](https://www.alsa-project.org/files/pub/manuals/intel/29802801_801_AC97.pdf), section 2.2: three audio engines and stereo PCM.
- [Intel ICH2/ICH2-M Datasheet 290687-002](https://www.intel.com/content/dam/doc/datasheet/82801ba-i-o-controller-hub-2-82801bam-i-o-controller-hub-2-mobile-datasheet.pdf), sections 13.1.2 and 13.2.8: identity and channel-selection bits.
- [Intel ICH3-S Datasheet 290733-002](https://www.intel.com/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf), sections 13.1.2 and 13.2.8: identity and channel-selection bits.

The Linux table independently groups these four models as `DEVICE_INTEL`,
and distinguishes `DEVICE_INTEL_ICH4`; it also describes the 440MX DMA erratum:

[Linux v6.12 intel8x0](https://github.com/torvalds/linux/blob/v6.12/sound/pci/intel8x0.c),
[QEMU AC'97 controller](https://github.com/qemu/qemu/blob/v9.2.0/hw/audio/ac97.c).
