# Audio drivers

LogitOS implements HDA, Intel ICH AC97, and Ensoniq ES1370 controllers under `c/drivers/audio/`. These drivers manage PCI resources, codec routing, DMA rings, and interrupts. The kernel audio layer connects application streams to the hardware through `c/kernel/audio/snd.h`; playback conversion, resampling, and mixing live in `c/kernel/audio/mixer.c` and `c/kernel/audio/pcm.c`.

## Controller families

All three drivers publish stereo PCM with signed samples of 16 bits. Each direction uses eight periods of 4,096 bytes, giving 1,024 stereo frames per period and a 32 KiB PCM ring.

| Family | PCI match | Register access | Playback engine | Capture engine | Native rate |
| --- | --- | --- | --- | --- | --- |
| HDA | Class `04:03`, with controller selection during probe | Memory mapped BAR0 | First output stream descriptor | First input stream descriptor | 48,000 Hz |
| AC97 | Intel `8086:2415`, `8086:2425`, `8086:2445`, `8086:2485` | Codec and bus master I/O BARs | PCM output | PCM input | 48,000 Hz |
| ES1370 | Ensoniq `1274:5000` | I/O BAR0 | DAC2 | ADC | 48,662 Hz |

HDA geometry is defined in `c/drivers/audio/hda.c`. AC97 identities and geometry come from `c/drivers/audio/ac97/ac97_models.h` and `c/drivers/audio/ac97/ac97_regs.h`. ES1370 defines its rate in `c/drivers/audio/es1370/registers.h` and geometry in `c/drivers/audio/es1370/es1370_internal.h`.

The drivers register through `DRIVER_DECLARE` and request interrupts through `dev_irq_request()`, declared in `c/drivers/core/driver.h`.

## Streams and period delivery

A playback driver supplies a `struct snd_device` containing its format, ring geometry, and start, stop, and position callbacks. Capture uses `struct snd_capdevice`: hardware writes the DMA ring, and the kernel copies completed periods into the application stream’s queue.

`snd_register_device()` selects the first successful playback registration. `snd_register_capture_device()` selects input through its own registry. AC97 and ES1370 can supply capture when another controller already owns playback, allowing HDA output with either card’s input. These registries live in `c/kernel/audio/mixer.c` and `c/kernel/audio/capture.c`.

Application playback queues retain samples in the application’s format. The `kaudio` worker converts them to signed PCM with 16 bits per sample, resamples to the selected device rate, and mixes streams with saturation. The `kcapture` worker copies input periods into its consumer’s queue. `c/kernel/audio/snd.c` handles audio syscall dispatch and user memory copying for the ABI in `include/abi/logit_abi.h`.

Playback startup checks scheduler availability and clears the hardware ring before calling the driver’s start callback. Opening a stream retries startup after probing during boot. Capture starts during stream open and stops when the worker reaps a closed stream.

Drivers call `snd_period_elapsed()` and `snd_capture_period_elapsed()` after acknowledging completions. These functions update completion counters and post semaphores to wake the workers. Playback fills future periods beyond the slot identified by its completion count. Capture skips periods that DMA has overwritten and excludes the slot currently being written. When the application capture queue fills, it retains queued bytes and drops incoming bytes, counting the loss as an overrun.

Playback devices can supply a housekeeping callback. `c/kernel/audio/mixer.c` invokes it after every two worker passes with an active engine, using `SND_HOUSEKEEPING_PERIODS` from `c/kernel/audio/snd.h`. It executes in thread context under the mixer lock, with interrupts disabled. HDA uses this callback to sample playback progress and poll output jacks.

## HDA

`c/drivers/audio/hda.c` manages both codec commands and PCM DMA. Commands travel through the Command Output Ring Buffer, CORB; responses return through the Response Input Ring Buffer, RIRB. PCM streams consume buffer descriptor lists, or BDLs. Converter stream tags connect the codec route to the DMA engine, so diagnosing output requires examining both the stream registers and the widget graph.

Probe first enables memory decoding with PCI bus mastering disabled. It stops inherited command rings and stream engines, checks controller reset, and allocates coherent CORB and RIRB storage. Controller capabilities select a DMA address mask of 32 or 64 bits. Setup of the command ring chooses the largest advertised size among 256, 16, and 2 entries, checks the selection, and publishes ring addresses before enabling bus mastering.

Controller selection examines the PCI inventory. An audio function sharing a slot with a display controller is classified as display audio; `10de:0fb9` also receives that classification when its display sibling is missing. Intel `8086:1c20`, `8086:1d20`, and `8086:1e20` at segment zero, bus zero, slot `1b`, function zero receive preference over other analog candidates.

Discovery selects the lowest codec address reported present after reset and walks its audio function groups. After confirming an audio function group has reached D0, it enumerates widgets and their capabilities. Output connection lists support short entries, long entries, and range expansion.

The earlier page described a direct search from pin to DAC, followed by a fallback that paired the first DAC and output pin. The current output search follows graph edges, including paths through one mixer or selector. `c/drivers/audio/hda_output_paths.c` collects line outputs, speakers, and headphone pins, orders them by role, association, sequence, and node identity, then assigns reachable analog DACs. Line outputs with a fixed function are classified as speakers. Three or four line outputs receive a surround and center/LFE ordering adjustment. Unused DACs are assigned first; further paths can share a converter from an earlier path. If this planner produces no paths, `c/drivers/audio/hda_route.c` searches for one reachable output route.

Route setup requests and confirms D0 for widgets that support power, programs converter format and stream tag, selects connections, configures amplifiers, and enables the output pin. Amplifier gain comes from the advertised step of zero dB, clamped to the supported range. The selected mixer input is unmuted, and other mixer inputs with mute support are muted. Each configured converter receives the same stereo stream starting at channel zero. The first path must configure successfully for probe to continue; failures on later paths are recorded.

Capture discovery follows an ADC’s connections to an input pin, directly or through one selector or mixer. Playback and capture receive their own PCM rings and BDLs. Each BDL entry requests an interrupt on completion. Engine startup updates the shared global interrupt control register under the HDA gate, writing the other direction’s enable bit along with its own. The interrupt handler checks and acknowledges both stream sources.

The previous position callback returned a wrapping ring offset. It now folds changes in link position into an accumulated byte count and reports frames consumed since playback start. Housekeeping and hardware status reads also sample that register.

## HDA jacks and diagnostics

Jack sensing controls output pin enables. A plugged headphone mutes line outputs and speakers; a plugged line output mutes speakers. Presence detection is used when the pin advertises it and firmware marks the presence line as usable. Pins requiring a triggered measurement receive that command before the sense read.

`c/drivers/audio/hda_output_paths.c` computes this auto-mute policy. `c/drivers/audio/hda.c` applies pin controls and reads them back for diagnostics. Worker housekeeping and hardware status reads request jack refreshes, subject to a 250 ms minimum polling interval. Setup also performs an initial forced poll.

A failed sense or pin-control transaction disables subsequent jack polling and starts an attempt to enable every usable output. The recovery stops at the next transaction failure.

The `hda-display` driver in `c/drivers/audio/hda.c` uses another controller context to read a GPU audio codec’s identity and survey pin presence and ELD validity. It then runs the controller cleanup path. Records retained for `c/drivers/audio/hda_status.h` allow the hardware panel to report codec identity and probe outcome.

With `HWHUD=1`, `c/kernel/diag/hwhud.c` displays controller identity, output pin roles, jack state, pin-enable readback, stream activity, accumulated bytes, completion interrupt count, interrupt mode, and the last codec error. Reading analog controller status also requests a jack refresh.

That build setting enables the boot tone in `c/kernel/audio/boot_test_tone.c`. At a panel update at least five seconds after its first tone poll, the panel schedules a kernel thread. The thread requests suspension of HDA auto-mute, waits 300 ms, then opens a playback stream and queues a 440 Hz stereo tone lasting one second. Its amplitude parameter is 8,192, with 10 ms fades. Auto-mute resumes after the playback attempt. The log records frames queued, reported device-position progress, and stream underruns. A boot module named `NOSOUND` suppresses the tone.

## AC97

The AC97 implementation divides controller operations, codec access, capture routing, stream ownership, PCI lifecycle, and native port access among files in `c/drivers/audio/ac97/`.

`c/drivers/audio/ac97/ac97_models.h` provides the inventory used for PCI matching and initialization profiles. ICH and ICH0 use fixed stereo engines. ICH2 and ICH3 initialization clears and checks the output channel-selection field because firmware can leave four or six channels selected. Stereo configuration keeps hardware sample grouping aligned with the PCM ring.

`c/drivers/audio/ac97/pci.c` validates PCI identity and both I/O resources before accessing audio registers. Initialization stops PCM input, PCM output, and the mono microphone engine, resets the link, and prepares the codec for 48 kHz playback. Codec transactions wait for the controller semaphore; reads also check timeout status.

The 32-entry descriptor list repeats the eight PCM periods four times. Descriptor lengths count individual samples of 16 bits: one 4,096-byte period holds 2,048 samples and 1,024 stereo frames. `ac97_build_descriptors()` in `c/drivers/audio/ac97/controller.c` encodes that unit.

Interrupt handling derives completed periods from the current descriptor index and terminal status, then advances the last valid index. Extending that index resumes a halted engine whose RUN bit remains set. Position accounting handles the interval between completion of a terminal descriptor and fetching its successor.

Capture uses the PCM input engine. `c/drivers/audio/ac97/capture.c` checks ADC readiness, routes the microphone to both input channels, and applies a recording gain code representing +12 dB. Microphone monitoring through the playback mixer is muted to prevent feedback.

`c/drivers/audio/ac97/streams.c` maintains controller state, PCM storage, descriptors, and DMA ownership tokens for each direction. One interrupt callback services both engines and issues a DMA read barrier before publishing capture completions.

## ES1370

The ES1370 driver uses DAC2 for playback, the ADC for capture, and an AK4531 codec. `c/drivers/audio/es1370/driver.c` validates PCI identity and the I/O resource, stops inherited engines, configures the codec, allocates buffers within a DMA mask of 32 bits, and registers available directions.

The clock divider is 27. `c/drivers/audio/es1370/registers.h` derives the reported integer rate as `1411200 / (27 + 2)`, giving 48,662 Hz. Both directions publish this rate. A 48 kHz playback stream passes through the shared resampler to match the programmed device rate.

Playback and capture share control registers, serial format state, the divider, and a banked DMA register window. `c/drivers/audio/es1370/engine.c` protects access with the card gate and selects the register page for each engine transaction. Bus mastering continues for the other direction when one stops.

A dedicated phantom DMA buffer supplies the controller’s phantom-channel target. Probe programs its address while bus mastering is disabled. The first active direction submits its ownership token, and stopping the final direction completes it.

Interrupt acknowledgement toggles the pending sources’ serial interrupt enables. Completion accounting reads hardware frame progress and derives the elapsed period count. Invalid counters, insufficient progress for a pending completion, a backwards clock, or a service gap reaching the ring duration trigger quarantine. The time bound protects against ambiguous progress after a complete ring wrap.

## Ports and codec routing

The CPU register interface and the physical signal route serve different purposes:

| Driver | CPU register interface | Configured signal route |
| --- | --- | --- |
| HDA | Volatile accesses through mapped BAR0 | Planned converter paths to output pins; selected input pin to ADC |
| AC97 | Absolute I/O ports through `struct ac97_bus` | PCM playback through master and PCM mixer controls; microphone to both PCM input channels |
| ES1370 | Port I/O with page selection for DMA registers | AK4531 voice playback to both outputs; microphone to both ADC channels |

`c/drivers/audio/ac97/ports.c` supplies native bus callbacks, bounded delays, and the DMA publication barrier. The callback interface lets host tests substitute a register model.

ES1370 port helpers live in `c/drivers/audio/es1370/common.c`, with AK4531 programming in `c/drivers/audio/es1370/codec.c`. HDA register access and route execution live in `c/drivers/audio/hda.c`; its route and output-path modules provide graph search, command generation, and auto-mute policy.

## DMA lifetime and removal

Coherent allocations carry a CPU pointer and a device address. Drivers program hardware from the allocation’s device address, submit ownership before starting DMA, and complete ownership after confirming hardware stop. `c/drivers/core/dma.h` and `c/drivers/core/dma.c` define these contracts.

Removal coordinates audio workers, interrupt callbacks, and controller DMA. The framework’s `dev_unbind()` drains interrupt delivery before calling the driver’s removal callback. HDA also checks interrupt release in its removal path to support direct diagnostic removal. AC97 and ES1370 teardown helpers detach audio consumers before stopping engines and releasing their interrupt.

When shutdown or interrupt release remains unconfirmed, teardown retains affected context or DMA storage. Quarantine blocks new submissions and preserves memory hardware may still access. HDA retains DMA from failed probes in a persistent ledger. ES1370 records quarantined PCI function identities to reject rebinding over unresolved state.

The power-transition path in `c/kernel/init/power.c` calls `dev_quiesce_all()` in `c/drivers/core/device.c`. Audio controllers pass through unbinding and removal, followed by PCI bus-master readback. The power abort path calls device probing again.

## Validation paths

These targets define controller, framework, and guest sample checks:

| Make target | Defined coverage |
| --- | --- |
| `test-audio-cards-host` | PCM primitives, HDA controller tests, AC97, ES1370, and playback and capture framework checks |
| `test-hda-x79-host` | Controller selection, inherited DMA shutdown, command-ring size negotiation, and probe/removal containment |
| `test-audio-cards-os` | QEMU playback through all three controller families and captured WAV inspection |
| `test-audio-capture-os` | PCM injection through QEMU’s D-Bus audio backend, recording, reopen behavior, and concurrent playback |
| `test-audio-multicard-os` | HDA output with AC97 or ES1370 input, including different native rates |

Definitions are in `tests/drivers/audio/tests.mk`, its included fragments, and `tests/hda_x79.mk`. The host targets attach mutation controls as prerequisites. Examples include incorrect AC97 descriptor lengths, discarded ES1370 completion notifications, and framework access to an active DMA slot.

`tests/unit/hda_x79_run.py` instruments the production HDA source for the PCI, MMIO, and DMA model in `tests/unit/hda_x79_test.c`.

`tests/drivers/audio/guest/oracle.py` checks playback duration, amplitude, channel polarity, sample progression, and cycle spacing, accounting for ES1370 resampling. `tests/drivers/audio/capture/oracle.py` compares recordings with injected samples inside backend enable intervals and checks playback during capture.

## Limitations

- The framework selects one playback device and one capture device. Runtime device selection is absent. Capture accepts one application stream in the native format and provides no resampling.
- HDA uses one analog controller instance and examines the first codec reported present. Output planning supports eight paths, sixteen expanded connections per widget, and one intermediate mixer or selector per path.
- HDA capture retains a short-form connection reader with eight entries and searches up to two connections deep. Its setup writes fixed amplifier values, writes zero to the input VREF bits, and ignores failures of individual setup verbs. Microphones requiring bias need further configuration.
- HDA publishes one notification per observed completion bit. Delayed service can merge completions. Its accumulated position also loses whole revolutions when sampling gaps reach the ring duration, about 171 ms.
- When HDA interrupt allocation fails, audio devices stay registered while refills need completion notifications. The diagnostic label `polled` denotes missing interrupt wiring; the driver provides no polling fallback for period delivery.
- HDA jack changes use polling with unsolicited codec responses disabled. Recovery after a polling failure is a bounded attempt; further transaction failures can leave outputs muted.
- GPU HDMI/DP audio receives codec and sink-status surveys. Playback requires further configuration of the display engine and sink capabilities.
- AC97 matching covers the four Intel profiles listed above. ICH4 and later controllers, 440MX, and other vendors need additional handling. ES1371 and later AudioPCI variants use a different codec and rate interface.
- These drivers provide neither USB Audio nor VirtIO Sound. QEMU sample checks exercise emulated controllers. Physical cards require interrupt, routing, playback, and capture validation on the target machine, as described in `c/drivers/audio/README.md`.

## See also

- [Kernel audio layer](../kernel/audio.md)
- [Device and driver model](../architecture/device-model.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
| --- | --- |
| `c/drivers/audio/hda.c` | HDA probing, command rings, codec discovery, route execution, jack polling, DMA, interrupts, diagnostics, and removal |
| `c/drivers/audio/hda_route.h` | Widget, route, and command structures and graph bounds |
| `c/drivers/audio/hda_route.c` | Route search and capability-based codec command generation |
| `c/drivers/audio/hda_output_paths.h` | Output plans, pin roles, jack states, and planner interfaces |
| `c/drivers/audio/hda_output_paths.c` | Pin classification, DAC assignment, ordering, and auto-mute policy |
| `c/drivers/audio/hda_status.h` | Controller diagnostics and auto-mute suspension interface |
| `c/drivers/audio/ac97/ac97_models.h` | Intel controller inventory and initialization profiles |
| `c/drivers/audio/ac97/ac97_regs.h` | Registers, PCM geometry, and codec constants |
| `c/drivers/audio/ac97/ac97.h` | Bus callbacks and controller interfaces |
| `c/drivers/audio/ac97/controller.c` | Reset, descriptors, completion counting, and position accounting |
| `c/drivers/audio/ac97/codec.c` | Codec transactions and playback configuration |
| `c/drivers/audio/ac97/capture.c` | ADC readiness, microphone routing, and recording gain |
| `c/drivers/audio/ac97/streams.c` | DMA ownership, audio callbacks, and interrupt dispatch |
| `c/drivers/audio/ac97/ports.c` | Native port callbacks, delays, and publication barrier |
| `c/drivers/audio/ac97/pci.c` | Resource validation, registration, containment, and removal |
| `c/drivers/audio/es1370/registers.h` | ES1370 and AK4531 registers and native rate |
| `c/drivers/audio/es1370/es1370_internal.h` | Ring geometry and shared card state |
| `c/drivers/audio/es1370/driver.c` | Probe, allocation, registration, and teardown |
| `c/drivers/audio/es1370/engine.c` | DMA programming, engine transitions, and completion checks |
| `c/drivers/audio/es1370/codec.c` | AK4531 reset, gain, and routing |
| `c/drivers/audio/es1370/common.c` | Port helpers, bounded polling, and quarantine records |
| `c/drivers/core/driver.h` | Driver registration and interrupt interfaces |
| `c/drivers/core/device.c` | Unbinding, resource control, and power-transition quiescing |
| `c/drivers/core/dma.h` | DMA allocation and ownership contracts |
| `c/drivers/core/dma.c` | Coherent allocation, ownership transitions, and quarantine |
| `c/kernel/audio/snd.h` | Device, stream, and housekeeping interfaces |
| `c/kernel/audio/mixer.c` | Playback registration, mixing worker, and housekeeping dispatch |
| `c/kernel/audio/capture.c` | Capture registration, queue, worker, and close/reopen lifecycle |
| `c/kernel/audio/pcm.c` | Conversion, resampling, saturation, and byte queues |
| `c/kernel/audio/snd.c` | Audio syscall dispatch and user memory copying |
| `c/kernel/audio/boot_test_tone.h` | Tone parameters and diagnostic states |
| `c/kernel/audio/boot_test_tone.c` | Tone generation, scheduling, playback, and progress logging |
| `c/kernel/diag/hwhud.c` | HDA status rows and boot tone polling |
| `c/kernel/init/power.c` | Device quiescing and probing after an aborted transition |
| `include/abi/logit_abi.h` | Audio syscall numbers, formats, and status structures |
| `Makefile` | Hardware-panel build setting |
| `tests/drivers/audio/tests.mk` | Aggregate host and playback guest targets |
| `tests/drivers/audio/ac97/test.mk` | AC97 host checks and mutation prerequisites |
| `tests/drivers/audio/es1370/test.mk` | ES1370 host checks and mutation prerequisites |
| `tests/drivers/audio/framework/tests.mk` | Capture framework checks and mutation prerequisites |
| `tests/drivers/audio/playback/tests.mk` | Playback framework checks and mutation prerequisites |
| `tests/drivers/audio/capture/tests.mk` | Capture and multiple-card guest targets |
| `tests/hda_x79.mk` | HDA controller-model target and mutation prerequisite |
| `tests/unit/hda_x79_run.py` | Production-source instrumentation and host-model runner |
| `tests/unit/hda_x79_test.c` | HDA PCI, MMIO, DMA, and teardown model |
| `tests/drivers/audio/guest/oracle.py` | Playback sample checker |
| `tests/drivers/audio/capture/oracle.py` | Capture sample, lifecycle, and concurrent-playback checker |
| `c/drivers/audio/README.md` | Driver scope and physical hardware validation requirements |
