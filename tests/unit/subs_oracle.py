#!/usr/bin/env python3
"""tests/unit/subs_oracle.py -- an INDEPENDENT WebVTT parser, for the diff.

Same relationship demux_diff.py has to ffprobe: c/lib/media/subs.c is the
implementation under test, and this is a second, independently-written
implementation of the same spec (https://w3c.github.io/webvtt/ section 6) so
the gate is "these two agree", not "this one implementation believes
itself". It deliberately does NOT share any code or helper with subs.c --
copy-pasting the C logic into Python would make the two outputs agree by
construction on every bug, which is exactly the failure mode an oracle
exists to avoid.

Usage:
    python3 tests/unit/subs_oracle.py <file.vtt>

Prints one line per cue, in the same canonical format subs_test.c's `cues`
subcommand prints, sorted by (start_ms, file order) -- see subs_test.c's
header for the exact field layout. A line "SKIPPED <n>" follows.
On a bad signature, prints "FORMAT-ERROR" and exits 1.
"""
import sys

WS = (' ', '\t', '\f')
WS_NL = WS + ('\n',)


def normalize(data: bytes) -> str:
    """NUL -> U+FFFD, CRLF/CR -> LF, at the BYTE level -- same as subs.c's
    normalize(), and for the same reason: this has to match a C
    implementation that never decodes UTF-8 at all (it treats the file as
    an opaque byte stream and copies non-NUL, non-CR bytes straight
    through, which is trivially correct because UTF-8 continuation bytes
    never collide with the ASCII bytes this parser actually inspects). The
    first draft of this function used `chr(byte_value)` for that passthrough
    -- Latin-1 semantics -- which mangled every multi-byte UTF-8 sequence
    ALREADY in the input (not a NUL substitution) into one Python character
    per BYTE. nulls.test's own "already contains U+FFFD, not a NUL, must
    pass through unchanged" case is exactly what caught it: byte-by-byte
    chr() turned the 3-byte sequence EF BF BD into 'ï¿½' (three codepoints,
    each one byte's numeric value) instead of leaving it as the one
    codepoint it decodes to. Fixed the only way that stays honest about
    working byte-for-byte like the C side: do the NUL/CR substitution on
    the BYTES, then decode the whole result as UTF-8 exactly once."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        c = data[i]
        if c == 0:
            out += b'\xef\xbf\xbd'
        elif c == 0x0D:
            out.append(0x0A)
            if i + 1 < n and data[i + 1] == 0x0A:
                i += 1
        else:
            out.append(c)
        i += 1
    return bytes(out).decode('utf-8', errors='surrogateescape')


def parse_percentage(tok):
    if len(tok) < 2 or tok[-1] != '%':
        return None
    body = tok[:-1]
    if not body:
        return None
    before = after = 0
    seen_dot = False
    for ch in body:
        if ch.isdigit():
            if seen_dot:
                after += 1
            else:
                before += 1
        elif ch == '.' and not seen_dot:
            seen_dot = True
        else:
            return None
    if before == 0 or (seen_dot and after == 0):
        return None
    try:
        v = float(body)
    except ValueError:
        return None
    if not (0.0 <= v <= 100.0):
        return None
    return v


def parse_signed_number(tok):
    if not tok:
        return None
    i = 0
    if tok[0] == '-':
        i = 1
        if '-' in tok[1:]:
            return None
    before = after = 0
    seen_dot = False
    for ch in tok[i:]:
        if ch.isdigit():
            if seen_dot:
                after += 1
            else:
                before += 1
        elif ch == '.':
            if seen_dot:
                return None
            seen_dot = True
        else:
            return None
    if before == 0 or (seen_dot and after == 0):
        return None
    v = float(tok)
    if v in (float('inf'), float('-inf')) or v != v:
        return None
    return v


def collect_digits(s, pos):
    start = pos
    while pos < len(s) and s[pos].isdigit():
        pos += 1
    return s[start:pos], pos


def collect_timestamp(s, pos):
    if pos >= len(s) or not s[pos].isdigit():
        return None, pos
    d1, pos = collect_digits(s, pos)
    v1 = int(d1)
    most_sig_hours = (len(d1) != 2 or v1 > 59)
    if pos >= len(s) or s[pos] != ':':
        return None, pos
    pos += 1
    d2, pos = collect_digits(s, pos)
    if len(d2) != 2:
        return None, pos
    v2 = int(d2)
    if most_sig_hours or (pos < len(s) and s[pos] == ':'):
        if pos >= len(s) or s[pos] != ':':
            return None, pos
        pos += 1
        d3, pos = collect_digits(s, pos)
        if len(d3) != 2:
            return None, pos
        v3 = int(d3)
    else:
        v3 = v2
        v2 = v1
        v1 = 0
    if pos >= len(s) or s[pos] != '.':
        return None, pos
    pos += 1
    d4, pos = collect_digits(s, pos)
    if len(d4) != 3:
        return None, pos
    v4 = int(d4)
    if v2 > 59 or v3 > 59:
        return None, pos
    return v1 * 3600000 + v2 * 60000 + v3 * 1000 + v4, pos


def skip_ws(s, pos):
    while pos < len(s) and s[pos] in WS:
        pos += 1
    return pos


def split_ws_nl(s):
    toks = []
    i, n = 0, len(s)
    while i < n:
        while i < n and s[i] in WS_NL:
            i += 1
        start = i
        while i < n and s[i] not in WS_NL:
            i += 1
        if i > start:
            toks.append(s[start:i])
    return toks


class Settings:
    def __init__(self):
        self.vertical = ''
        self.snap_to_lines = True
        self.line = 'auto'
        self.line_is_percent = False
        self.line_align = 'start'
        self.position = 'auto'
        self.position_align = 'auto'
        self.size = 100.0
        self.align = 'center'
        self.region = None  # region id string, or None


def region_lookup(regions, val):
    for r in reversed(regions):
        if r['id'] == val:
            return r
    return None


def apply_cue_setting(st, name, val, regions):
    if name == 'region':
        st.region = val if region_lookup(regions, val) is not None else None
    elif name == 'vertical':
        if val == 'rl':
            st.vertical = 'rl'
        elif val == 'lr':
            st.vertical = 'lr'
        else:
            return
        st.region = None
    elif name == 'line':
        if ',' in val:
            pospart, alignpart = val.split(',', 1)
        else:
            pospart, alignpart = val, None
        if not any(c.isdigit() for c in pospart):
            return
        if pospart.endswith('%'):
            number = parse_percentage(pospart)
            is_pct = True
        else:
            number = parse_signed_number(pospart)
            is_pct = False
        if number is None:
            return
        new_align = st.line_align
        align_set = False
        if alignpart is not None:
            if alignpart == 'start':
                new_align = 'start'; align_set = True
            elif alignpart == 'center':
                new_align = 'center'; align_set = True
            elif alignpart == 'end':
                new_align = 'end'; align_set = True
            else:
                return
        st.line = number
        st.line_is_percent = is_pct
        if align_set:
            st.line_align = new_align
        st.snap_to_lines = not is_pct
        st.region = None
    elif name == 'position':
        if ',' in val:
            pospart, alignpart = val.split(',', 1)
        else:
            pospart, alignpart = val, None
        number = parse_percentage(pospart)
        if number is None:
            return
        new_align = 'auto'
        if alignpart is not None:
            if alignpart == 'line-left':
                new_align = 'line-left'
            elif alignpart == 'center':
                new_align = 'center'
            elif alignpart == 'line-right':
                new_align = 'line-right'
            else:
                return
        st.position = number
        st.position_align = new_align
    elif name == 'size':
        number = parse_percentage(val)
        if number is None:
            return
        st.size = number
        if number != 100:
            st.region = None
    elif name == 'align':
        if val in ('start', 'center', 'end', 'left', 'right'):
            st.align = val


def parse_cue_settings(remainder, regions):
    st = Settings()
    for tok in split_ws_nl(remainder):
        if ':' not in tok:
            continue
        i = tok.index(':')
        if i == 0 or i == len(tok) - 1:
            continue
        apply_cue_setting(st, tok[:i], tok[i + 1:], regions)
    return st


def parse_cue_timings(line, regions):
    pos = skip_ws(line, 0)
    start, pos = collect_timestamp(line, pos)
    if start is None:
        return None
    pos = skip_ws(line, pos)
    for ch in '-->':
        if pos >= len(line) or line[pos] != ch:
            return None
        pos += 1
    pos = skip_ws(line, pos)
    end, pos = collect_timestamp(line, pos)
    if end is None:
        return None
    st = parse_cue_settings(line[pos:], regions)
    return start, end, st


def region_defaults():
    return {'id': '', 'width': 100.0, 'lines': 3, 'anchor_x': 0.0, 'anchor_y': 100.0,
            'viewport_x': 0.0, 'viewport_y': 100.0, 'scroll': ''}


def parse_region_settings(body):
    r = region_defaults()
    for tok in split_ws_nl(body):
        if ':' not in tok:
            continue
        i = tok.index(':')
        if i == 0 or i == len(tok) - 1:
            continue
        name, val = tok[:i], tok[i + 1:]
        if name == 'id':
            r['id'] = val
        elif name == 'width':
            v = parse_percentage(val)
            if v is not None:
                r['width'] = v
        elif name == 'lines':
            if val.isdigit():
                r['lines'] = int(val) & 0x7FFFFFFF if int(val) < 2**31 else 2147483647
        elif name == 'regionanchor':
            if ',' in val:
                a, b = val.split(',', 1)
                va, vb = parse_percentage(a), parse_percentage(b)
                if va is not None and vb is not None:
                    r['anchor_x'], r['anchor_y'] = va, vb
        elif name == 'viewportanchor':
            if ',' in val:
                a, b = val.split(',', 1)
                va, vb = parse_percentage(a), parse_percentage(b)
                if va is not None and vb is not None:
                    r['viewport_x'], r['viewport_y'] = va, vb
        elif name == 'scroll':
            if val == 'up':
                r['scroll'] = 'up'
    return r


BLOCK_NONE, BLOCK_CUE, BLOCK_STYLE, BLOCK_REGION = range(4)


def collect_block(lines, i, in_header, regions, seen_cue):
    n = len(lines)
    prev_i = i
    line_count = 0
    seen_arrow = False
    buf = ''
    kind = BLOCK_NONE
    cue = None
    skipped_here = 0

    def finish(next_i):
        # REGION's settings string ("buf") is only ever finalized HERE, once,
        # regardless of which of the three exit paths below is taken -- an
        # earlier draft built the bare-defaults region dict inline where
        # STYLE/REGION is first recognised and never actually called
        # parse_region_settings on the accumulated body at all, so every
        # region came out with every field at its default no matter what the
        # file said. regions-id.test (four regions, same id reused, only
        # `lines:` distinguishes them) is what exposed it: all four regions
        # printed identically.
        region = parse_region_settings(buf) if kind == BLOCK_REGION else None
        return kind, cue, region, buf, next_i, skipped_here

    while True:
        is_eof = i >= n
        line = '' if is_eof else lines[i]
        line_count += 1

        if not is_eof and '-->' in line:
            eligible = (not in_header) and (line_count == 1 or (line_count == 2 and not seen_arrow))
            if eligible:
                seen_arrow = True
                prev_i = i + 1
                parsed = parse_cue_timings(line, regions)
                # NOT `and parsed[1] >= parsed[0]`: see subs.c's matching
                # comment -- web-platform-tests' timings-negative.test
                # requires accepting end < start as a valid cue.
                if parsed:
                    start, end, st = parsed
                    cue = {'id': buf, 'start': start, 'end': end, 'settings': st}
                    buf = ''
                    kind = BLOCK_CUE
                    seen_cue[0] = True
                else:
                    skipped_here += 1
                i += 1
                continue
            else:
                return finish(prev_i)

        elif line == '':
            i += 1
            return finish(i)

        else:
            if not in_header and line_count == 2 and not seen_cue[0] and kind == BLOCK_NONE:
                if buf.startswith('STYLE') and buf[5:].strip(' \t\f') == '':
                    kind = BLOCK_STYLE
                    buf = ''
                elif buf.startswith('REGION') and buf[6:].strip(' \t\f') == '':
                    kind = BLOCK_REGION
                    buf = ''
            if buf:
                buf += '\n'
            buf += line
            prev_i = i + 1
            i += 1

        if is_eof:
            return finish(i)


def parse_vtt(data: bytes):
    if len(data) >= 3 and data[0:3] == b'\xef\xbb\xbf':
        data = data[3:]
    text = normalize(data)
    n = len(text)
    if n < 6 or text[:6] != 'WEBVTT' or (n > 6 and text[6] not in (' ', '\t', '\n')):
        return None  # FORMAT-ERROR

    lines = text.split('\n')
    # text.split('\n') on a string ending in '\n' yields a trailing '' entry,
    # matching the C side's split_lines (which also emits a final empty line
    # for a trailing separator) -- kept identical on purpose, see subs_test.c.

    li = 1
    seen_cue = [False]
    regions = []
    cues = []
    skipped = 0

    if li >= len(lines):
        return {'cues': [], 'regions': [], 'skipped': 0}

    if lines[li] != '':
        _, _, _, _, li, sk = collect_block(lines, li, True, regions, seen_cue)
        skipped += sk
    else:
        li += 1
    while li < len(lines) and lines[li] == '':
        li += 1

    while li < len(lines):
        kind, cue, region, buf, li, sk = collect_block(lines, li, False, regions, seen_cue)
        skipped += sk
        if kind == BLOCK_CUE:
            cue['text'] = buf
            cues.append(cue)
        elif kind == BLOCK_REGION:
            region['id_dup_ok'] = True
            regions.append(region)
        while li < len(lines) and lines[li] == '':
            li += 1

    return {'cues': cues, 'regions': regions, 'skipped': skipped}


def fmt_num(v):
    if v == 'auto':
        return 'auto'
    if float(v).is_integer() and abs(v) < 1e15:
        return '%d' % int(v)
    return repr(float(v))


def main():
    if len(sys.argv) != 2:
        print('usage: subs_oracle.py <file.vtt>', file=sys.stderr)
        return 2
    data = open(sys.argv[1], 'rb').read()
    result = parse_vtt(data)
    if result is None:
        print('FORMAT-ERROR')
        return 1

    cues = sorted(enumerate(result['cues']), key=lambda p: (p[1]['start'], p[0]))
    for _, c in cues:
        st = c['settings']
        region_idx = -1
        if st.region is not None:
            for idx, r in enumerate(result['regions']):
                if r['id'] == st.region:
                    region_idx = idx
        text = c['text'].replace('\\', '\\\\').replace('\n', '\\n')
        cid = c['id'].replace('\\', '\\\\').replace('\n', '\\n')
        # id/text last and in that order -- see subs_test.c's dump_track for
        # why (subs_diff.py relies on this exact layout).
        print('CUE %d %d vert=%s snap=%d line=%s lalign=%s pos=%s palign=%s size=%s align=%s region=%d id=%s text=%s' % (
            c['start'], c['end'], st.vertical, 1 if st.snap_to_lines else 0,
            fmt_num(st.line), st.line_align, fmt_num(st.position), st.position_align,
            fmt_num(st.size), st.align, region_idx, cid, text))
    for i, r in enumerate(result['regions']):
        print('REGION %d id=%s width=%s lines=%d ax=%s ay=%s vx=%s vy=%s scroll=%s' % (
            i, r['id'], fmt_num(r['width']), r['lines'], fmt_num(r['anchor_x']), fmt_num(r['anchor_y']),
            fmt_num(r['viewport_x']), fmt_num(r['viewport_y']), r['scroll']))
    print('SKIPPED %d' % result['skipped'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
