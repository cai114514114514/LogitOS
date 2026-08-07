#!/usr/bin/env bash
# Boot LogitOS headless and screendump the guest's scanout to a PNG.
#
# The point of this is diagnostic separation. When `make run` shows a QEMU entry
# in the taskbar but no picture, there are two very different causes and they
# look identical from the outside: either the guest never drew anything, or it
# drew fine and the host window is not painting it. This reads the framebuffer
# the guest produced, over QMP, with no host window in the path -- so if the
# image is right, the bug is host-side and `make run DISP=...` is where to look.
#
# Usage: tools/shot.sh [seconds-to-wait] [out.png]
set -u

WAIT="${1:-25}"
OUT="${2:-build/desktop.png}"
QEMU="${QEMU:-qemu-system-x86_64}"
SOCK="$(mktemp -u /tmp/logit-shot-XXXXXX.sock)"
PPM="$(mktemp -u /tmp/logit-shot-XXXXXX.ppm)"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$SOCK" "$PPM"
}
trap cleanup EXIT

# -snapshot so a diagnostic run can never modify the disk image.
"$QEMU" -cdrom build/logit.iso \
    -drive file=build/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -cpu "${QEMU_CPU_MODEL:-max}" \
    -rtc base=localtime \
    -vga none -device virtio-gpu-pci,xres=1280,yres=800 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial file:/tmp/logit-shot-serial.log -no-reboot \
    -display none -qmp "unix:$SOCK,server,nowait" >/dev/null 2>&1 &
QPID=$!

# Wait for the socket, then for the desktop to come up. Polling the serial log
# beats a fixed sleep: a slow host would otherwise screendump a blank boot.
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL: qemu never opened its QMP socket"; exit 1; }

for _ in $(seq 1 "$((WAIT * 10))"); do
    grep -aq "desktop live" /tmp/logit-shot-serial.log 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 2                    # let the first frames land after the desktop reports live

python3 - "$SOCK" "$PPM" <<'PY'
import json, socket, sys
sock, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile('rw'); f.readline()
def cmd(c, **a):
    f.write(json.dumps({'execute': c, 'arguments': a} if a else {'execute': c}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
cmd('qmp_capabilities')
r = cmd('screendump', filename=ppm)
if 'error' in r: print('screendump failed:', r['error']); sys.exit(1)
PY
[ -s "$PPM" ] || { echo "FAIL: no screendump produced"; exit 1; }

# PPM -> PNG with no image library: zlib is in the stdlib and PNG's filter-0
# scanline format is a byte length away from raw RGB.
python3 - "$PPM" "$OUT" <<'PY'
import struct, sys, zlib
ppm, out = sys.argv[1], sys.argv[2]
d = open(ppm, 'rb').read()
i = d.index(b'255\n') + 4
w, h = (int(x) for x in d[:i].split()[1:3])
px = d[i:]
raw = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, data):
    c = t + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
open(out, 'wb').write(
    b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(raw, 6))
    + chunk(b'IEND', b''))

# Report whether it actually contains a desktop, so a blank frame is not
# mistaken for success by someone who does not open the file.
step = 7
seen = {px[((y*w + x) * 3):((y*w + x) * 3) + 3] for y in range(0, h, step) for x in range(0, w, step)}
print("%s  %dx%d  %d distinct colours sampled" % (out, w, h, len(seen)))
print("guest IS rendering" if len(seen) > 32 else
      "guest drew (almost) nothing -- this one really is the OS")
PY
