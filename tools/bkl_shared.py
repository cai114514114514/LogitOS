#!/usr/bin/env python3
"""Every file-scope mutable static in the subsystems the BKL still protects.

WHY THIS IS A TOOL AND NOT A PARAGRAPH. c/fs/logitfs.c carries the most honest
comment in the tree about this -- "every op runs under the kernel BKL and the
shared static staging buffers above carry no lock of their own; correctness
relies on the BKL never being dropped mid-operation" -- and it names four of the
ten statics declared immediately above it. Five of the six it omits are the
journal TRANSACTION itself.

If the careful comment lists less than half, prose is the wrong medium. The list
has to come from the declarations.

WHAT IT FINDS, and what it deliberately does not:

  * file-scope `static` objects that are not `const`, in the subsystems named
    below. That is the definition of "shared mutable state a second core could
    reach", and it is syntactic, so it cannot forget.
  * NOT function-local statics. They are equally dangerous and much rarer here;
    a second pass can add them once the file-scope list is dealt with, and
    mixing them in now would bury the buffers that matter under counters.
  * NOT an opinion about whether each one is a bug. Plenty are single-writer by
    construction, or are only touched at boot. The tool's claim is that the LIST
    is complete, not that every entry needs a lock -- and a list you have to
    argue down is worth far more than one you have to remember to extend.

IT IS A LOWER BOUND, and the first thing it missed is worth naming so nobody
quotes the number as exact: `static uint8_t (*tx_bufs)[BS];` in logitfs.c --
a pointer to an array -- does not match the pattern below, and that particular
one is the journal's staged-block storage. A regex over C declarations gets the
common shapes; the pointer-to-array and function-pointer forms need either a
real parser or an added case. Reported as "at least N", never as "N".

Usage:  python3 tools/bkl_shared.py [subsystem ...]
        python3 tools/bkl_shared.py --counts
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The subsystems the M25 P3 audit left BKL-guarded, plus the two the removal
# plan's steps 2 and 3 touch. c/kernel/mm and c/kernel/sched are excluded on
# purpose: they have their own locks (pmm_lock, kheap_lock, g_sched_lock) and
# are not waiting on this work.
DEFAULT = ["c/net", "c/fs", "c/kernel/gui", "c/drivers"]

# `static` at column 0, not const, not a function definition or prototype.
DECL = re.compile(r"^static\s+(?!const\b)(?!inline\b)([A-Za-z_][\w\s\*\(\)\[\]]*?)"
                  r"\b([A-Za-z_]\w*)\s*(\[[^;]*\])?\s*(=[^;]*)?;", re.M)


def scan(path):
    try:
        src = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    # strip comments and strings so a `static` inside either is not a decl
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r'"(\\.|[^"\\])*"', '""', src)
    out = []
    for m in DECL.finditer(src):
        typ, name, arr = m.group(1).strip(), m.group(2), m.group(3) or ""
        if "(" in typ and "*" not in typ:        # function definition
            continue
        out.append((name, (typ + " " + arr).strip()))
    return out


def main(argv):
    counts = "--counts" in argv
    dirs = [a for a in argv[1:] if not a.startswith("--")] or DEFAULT
    total = 0
    for d in dirs:
        base = os.path.join(ROOT, d)
        for dp, _, fns in os.walk(base):
            for fn in sorted(fns):
                if not fn.endswith(".c"):
                    continue
                p = os.path.join(dp, fn)
                found = scan(p)
                if not found:
                    continue
                total += len(found)
                rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
                if counts:
                    print("%4d  %s" % (len(found), rel))
                else:
                    print("%s  (%d)" % (rel, len(found)))
                    for name, typ in found:
                        print("        %-28s %s" % (name, typ))
    print("\nat least %d file-scope mutable static(s) across %s"
          % (total, ", ".join(dirs)))
    print("(a lower bound: see the pointer-to-array note at the top of this file)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
