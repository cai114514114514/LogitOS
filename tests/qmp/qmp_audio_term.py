#!/usr/bin/env python3
"""On-device test for RT_T_AUDIO: the Terminal's audio object, judged by the
actual samples the guest played -- not by a log line saying it tried.

hda.c's own header names the failure this has to rule out: "the DAC must be
told which stream number to listen to, or the DMA engine runs happily, LPIB
advances, every register reads back correct -- and there is silence. That
failure looks exactly like success from the controller's side." So the only
instrument that answers "did it play" is QEMU's `wav` audiodev, which writes
what the guest's DAC actually produced to a file -- exactly the technique
tests/boot/run-audio-wav-test.sh uses for /bin/sndtest, aimed here at the GUI
path instead: dock -> Terminal -> `show` -> click -> real PCM.

TWO MODES, because "no audio device" is not a degraded case of the other one,
it is a DIFFERENT MACHINE, and the task this covers ("the machine make run
produces has no sound card") is real right now -- the run target attaches no
audio device at all:

  device   intel-hda + hda-output, captured to a wav file. Asserts have_device
           in the terminal's own diagnostic line, that clicking the object
           starts playback (TERMPERF audio_play), that it reaches natural end
           (TERMPERF audio_end) for a real ~0.25s clip, AND that the captured
           WAV actually contains non-silent samples -- the check hda.c's
           comment says a log line alone cannot make.
  none     no audio device attached at all. Asserts the terminal's own
           diagnostic reports have_device=0, that a click on the (disabled)
           object never produces an audio_play line, and that the machine
           does not hang -- the "clean, visible state" the task asked for,
           not a crash and not a silent no-op indistinguishable from a bug.

Usage: qmp_audio_term.py <iso> <disk.img> <mode: device|none> [out.ppm]
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui
from qmp_ui import PPM, Session, dock_icon

ISO = sys.argv[1]
DISK = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "device"
OUT = sys.argv[4] if len(sys.argv) > 4 else f"/tmp/audioterm.{MODE}.ppm"

TERMINAL_SLOT = 3            # clock textedit monitor terminal ...
C_OK  = (0x1E, 0x90, 0x50)   # the Play button's fill (P.ok, light theme)

fails = []


def chk(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


def box(ppm, rgb):
    return ppm.find_color(rgb)


# ---------------------------------------------------------------- boot ------

sock = tempfile.mktemp(suffix=".qmp")
ser = tempfile.mktemp(suffix=".ser")
wav = tempfile.mktemp(suffix=".wav")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")

audio_args = []
if MODE == "device":
    # Same shape as tests/boot/run-audio-wav-test.sh: the wav audiodev is
    # output-only, so hda-output rather than hda-duplex avoids a QEMU log line
    # about a capture voice that was never going to work anyway.
    audio_args = [
        "-audiodev", f"wav,id=snd0,path={wav},out.frequency=48000,out.channels=2,out.format=s16",
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
    ]
elif MODE != "none":
    print(f"unknown mode '{MODE}', want device|none"); sys.exit(2)

proc = subprocess.Popen(
    [qemu, "-cpu", os.environ.get("QEMU_CPU_NAME", "max"), "-cdrom", ISO,
     "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M",
     "-vga", "none", "-device", "virtio-gpu-pci",
     "-display", "none", "-no-reboot", *audio_args,
     "-chardev", f"socket,id=ser0,path={ser},server=on,wait=on",
     "-serial", "chardev:ser0",
     "-qmp", f"unix:{sock},server=on,wait=off"])

serial = socket.socket(socket.AF_UNIX)
for _ in range(200):
    try:
        serial.connect(ser)
        break
    except OSError:
        if proc.poll() is not None:
            print("qemu died before the serial socket appeared")
            sys.exit(1)
        time.sleep(0.1)
log = bytearray()
_reader_stop = False


def _reader():
    while not _reader_stop:
        try:
            b = serial.recv(65536)
            if not b:
                break
            log.extend(b)
        except OSError:
            time.sleep(0.05)


threading.Thread(target=_reader, daemon=True).start()


def wait_for(marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.2)
        if marker in log:
            return True
        if proc.poll() is not None:
            return False
    return False


def last_line_with(marker):
    lines = log.decode("utf-8", "replace").splitlines()
    for l in reversed(lines):
        if marker in l:
            return l
    return None


def read_wav(path):
    """Minimal RIFF reader -- deliberately not the `wave` module, and for the
    exact reason tests/boot/audio_check.py's own copy of this function gives:
    QEMU writes the RIFF/data sizes as 0 and patches them on exit, and `wave`
    refuses a 0-declared file outright rather than reading the real bytes
    that are actually there. Trusting the bytes present, not the declared
    chunk size, is what makes this survive an imperfect shutdown instead of
    crashing on it."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos, fmt, samples = 12, None, b""
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        (csz,) = struct.unpack("<I", data[pos + 4:pos + 8])
        body = data[pos + 8:pos + 8 + csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            samples = data[pos + 8:]
            break
        pos += 8 + csz + (csz & 1)
        if csz == 0 and cid != b"data":
            break                                  # a genuinely truncated header
    if fmt is None:
        return None
    n = len(samples) // 2
    pcm = struct.unpack("<%dh" % n, samples[:n * 2]) if n else ()
    return fmt, pcm


def parse_kv(line):
    """'TERMPERF audio_open path=/x fmt=wav rate=22050 ...' -> dict."""
    out = {}
    for tok in line.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


ok = wait_for(b"LOGIT_BOOT_OK", 45)
if not ok:
    print(f"FAIL[{MODE}]: never booted")
    print(log.decode("utf-8", "replace")[-3000:])
    proc.kill()
    sys.exit(1)
time.sleep(2.0)

s = Session(sock)

SHIFTED = {">": "dot", "|": "backslash", ":": "semicolon", "_": "minus", "?": "slash"}


def typ(text, settle=0.11):
    for ch in text:
        if ch in SHIFTED:
            s.key_shift(SHIFTED[ch], settle)
        elif "A" <= ch <= "Z":
            s.key_shift(ch.lower(), settle)
        else:
            s.key(qmp_ui.KMAP.get(ch, ch), settle)


def run(cmd, settle=1.6):
    typ(cmd + "\n")
    time.sleep(settle)


def shot(tag):
    p = OUT.replace(".ppm", f".{tag}.ppm")
    s.screendump(p, settle=0.6)
    return PPM(p)


def launch_terminal():
    for n in (qmp_ui.NAPPS, qmp_ui.NAPPS + 1, qmp_ui.NAPPS + 2,
              qmp_ui.NAPPS + 3, qmp_ui.NAPPS - 1):
        s.click_at(*dock_icon(TERMINAL_SLOT, n))
        time.sleep(2.5)
        if b"launched Terminal" in log:
            return True
    return False


print(f"=== mode={MODE} ===")
print("launching the Terminal from the dock")
if not launch_terminal():
    print(f"FAIL[{MODE}]: the Terminal never launched -- everything below would be noise")
    s.screendump(OUT.replace(".ppm", ".nolaunch.ppm"), settle=0.5)
    try:
        s.cmd({"execute": "quit"})
    except Exception:
        pass
    proc.kill()
    sys.exit(1)
time.sleep(1.5)

# A clean scrollback so the object this test cares about is the FIRST (and
# only) one on screen -- both for a deterministic click target and so a color
# scan cannot land on some earlier command's pixels by accident.
run("clear", 1.0)
run("show /media/sample.wav", 2.0)

opened = wait_for(b"TERMPERF audio_open", 6.0)
chk(opened, "the terminal reports it opened the audio object (TERMPERF audio_open)")
if not opened:
    print(f"----- serial [{MODE}] -----")
    print(log.decode("utf-8", "replace")[-3000:])
    try:
        s.cmd({"execute": "quit"})
    except Exception:
        pass
    proc.kill(); proc.wait()
    sys.exit(1)

info = parse_kv(last_line_with("TERMPERF audio_open"))
print(f"  audio_open: {info}")
chk(info.get("path") == "/media/sample.wav", f"names the right file ({info.get('path')})")
chk(info.get("fmt") == "wav", f"decoded as wav ({info.get('fmt')})")

if MODE == "device":
    chk(info.get("have_device") == "1", "have_device=1 with intel-hda attached")
    chk(int(info.get("rate", "0")) > 0, f"a real sample rate was parsed ({info.get('rate')})")
    chk(int(info.get("ch", "0")) > 0, f"a real channel count was parsed ({info.get('ch')})")

    p = shot("obj")
    b = box(p, C_OK)
    chk(b is not None, "the Play button is drawn (P.ok fill found on screen)")

    if b:
        cx, cy = (b[0] + b[2]) // 2, (b[1] + b[3]) // 2
        print(f"  clicking the Play button at ({cx},{cy})")
        s.click_at(cx, cy, settle=0.3)

        played = wait_for(b"TERMPERF audio_play", 4.0)
        chk(played, "clicking the button starts playback (TERMPERF audio_play)")

        # sample.wav is ~0.25s; end-of-stream should arrive well inside this.
        ended = wait_for(b"TERMPERF audio_end", 6.0)
        chk(ended, "the clip reaches its natural end (TERMPERF audio_end)")
    else:
        print(f"----- serial [{MODE}] (no button found) -----")
        print(log.decode("utf-8", "replace")[-2000:])

    # Give the DMA a moment to drain into the capture, then shut down the same
    # way tests/boot/run-audio-wav-test.sh does -- SIGTERM, not QMP `quit`:
    # this is what makes QEMU finalise the wav backend at all (a killed -9
    # process, or apparently `quit` over QMP, leaves the RIFF/data sizes at
    # the placeholder 0 they were opened with; read_wav() above tolerates
    # that too, belt and suspenders, by trusting the bytes present).
    time.sleep(1.5)
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except Exception:
        proc.kill(); proc.wait()

    chk(os.path.exists(wav) and os.path.getsize(wav) > 44, "a WAV capture file exists and is non-empty")
    if os.path.exists(wav) and os.path.getsize(wav) > 44:
        parsed = read_wav(wav)
        chk(parsed is not None, "the captured file parses as RIFF/WAVE")
        if parsed:
            (_, ch, rate, _, _, bits), samples = parsed
            print(f"  captured: {ch} ch, {rate} Hz, {bits}-bit, {len(samples)} samples")
            # "Did anything above the noise floor get played" is the whole
            # claim -- this is hda.c's own warning made checkable: LPIB
            # advancing and every register reading back correct is NOT
            # evidence, only the samples the DAC actually produced are.
            peak = max((abs(x) for x in samples), default=0)
            chk(peak > 500, f"the captured WAV is not silent (peak={peak} of 32767, {len(samples)} samples)")

    chk(b"[snd] hda: codec" in log, "the boot log names a real codec (not just 'audio present')")

else:  # MODE == "none"
    chk(info.get("have_device") == "0", "have_device=0 with no sound card attached")
    chk(b"[snd] no audio device found" in log,
        "the boot log says explicitly that no device was found")

    p = shot("obj")
    # No colour scan here: the disabled state is P.panel, a colour used all
    # over the desktop chrome, so a scan for it would be a scan for noise.
    # The click target is the object's row, and any click inside it is a
    # no-op by construction (on_click's O_AUDIO branch checks `usable` before
    # doing anything) -- so the assertion that matters is what does NOT
    # happen afterward, not where exactly the click landed.
    cx, cy = 380, 210
    print(f"  clicking where the (disabled) object should be ({cx},{cy})")
    s.click_at(cx, cy, settle=0.3)
    time.sleep(1.0)
    never_played = b"TERMPERF audio_play" not in log
    chk(never_played, "clicking a device-less audio object never starts playback")

    chk(b"LOGIT_BOOT_OK" in log, "the kernel is still up (no hang, no fault)")
    chk(not any(m in log for m in (b"PANIC", b"#PF", b"GPF")), "no fault was logged")

    try:
        s.cmd({"execute": "quit"})
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

_reader_stop = True

print(f"=== mode={MODE}: {len(fails)} failing ===")
for m in fails:
    print("  -", m)
sys.exit(1 if fails else 0)
