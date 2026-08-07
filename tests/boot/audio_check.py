#!/usr/bin/env python3
"""Check a WAV captured from QEMU against what the guest claimed to play.

This is where the audio assertions actually live. The guiding rule: an
assertion about sound is only worth making if a plausible WRONG driver fails
it. So each check below names the bug it catches.

Usage: audio_check.py <file.wav> <ramp|mix|underrun>
"""
import struct
import sys

RATE = 48000
CH = 2
RAMP_MOD = 240
RAMP_STEP = 256
RAMP_BASE = -30720


def read_wav(path):
    """Minimal RIFF reader. Deliberately not the `wave` module: QEMU writes the
    header before it knows the length and patches it on exit, and a truncated
    run leaves sizes that `wave` refuses outright -- we would rather report
    'captured 0 frames' than raise."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 44 or data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('FAIL: not a RIFF/WAVE file')

    pos, fmt, samples = 12, None, b''
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        (csz,) = struct.unpack('<I', data[pos + 4:pos + 8])
        body = data[pos + 8:pos + 8 + csz]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            # Trust the bytes present, not the declared size: if QEMU was
            # killed the declared size can be 0 while the samples are there.
            samples = data[pos + 8:]
            break
        pos += 8 + csz + (csz & 1)
    if fmt is None:
        raise SystemExit('FAIL: no fmt chunk')
    _, ch, rate, _, _, bits = fmt
    n = len(samples) // 2
    pcm = struct.unpack('<%dh' % n, samples[:n * 2])
    return ch, rate, bits, pcm


def split(pcm, ch):
    return pcm[0::ch], pcm[1::ch]


def trim_silence(x, thresh=200):
    """Drop leading/trailing silence. The card runs continuously, so the capture
    always brackets the signal with the silence played before and after it."""
    a, b = 0, len(x)
    while a < b and abs(x[a]) < thresh:
        a += 1
    while b > a and abs(x[b - 1]) < thresh:
        b -= 1
    return a, b


def check_ramp(left, right):
    """The core assertion of the whole suite.

    Every frame carries its own index. Recover it and require it to advance by
    exactly one, everywhere. This fails against:
      - a driver that never refills (QEMU replays period 0 -> the index jumps
        backwards every 1024 frames),
      - one that fills the period the card is CURRENTLY playing (a tear at a
        random offset),
      - one whose rate is wrong (the count of frames is wrong),
      - one that swaps channels (right != -left).
    """
    ok = True
    a, b = trim_silence(left)
    seg_l, seg_r = left[a:b], right[a:b]
    if len(seg_l) < RATE // 4:
        print('FAIL: captured only %d frames of signal (want >= %d)'
              % (len(seg_l), RATE // 4))
        return False
    print('ok  captured %d frames of signal (%.0f ms)'
          % (len(seg_l), 1000.0 * len(seg_l) / RATE))

    # Recover the frame index from the sample value. The step is 256, so
    # rounding to the nearest index tolerates anything the host can add.
    idx = [int(round((v - RAMP_BASE) / float(RAMP_STEP))) for v in seg_l]

    # Skip the first and last few frames: the very start is the mixer ramping
    # in from silence, and the tail can be clipped by when QEMU was stopped.
    lo, hi = 8, len(idx) - 8
    bad_step = bad_range = 0
    first_bad = None
    for i in range(lo, hi):
        if not (0 <= idx[i] < RAMP_MOD):
            bad_range += 1
            if first_bad is None:
                first_bad = (i, seg_l[i], 'out of range')
            continue
        if i > lo:
            want = (idx[i - 1] + 1) % RAMP_MOD
            if idx[i] != want:
                bad_step += 1
                if first_bad is None:
                    first_bad = (i, seg_l[i], 'index %d after %d, wanted %d'
                                 % (idx[i], idx[i - 1], want))
    n = hi - lo
    if bad_range or bad_step:
        print('FAIL: %d/%d frames break the index sequence '
              '(%d out of range, %d wrong step); first at %r'
              % (bad_range + bad_step, n, bad_range, bad_step, first_bad))
        ok = False
    else:
        print('ok  all %d frames advance by exactly one index '
              '(no drop, repeat, tear or rate error)' % n)

    # Amplitude: the signal must span the full designed range, not a fraction
    # of it. A wrong format (say s16 read as u8) or a codec amp left at its
    # floor shows up here and nowhere else.
    lo_v, hi_v = min(seg_l), max(seg_l)
    if lo_v > RAMP_BASE + 2000 or hi_v < RAMP_BASE + (RAMP_MOD - 1) * RAMP_STEP - 2000:
        print('FAIL: amplitude range %d..%d, expected about %d..%d'
              % (lo_v, hi_v, RAMP_BASE, RAMP_BASE + (RAMP_MOD - 1) * RAMP_STEP))
        ok = False
    else:
        print('ok  amplitude spans %d..%d as designed' % (lo_v, hi_v))

    # Channel mapping: right must be the negation of left.
    mism = sum(1 for i in range(lo, hi) if abs(seg_r[i] + seg_l[i]) > 300)
    if mism > n // 100:
        print('FAIL: %d/%d frames have right != -left (channels wrong)' % (mism, n))
        ok = False
    else:
        print('ok  right channel is the negation of left (channel map correct)')
    return ok


def plateaus(x, minlen):
    """Split into runs of roughly-constant value. Returns [(value, length)]."""
    out = []
    i = 0
    while i < len(x):
        j = i
        # 700 is wide enough to ride over the mixer's ramp between plateaus and
        # far narrower than the 3000 that separates the levels being tested.
        while j < len(x) and abs(x[j] - x[i]) < 700:
            j += 1
        if j - i >= minlen:
            seg = x[i:j]
            out.append((sum(seg) // len(seg), j - i))
        i = j
    return out


def check_mix(left, _right):
    """Two streams summed. Fails against a mixer that overwrites (the middle
    plateau would read -3000) or that drops the second stream (8000
    throughout)."""
    ok = True
    a, b = trim_silence(left)
    seg = left[a:b]
    if len(seg) < RATE // 2:
        print('FAIL: only %d frames captured' % len(seg))
        return False

    ps = [p for p in plateaus(seg, RATE // 20)]   # >= 50 ms to count
    print('    plateaus: %s' % ps)
    levels = [v for v, _ in ps]

    def near(v, want, tol=900):
        return abs(v - want) <= tol

    # The exact sequence 8000 -> 5000 -> 8000 must appear in order.
    seq_ok = False
    for i in range(len(levels) - 2):
        if near(levels[i], 8000) and near(levels[i + 1], 5000) and near(levels[i + 2], 8000):
            seq_ok = True
            break
    if seq_ok:
        print('ok  found 8000 -> 5000 -> 8000: the second stream was SUMMED, '
              'and starting/stopping it did not disturb the first')
    else:
        print('FAIL: no 8000 -> 5000 -> 8000 sequence. '
              'Overwrite instead of sum would give -3000 in the middle; '
              'a dropped second stream would give 8000 throughout.')
        ok = False
    return ok


def check_underrun(left, _right):
    """A ring that runs dry must produce SILENCE and then recover. Fails
    against a driver that leaves the stale buffer in place -- which repeats the
    last period forever and passes any is-it-still-playing check."""
    ok = True
    a, b = trim_silence(left)
    seg = left[a:b]
    if len(seg) < RATE // 2:
        print('FAIL: only %d frames captured' % len(seg))
        return False

    # Walk in 10 ms blocks and classify each as tone or silence.
    blk = RATE // 100
    marks = []
    for i in range(0, len(seg) - blk, blk):
        peak = max(abs(v) for v in seg[i:i + blk])
        marks.append('T' if peak > 6000 else ('s' if peak < 1500 else '?'))
    s = ''.join(marks)
    print('    block map (T=tone s=silence, 10 ms each):')
    print('    ' + s)

    # Require tone, then a run of silence at least 150 ms long, then tone.
    import re
    m = re.search(r'T[T?]*[sT?]*?(s{15,})[s?]*T', s)
    if m:
        print('ok  the ring ran dry -> %d ms of SILENCE -> tone recovered'
              % (len(m.group(1)) * 10))
    else:
        if 'T' in s and 's' not in s:
            print('FAIL: no silence anywhere -- the buffer REPEATED its last '
                  'period instead of going quiet when it ran dry')
        else:
            print('FAIL: no tone -> silence(>=150ms) -> tone pattern found')
        ok = False

    if not s.rstrip('s?').endswith('T'):
        print('FAIL: playback did not recover after the underrun')
        ok = False
    else:
        print('ok  playback recovered after the underrun (no deadlock, no stall)')
    return ok


def main():
    path, mode = sys.argv[1], sys.argv[2]
    ch, rate, bits, pcm = read_wav(path)
    frames = len(pcm) // ch if ch else 0
    print('    capture: %d ch, %d Hz, %d bit, %d frames (%.2f s)'
          % (ch, rate, bits, frames, frames / float(rate or 1)))

    ok = True
    if (ch, rate, bits) != (CH, RATE, 16):
        print('FAIL: capture is %d ch/%d Hz/%d bit, wanted %d/%d/16'
              % (ch, rate, bits, CH, RATE))
        ok = False
    if frames < RATE // 4:
        print('FAIL: only %d frames captured -- the DMA engine produced almost '
              'nothing' % frames)
        return 1

    left, right = split(pcm, ch)
    if mode == 'ramp':
        ok = check_ramp(left, right) and ok
    elif mode == 'mix':
        ok = check_mix(left, right) and ok
    elif mode == 'underrun':
        ok = check_underrun(left, right) and ok
    else:
        print('FAIL: unknown mode %s' % mode)
        return 1
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
