# Kernel audio layer

The kernel audio layer connects application PCM streams to sound controller DMA rings. The `kaudio` worker converts, resamples and mixes playback streams; `kcapture` copies completed input periods into a capture stream. Drivers provide buffers, engine callbacks and completion notifications through `c/kernel/audio/snd.h`. Applications decode compressed audio before submitting PCM, as illustrated by `c/apps/gui/preview/preview.c`.

## Data flow

Playback passes through an application stream ring, format conversion, resampling and mixing before reaching the driver’s DMA ring. Capture flows from the controller’s DMA ring into a consumer ring, which application reads drain.

Driver interrupts report completed periods. The audio layer advances completion counters and posts semaphores; workers wake to process the buffers and notify waiting applications. This ties processing to hardware progress and lets the workers sleep between periods. The playback scheduling rationale is documented in `c/kernel/audio/mixer.c`.

Buffer ownership defines the driver interface. The mixer writes playback DMA periods, while the capture worker reads completed input periods. Drivers allocate the DMA memory and control the hardware that consumes or produces it.

## PCM formats and conversion

A PCM frame contains one sample per channel. `struct logit_sndfmt` in `include/abi/logit_abi.h` describes the sample rate, interleaved channel count, sample format, requested buffer duration and flags.

Playback accepts rates from 4,000 through 192,000 Hz and one through eight source channels. Stream opening also checks the application rate against the device rate and the mixer’s conversion scratch capacity.

`c/kernel/audio/pcm.c` converts application samples to signed 16-bit PCM:

| Format | Conversion |
|---|---|
| `SND_FMT_S16` | Copies signed 16-bit samples |
| `SND_FMT_U8` | Subtracts 128 and scales by 256 |
| `SND_FMT_S32` | Takes the upper 16 bits |
| `SND_FMT_F32` | Clamps to −1 through +1, scales by 32,767 and converts; NaN maps to −1 |

`pcm_to_s16()` duplicates mono input across stereo output. For mono output from multiple source channels, it averages the first two channels; stereo output takes that first pair. `pcm_silence()` produces each format’s silence representation, including `0x80` for unsigned 8-bit PCM.

`pcm_resample()` performs linear interpolation with a Q16.16 phase accumulator. Each stream carries its phase and previous input frame across calls. Equal sample rates use a direct copy path. Integer interpolation gives the host tests and kernel the same arithmetic, as explained beside the interface in `c/kernel/audio/snd.h`.

The mixer peeks at source bytes before resampling, then advances the stream ring by the input frames reported as consumed. This preserves input needed by the next period. `pcm_mix_add()` adds each stream through a 32-bit intermediate and clamps each addition to the signed 16-bit range.

## Playback streams

`c/kernel/audio/mixer.c` provides eight playback slots. Each stream has a handle and owner token. The syscall layer supplies the current process pointer, and handle lookup checks both values. Kernel callers use a null owner.

Each stream stores bytes in the application’s format in a `struct pcm_ring`. Buffer duration defaults to 200 ms and is clamped to 20–2,000 ms, with a minimum allocation of 4,096 bytes.

The playback path is:

1. `snd_stream_open()` ensures the engine is available, validates the requested format and allocates a stream ring.
2. `snd_stream_write()` copies the bytes that fit into that ring.
3. The driver reports hardware completions through `snd_period_elapsed()`.
4. `kaudio` converts, resamples and mixes streams into future DMA periods, then wakes waiting writers.

Before mixing, the worker clears the destination period and each stream’s output scratch. A shortage contributes silence for the remaining frames and increments the stream’s underrun counter while it is running. A draining stream follows its completion path when its queued bytes are exhausted.

A write to a full ring waits for up to 500 ms. `SND_F_NONBLOCK` makes that case return zero immediately. Applications advance by the returned byte count and retry any remaining bytes.

Closing with drain requested waits up to three seconds for queued application bytes to be consumed, then marks the stream for reclamation. The worker frees closing streams under the lock used for mixing.

## Refilling the DMA ring

Playback refill scheduling uses the completion notification count in `c/kernel/audio/mixer.c`. With a count of `N`, the worker treats period `N` as the period currently playing and begins refilling at `N + 1`, with indices mapped into the circular DMA buffer.

`SND_FILL_LEAD` sets the target to three periods ahead, capped by the device’s ring geometry. A late worker advances its refill index to the next safe position and adds the skipped positions to the device underrun counter. When the ring has spare capacity, it clears the slot beyond the prepared audio.

The comments preserve an earlier indexing error: the worker once refilled the period under the hardware playback pointer. The corrected calculation places writes ahead of that pointer. The larger lead provides scheduling margin under QEMU TCG.

After servicing a running device, the worker advances its housekeeping counter. `SND_HOUSEKEEPING_PERIODS` in `c/kernel/audio/snd.h` sets the callback interval to two worker passes. With one pass per HDA period, that is about 43 ms. The optional callback runs in thread context while the mixer lock protects the device’s lifetime.

## Capture

`c/kernel/audio/capture.c` receives input through `struct snd_capdevice`. The controller fills its DMA ring, and `kcapture` copies completed periods into the open stream’s byte ring before waking readers.

For `SYS_SND_CAP_OPEN`, setting `rate`, `channels` and `format` all to zero requests the device’s native format. The kernel fills those fields before returning. `c/apps/coreutils/rec.c` uses this request and calculates its recording size from the returned rate.

Capture buffering uses the same default duration, bounds and minimum allocation as playback. An empty read waits for up to 500 ms and can return zero. Closing marks the stream for reclamation; the worker stops the input engine and frees the consumer buffer. A later open reuses the worker and restarts capture.

The worker handles loss at two buffer boundaries:

| Buffer | Handling |
|---|---|
| Hardware DMA ring | Skips overwritten periods and resumes at the oldest completed period still available, excluding the slot being written |
| Capture stream ring | Preserves queued bytes and accepts the portion of new input that fits |

Skipped DMA periods increment the stream’s overrun counter. A partially or wholly rejected period at the consumer ring adds one overrun. The header correction in `c/kernel/audio/capture.c` preserves the older claim that consumer overflow dropped the oldest queued audio, then explains the implemented behavior: `pcm_ring_write()` retains queued bytes and drops new bytes when full.

## Driver integration

Drivers register playback through `snd_register_device()` and capture through `snd_register_capture_device()`. Each direction retains the first accepted registration. AC’97 and ES1370 can supply capture while another controller supplies playback, as shown in `c/drivers/audio/ac97/pci.c` and `c/drivers/audio/es1370/driver.c`.

The contracts in `c/kernel/audio/snd.h` contain:

| Field | Purpose |
|---|---|
| Name and codec description | Device identification and reports |
| Rate, channels and format | Native PCM geometry |
| Period size, period count and ring pointer | CPU access to DMA periods |
| `start` and `stop` callbacks | Engine control |
| Playback `position` callback | Hardware progress reporting |
| Optional playback `housekeeping` callback | Device maintenance from `kaudio` |
| IRQ mode and private pointer | Reporting and driver state |

Playback registration validates ring alignment, frame alignment, geometry bounds and signed 16-bit mono or stereo output. It allocates conversion scratch before publishing the device. Capture registration validates its signed 16-bit geometry and initializes service queues before publication.

Drivers allocate DMA buffers and program controller descriptors. Their interrupt handlers acknowledge hardware status and call `snd_period_elapsed()` or `snd_capture_period_elapsed()` for completed periods. HDA checks both playback and capture status on its shared interrupt in `c/drivers/audio/hda.c`.

The drivers configure stereo signed 16-bit PCM with these geometries:

| Driver | Reported native rate | DMA buffering per direction |
|---|---:|---|
| HDA | 48,000 Hz | Eight periods of 4,096 bytes |
| AC’97 | 48,000 Hz | Eight periods of 4,096 bytes |
| ES1370 | 48,662 Hz | Eight periods of 4,096 bytes |

HDA defines its geometry in `c/drivers/audio/hda.c`; AC’97 uses `c/drivers/audio/ac97/ac97_regs.h`. ES1370 defines its geometry in `c/drivers/audio/es1370/es1370_internal.h` and derives its reported rate from the clock divider in `c/drivers/audio/es1370/registers.h`. This lets playback resampling follow the configured device rate.

At 48 kHz, a 4,096-byte stereo period contains 1,024 frames and lasts about 21.3 ms. HDA’s comments explain this period size in terms of scheduling margin under QEMU TCG.

## HDA maintenance and routing

HDA’s output planner in `c/drivers/audio/hda_output_paths.c` orders connected line-out, speaker and headphone pins, then assigns reachable analog DACs. A path can share a DAC assigned to an earlier path. The driver configures the converters to receive the same stereo stream.

`c/drivers/audio/hda_route.c` compiles paths into power, format, stream, connection selection, amplifier and pin-control commands. Amplifier gain comes from the codec’s capability fields. The driver confirms D0 through bounded power-status polling before continuing past each output power request.

Jack polling applies auto-mute: a plugged headphone mutes line-outs and speakers, and a plugged line-out mutes speakers. Headphone paths remain enabled by this policy. The driver checks whether presence detection is usable, triggers a measurement when required and reads back pin control. Periodic polls use a 250 ms minimum interval and run through housekeeping or status reads. A codec transport timeout disables further polling and triggers a bounded attempt to re-enable usable outputs.

Housekeeping also folds the wrapping hardware position register into an accumulated byte count. `snd_device_position_frames()` in `c/kernel/audio/mixer.c` invokes the selected playback device’s position callback under the mixer lock; HDA returns its accumulated count in frames.

The analog driver gives supported Intel PCH controllers at PCI address 00:1b.0 priority. The `hda-display` driver surveys GPU audio codec identity and sink information, then runs controller teardown. Both paths are implemented in `c/drivers/audio/hda.c`.

## Startup, locking and removal

Playback initialization prepares service queues and attempts engine startup when the scheduler is available. Stream opening provides another startup point. Before invoking the driver’s start callback, the playback layer clears the DMA ring.

Capture creates its worker during opening and checks worker creation before starting the engine. Both directions reuse their workers across device lifetimes.

Lifecycle serialization protects registration, opening and removal across allocation and worker creation. Stream operations and worker processing share spinlocks. Blocking reads and writes release those locks while waiting, then look up their handles again after waking.

Playback completion notification uses a short event lock. Driver start callbacks enter the driver while holding the mixer lock, while driver interrupts can already hold the driver’s lock. Keeping completion notification on the event lock avoids reversing that order. The rationale is documented in `c/kernel/audio/mixer.c`.

The housekeeping contract in `c/kernel/audio/snd.h` requires callbacks to complete without sleeping or calling back into the mixer.

Removal coordinates audio-layer detachment with driver teardown. `snd_unregister_device()` excludes the playback worker, reclaims streams and releases conversion scratch. `snd_unregister_capture_device()` clears the selected input and schedules stream reclamation. Drivers handle hardware stopping, interrupt teardown and DMA release. HDA retains DMA allocations in quarantine when stopping or reset remains unconfirmed.

## Syscalls and status

`c/kernel/exec/syscall.c` forwards audio calls to `snd_syscall()` in `c/kernel/audio/snd.c`. The handler validates user ranges and transfers PCM through a heap buffer owned by each call. Playback writes and capture reads transfer at most 8,192 bytes per syscall. Application wrappers are in `c/apps/logit.h`.

`struct logit_sndstate` serves both directions:

| Field | Playback meaning | Capture meaning |
|---|---|---|
| `frames_written` | Application frames counted on accepted writes | Zero |
| `frames_played` | Source frames consumed by the mixer | Frames accepted into the consumer ring |
| `avail_bytes` | Free stream ring space | Queued captured bytes |
| `ring_bytes` | Stream ring capacity | Consumer ring capacity |
| `underruns` | Stream shortages while running | Capture overruns |

`SYS_SND_INFO` fills `struct logit_sndinfo` with the selected playback device, DMA geometry, stream counts, IRQ mode and device underrun counter. That counter includes skipped refill positions and mixed periods with no active stream, so idle output contributes to it.

Text reports identify the driver, codec, period geometry and IRQ mode. `hda_status_get()` exposes controller diagnostics through `struct hda_controller_status` in `c/drivers/audio/hda_status.h`: PCI location, codec identity, binding status, routed pins, jack states, output enables, stream activity, accumulated bytes, interrupts and the last codec error.

Analog status reads refresh jack state subject to the polling interval. `c/kernel/diag/hwhud.c` displays these values and mirrors its HDA rows to the serial log when the boot tone state changes.

## Diagnostic boot tone

Builds with `HWHUD=1` enable the tone in `c/kernel/audio/boot_test_tone.c`. The hardware panel calls `boot_test_tone_poll()`, which schedules the tone worker once at least five seconds have elapsed since its first call.

The worker submits one second of 440 Hz stereo signed 16-bit PCM at 48 kHz through a kernel-owned playback stream. The generator uses integer arithmetic, a peak amplitude of 8,192 and 10 ms fades.

When the HDA hook is available, the worker suspends auto-mute and waits 300 ms to allow jack polling to apply the change before opening the stream. It restores auto-mute after draining the stream and making a bounded wait for hardware position progress. The log records submitted frames, hardware positions and the stream underrun count.

A boot module named `NOSOUND` suppresses the tone. The UEFI loader’s firmware-index mechanism in `c/boot/efi/loader.c` supplies boot modules and requires indexed files to have a nonzero size. Use a dedicated `BUILD` directory for `HWHUD=1`; `tests/uefi.mk` explains that changing the flag within an existing build directory is not tracked.

## Test coverage

`make test-audio-pcm` compiles the production PCM primitives with `tests/unit/audio_pcm_test.c`. The suite checks conversion, resampling across calls, saturation, silence values and ring wraparound. `tests/drivers/audio/tests.mk` makes the saturation negative control a prerequisite.

The same fragment defines `make test-audio-cards-host`, which includes playback and capture framework checks and the HDA controller-selection and ownership gate defined in `tests/hda_x79.mk`.

## Limitations

- Playback output supports mono or stereo, and the application rate must be at most four times the device rate. Source channels beyond the first pair are discarded; surround downmix coefficients are absent from `c/kernel/audio/pcm.c`.
- Capture permits one open consumer and transfers the native format without resampling. A supplied format triple must match the device. `snd_cap_read()` ignores `SND_F_NONBLOCK`, despite the wrapper comment in `c/apps/logit.h`.
- The ABI comment describes playback `frames_played` as hardware consumption. The implementation counts mixer consumption, and draining waits for the application ring to empty. Neither reports when the queued samples reach the output.
- `snd_owner_release()` and `snd_cap_owner_release()` have no callers in the code. Process exit cleanup in `c/kernel/exec/proc.c` leaves audio stream cleanup unwired.
- Playback startup ignores the return from `thread_create()` and records its worker as created. Despite the older comment describing a void return, the scheduler interface returns an error on allocation failure. Such a failure can leave DMA running without a mixer worker.
- HDA output planning supports eight paths, each containing a pin, at most one mixer or selector, and a DAC. HDMI/DP playback awaits GPU display-engine programming. The routing sources describe model and QEMU evidence and mark X79-VG4 hardware verification as pending.
- HDA’s accumulated position depends on sampling within each DMA ring lap, about 171 ms. Longer gaps can lose complete laps. The boot tone can enter its completed state after the position wait times out; its log and state do not establish audible output.

## See also

- [Audio drivers](../drivers/audio.md)
- [System calls](../architecture/system-calls.md)
- [Scheduling](scheduling.md)
- [Synchronisation](synchronization.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/audio/snd.h` | Device contracts, stream interfaces and PCM declarations |
| `c/kernel/audio/snd.c` | Syscall handling, user memory validation and copying |
| `c/kernel/audio/pcm.c` | Conversion, interpolation, saturation and byte rings |
| `c/kernel/audio/mixer.c` | Playback registration, streams, worker and refill scheduling |
| `c/kernel/audio/capture.c` | Capture registration, worker, buffering and overrun handling |
| `c/kernel/audio/boot_test_tone.c` | Tone generation, playback and position logging |
| `c/kernel/audio/boot_test_tone.h` | Tone parameters, state and suppression contract |
| `include/abi/logit_abi.h` | Audio syscall numbers, formats and public structures |
| `c/kernel/exec/syscall.c` | Audio syscall forwarding |
| `c/kernel/exec/proc.c` | Process exit and resource teardown |
| `c/kernel/sched/sched.h` | Worker creation return contract |
| `c/kernel/sched/sched.c` | Worker allocation and creation |
| `c/drivers/audio/hda.c` | HDA setup, interrupts, maintenance, diagnostics and removal |
| `c/drivers/audio/hda_route.c` | Output route search and codec command generation |
| `c/drivers/audio/hda_route.h` | Widget graph, route structures and routing bounds |
| `c/drivers/audio/hda_output_paths.c` | Analog output planning, DAC assignment and auto-mute |
| `c/drivers/audio/hda_output_paths.h` | Output plan contracts, roles and jack states |
| `c/drivers/audio/hda_status.h` | Controller diagnostics and auto-mute suspension interface |
| `c/drivers/audio/ac97/pci.c` | AC’97 registration and teardown |
| `c/drivers/audio/ac97/streams.c` | AC’97 DMA buffers, callbacks and completion delivery |
| `c/drivers/audio/ac97/ac97_regs.h` | AC’97 rate and buffer geometry |
| `c/drivers/audio/es1370/driver.c` | ES1370 registration and teardown |
| `c/drivers/audio/es1370/pcm.c` | ES1370 playback and capture interfaces |
| `c/drivers/audio/es1370/registers.h` | ES1370 clock divider and reported rate |
| `c/drivers/audio/es1370/es1370_internal.h` | ES1370 buffer geometry and driver state |
| `c/kernel/diag/hwhud.c` | Hardware panel audio diagnostics and tone polling |
| `c/boot/efi/loader.c` | Firmware-index boot module loading |
| `Makefile` | Kernel source collection and `HWHUD` build selection |
| `tests/uefi.mk` | UEFI build flags and build-directory guidance |
| `c/apps/logit.h` | Application audio syscall wrappers |
| `c/apps/coreutils/rec.c` | Capture client using the returned native format |
| `c/apps/gui/preview/preview.c` | Application decoding and playback setup |
| `tests/unit/audio_pcm_test.c` | PCM conversion, resampling, saturation and ring checks |
| `tests/audio.mk` | PCM and guest audio test targets |
| `tests/drivers/audio/tests.mk` | Driver test aggregates and PCM negative-control prerequisite |
| `tests/hda_x79.mk` | HDA controller-selection and ownership test targets |