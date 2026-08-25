#!/usr/bin/env python3
"""tests/unit/subs_negctl.py -- the -DSUBS_STRICT negative control, automated.

subs_test built with -DSUBS_STRICT aborts the whole parse on the FIRST
malformed cue instead of skipping and counting it (see subs.c's header for
the exact definition of "malformed" this uses). This script asserts the
control does EXACTLY what it should, on EVERY fixture, not just a couple by
hand:

  - a fixture the PLAIN build reports skipped>0 for MUST redden under
    -DSUBS_STRICT (subs_test exits 1 and prints STRICT-ABORT)
  - a fixture the plain build reports skipped==0 for MUST produce the
    IDENTICAL dump under -DSUBS_STRICT -- not just "also succeeds", the
    SAME bytes out, because STRICT is only supposed to change behaviour on
    inputs that have something to be strict about

    python3 tests/unit/subs_negctl.py <plain_binary> <strict_binary> <fixture_dir> [file...]
"""
import glob, os, subprocess, sys


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, encoding='utf-8', errors='surrogateescape')
    return p.returncode, p.stdout


def main():
    if len(sys.argv) < 4:
        print('usage: subs_negctl.py <plain> <strict> <fixture_dir> [file...]')
        return 2
    plain, strict, fxdir = sys.argv[1], sys.argv[2], sys.argv[3]
    files = sys.argv[4:]
    if not files:
        files = sorted(glob.glob(os.path.join(fxdir, '*.vtt'))) + sorted(glob.glob(os.path.join(fxdir, '*.srt')))

    subcmd = {'vtt': 'cues', 'srt': 'srt'}
    reddened, unchanged, problems = [], [], []

    for path in files:
        ext = os.path.splitext(path)[1].lstrip('.')
        cmd = subcmd.get(ext)
        if cmd is None:
            continue
        rc_p, out_p = run([plain, cmd, path])
        rc_s, out_s = run([strict, cmd, path])

        if out_p.startswith('FORMAT-ERROR'):
            # Not what this control is about -- a bad signature is a
            # different failure mode, and both builds must agree on it
            # exactly the same way plain-vs-plain already does (subs_diff.py
            # covers that). Just confirm STRICT didn't change ITS answer.
            if out_s.strip() != 'FORMAT-ERROR':
                problems.append('%s: FORMAT-ERROR under plain but %r under strict' %
                                 (os.path.basename(path), out_s.splitlines()[:1]))
            continue

        skipped = None
        for line in out_p.split('\n'):
            if line.startswith('SKIPPED '):
                skipped = int(line.split(' ', 1)[1])
        if skipped is None:
            problems.append('%s: plain build produced no SKIPPED line (rc=%d)' % (path, rc_p))
            continue

        name = os.path.basename(path)
        if skipped > 0:
            if rc_s == 0 or not out_s.startswith('STRICT-ABORT'):
                problems.append('%s: skipped=%d under plain but strict did NOT abort (rc=%d, first line %r)' %
                                 (name, skipped, rc_s, out_s.splitlines()[:1]))
            else:
                reddened.append(name)
        else:
            if out_s != out_p:
                problems.append('%s: skipped=0 but strict output DIFFERS from plain output' % name)
            else:
                unchanged.append(name)

    print('reddened under -DSUBS_STRICT (%d): %s' % (len(reddened), ', '.join(reddened)))
    print('unchanged under -DSUBS_STRICT (%d, byte-identical output)' % len(unchanged))
    if problems:
        print('NEGCTL-FAIL (%d problem(s)):' % len(problems))
        for p in problems:
            print('  %s' % p)
        return 1
    if not reddened:
        print('NEGCTL-FAIL: nothing reddened -- the control has nothing to prove it is live')
        return 1
    print('negctl: -DSUBS_STRICT reddens exactly the %d fixture(s) with a malformed cue, '
          'and %d fixture(s) with none are byte-identical to the plain build' % (len(reddened), len(unchanged)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
