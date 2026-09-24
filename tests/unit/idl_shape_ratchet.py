#!/usr/bin/env python3
"""The shape gate's verdict over a shape.js dump (tests/fixtures/webidl_shape).

    idl_shape_ratchet.py REF.json OURS.json BASELINE.json [--write] [--control]

Two questions, answered from the dump alone -- so the SAME script can be pointed
at a guest dump (tests/qmp/qmp_site.py over shape.html) as at the host one:

  MUST-HOLD   things the shape pass exists to make true, each named:
                document owns nothing Chrome's does not   (['location'])
                navigator/screen/history/performance/crypto own nothing
                Object.prototype.toString.call(navigator) is '[object Navigator]'
                Performance.prototype chains to EventTarget.prototype
                Screen and Crypto exist as interfaces
  RATCHET     every category tests/qmp/webidl_shape_diff.py counts is at or
              below BASELINE. A count BELOW it is reported, never failed: the
              baseline is lowered on purpose (make idl-shape-baseline), in a
              commit that says why, not silently by the gate.

--write     rewrite BASELINE from OURS (the must-hold assertions still apply).
--control   the negative control's mode: OURS comes from a build with the pass
            compiled out, and EVERY must-hold assertion must FAIL. One that
            passes there is not measuring the pass.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIFF = os.path.join(HERE, '..', 'qmp', 'webidl_shape_diff.py')
KINDS = ['MISSING-IFACE', 'SHARED-PROTO', 'CHAIN', 'CTOR', 'TAG', 'MISPLACED', 'ABSENT',
         'KIND', 'EXTRA-OWN', 'NATIVE']


def load(p):
    d = json.load(open(p))
    return json.loads(d) if isinstance(d, str) else d


def must_hold(ref, ours):
    """[(name, ok, detail)] -- evaluated on the dump, never on a live page."""
    out = []
    S, I = ours.get('singletons', {}), ours.get('ifaces', {})
    rdoc = (ref.get('singletons', {}).get('document') or {}).get('own') or []
    doc = S.get('document') or {}
    own = doc.get('own') if isinstance(doc.get('own'), list) else None
    extra = [m for m in (own or []) if m not in rdoc]
    out.append(('document owns nothing the reference document does not', own is not None and not extra,
                '%d extra: %s' % (len(extra), ' '.join(extra[:12])) if own is not None else 'no document'))
    for s in ('navigator', 'screen', 'history', 'performance', 'crypto'):
        rec = S.get(s)
        o = rec.get('own') if rec else None
        out.append(('%s owns nothing' % s, rec is not None and o == [],
                    'absent' if rec is None else '%d own: %s' % (len(o or []), ' '.join((o or [])[:12]))))
    nav = S.get('navigator') or {}
    out.append(("Object.prototype.toString.call(navigator) === '[object Navigator]'",
                nav.get('tag') == '[object Navigator]', str(nav.get('tag'))))
    perf = I.get('Performance') or {}
    out.append(('Performance.prototype chains to EventTarget.prototype',
                (perf.get('chain') or [None])[0] == 'EventTarget', ' > '.join(perf.get('chain') or [])))
    out.append(('Screen and Crypto are interfaces', bool(I.get('Screen')) and bool(I.get('Crypto')),
                'Screen=%s Crypto=%s' % (bool(I.get('Screen')), bool(I.get('Crypto')))))
    return out


def counts(ref_path, ours_path):
    tmp = ours_path + '.findings.json'
    subprocess.run([sys.executable, DIFF, ref_path, ours_path, '--json', tmp],
                   stdout=subprocess.DEVNULL, check=True)
    findings = json.load(open(tmp))
    c = {k: 0 for k in KINDS}
    for f in findings:
        c[f['kind']] = c.get(f['kind'], 0) + 1
    return c, len(findings)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    write, control = '--write' in argv, '--control' in argv
    if len(args) != 3:
        print(__doc__)
        return 2
    ref_path, ours_path, base_path = args
    ref, ours = load(ref_path), load(ours_path)
    bad = 0

    mh = must_hold(ref, ours)
    for name, ok, detail in mh:
        print('%s %s%s' % ('MUST-HOLD ok  ' if ok else 'MUST-HOLD FAIL', name,
                          '' if ok else '  (%s)' % detail))
    if control:
        held = [n for n, ok, _ in mh if ok]
        if held:
            print('idl-shape control: BROKEN -- with the pass compiled out these still hold,')
            print('  so they are not measuring the pass:')
            for n in held:
                print('    ' + n)
            return 1
        print('idl-shape control: ok -- all %d must-hold assertions fail without the pass' % len(mh))
        return 0
    bad += sum(1 for _, ok, _ in mh if not ok)

    c, total = counts(ref_path, ours_path)
    print('shape diff vs %s: %d findings' % (os.path.basename(ref_path), total))
    if write:
        json.dump({'reference': os.path.basename(ref_path), 'total': total, 'counts': c},
                  open(base_path, 'w'), indent=1, sort_keys=True)
        open(base_path, 'a').write('\n')
        print('idl-shape: baseline written to %s' % base_path)
    else:
        base = json.load(open(base_path))['counts']
        for k in KINDS:
            b = base.get(k, 0)
            mark = ''
            if c[k] > b:
                mark = '  REGRESSED (baseline %d)' % b
                bad += 1
            elif c[k] < b:
                mark = '  below baseline %d -- make idl-shape-baseline to tighten' % b
            print('  %-14s %4d%s' % (k, c[k], mark))
    if bad:
        print('idl-shape: FAILED (%d)' % bad)
        return 1
    print('idl-shape: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
