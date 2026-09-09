#!/usr/bin/env python3
"""guest-diff.py -- slice the guest serial transcript into per-case stdout and
diff each case against the node oracle output produced by run-objweak.sh.

WHY A SEPARATE STEP. run-guest.sh boots QEMU, runs /bin/jssem over every case
and prints "guest transcript: <path>" -- and there it stopped. Nothing compared
it to anything, so the run that COUNTS (the guest engine, linked from
$(ENGINE_OBJ), the literal objects build/browser.elf links) produced a log a
human had to read. The host differential had a byte comparison and the guest did
not, which is backwards: CLAUDE.md's rule is that the host target is the
iteration loop and the guest is the evidence.

The BEGIN/END markers js_sem_probe.c prints exist for exactly this ("so the
guest run can be sliced out of a serial log" -- its own comment); this is the
consumer they were written for.

THE SERIAL LOG IS NOT A CLEAN STDOUT and that is the whole difficulty: the
kernel writes boot lines, the shell writes a prompt, and CR/LF is the serial
convention. Only the bytes between BEGIN and END for a given case are taken,
and CR is stripped. Anything this cannot attribute to a case is reported rather
than dropped -- a slicer that silently loses a line would turn a real difference
into a green.

    guest-diff.py <guest.log> <oracle-dir>

<oracle-dir> is run-objweak.sh's output directory, which holds <case>.node.out
for every case. Exit 1 if any case differs or any case is missing.
"""
import os
import re
import sys


# Kernel diagnostics share the serial line with the program's stdout, so a
# "[time] tickloss ..." or "[mm] low: ..." can land in the MIDDLE of a case.
# They must be removed, and it matters that they are removed rather than
# tolerated: they do not merely add a line, they SHIFT every following line by
# one, so a positional comparison reports the whole remainder of the case as
# differing. Measured: two cases (08-regexp, 11-intl-shipped) read as
# "HOST/GUEST DISAGREE" on ~60 lines that were in fact identical and offset --
# which is exactly the shape CLAUDE.md warns about, an instrument reporting the
# largest possible finding for a reason that has nothing to do with the subject.
# The count of dropped lines is PRINTED rather than swallowed, so this filter
# can never quietly eat a line the engine produced.
KERNEL_NOISE = re.compile(r"^\[[a-z]+\] ")


def slice_cases(text, dropped):
    """Return {case_name: [lines]} for every BEGIN/END pair in the transcript."""
    cases, cur, buf = {}, None, []
    for raw in text.split("\n"):
        line = raw.replace("\r", "")
        if KERNEL_NOISE.match(line):
            dropped.append(line)
            continue
        m = re.match(r"^=== BEGIN (\S+)", line)
        if m:
            cur, buf = os.path.basename(m.group(1)), []
            continue
        m = re.match(r"^=== END (\S+)", line)
        if m:
            if cur is not None:
                cases[cur] = buf
            cur, buf = None, []
            continue
        if cur is not None:
            buf.append(line)
    if cur is not None:
        cases[cur] = buf          # unterminated: keep it, and it will be flagged
    return cases


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: guest-diff.py <guest.log> <oracle-dir>")
    guest_log, oracle_dir = sys.argv[1], sys.argv[2]
    text = open(guest_log, "r", errors="replace").read()
    if "JSSEM-DONE" not in text:
        sys.exit("FAIL: transcript has no JSSEM-DONE -- the guest did not finish")
    dropped = []
    cases = slice_cases(text, dropped)
    if dropped:
        print("kernel serial lines removed from the case bodies: %d" % len(dropped))
        for d in dropped:
            print("    | %s" % d)
        print("")

    bad = 0
    ctl_fired = False
    for name in sorted(cases):
        base = name[:-3] if name.endswith(".js") else name
        oracle = os.path.join(oracle_dir, base + ".node.out")
        if not os.path.exists(oracle):
            print("MISSING-ORACLE %s" % base)
            bad += 1
            continue
        want = open(oracle, "r", errors="replace").read().split("\n")
        while want and want[-1] == "":
            want.pop()
        got = list(cases[name])
        while got and got[-1] == "":
            got.pop()

        # 99-control is the harness's planted difference. run-objweak.sh injects
        # one value per SIDE, and the guest is handed the case file raw with no
        # injection at all, so this case MUST differ here and its differing is
        # the evidence that this comparison can see a difference. Rule 5.
        if base == "99-control":
            ctl_fired = got != want
            print("%-18s %s" % (base, "control fired" if ctl_fired else
                                "CONTROL DID NOT FIRE"))
            continue

        if got == want:
            print("%-18s SAME (%d lines)" % (base, len(got)))
        else:
            n = sum(1 for a, b in zip(want, got) if a != b) + abs(len(want) - len(got))
            print("%-18s DIFF (%d lines)" % (base, n))
            bad += 1
            for a, b in zip(want, got):
                if a != b:
                    print("    node   : %s" % a)
                    print("    guest  : %s" % b)
            for extra in want[len(got):]:
                print("    node-only  : %s" % extra)
            for extra in got[len(want):]:
                print("    guest-only : %s" % extra)

    print("\n%d cases sliced, %d differ" % (len(cases), bad))
    if not ctl_fired:
        print("HARNESS BROKEN: the control did not differ; this comparison "
              "detects nothing.")
        return 2
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
