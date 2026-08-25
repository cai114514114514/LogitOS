#!/usr/bin/env python3
"""tests/unit/subs_srt_roundtrip.py -- WebVTT and the SRT ffmpeg makes of it
must describe the SAME cues.

    python3 tests/unit/subs_srt_roundtrip.py <subs_test_binary> <sample.vtt> <sample.srt>

Runs `subs_test cues sample.vtt` and `subs_test srt sample.srt` and checks,
per cue: same start_ms, same end_ms, same id (ffmpeg's SRT muxer numbers
cues 1..n, matching this sample's own WebVTT cue identifiers, which are
ALSO "1".."6" -- not a coincidence, chosen so this check has something to
compare). Text is checked only for cues the sample deliberately kept free of
WebVTT-specific tags: ffmpeg's WebVTT reader keeps <b>/<i>/<u> (SRT's own
"HTML-ish subset") but drops <v ...>/<c ...>/<ruby><rt> and karaoke
timestamp spans down to their inner text, which is ffmpeg's transcoding
choice to make, not a claim this library's raw-text-preserving parse is
wrong about the ORIGINAL VTT -- see subs.h's "carried, not applied" note.
Cue 6 (the multi-line one) checks that the line-joined text survives too.

`sample.ffmpeg.srt` is COMMITTED (generated once by
`ffmpeg -y -i sample.vtt -f srt sample.ffmpeg.srt`) for the same reason
tests/fixtures/media are: the gate has to mean something on a machine
without ffmpeg installed. If ffmpeg IS available, this script re-generates
it into a temp file first and diffs that against the committed copy --
divergence there means either ffmpeg's WebVTT->SRT transcoding changed
upstream or sample.vtt was edited without regenerating the committed SRT,
and either is worth knowing about even though it does not fail the gate by
itself (a newer ffmpeg making a different but still-valid transcoding
choice is not this library's bug).
"""
import shutil, subprocess, sys, tempfile, os

FIELDS = ('start_ms', 'end_ms', 'id')
# Cue index (1-based, matching the id) -> expected exact text survives the
# VTT->SRT hop unchanged.
TEXT_CHECKED = {'1', '2', '6'}


def parse(text):
    cues = []
    cur = None
    for line in text.split('\n'):
        if line.startswith('CUE '):
            parts = line.split(' ', 12)
            cur = {'start_ms': int(parts[1]), 'end_ms': int(parts[2])}
            remainder = parts[12]
            idx = remainder.rfind(' text=')
            cur['id'] = remainder[3:idx]
            cur['text'] = remainder[idx + len(' text='):]
            cues.append(cur)
    return cues


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, encoding='utf-8', errors='surrogateescape')
    return p.returncode, p.stdout, p.stderr


def main():
    if len(sys.argv) != 4:
        print('usage: subs_srt_roundtrip.py <subs_test> <sample.vtt> <sample.srt>')
        return 2
    cbin, vtt_path, srt_path = sys.argv[1:4]

    if shutil.which('ffmpeg'):
        with tempfile.TemporaryDirectory() as td:
            gen = os.path.join(td, 'gen.srt')
            rc, out, err = run(['ffmpeg', '-y', '-i', vtt_path, '-f', 'srt', gen])
            if rc != 0:
                print('WARNING: ffmpeg failed to transcode %s (%s) -- comparing against the committed'
                      ' sample.ffmpeg.srt only, not regenerating it' % (vtt_path, err.strip().splitlines()[-1:]))
            else:
                committed = open(srt_path, 'rb').read()
                fresh = open(gen, 'rb').read()
                if committed != fresh:
                    print('NOTE: a freshly ffmpeg-transcoded %s differs from the committed %s byte for'
                          ' byte -- not a failure (ffmpeg on this machine may differ from the one that'
                          ' generated the committed copy), but worth knowing before trusting this gate'
                          ' blindly.' % (vtt_path, srt_path))
    else:
        print('no ffmpeg on this machine -- comparing subs_test against the COMMITTED sample.ffmpeg.srt only')

    rc, vtt_out, err = run([cbin, 'cues', vtt_path])
    if rc != 0:
        print('FAIL: subs_test cues %s failed: %s' % (vtt_path, err))
        return 1
    rc, srt_out, err = run([cbin, 'srt', srt_path])
    if rc != 0:
        print('FAIL: subs_test srt %s failed: %s' % (srt_path, err))
        return 1

    vtt_cues = parse(vtt_out)
    srt_cues = parse(srt_out)
    if len(vtt_cues) != len(srt_cues):
        print('FAIL: cue count differs: vtt=%d srt=%d' % (len(vtt_cues), len(srt_cues)))
        return 1

    problems = []
    for i, (v, s) in enumerate(zip(vtt_cues, srt_cues)):
        for f in FIELDS:
            if v[f] != s[f]:
                problems.append('cue %d %s: vtt=%r srt=%r' % (i, f, v[f], s[f]))
        if v['id'] in TEXT_CHECKED and v['text'] != s['text']:
            problems.append('cue %d text (tag-free, must survive exactly): vtt=%r srt=%r' %
                             (i, v['text'], s['text']))

    if problems:
        print('FAIL: %d field mismatch(es) between VTT and its ffmpeg-transcoded SRT' % len(problems))
        for p in problems:
            print('  %s' % p)
        return 1

    print('pass: %d cues agree between %s and its ffmpeg SRT transcode (start/end/id exact; '
          'text exact on the %d tag-free cues)' % (len(vtt_cues), os.path.basename(vtt_path), len(TEXT_CHECKED)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
