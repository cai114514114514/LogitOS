# Shared playback lifetime gate

This gate includes the production `c/kernel/audio/mixer.c` and links the real
`pcm.c`. Kernel allocation, wait queues and scheduler dispatch are controlled
services; the test runs the actual mixer worker and checks bytes written into
each device's DMA ring. It does not implement a second mixer oracle.

The service now separates its permanent wait queues, semaphore and worker from
the registered device's scratch buffers, DMA-running state and stream handles.
Registration validates the device geometry and allocates all three scratch
buffers before publication. Each failed allocation releases partial work.
Repeated `snd_init()` preserves existing queues, semaphore tokens and scratch.
Unregister excludes the worker, invalidates and wakes old handles, and frees
CPU scratch before a different device can register. It does not claim to stop
hardware: the card driver still masks/releases IRQs and proves DMA quiescence
before releasing its DMA allocation.

Lock order is the lifecycle domain, then the mixer lock, then the short event
lock. Driver callbacks may own their device gate, so the IRQ callback takes only
the event lock; it never takes the mixer lock or enters the sleeping lifecycle
domain. The test delivers an IRQ synchronously from `start()` to detect that
specific inversion. The lifecycle domain is recursive by task, allowing an open
to enter the engine-start path without treating another task on the same CPU as
its owner.

A failed hardware start disables the binding until driver removal. It is not
retried and its DMA ring is not cleared again: a failed start and a void stop
callback provide no independent proof that hardware stopped. The scheduler's
`thread_create()` also returns void and silently returns on allocation failure;
this layer cannot detect that failure or promise a reliable worker-creation
retry without a scheduler API change. Normal worker creation and device rebinding
are covered here; successful physical hotplug is not claimed.

Device output remains S16, one or two channels. Application input may use all
eight channels already accepted by `snd_fmt_ok`, so raw scratch now reserves
eight-channel F32 capacity. Period geometry is bounded both by allocation sizes
and the resampler's Q16.16 integer range: `period_frames * 4 + 4 <= 65535`.
This refuses periods whose phase would wrap within one resampling call.

The positive cases cover:

- Pre-scheduler initialization and repeated initialization with live queues.
- Geometry rejection, partial-allocation failure and start-failure retention.
- Real PCM mixing, synchronous/current-device IRQs and foreign IRQ rejection.
- Stereo 48 kHz / 4096-byte periods followed by mono 22.05 kHz / 64-byte periods.
- A two-period ring whose current DMA slot must remain untouched by silence fill.
- Eight-channel F32 input at 192 kHz converted to stereo 48 kHz, checked against
  literal S16 samples; input exceeds the previous two-channel scratch capacity.
- Unregister/re-register during a blocked write, preventing old-handle data from
  entering the new sink even when the static stream slot is reused.

Four mandatory negative controls precede the positive run. Reinitializing the
service, accepting a foreign IRQ and zeroing the current DMA slot each cause
specific functional assertions to fail. Restoring two-channel raw scratch must
produce an ASan heap-buffer-overflow in `pcm_ring_peek`, reached from the explicit
eight-channel input case. An unrelated crash does not satisfy that control.
ASan and UBSan are enabled for every binary. These tests validate bounded
interleavings, memory lifetime and DMA contents; guest scheduling deadlines and
actual controller playback are checked by the separate QEMU audio gates.

```sh
make -f tests/drivers/audio/playback/tests.mk \
  BUILD=build/drivers/audio/expand-playback CC=clang test-playback-framework
```
