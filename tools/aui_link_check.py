#!/usr/bin/env python3
"""Link-wiring checker for the AUI toolkit objects.

Scans Makefile and tests/**/*.mk (continuations joined, comments skipped) and
reports `FAIL bypass <file>:<line>` for every link site that
  (a) names literal apps/aui.o in a link recipe or link prerequisite (instead of
      $(AUI_OBJ)), other than the AUI_OBJ definition, the aui.o rule itself and
      recipes that only `rm` it; or
  (b) names an aui*.o variant (aui_noaa.o, aui_cost.o, scroll/aui_instant.o ...)
      in a link rule without $(AUI_LIBS) in the same rule.
A "link rule" is a rule whose target ends in .elf/.aex/.exe or whose recipe runs
$(LD)/ld. `--inject TEXT` appends a synthetic fragment (for the negative control).
Exit status is non-zero when any site fails.
"""
import argparse, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUI_LIT = re.compile(r'apps/aui\.o\b')
LINK_TGT = re.compile(r'\.(elf|aex|exe)$')


def logical_lines(text):
    """Yield (first_lineno, joined_text, is_recipe)."""
    out, buf, start = [], None, 0
    for n, raw in enumerate(text.split('\n'), 1):
        line = raw.rstrip('\r')
        if buf is None:
            start, buf = n, line
        else:
            buf += ' ' + line.strip()
        if buf.endswith('\\'):
            buf = buf[:-1].rstrip()
            continue
        out.append((start, buf))
        buf = None
    if buf is not None:
        out.append((start, buf))
    return out


def rules(text):
    """Group logical lines into rules: (target_line, hdr_lineno, prereq, [(lineno, recipe)])."""
    res, cur = [], None
    for n, l in logical_lines(text):
        if l.startswith('\t'):
            if cur:
                cur[3].append((n, l.strip()))
            continue
        s = l.strip()
        if not s or s.startswith('#'):
            continue
        m = re.match(r'^([^\s:=][^:=]*?)\s*:(?!=)\s*(.*)$', s)
        if m and not re.match(r'^(ifeq|ifneq|ifdef|ifndef|else|endif|include|-include|define|endef|export|override)\b', s):
            cur = (m.group(1), n, m.group(2), [])
            res.append(cur)
        else:
            cur = None
    return res


def strip_comment(s):
    return re.sub(r'(?<!\\)#.*$', '', s)


def check(name, text):
    fails = []
    for tgt, n, pre, rec in rules(text):
        pre = strip_comment(pre)
        recs = [(rn, strip_comment(r)) for rn, r in rec]
        is_link = bool(LINK_TGT.search(tgt.split()[-1] if tgt.split() else '')) or \
            any(re.search(r'(\$\(\$?LD\)|\bld\b)', r) for _, r in recs)
        if not is_link:
            continue
        if tgt.strip().endswith('apps/aui.o'):
            continue
        body = pre + ' ' + ' '.join(r for _, r in recs)
        # (a) literal apps/aui.o in prerequisites or in a non-rm recipe line
        bad = []
        if AUI_LIT.search(pre):
            bad.append(n)
        for rn, r in recs:
            if AUI_LIT.search(r) and not re.match(r'^@?\s*rm\b', r):
                bad.append(rn)
        # (b) variant objects without $(AUI_LIBS)
        if not bad:
            vb = [v for v in re.findall(r'\S*aui[\w\-]*\.o\b', body)
                  if not v.endswith('apps/aui.o') and 'AUI_OBJ' not in v]
            if vb and '$(AUI_LIBS)' not in body:
                bad.append(n)
        for b in sorted(set(bad)) or []:
            fails.append('FAIL bypass %s:%d' % (name, b))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', default=None)
    a = ap.parse_args()
    files = [os.path.join(ROOT, 'Makefile')] + sorted(glob.glob(os.path.join(ROOT, 'tests', '**', '*.mk'), recursive=True))
    fails = []
    for f in files:
        try:
            t = open(f, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        fails += check(os.path.relpath(f, ROOT), t)
    if a.inject is not None:
        fails += check('<inject>', a.inject.replace('\\n', '\n'))
    for f in fails:
        print(f)
    if fails:
        print('aui_link_check: %d bypassing site(s)' % len(fails))
        return 1
    print('aui_link_check: 0 bypassing sites')
    return 0


if __name__ == '__main__':
    sys.exit(main())
