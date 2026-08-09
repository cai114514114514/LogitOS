#!/usr/bin/env python3
"""Type a password into the desktop's greeter, like a human, and watch it refuse
one first.

    python3 tests/qmp/qmp_greeter.py <iso> <disk.img>

WHY THIS AND NOT THE BOOT HARNESS. tests/boot/run-desktop-test.sh already proves
the desktop does not start until somebody authenticates -- but it authenticates
on the SERIAL CONSOLE. That says nothing about whether the greeter itself takes
a password, masks it, refuses the wrong one, or is even drawn. This drives the
real machine: the emulated PS/2 keyboard types into the real window, and the
answer is read off three independent channels, because any one of them alone can
lie.

  1. THE SERIAL LINE. GREETER-DENIED for the wrong password, GREETER-OK with the
     uid for the right one. This is what the program believes happened.

  2. THE PIXELS. The greeter paints a 6x6 probe in a colour nothing else on this
     desktop uses, so the harness can find it without OCR. Typing must make INK
     APPEAR in the password field -- a field that accepts characters invisibly
     is a different bug, not a fixed one -- and after the right password the
     probe colour must be GONE FROM THE SCREEN. "The machine let me in" is then
     a fact about what is drawn, not only a line on a wire.

  3. THE WINDOW MANAGER'S LAUNCHER. "[wm] launched Finder" must appear only
     AFTER the unlock. That is the mechanism rather than the appearance: the
     lock is wm_launch() refusing, and the log says whether it refused.

TWO QEMU RUNS, ONE DISK, NO -snapshot ON THE FIRST. There is no credential in
the shipped image and there never will be, so the account this signs in as has
to be created first -- by typing a password at the console on a scratch copy of
the disk, exactly as a human would. The second run boots the same disk and finds
a greeter in front of it.

THE NEGATIVE CONTROL is `make test-greeter-negctl`, which builds this same
machine with -DLOGIN_NEGCTL_ACCEPT_ANY (one flag, one definition of the check --
acct_check_password in c/apps/coreutils/accounts.h -- so it covers the console
login and the greeter together). The greeter appears, the field masks, the
timing line prints, the desktop unlocks, and every password is correct. The
first assertion below is the one that must then go red.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM                                   # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/logit.iso"
DISK = sys.argv[2] if len(sys.argv) > 2 else "build/disk.img"
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# A TEST PASSWORD FOR A DISPOSABLE IMAGE, typed at a prompt on a scratch copy of
# the disk that is deleted when this exits. Nothing is written into
# build/disk.img and nothing is shipped. Lowercase and hyphens only, because
# every character has to go through a QEMU qcode and this is not the place to
# discover that one of them does not.
PW = "correct-horse-battery"
BADPW = "correct-horse-batteru"

PROBE = (0x00, 0xE5, 0xC8)          # c/apps/gui/greeter.c
XRES, YRES = 1280, 800

fails = []


def ck(cond, what, detail=""):
    print("%-4s %s%s" % ("ok" if cond else "FAIL", what, ("  [%s]" % detail) if detail else ""))
    if not cond:
        fails.append(what)


def read(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read().replace("\r", "")
    except OSError:
        return ""


def wait_for(path, text, secs, proc=None):
    """Wait for `text` on the serial log. Never sleep at a boot -- the rule
    tests/boot/bootwait.sh exists for, and the reason eleven harnesses stopped
    reporting 'never finished its commands' when nothing had been typed."""
    end = time.time() + secs
    while time.time() < end:
        if text in read(path):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.25)
    return False


def qemu_args(disk, serial_to_file, tmp, snapshot):
    a = [QEMU, "-cdrom", ISO,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
         "-display", "none", "-no-reboot"]
    if snapshot:
        a.append("-snapshot")
    if serial_to_file:
        a += ["-serial", "file:" + os.path.join(tmp, "s2.log")]
    else:
        a += ["-serial", "stdio"]
    return a


def kill(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.kill()
    for _ in range(100):
        if proc.poll() is not None:
            return
        time.sleep(0.1)


# ---------------------------------------------------------------- run 1 -----
# Enrol, by typing. The shipped image has no account, so this is the human
# creating one -- and it is also, incidentally, the assertion that a machine
# with no accounts still comes up unlocked with a root shell on the console.
def enrol(tmp, disk):
    log = os.path.join(tmp, "s1.log")
    proc = subprocess.Popen(qemu_args(disk, False, tmp, snapshot=False),
                            stdin=subprocess.PIPE, stdout=open(log, "wb"),
                            stderr=subprocess.DEVNULL)

    def send(s, settle=0.4):
        proc.stdin.write((s + "\n").encode())
        proc.stdin.flush()
        time.sleep(settle)

    try:
        if not wait_for(log, "LogitOS shell", 240, proc):
            return log, False
        ck("[wm] no accounts on this machine" in read(log),
           "with no accounts the desktop comes up unauthenticated, as it always did")
        ck("[wm] launched Finder" in read(log),
           "...and it launches the file manager, which sixty other harnesses expect")
        send("login -a alice")
        if not wait_for(log, "New password:", 60, proc):
            return log, False
        send(PW)
        if not wait_for(log, "Retype password:", 60, proc):
            return log, False
        send(PW)
        ok = wait_for(log, "ENROLLED alice", 180, proc)
        time.sleep(3)          # let the store reach the medium before the kill
        return log, ok
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        kill(proc)


# ---------------------------------------------------------------- run 2 -----
def ink(ppm_path, x0, y0, x1, y1):
    """How many pixels in a box are not the background it started as. Compared
    between two shots rather than against a fixed colour, so the count survives
    a theme change and a font change."""
    p = PPM(ppm_path)
    out = []
    for y in range(y0, min(y1, p.h)):
        for x in range(x0, min(x1, p.w)):
            o = (y * p.w + x) * 3
            out.append((p.px[o], p.px[o + 1], p.px[o + 2]))
    return out


def differing(a, b):
    return sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])


def main():
    tmp = tempfile.mkdtemp(prefix="logit-greeter-")
    disk = os.path.join(tmp, "disk.img")
    subprocess.run(["cp", DISK, disk], check=True)

    log1, ok = enrol(tmp, disk)
    ck(ok, "an account was created by typing a password at the console")
    if not ok:
        print("---- run 1 tail ----")
        print("\n".join(read(log1).splitlines()[-25:]))
        return 1

    sock = os.path.join(tmp, "qmp.sock")
    log = os.path.join(tmp, "s2.log")
    proc = subprocess.Popen(qemu_args(disk, True, tmp, snapshot=True) +
                            ["-qmp", "unix:%s,server,nowait" % sock],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for(log, "GREETER-READY", 300, proc):
            ck(False, "the greeter came up on a machine that has an account")
            print("\n".join(read(log).splitlines()[-25:]))
            return 1
        ck("[wm] LOCKED: /etc/passwd exists" in read(log),
           "the machine came up LOCKED because it has an account")
        ck("[wm] launched Finder" not in read(log),
           "THE DESKTOP HAS NOT STARTED: nothing is running behind the greeter")
        time.sleep(4)

        ui = Session(sock, serial=log)
        empty = os.path.join(tmp, "empty.ppm")
        ui.screendump(empty, settle=1.0)

        p = PPM(empty)
        box = p.find_color(PROBE)
        ck(box is not None, "the greeter is ON SCREEN (its probe is in the frame)")

        # The password field, in device pixels. Derived the same way the greeter
        # derives it (c/apps/gui/greeter.c paint()), from the screen size, at
        # scale 100 -- so this is a band of the картинка and not a magic number:
        #   card 380x250 centred, field at card-local (28, 178) size (324, 36).
        cw, ch = 380, 250
        cx, cy = (XRES - cw) // 2, (YRES - ch) // 2 - 20
        fx, fy, fw, fh = cx + 28, cy + 178, cw - 56, 36
        before = ink(empty, fx, fy, fx + fw, fy + fh)

        # --- the WRONG password ------------------------------------------
        # The greeter pre-filled the only account's name and put the cursor in
        # the password field, so this types straight into it.
        ui.typ(BADPW)
        typed = os.path.join(tmp, "typed.ppm")
        ui.screendump(typed, settle=1.0)
        after = ink(typed, fx, fy, fx + fw, fy + fh)
        ck(differing(before, after) > 40,
           "typing put INK in the password field", "%d px changed" % differing(before, after))

        ui.key("ret")
        got = wait_for(log, "GREETER-DENIED", 120, proc)
        ck(got, "A WRONG PASSWORD WAS REFUSED. This is the assertion the negative\n"
                "     control (make test-greeter-negctl) must turn red")
        ck("GREETER-OK" not in read(log),
           "...and it did not let anybody in")
        ck("[wm] launched Finder" not in read(log),
           "...and the desktop still has not started")
        denied = os.path.join(tmp, "denied.ppm")
        ui.screendump(denied, settle=1.0)
        ck(PPM(denied).find_color(PROBE) is not None,
           "the greeter is still on screen after the refusal")

        # --- the RIGHT password -------------------------------------------
        ui.typ(PW)
        ui.key("ret")
        ck(wait_for(log, "GREETER-OK alice uid=1000 gid=1000", 180, proc),
           "the right password signed alice in as uid 1000")
        ck(wait_for(log, "[wm] UNLOCKED by session uid=1000", 60, proc),
           "the window manager unlocked on the SESSION, not on a message")
        ck(wait_for(log, "[wm] launched Finder", 60, proc),
           "the desktop started -- and only now")
        ck(wait_for(log, "SETTINGS_USER uid=1000 store=/home/alice/.config/settings.conf", 60, proc),
           "signing in switched the settings store to her home")
        time.sleep(4)
        desk = os.path.join(tmp, "desktop.ppm")
        ui.screendump(desk, settle=1.5)
        ck(PPM(desk).find_color(PROBE) is None,
           "the greeter is GONE from the screen")

        # The screenshots are worth keeping: they are the only artifact that
        # shows what a person actually sees.
        for name in ("empty", "typed", "denied", "desktop"):
            src = os.path.join(tmp, name + ".ppm")
            dst = os.path.join("build", "greeter-%s.ppm" % name)
            if os.path.exists(src):
                subprocess.run(["cp", src, dst], check=False)
    finally:
        kill(proc)

    print()
    if fails:
        print("FAIL: %d of the greeter's claims did not hold" % len(fails))
        for f in fails:
            print("  - " + f)
        print("  (serial log tail)")
        print("\n".join(read(log).splitlines()[-20:]))
        return 1
    print("PASS: the greeter drew itself, took a password, REFUSED the wrong one,")
    print("      accepted the right one as uid 1000, and only then did the")
    print("      desktop start. Screenshots in build/greeter-*.ppm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
