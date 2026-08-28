#!/usr/bin/env python3
"""AD HOC device proof for the address-bar caret/selection rework -- NOT a
committed gate, just this session's evidence that the rewrite behaves on the
actual guest and not only in a host compile. Modelled on qmp_site.py's boot
sequence (same markers, same paced Session helpers from qmp_ui.py) with
everything unrelated to the address bar stripped out.

WHAT THIS PROVES AND WHAT IT DOES NOT:
QMP injects scancodes beneath the host keyboard (into the PS/2 queue QEMU
emulates), so a pass here proves the GUEST's key-handling logic is correct.
It does NOT prove a person's fingers on a real Mac keyboard produce the same
scancodes -- Ctrl+A/C/X/V, specifically, could in principle be intercepted by
the host OS or by a wrapping application before ever reaching the emulator.
On this project Ctrl (not Cmd) is what carries these chords, and Ctrl is not
one of the shortcuts macOS itself reserves (Cmd is, which is why every OTHER
chord in this browser -- new tab, close tab, bookmark -- is bound to Cmd in
is_cmd()). So the READING here is: Ctrl+A/C/X/V should reach the guest
unmolested inside QEMU's own window (not a browser tab embedding it), same as
every existing forms.c control that already used these exact codes. That is
an argument, not a device measurement -- nobody's physical hand pressed
these keys for this run.

Usage: python3 tests/qmp/qmp_addrbar_probe.py --iso build-addrbar/logit.iso \
    --disk build-addrbar/disk.img
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, dock_icon, BROWSER_SLOT  # noqa: E402


def ctrl(ui, qcode):
    ui.key_mods(("ctrl",), qcode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="addrbar_probe_")
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")

    cmd = [args.qemu, "-cpu", "max", "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    qlog = open(os.path.join(tmp, "qemu.log"), "wb")
    proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)

    def serial(frm=0):
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")[frm:]
        except OSError:
            return ""

    def wait_for(needle, secs, frm=0):
        end = time.time() + secs
        while time.time() < end:
            s = serial(frm)
            if needle in s:
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def settle(mark, secs=25):
        """Wait for a just-fired navigation's fetch to FINISH (pass or fail)
        before sending the next keystroke. The browser is single-threaded and
        cooperative: while it is blocked inside a fetch/retry, the PS/2
        controller QEMU emulates cannot be drained and DROPS scancodes rather
        than queuing them (see qmp_ui.py's key_mods docstring) -- so keys
        typed the instant after Enter, before the fetch settles, are lost,
        not delayed. Waiting for the OUTCOME line rather than the LOAD line
        is what fixed that (measured: test 2's own next navigation vanished
        without this, every key of it silently dropped mid-retry)."""
        end = time.time() + secs
        while time.time() < end:
            s = serial(mark)
            if "[browser] page fetch failed" in s or "[browser] load done" in s:
                break
            if proc.poll() is not None:
                break
            time.sleep(0.3)
        time.sleep(0.3)

    def fail(why):
        print("FAIL: %s" % why)
        print("---- last 4000 bytes of serial ----")
        print(serial()[-4000:])
        try:
            proc.kill()
        except OSError:
            pass
        sys.exit(1)

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print(("PASS " if cond else "FAIL ") + name + (("  -- " + detail) if detail else ""))

    try:
        if not wait_for("LOGIT_BOOT_OK", 300):
            fail("kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            fail("window manager never brought the desktop up")
        time.sleep(3)

        ui = Session(qmp_path, serial=serial_path)
        ui.click_at(*dock_icon(BROWSER_SLOT))
        for _ in range(5):
            if wait_for("launched Browser", 15):
                break
            ui.click_at(*dock_icon(BROWSER_SLOT))
        else:
            fail("the Dock never launched the Browser")
        time.sleep(7)

        # ---- WARM-UP, absorbing a ~20s one-time stall that is NOT the
        # address bar's: the first real network use on this image blocks the
        # whole kernel (route/DHCP negotiation under TCG) for a long single
        # stretch, and the PS/2 controller QEMU emulates has a ONE-BYTE
        # buffer -- see qmp_ui.py's key_mods docstring -- so scancodes sent
        # WHILE the guest cannot drain them are lost, not queued. Measured on
        # the first run of this file: every key of test 1 landed inside that
        # exact window and none of it arrived. Eating the stall here, on a
        # navigation nothing below depends on, means the timed tests start
        # once the guest is actually consuming its input again. */
        mark0 = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:1/warmup")
        ui.key("ret")
        wait_for("[browser] load: http://10.0.2.2:1/warmup", 45, mark0)
        wait_for("[browser] page fetch failed", 45, mark0)
        time.sleep(2)

        # ---------------------------------------------------------------
        # TEST 1: Ctrl+A + a short replacement clears a long URL in ONE
        # edit, where the owner's original complaint needed ~62 Backspaces.
        # ---------------------------------------------------------------
        # The owner's own example is
        # "https://www.google.com/search?q=python&sei=O4aRavHyJ-Cqur8PtPOCsQM"
        # (69 bytes) -- '&' has no mapping in qmp_ui.py's KMAP/SHIFT tables
        # (nothing in this tree's other QMP drivers ever needed to TYPE one),
        # so this is the same shape and length without it, rather than
        # growing a third copy of that table for one character. The
        # replacement is "http://10.0.2.2:1/x", not the bare "x" the task's
        # own example implies, because a SEPARATE fix landed in this same
        # file this session -- addr_infer_scheme() -- and it deliberately
        # REFUSES to navigate a bare word with no dot and no scheme ("no
        # search engine is configured here"). That is correct behaviour for
        # that feature, not a bug in this one; "x" alone would leave
        # `editing` true and print no load line at all, which would look
        # like this test failing. A local target (rather than a real
        # hostname like the owner's own example) is deliberate too: this
        # sandbox's SLIRP resolves ANY hostname to a placeholder IP and then
        # spends several real seconds retrying the connection, which stalls
        # the single-threaded guest long enough to drop the NEXT test's
        # keystrokes -- measured, not guessed, on an earlier run of this
        # file. 19 keystrokes replacing 60+ is still the point being proven.
        long_url = "https://www.google.com/search?q=python-tutorial-guide-012345"
        mark = len(serial())
        ctrl(ui, "t")
        ui.typ(long_url)
        ctrl(ui, "a")  # Ctrl+A: select all
        ui.typ("http://10.0.2.2:1/x")
        ui.key("ret")
        expect1 = "[browser] load: http://10.0.2.2:1/x"
        ok1 = wait_for(expect1, 20, mark)
        check("Ctrl+A then a short replacement replaces the whole address",
              ok1, "expected %r" % expect1)
        settle(mark)

        # ---------------------------------------------------------------
        # TEST 2: KEY_LEFT moves the caret while editing instead of firing
        # history back -- and the resulting mid-string edit is byte-correct.
        # ---------------------------------------------------------------
        mark2 = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:1/first")
        ui.key("ret")
        if not wait_for("[browser] load: http://10.0.2.2:1/first", 20, mark2):
            fail("first navigation of test 2 never reached load_once")
        settle(mark2)

        markb = len(serial())
        ctrl(ui, "l")  # focus + SELECT (not clear) the current address
        ui.typ("http://10.0.2.2:1/second")  # types over the selection
        ui.key("ret")
        if not wait_for("[browser] load: http://10.0.2.2:1/second", 20, markb):
            fail("second navigation of test 2 never reached load_once")
        settle(markb)

        mark3 = len(serial())
        ctrl(ui, "l")          # select-all "http://10.0.2.2:1/second"
        ui.key("end")          # deselect, caret at the end (no history call)
        ui.key("left")
        ui.key("left")
        ui.key("left")         # caret now 3 UTF-8 chars from the end: ".../sec|ond"
        # THE DIFFERENTIAL: under the OLD code, KEY_LEFT called hist_go(-1)
        # unconditionally -- with a real entry ("first") on the stack, that is
        # outcome 1 (a REAL navigation), which fires load_once and prints a
        # SECOND "[browser] load: http://10.0.2.2:1/first" line immediately,
        # with no Enter pressed. Seeing that line here means the bug is back.
        no_stray_nav = "[browser] load: http://10.0.2.2:1/first" not in serial(mark3)
        check("KEY_LEFT during editing does not fire history navigation",
              no_stray_nav,
              "a second 'load: .../first' appeared with no Enter pressed" if not no_stray_nav else "")

        ui.typ("ZZZ")  # not "!!!" -- '!' has no qmp_ui.py KMAP/SHIFT mapping
        ui.key("ret")
        expect = "[browser] load: http://10.0.2.2:1/secZZZond"
        ok2 = wait_for(expect, 20, mark3)
        check("mid-string caret + insert lands exactly between 'sec' and 'ond'",
              ok2, "expected %r" % expect)
        settle(mark3)

        # ---------------------------------------------------------------
        # TEST 3: Backspace at the caret (not always the end), and Ctrl+C
        # + Ctrl+V round-trip through the real kernel clipboard.
        # ---------------------------------------------------------------
        mark4 = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:1/abcdef")
        ui.key("left"); ui.key("left"); ui.key("left")  # caret before "def"
        ui.key("backspace")  # should remove 'c', leaving ".../abdef"
        ui.key("ret")
        expect3 = "[browser] load: http://10.0.2.2:1/abdef"
        ok3 = wait_for(expect3, 20, mark4)
        check("Backspace deletes AT THE CARET, not always the last byte",
              ok3, "expected %r" % expect3)
        settle(mark4)

        mark5 = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:1/clipcopy")
        ctrl(ui, "a")
        ctrl(ui, "c")           # copy the selection to the kernel clipboard
        time.sleep(0.1)
        ui.key("end")
        ctrl(ui, "v")           # paste it back, doubling the address
        ui.key("ret")
        expect4 = "[browser] load: http://10.0.2.2:1/clipcopyhttp://10.0.2.2:1/clipcopy"
        ok4 = wait_for(expect4, 20, mark5)
        check("Ctrl+C / Ctrl+V round-trip through SYS_CLIP_SET/GET",
              ok4, "expected %r" % expect4)

        # ---------------------------------------------------------------
        # TEST 4: the UTF-8 clamp, driven through the REAL pinyin IME (not a
        # synthetic multi-byte insert) -- because "this machine has a Chinese
        # IME" is exactly the case the caret-stepping comment in browser.c
        # names as the risk. Shift+Space, "nihao", Space commits candidate 1
        # -- same sequence tests/boot/ime_type.py already proves reaches
        # TextEdit -- typed here into the ADDRESS BAR instead. Left three
        # times crosses "B", then one whole Han character "好", then lands
        # between "A" and "你" (each Han character is ONE addr_step, not
        # three) -- so Delete there must remove exactly "你" (3 bytes, E4 BD
        # A0) and leave "好" (E5 A5 BD) intact, byte for byte. A caret that
        # can land mid-character would split one of those two Han characters
        # instead of removing either whole -- and load()'s own RFC 3986
        # percent-encoding of every non-ASCII byte (see its comment) turns
        # that split into a plainly wrong hex byte in the load: line, not
        # something that needs a second decoder to notice.
        # ---------------------------------------------------------------
        mark6 = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:1/A")
        markime = len(serial())
        ui.key_shift("spc")
        ime_on = wait_for("pinyin ON", 10, markime)
        check("Shift+Space turns the address bar's IME on", ime_on)
        if ime_on:
            for q in ("n", "i", "h", "a", "o"):
                ui.key(q)
            ui.key("spc")       # commit candidate 1: inserts U+4F60 U+597D (你好)
            ui.typ("B")
            ui.key("left"); ui.key("left"); ui.key("left")  # .../A|你好B -> .../A你好|B -> .../A你|好B -> .../A|你好B
            ui.key("delete")    # forward-delete AT THE CARET: removes '你' whole
            ui.key("ret")
            expect6 = "[browser] load: http://10.0.2.2:1/A%E5%A5%BDB"
            ok6 = wait_for(expect6, 20, mark6)
            check("forward-delete removes one whole Han character (你), not a torn byte",
                  ok6, "expected %r" % expect6)
        else:
            check("forward-delete removes one whole Han character (你), not a torn byte",
                  False, "skipped: IME never turned on, see the check above")

        n_ok = sum(1 for _, ok, _ in checks if ok)
        print("\n%d/%d checks passed" % (n_ok, len(checks)))
        try:
            proc.kill()
        except OSError:
            pass
        sys.exit(0 if n_ok == len(checks) else 1)
    except SystemExit:
        raise
    except Exception as e:
        fail("exception: %r" % (e,))


if __name__ == "__main__":
    main()
