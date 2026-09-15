# External ADC capture evidence

This gate supplies deterministic, nonperiodic stereo PCM through QEMU's public
D-Bus AudioInListener. The emulated controller transfers that input into guest
DMA memory; the existing `/bin/rec` reads through normal capture syscalls and
writes a WAV on a private disk copy. The host extracts those saved files and
checks every sample. No driver-memory hook, verification-only kernel macro,
physical microphone, host sound device or audio daemon is involved.

```sh
make BUILD=build/drivers/audio/capture-final audio-capture-backend test-audio-capture-oracle
python3 tests/drivers/audio/capture/guest.py \
  --iso build/drivers/audio/capture-kernel/logit.iso --disk build/disk.img \
  --device AC97 --backend build/drivers/audio/capture-final/audio-capture/backend \
  --output build/drivers/audio/capture-final/ac97-run
```

The output directory must be new. `--device` also accepts `intel-hda` (with an
HDA duplex codec) and `ES1370`. Add `--output-device intel-hda` to an AC97 or
ES1370 run to place an output-only HDA codec before the input card. This forces
independent device selection: HDA cannot satisfy recording in that topology.
`profiles.py` owns topology and native-rate definitions; `guest.py` owns boot,
application commands and artifact collection; `backend.c` owns D-Bus transport;
`oracle.py` owns PCM and lifecycle verification.

The complete three-card target is
`test-audio-capture-os`, with `AUDIO_CAPTURE_ISO` and `AUDIO_CAPTURE_DISK`
overrides. `test-audio-multicard-os` runs both separate-input configurations.
Each Make invocation creates a unique retained guest-run directory;
repeating the target cannot overwrite older results. Its ISO is a dependency,
but the existing disk template is only read and is never rebuilt. The runner
checks the original disk hash around copying and again after the run. It also
requires a clean boot marker, no panic, two exact native-format recording
completion markers and the expected 144000-frame playback completion.

It requires QEMU D-Bus display/audio support and a C compiler with
GIO Unix FD support (`pkg-config gio-unix-2.0`). The backend builds with
ASan/UBSan. Missing prerequisites fail explicitly; they never become passes.

The runner freezes its ISO and makes a private writable copy of the supplied
disk. It pauses QEMU before connecting a private D-Bus peer through QMP
`getfd`/`add_client`, registers both input and output listeners, then resumes
boot. No session bus, authentication account, or shared host audio service is
used. Backend interfaces validate stereo s16 little-endian PCM and native rate
before accepting data.

Two ordinary `rec 1` invocations exercise capture close/reopen. The second runs
while the existing `sndtest ramp 3000` plays in the background. QEMU backend
`SetEnabled` events must prove that the whole second ADC interval is inside the
DAC interval, including playback continuing after the ADC stops. Output PCM
must independently retain the three-second 200 Hz ramp, amplitude, polarity,
per-sample slope and phase continuity. An enable bit or successful syscall alone
cannot satisfy these checks.

Input is a long seeded sequence with different left/right values. Every value
is a multiple of 15 to make the observed AC97 QEMU volume exactly representable.
The listener implements D-Bus endpoint mute and per-channel volume requests,
saving every byte actually returned to QEMU separately from the source. AC97
must request unmuted volume 136/255 (its modeled Record Gain 0x0808); HDA and
ES1370 use unity. The oracle independently verifies this complete source-to-
injected transformation, then requires each complete one-second recording to
be a byte-exact contiguous slice inside its own enable interval. A maximum
one-ring initial offset is allowed because capture begins on hardware period
boundaries; no missing or changed sample inside the recorded interval is
allowed. A reopened recording cannot reuse the previous interval's bytes.

There are 52 mandatory oracle controls across four rate/gain profiles, including
48662 Hz input with independent 48000 Hz output. The earlier 24-control suite
assumed a shared native rate; both the backend format check and output oracle
now receive their own rate, preserving native output rather than silently
asking QEMU to resample it to the input rate. In addition
to valid full capture/duplex fixtures, controls reject silence, swapped
channels, reordered/repeated periods, short recordings, wrong rates, stale
reopen data, missing stop, absent duplex overlap, corrupt playback and corrupt
injected data and a wrong output rate. These controls run before every guest, including direct Python
invocations. `oracle-controls.json` retains the actual reasons for rejection.

Artifacts include source and injected PCM, captured playback PCM, both guest
WAV files, private disk, frozen ISO, serial/QEMU/backend logs, QMP replies,
command arguments, input/artifact/tool hashes and `result.json`. Both external listener connections
must close after QMP quit, allowing the backend to exit and flush normally.
Artifact hashes are computed only after all processes have exited. Failed runs
retain their artifacts too. Early feasibility results are preserved separately:
HDA, AC97 and ES1370 each produced two complete recordings matching external
injection, and all three passed the subsequent full trajectory/duplex oracle.
The final source/ISO acceptance belongs to each run's own recorded hashes.

These results prove the emulated ADC/DMA/syscall/file path. They do not prove
physical microphone gain, analog quality or independent stereo microphone
routing: both physical drivers currently select a mono microphone into both
ADC channels, while QEMU accepts independent synthetic stereo backend input.
AC97's QEMU 136/255 endpoint gain is specifically a model convention, not the
physical codec's +12 dB analog law. ES1370's native 48662 Hz follows its integer
clock divider; the capture API does not pretend it is exactly 48000 Hz.

Primary references: [QEMU D-Bus AudioInListener and AudioOutListener API](https://www.qemu.org/docs/master/interop/dbus-display.html),
[QEMU v11 D-Bus audio backend](https://github.com/qemu/qemu/blob/v11.0.0/audio/dbusaudio.c),
[QEMU peer-connection tests](https://github.com/qemu/qemu/blob/v11.0.0/tests/qtest/dbus-display-test.c).
