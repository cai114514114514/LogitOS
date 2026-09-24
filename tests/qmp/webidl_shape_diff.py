#!/usr/bin/env python3
"""Diff two platform-shape dumps (tests/fixtures/webidl_shape/shape.js).

    webidl_shape_diff.py REFERENCE.json OURS.json [--json OUT] [--iface NAME ...]

The reference is a real browser's dump of the same page; the question is not
"which features exist" but "is the object model LAID OUT like a browser's".
Every finding is one of:

  MISSING-IFACE   the reference has a global interface we do not
  SHARED-PROTO    our X.prototype is some other interface's prototype object
                  (EventTarget/Node/Element were once one object)
  CHAIN           X.prototype's prototype chain differs
  CTOR            X.prototype.constructor !== X
  TAG             Object.prototype.toString.call(X.prototype) differs
  MISPLACED       a member the reference has on X.prototype exists here, but
                  somewhere else (an instance, another prototype)
  ABSENT          ... and here it exists nowhere we looked
  KIND            the member is here, on the right prototype, but as a
                  different kind (method vs accessor vs data, setter missing)
  EXTRA-OWN       a singleton owns names the reference's does not
  NATIVE          a platform function stringifies to source, or has .prototype

Counts come first, then detail. Nothing here decides what to fix; it says
where the layout differs and by how much.
"""
import json
import sys


def load(p):
    d = json.load(open(p))
    if isinstance(d, str):
        d = json.loads(d)
    return d


def main(argv):
    ref, ours = load(argv[1]), load(argv[2])
    only = set()
    out_json = None
    i = 3
    while i < len(argv):
        if argv[i] == '--json':
            out_json = argv[i + 1]; i += 2
        elif argv[i] == '--iface':
            only.add(argv[i + 1]); i += 2
        else:
            i += 1
    findings = []

    def add(kind, where, detail):
        findings.append({'kind': kind, 'where': where, 'detail': detail})

    # Everywhere a member lives in OUR dump, for MISPLACED vs ABSENT.
    ours_home = {}
    for iname, rec in ours['ifaces'].items():
        if rec and rec.get('members'):
            for m in rec['members']:
                ours_home.setdefault(m, set()).add(iname + '.prototype')
    for sname, rec in ours['singletons'].items():
        if rec and isinstance(rec.get('own'), list):
            for m in rec['own']:
                ours_home.setdefault(m, set()).add(sname + ' (own)')

    for iname, r in ref['ifaces'].items():
        if only and iname not in only:
            continue
        o = ours['ifaces'].get(iname)
        if not r:
            continue
        if not o:
            add('MISSING-IFACE', iname, '')
            continue
        if r.get('hasProto') and not o.get('hasProto'):
            add('CHAIN', iname, 'no prototype object')
            continue
        if not r.get('members'):
            continue
        if o.get('self') not in (iname, None):
            add('SHARED-PROTO', iname, 'our %s.prototype is named %r' % (iname, o.get('self')))
        if o.get('chain') != r.get('chain'):
            add('CHAIN', iname, 'ref %s | ours %s' % (' > '.join(r.get('chain') or []), ' > '.join(o.get('chain') or [])))
        if r.get('ctorOk') and not o.get('ctorOk'):
            add('CTOR', iname, '%s.prototype.constructor !== %s' % (iname, iname))
        if r.get('tag') != o.get('tag'):
            add('TAG', iname, 'ref %s | ours %s' % (r.get('tag'), o.get('tag')))
        om = o.get('members') or {}
        for m, rk in r['members'].items():
            if m in ('constructor',):
                continue
            if m not in om:
                homes = sorted(ours_home.get(m, ()))
                if homes:
                    add('MISPLACED', iname + '.' + m, 'here: ' + ', '.join(homes[:4]))
                else:
                    add('ABSENT', iname + '.' + m, rk)
            else:
                # compare the shape letters only (m/d/g/gs), not e/c flags
                base = lambda k: k.rstrip('ec') if k else k
                if base(rk) != base(om[m]):
                    add('KIND', iname + '.' + m, 'ref %s | ours %s' % (rk, om[m]))

    for sname, r in ref['singletons'].items():
        o = ours['singletons'].get(sname)
        if not r:
            continue
        if not o:
            add('MISSING-IFACE', sname, 'singleton absent')
            continue
        if r.get('chain') != o.get('chain'):
            add('CHAIN', sname, 'ref %s | ours %s' % (' > '.join(r['chain']), ' > '.join(o['chain'])))
        if r.get('tag') != o.get('tag'):
            add('TAG', sname, 'ref %s | ours %s' % (r.get('tag'), o.get('tag')))
        if isinstance(r.get('own'), list) and isinstance(o.get('own'), list):
            extra = [m for m in o['own'] if m not in r['own']]
            if extra:
                add('EXTRA-OWN', sname, '%d: %s' % (len(extra), ' '.join(extra)))

    for p, rv in ref.get('native', {}).items():
        ov = ours.get('native', {}).get(p)
        if ov != rv:
            add('NATIVE', p, 'ref %s | ours %s' % (rv, ov))

    kinds = ['MISSING-IFACE', 'SHARED-PROTO', 'CHAIN', 'CTOR', 'TAG', 'MISPLACED', 'ABSENT', 'KIND', 'EXTRA-OWN', 'NATIVE']
    print('webidl shape diff: %d findings' % len(findings))
    for k in kinds:
        print('  %-14s %d' % (k, sum(1 for f in findings if f['kind'] == k)))
    for k in kinds:
        fs = [f for f in findings if f['kind'] == k]
        if not fs:
            continue
        print('\n== %s (%d)' % (k, len(fs)))
        for f in fs:
            print('  %-48s %s' % (f['where'], f['detail']))
    if out_json:
        json.dump(findings, open(out_json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
