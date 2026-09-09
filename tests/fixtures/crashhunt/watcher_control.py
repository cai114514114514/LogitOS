#!/usr/bin/env python3
"""The crashhunt serial-classifier control: reflexes, both directions.

Usage:
    python3 watcher_control.py <log-out>          # positive: all reflexes correct
    CH_SABOTAGE=1 python3 watcher_control.py <log-out>   # negctl: must FAIL (exit 3)

WHY THIS EXISTS (rule 1 and rule 5 of AGENTS.md): the crashhunt verdicts are
manufactured from serial text by tests/qmp/qmp_crashhunt.py's Watcher. A
classifier that has never been shown to mis-classify is not evidence of
anything, and the specific way it WILL rot is known: somebody "simplifies"
the oom handling and `[oom] pmm_alloc refused` (PRESSURE, printed by the
hundreds under load) becomes a death verdict, turning every memory-tight run
into BROWSER-DIED-OOM. The sabotage mode performs exactly that edit in memory
and requires this control to catch it.

Exits: 0 = all reflexes correct; 3 = sabotage detected (the NEGCTL's expected
red); 1 = an unrelated failure (also red, but named as not-the-sabotage).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, "qmp"))
from qmp_crashhunt import Watcher                     # noqa: E402


class FakeProc:
    """The watcher only ever calls poll() on the process object."""
    def poll(self):
        return None


CASES = [
    # (name, serial text, death?, the one line that justifies the verdict)
    ("fault", "[fault] app exception: page fault (vector 14) rip=0x1 -- terminating app\n",
     True, "a ring-3 fault kill is a death"),
    ("panic", "LOGIT_PANIC: kernel heap corruption at 0x1234\n", True,
     "a kernel panic is a death (of everything)"),
    ("oom-victim", '[oom] page fault: out of memory -- 3 frames free of 131072, 2 processes\n'
                   '[oom] victim: pid 4 "browser" rss=91000 frames (355 MiB) [window] -- the largest process\n',
     True, "an oom VICTIM mark on the browser is a death"),
    ("oom-pressure", "[oom] pmm_alloc refused (256 so far); free=900 reserve=128\n"
                     "[oom] kmalloc(64) refused -- 1 refusals so far, 900 frames free\n",
     False, "refusals are PRESSURE, not death -- the module comment says so"),
    ("oom-reaped", "[oom] page fault: 1024 frames (4096 KiB) recovered from 1 process(es) "
                   "that had already exited -- nobody killed\n",
     False, "a reap that answered the shortage killed nobody"),
    ("watchdog", "[js] watchdog: script exceeded its CPU slice (wall time) -- interrupted\n"
                 "[js] watchdog: fuel=22258 since_begin_ms=45010\n",
     False, "the watchdog aborts the SCRIPT; the browser survives"),
    ("quiet", "[wm] perf t=1000 composites=10\n[time] tickloss; uptime 10s\n",
     False, "an ordinary quiet serial is no death"),
]


def run(log_path):
    failures = []
    for name, text, want_death, why in CASES:
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
            fh.write(text)
            spath = fh.name
        try:
            w = Watcher(spath, FakeProc())
            time.sleep(0.6)          # one poll period of the watcher thread
            w.stop.set()
            got = w.death.is_set()
            line = "%-13s want_death=%-5s got=%-5s -- %s" % (name, want_death, got, why)
            print(line)
            if got != want_death:
                failures.append(line)
        finally:
            os.unlink(spath)
    return failures


def main():
    log_path = sys.argv[1]
    sab = os.environ.get("CH_SABOTAGE") == "1"

    if sab:
        # THE EDIT THAT WILL HAPPEN SOMEDAY, made here instead: treat every
        # [oom] line as a death, which merges pressure into verdicts.
        import qmp_crashhunt
        orig = qmp_crashhunt.Watcher._parse

        def sabotaged(self, text):
            orig(self, text)
            for ln in text.splitlines():
                if ln.startswith("[oom]"):
                    self.death.set()

        qmp_crashhunt.Watcher._parse = sabotaged

    failures = run(log_path)
    with open(log_path, "w") as fh:
        fh.write("\n".join(failures) if failures else "all reflexes correct")

    if not sab and failures:
        print("FAIL: %d misclassification(s): %s" % (len(failures), failures))
        return 1
    if sab:
        # The sabotage must have been CAUGHT by the oom-pressure or oom-reaped
        # case (both become deaths under it). Anything else is not the edit we
        # claimed to test.
        caught = any("pressure" in f or "reaped" in f for f in failures)
        if caught:
            print("SABOTAGE-CAUGHT: pressure lines read as deaths: %s" % failures)
            return 3
        print("SABOTAGE-MISSED: the sabotage had no effect on the reflexes "
              "-- the control is not testing what it claims")
        return 1
    print("watcher reflexes: %d cases ok" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
