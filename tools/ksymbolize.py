#!/usr/bin/env python3
"""Turn a kprof report's addresses into function names, host-side.

    cat /dev/kprof > prof.txt          # on the machine
    python3 tools/ksymbolize.py prof.txt

    # a ring-3 profile as well as the kernel one:
    python3 tools/ksymbolize.py prof.txt --user build/browser.elf

WHY HOST-SIDE, AND WHETHER AN IN-KERNEL SYMBOL TABLE IS WORTH IT
----------------------------------------------------------------
The panic path already resolves addresses against build/kernel.map in its
harness (tests/boot/run-panic-test.sh), and the linker map is emitted by every
build for free.  Reusing it costs nothing, adds nothing to the kernel image and
cannot go stale relative to the binary it came from -- the map and the ELF are
written by the same link.

A kallsyms-style embedded table would let `cat /dev/kprof` print names on a
machine with no host tree.  That is a real convenience and it is the obvious
follow-up the panic line flagged, but it is NOT free and it should be spent
deliberately:

  - it costs image size (this kernel has ~3000 symbols; a compressed name blob
    plus an address array is 40-60 KiB, against a 3.5 MB image -- so the size is
    not the objection);
  - it costs a TWO-PASS LINK.  The table's contents depend on the final symbol
    addresses, which depend on the table's size.  Every scheme for this
    (Linux's included) links once, generates, and links again with padding
    held constant.  That is a real change to the build for every developer, and
    a class of "the map disagrees with the binary" failure this tree does not
    have today;
  - and it buys nothing for the case that motivated the profiler.  The five
    lines waiting on this are iterating on a host with the tree checked out.

So: host-side now.  The argument for kallsyms becomes strong the day someone
profiles on real hardware away from a build tree, and on that day the honest
starting point is that the DUMP FORMAT already carries raw addresses, so
nothing written now has to be redone.

FOLDING
-------
Sampling gives a histogram of instruction pointers.  What anyone actually wants
is a histogram of FUNCTIONS, so this folds every address into the symbol that
contains it and sums.  The per-address detail is kept and printed under each
function with --lines, because "which instruction" is the question you ask
second, after "which function".
"""

import argparse
import bisect
import re
import subprocess
import sys

MAP_LINE = re.compile(
    r'^\s*([0-9a-f]{6,16})\s+[0-9a-f]{6,16}\s+([0-9a-f]+)\s+\d+\s{4,}(\S+)\s*$')


def load_map(path):
    """Parse ld.lld's -Map output.  Same shape the panic harness reads."""
    syms = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MAP_LINE.match(line)
            if not m:
                continue
            name = m.group(3)
            if name.startswith('.') or '/' in name:
                continue
            syms.append((int(m.group(1), 16), int(m.group(2), 16), name))
    syms.sort()
    return syms


def load_elf(path):
    """Symbols out of an ELF via nm.  Used for ring-3 images, which have no map."""
    syms = []
    for tool in ('llvm-nm', 'nm', 'x86_64-elf-nm'):
        try:
            out = subprocess.run([tool, '--defined-only', '-S', path],
                                 capture_output=True, text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 4 and f[2].lower() in 'tw':
                try:
                    syms.append((int(f[0], 16), int(f[1], 16), f[3]))
                except ValueError:
                    pass
            elif len(f) >= 3 and f[1].lower() in 'tw':
                try:
                    syms.append((int(f[0], 16), 0, f[2]))
                except ValueError:
                    pass
        if syms:
            break
    syms.sort()
    return syms


class Table:
    def __init__(self, syms):
        self.syms = syms
        self.addrs = [a for a, _, _ in syms]

    def lookup(self, a):
        if not self.syms:
            return None, 0
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return None, 0
        base, size, name = self.syms[i]
        # A size of 0 means the map did not record one; fall back to the gap to
        # the next symbol rather than claiming an unbounded match.
        limit = size if size else (
            self.addrs[i + 1] - base if i + 1 < len(self.addrs) else 1 << 20)
        if a - base >= limit:
            return None, 0
        return name, a - base


SAMPLE = re.compile(r'^([0-9a-f]{16})\s+(\d+)\s+\S+\s+([ku])\s*$')
SPAN = re.compile(r'^span\s+(\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('report', nargs='?', default='-',
                    help='a `cat /dev/kprof` dump (default: stdin)')
    ap.add_argument('--map', default='build/kernel.map',
                    help='linker map for ring-0 addresses')
    ap.add_argument('--user', default=None,
                    help='ELF for ring-3 addresses (e.g. build/browser.elf)')
    ap.add_argument('--lines', action='store_true',
                    help='also list the individual addresses inside each function')
    ap.add_argument('--top', type=int, default=25, help='functions to print')
    args = ap.parse_args()

    text = sys.stdin.read() if args.report == '-' else open(
        args.report, encoding='utf-8', errors='replace').read()

    try:
        ktab = Table(load_map(args.map))
    except OSError as e:
        print('warning: %s' % e, file=sys.stderr)
        ktab = Table([])
    utab = Table(load_elf(args.user)) if args.user else Table([])

    header = {}
    for line in text.splitlines():
        m = re.match(r'^([a-z_]+)\s+(\S.*)$', line)
        if m and m.group(1) not in ('span',):
            header.setdefault(m.group(1), m.group(2).strip())

    integrity = None
    for line in text.splitlines():
        if line.startswith('KPROF_INTEGRITY'):
            integrity = line.strip()

    samples, spans = [], []
    for line in text.splitlines():
        m = SAMPLE.match(line.strip())
        if m:
            samples.append((int(m.group(1), 16), int(m.group(2)), m.group(3)))
            continue
        m = SPAN.match(line.strip())
        if m:
            spans.append((m.group(1), int(m.group(2)), int(m.group(3)),
                          int(m.group(4)), int(m.group(5))))

    if not samples and not spans:
        print('no kprof samples or spans found in the report', file=sys.stderr)
        return 1

    for k in ('state', 'run_ns', 'samples', 'samples_kernel', 'samples_user',
              'sample_hz_measured', 'cores_sampled', 'overflow', 'distinct_rips'):
        if k in header:
            print('%-18s %s' % (k, header[k]))
    if integrity:
        # Printed unmissably: if the accumulator tore, every share below it is
        # wrong by an unknown amount and nothing else on this page is evidence.
        print(integrity if integrity.split()[1] == 'ok'
              else '!!! %s -- the numbers below are NOT trustworthy' % integrity)
    print()

    if spans:
        print('%-22s %8s %14s %14s %14s' % ('span', 'count', 'total_ms',
                                            'max_us', 'avg_us'))
        total = sum(t for _, _, t, _, _ in spans) or 1
        for name, cnt, tot, mx, avg in sorted(spans, key=lambda r: -r[2]):
            print('%-22s %8d %14.3f %14.1f %14.1f   %5.1f%%'
                  % (name, cnt, tot / 1e6, mx / 1e3, avg / 1e3, 100.0 * tot / total))
        print()

    if samples:
        total = sum(h for _, h, _ in samples)
        folded = {}
        for addr, hits, ring in samples:
            tab = utab if ring == 'u' else ktab
            name, off = tab.lookup(addr)
            key = (ring, name if name else '0x%x' % (addr & ~0xFFF))
            e = folded.setdefault(key, [0, []])
            e[0] += hits
            e[1].append((addr, hits, off if name else None))

        print('%7s %10s  %s' % ('share', 'hits', 'function'))
        shown = sorted(folded.items(), key=lambda kv: -kv[1][0])[:args.top]
        for (ring, name), (hits, addrs) in shown:
            print('%6.2f%% %10d  %s%s' % (100.0 * hits / total, hits, name,
                                          '  [ring 3]' if ring == 'u' else ''))
            if args.lines:
                for addr, h, off in sorted(addrs, key=lambda a: -a[1]):
                    loc = '+0x%x' % off if off is not None else ''
                    print('         %10d    %016x %s' % (h, addr, loc))
        unresolved = sum(h for (r, n), (h, _) in folded.items()
                         if n.startswith('0x'))
        if unresolved:
            print('\n%.1f%% of samples did not resolve to a symbol '
                  '(wrong map, or a ring-3 profile with no --user ELF)'
                  % (100.0 * unresolved / total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
