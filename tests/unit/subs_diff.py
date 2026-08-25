#!/usr/bin/env python3
"""tests/unit/subs_diff.py -- diff subs_test's WebVTT parse against the
independent oracle, field by field, for every fixture in a directory.

Same shape as tests/unit/demux_diff.py's relationship to ffprobe: the point
is not "does it run", it is "do the two independently-written parsers agree
on every field of every cue". Numeric fields (line/position/size/region
anchors) are compared with a tolerance -- a WebVTT `line` value can
legitimately be Number.MAX_VALUE, and string-diffing two languages' float
formatting of that would be testing printf, not parsing. Everything else
(start/end ms, id, text, every enum, the skipped count) is compared exactly.

    python3 tests/unit/subs_diff.py <subs_test_binary> <fixture_dir> [file...]

With no file arguments, every *.vtt directly inside <fixture_dir> is used.
Prints one PASS/FAIL line per file and a wrong-field tally per failing file
(the per-case total this tree's gates are expected to report), then a
summary line, and exits nonzero if anything disagreed.
"""
import glob, os, subprocess, sys

# Files with no valid WebVTT signature at all -- both subs_test and the
# oracle are expected to print exactly "FORMAT-ERROR" (exit 1) for these,
# never a cue dump. Everything under tests/fixtures/subs/wpt/ whose name
# starts with "signature-" or is "empty" is one of these UNLESS it is one of
# the seven WPT .test-derived cases below, which have a VALID signature and
# assert cues.length == 0 (not a format error) -- see that directory's
# README for the split between the two shapes.
VALID_SIGNATURE_CASES = {
    'signature-bom', 'signature-no-newline', 'signature-space',
    'signature-space-no-newline', 'signature-tab', 'signature-tab-no-newline',
    'signature-timings',
}


def expect_format_error(name):
    base = os.path.splitext(name)[0]
    if base == 'empty':
        return True
    if base.startswith('signature-') and base not in VALID_SIGNATURE_CASES:
        return True
    return False


_ENV = dict(os.environ, PYTHONIOENCODING='utf-8:surrogateescape')


def run(cmd):
    # Explicit UTF-8 on BOTH ends: subs_test's output can contain the raw
    # 3-byte encoding of U+FFFD (NUL replacement -- see subs.c's
    # normalize()), and neither our own decode of a child's stdout nor a
    # child python3's own stdout encoding is guaranteed to be UTF-8 by
    # default -- both fall back to locale.getpreferredencoding(), which in a
    # bare/POSIX-locale environment can be ASCII. Getting either end wrong
    # doesn't just mangle a display character: with a lossy error handler it
    # can change the BYTE COUNT of a line (three raw bytes becoming three
    # separate replacement characters), which then misaligns every
    # fixed-position field parsed after it. PYTHONIOENCODING fixes the
    # subprocess's OWN writing side; encoding=/errors= below fixes our
    # reading side; surrogateescape round-trips instead of lying about what
    # arrived.
    p = subprocess.run(cmd, capture_output=True, encoding='utf-8', errors='surrogateescape', env=_ENV)
    return p.returncode, p.stdout, p.stderr


def parse_dump(text):
    """Returns (cues, regions, skipped) or ('FORMAT-ERROR', None, None)."""
    # NOT str.splitlines() -- it treats \v, \f, \x1c-\x1e, \x85, U+2028/2029
    # as line boundaries too, and a cue/region id can legitimately contain a
    # raw \v byte (regions-id.test's fourth region: "id:\v"), which
    # splitlines() then cuts the dump's OWN "REGION ... id=<here>" line in
    # half -- the rest of that line (width=/lines=/...) silently becomes a
    # separate non-matching line and disappears. The dump format's only
    # line terminator is the literal '\n' every printf/print call here
    # writes, so split on exactly that.
    lines = text.split('\n')
    if lines and lines[0].strip() == 'FORMAT-ERROR':
        return 'FORMAT-ERROR', None, None
    cues, regions, skipped = [], [], None
    for line in lines:
        if line.startswith('CUE '):
            cues.append(parse_cue(line))
        elif line.startswith('REGION '):
            regions.append(parse_region(line))
        elif line.startswith('SKIPPED '):
            skipped = int(line.split(' ', 1)[1])
    return cues, regions, skipped


def parse_cue(line):
    # "CUE <start> <end> vert=.. snap=.. line=.. lalign=.. pos=.. palign=.. size=.. align=.. region=.. id=.. text=.."
    parts = line.split(' ', 12)
    assert parts[0] == 'CUE'
    start, end = int(parts[1]), int(parts[2])
    fields = {}
    for tok in parts[3:12]:
        k, v = tok.split('=', 1)
        fields[k] = v
    remainder = parts[12]
    assert remainder.startswith('id=')
    idx = remainder.rfind(' text=')
    cid = remainder[3:idx]
    text = remainder[idx + len(' text='):]
    fields['id'] = cid
    fields['text'] = text
    fields['start'] = start
    fields['end'] = end
    return fields


def parse_region(line):
    # "REGION <idx> id=.. width=.. lines=.. ax=.. ay=.. vx=.. vy=.. scroll=.."
    parts = line.split(' ')
    assert parts[0] == 'REGION'
    fields = {'idx': int(parts[1])}
    for tok in parts[2:]:
        k, v = tok.split('=', 1)
        fields[k] = v
    return fields


NUMERIC_CUE_FIELDS = ('line', 'pos', 'size')
NUMERIC_REGION_FIELDS = ('width', 'ax', 'ay', 'vx', 'vy')
STRING_CUE_FIELDS = ('vert', 'snap', 'lalign', 'palign', 'align', 'region', 'id', 'text')
STRING_REGION_FIELDS = ('id', 'lines', 'scroll')


def num_close(a, b):
    if a == 'auto' or b == 'auto':
        return a == b
    fa, fb = float(a), float(b)
    if fa == fb:
        return True
    tol = max(1e-6, abs(fa) * 1e-9, abs(fb) * 1e-9)
    return abs(fa - fb) <= tol


def diff_one(cbin, oracle, path):
    rc_c, out_c, err_c = run([cbin, 'cues', path])
    rc_o, out_o, err_o = run([sys.executable, oracle, path])

    got = parse_dump(out_c)
    want = parse_dump(out_o)

    if got[0] == 'FORMAT-ERROR' or want[0] == 'FORMAT-ERROR':
        if got[0] == want[0]:
            return []
        return ['FORMAT-ERROR mismatch: subs_test=%r oracle=%r (stderr c=%r o=%r)' %
                (got[0], want[0], err_c.strip(), err_o.strip())]

    c_cues, c_regions, c_skipped = got
    o_cues, o_regions, o_skipped = want
    problems = []

    if len(c_cues) != len(o_cues):
        problems.append('cue count: subs_test=%d oracle=%d' % (len(c_cues), len(o_cues)))
    if len(c_regions) != len(o_regions):
        problems.append('region count: subs_test=%d oracle=%d' % (len(c_regions), len(o_regions)))
    if c_skipped != o_skipped:
        problems.append('skipped count: subs_test=%d oracle=%d' % (c_skipped, o_skipped))

    for i, (cc, oc) in enumerate(zip(c_cues, o_cues)):
        if cc['start'] != oc['start']:
            problems.append('cue %d start: %d != %d' % (i, cc['start'], oc['start']))
        if cc['end'] != oc['end']:
            problems.append('cue %d end: %d != %d' % (i, cc['end'], oc['end']))
        for f in NUMERIC_CUE_FIELDS:
            if not num_close(cc[f], oc[f]):
                problems.append('cue %d %s: %s != %s' % (i, f, cc[f], oc[f]))
        for f in STRING_CUE_FIELDS:
            if cc[f] != oc[f]:
                problems.append('cue %d %s: %r != %r' % (i, f, cc[f], oc[f]))

    for i, (cr, orr) in enumerate(zip(c_regions, o_regions)):
        for f in NUMERIC_REGION_FIELDS:
            if not num_close(cr[f], orr[f]):
                problems.append('region %d %s: %s != %s' % (i, f, cr[f], orr[f]))
        for f in STRING_REGION_FIELDS:
            if cr[f] != orr[f]:
                problems.append('region %d %s: %r != %r' % (i, f, cr[f], orr[f]))

    return problems


def main():
    if len(sys.argv) < 3:
        print('usage: subs_diff.py <subs_test_binary> <fixture_dir> [file...]')
        return 2
    cbin, fxdir = sys.argv[1], sys.argv[2]
    oracle = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'subs_oracle.py')

    files = sys.argv[3:]
    if not files:
        files = sorted(glob.glob(os.path.join(fxdir, '*.vtt')))

    total_problems = 0
    failed_files = 0
    for path in files:
        name = os.path.basename(path)
        expect_err = expect_format_error(name)
        problems = diff_one(cbin, oracle, path)
        if expect_err and not problems:
            # diff_one already confirms both sides agree; still worth
            # asserting explicitly that the agreement WAS format-error, not
            # both sides coincidentally producing zero cues for some other
            # reason (an empty valid file also has zero cues).
            rc_c, out_c, _ = run([cbin, 'cues', path])
            if not out_c.startswith('FORMAT-ERROR'):
                problems = ['expected FORMAT-ERROR for %s but got a cue dump' % name]
        if problems:
            failed_files += 1
            total_problems += len(problems)
            print('FAIL %-32s %d field mismatch(es)' % (name, len(problems)))
            for p in problems[:12]:
                print('       %s' % p)
            if len(problems) > 12:
                print('       ... and %d more' % (len(problems) - 12))
        else:
            print('pass %-32s' % name)

    print('---')
    print('%d/%d fixtures agree with the oracle field-for-field (%d total field mismatches)' %
          (len(files) - failed_files, len(files), total_problems))
    return 1 if failed_files else 0


if __name__ == '__main__':
    sys.exit(main())
