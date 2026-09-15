# Audio guest capture

Run the existing ring-3 producer through the serial shell; no special kernel or
application test mode is installed:

```sh
python3 tests/drivers/audio/guest.py --self-test
python3 tests/drivers/audio/guest.py \
  --iso build/drivers/audio/kernel/logit.iso --disk build/disk.img \
  --device intel-hda --output build/drivers/audio/hda-capture
```

`--device` accepts `intel-hda`, `AC97`, or `ES1370`. HDA attaches `intel-hda` plus
`hda-output`; other cards attach their named PCI device. The output directory
must not exist, preventing a previous WAV or successful log from being reused.
Each guest run executes all oracle controls first. `--timeout` is a hang limit,
not a performance measurement. Missing QEMU is reported explicitly and returns
nonzero; it never counts as successful audio evidence.

The runner copies the ISO into the output directory before boot, uses `-snapshot`
for the disk and checks the original disk's SHA-256 before and after execution.
Only an unmodified `sndtest ramp 1000` is sent after the actual serial-shell
prompt. The guest must identify the requested device and report all 48000 input
frames completed. Captured PCM must independently agree; guest output alone is
insufficient.

HDA and AC97 use 48000 Hz capture and the existing exact frame-index checker.
ES1370 DAC2's integer divider yields 48662 Hz for the selected divider. Its
capture is pinned to that native rate, while the existing mixer resamples the
application's 48000 Hz input. An independent oracle checks its ramp slope,
200 Hz zero-crossing spacing, full amplitude, opposite-polarity channels and
one-second signal length. The 64-frame length tolerance admits boundary
rounding but rejects a missing 1024-frame period. Timing is measured from
captured frames, not elapsed host time. This periodic ramp cannot distinguish
a substitution by an identical complete 240-source-frame cycle; this gate does
not claim byte provenance beyond the observable waveform.

There are 26 oracle cases across both rates: valid PCM and valid zero-length
streaming headers must pass; silence, wrong speed, swapped channels, shortened
signals, repeated periods, damaged/truncated WAV, wrong declared sample rate,
a zero header with truncated/short payload, and only one zero length field must
fail. Their actual observations are retained in `oracle.json`; merely expecting
an exception is not used as a substitute for running the PCM oracle.

## QEMU 11 WAV header correction

The old runner states that exiting QEMU finalizes WAV lengths. On the installed
QEMU 11.0.0, both SIGTERM with exit status zero and acknowledged QMP `quit` left
the initial RIFF/data length fields zero. That does not mean no PCM was written:
the HDA baseline contained the full 48000-frame ramp. This runner first waits for
QMP termination and records the replies and exit status. It then accepts only
the specific fixed 44-byte PCM header with **both** length fields zero, complete
four-byte stereo frames, and all the same signal checks. Arbitrary inconsistent
lengths or a partially truncated frame remain failures.

`capture.wav` is the raw evidence and is never rewritten. `playable.wav`, when
needed, changes only the two length fields, records its own SHA-256, and makes
the same PCM playable by ordinary WAV readers. `result.json` explicitly records
`header_finalized: false`; this is a capture-container correction, not a driver
fix or repaired audio. SIGKILL or unsuccessful QEMU exit cannot activate this
compatibility path.

Other retained evidence includes `serial.log`, `qemu.stderr`, `qmp.json`,
`command.json`, frozen `input.iso`, input hashes, oracle-source hashes, PCM
metrics and raw/derived WAV hashes. Expected capture-only warnings from AC97
or ES1370 are retained; a nonempty stderr is not automatically a PCM failure.
All results are evidence about emulated PCI hardware, not physical sound cards.

Primary references: [QEMU invocation and WAV backend parameters](https://www.qemu.org/docs/master/system/qemu-manpage.html),
[QEMU 11 WAV header creation/finalization](https://github.com/qemu/qemu/blob/v11.0.0/audio/wavaudio.c).
