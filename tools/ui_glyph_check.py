#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UI string lint: glyph coverage and English-in-UI (design 3.6, addendum E).

    ui_glyph_check.py [--diff REV] [--english] [--root DIR] [--font-root DIR] [paths...]

Extracts C string literals from the given files/directories (.c .h .inc), or
-- with --diff REV -- only literals that START on a line added since REV under
c/ (intersected with `paths` when given).

GLYPHS. For each non-ASCII codepoint the renderer's REAL fallback chain is
walked: the order of `order_reg[]` in c/kernel/gui/text.c (read from the
source, not hard-coded), each face resolved through text_init()'s load_font()
calls and the Makefile's DISK_PACK `src:/fonts/<file>` entries (so /fonts/text.ttf
is third_party/fonts/DejaVuSans.ttf via $(FONT_TEXT)). cmap formats 4 and 12 are
read by a small pure-Python sfnt parser. One line per codepoint:

    U+2318 text.ttf

and `FAIL glyph U+XXXX <file>:<line>` for every literal holding a codepoint no
face in the chain has.

ENGLISH (--english). ASCII words of 2+ letters in literals that are arguments
of text-drawing calls (TEXT_CALLS below) are flagged as
`FAIL english <word> <file>:<line>`, except the allowlist (proper names,
protocol names, units), format specifiers, and tokens that look like file names
or code (contain . / _ : @ \\ < > = # or a digit).

Exit status 1 when any FAIL line was printed.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Calls whose string arguments are shown to the user.
TEXT_CALLS = re.compile(
    r"^(aui_text\w*|aui_label|aui_heading|aui_button\w*|aui_sidebar_row|"
    r"aui_tb_title|aui_alert|aui_empty_state|gui_text\w*|gui_create|"
    r"gui_menu_set|wm_text\w*)$")

ALLOW = {
    "LogitOS", "Logit", "AngelScript", "AAC", "PNG", "JPEG", "KB", "MB", "GB",
    "UTF-8", "IPv4", "DHCP", "DNS", "PID", "CPU", "URL", "https",
    # units (addendum E: "units")
    "px", "pt", "ms", "ns", "Hz", "kHz", "MHz", "GHz", "KiB", "MiB", "GiB",
    "TB", "dpi", "fps",
}

SRC_EXT = (".c", ".h", ".inc")
FMT_RE = re.compile(r"%[-+ 0#]*(\*|\d+)?(\.(\*|\d+))?(hh|h|ll|l|z|j|t|L)?[a-zA-Z%]")
CODEISH = re.compile(r"[./_:@\\<>=#0-9]")


# --------------------------------------------------------------------------
# sfnt cmap

class Cmap:
    """Membership test for a TrueType file's Unicode cmap (formats 4 and 12)."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            d = fh.read()
        self.path = path
        self.name = os.path.basename(path)
        self.d = d
        self.f4 = []      # (end, start, delta, rangeoff, rangeoff_pos)
        self.f12 = []     # (start, end, startgid)
        num = self._u16(4)
        cmap = None
        for i in range(num):
            rec = 12 + 16 * i
            if d[rec:rec + 4] == b"cmap":
                cmap = self._u32(rec + 8)
        if cmap is None:
            raise ValueError("%s: no cmap table" % path)
        n = self._u16(cmap + 2)
        # The renderer (c/lib/text/ttf.c pick_cmap) uses ONE subtable: (3,10)
        # format 12, else (0,*) format 12, else (3,1) format 4, else (0,*)
        # format 4. Mirror that choice; do not union the tables.
        best, best_score = None, -1
        for i in range(n):
            pid = self._u16(cmap + 4 + 8 * i)
            eid = self._u16(cmap + 6 + 8 * i)
            off = cmap + self._u32(cmap + 8 + 8 * i)
            fmt = self._u16(off)
            score = -1
            if pid == 3 and eid == 10 and fmt == 12:
                score = 4
            elif pid == 0 and fmt == 12:
                score = 3
            elif pid == 3 and eid == 1 and fmt == 4:
                score = 2
            elif pid == 0 and fmt == 4:
                score = 1
            if score > best_score:
                best, best_score = (fmt, off), score
        if best is not None:
            if best[0] == 4:
                self._parse4(best[1])
            else:
                self._parse12(best[1])

    def _u16(self, o):
        return int.from_bytes(self.d[o:o + 2], "big")

    def _u32(self, o):
        return int.from_bytes(self.d[o:o + 4], "big")

    def _parse4(self, off):
        segx2 = self._u16(off + 6)
        seg = segx2 // 2
        ends = off + 14
        starts = ends + segx2 + 2
        deltas = starts + segx2
        ranges = deltas + segx2
        for i in range(seg):
            self.f4.append((self._u16(ends + 2 * i), self._u16(starts + 2 * i),
                            self._u16(deltas + 2 * i), self._u16(ranges + 2 * i),
                            ranges + 2 * i))

    def _parse12(self, off):
        n = self._u32(off + 12)
        for i in range(n):
            g = off + 16 + 12 * i
            self.f12.append((self._u32(g), self._u32(g + 4), self._u32(g + 8)))

    def gid(self, cp):
        for s, e, g in self.f12:
            if s <= cp <= e:
                return g + cp - s
        if cp > 0xFFFF:
            return 0
        for end, start, delta, ro, pos in self.f4:
            if start <= cp <= end:
                if ro == 0:
                    return (cp + delta) & 0xFFFF
                p = pos + ro + 2 * (cp - start)
                if p + 2 > len(self.d):
                    return 0
                g = self._u16(p)
                return 0 if g == 0 else (g + delta) & 0xFFFF
        return 0

    def has(self, cp):
        return self.gid(cp) != 0


# --------------------------------------------------------------------------
# the renderer's fallback chain, read from the sources

def _make_vars(text):
    vs = {}
    for m in re.finditer(r"^([A-Z_][A-Z0-9_]*)\s*:?=\s*(\S+)\s*$", text, re.M):
        vs.setdefault(m.group(1), m.group(2))
    return vs


def _expand(s, vs):
    for _ in range(5):
        s2 = re.sub(r"\$\(([A-Za-z0-9_]+)\)", lambda m: vs.get(m.group(1), m.group(0)), s)
        if s2 == s:
            break
        s = s2
    return s


def font_chain(root):
    """[(basename, path)] in the order text.c walks for a regular-weight run."""
    text_c = open(os.path.join(root, "c/kernel/gui/text.c"), errors="replace").read()
    mk = open(os.path.join(root, "Makefile"), errors="replace").read()
    slots = {}
    for m in re.finditer(r'load_font\("(/fonts/[^"]+)",\s*(F_\w+)\)', text_c):
        slots[m.group(2)] = m.group(1)
    m = re.search(r"order_reg\[\d*\]\s*=\s*\{([^}]*)\}", text_c)
    if not m:
        raise SystemExit("ui_glyph_check: cannot find order_reg[] in c/kernel/gui/text.c")
    order = [t.strip() for t in m.group(1).split(",") if t.strip()]
    vs = _make_vars(mk)
    packed = {}
    for pm in re.finditer(r"(\S+):(/fonts/[A-Za-z0-9_.-]+)", mk):
        packed.setdefault(pm.group(2), _expand(pm.group(1), vs))
    chain = []
    for slot in order:
        vfs = slots.get(slot)
        if not vfs:
            raise SystemExit("ui_glyph_check: no load_font() for %s in text.c" % slot)
        src = packed.get(vfs) or ("fsroot" + vfs)
        path = os.path.join(root, src)
        if not os.path.exists(path):
            raise SystemExit("ui_glyph_check: font %s (%s) not found at %s" % (vfs, slot, path))
        chain.append((os.path.basename(vfs), path))
    return chain


# --------------------------------------------------------------------------
# C literal extraction

ESC = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
       "\\": 92, "'": 39, '"': 34, "?": 63, "e": 27}


def literals(src):
    """Yield (line, text, callee) for every string literal in C source `src`.
    `callee` is the identifier of the innermost enclosing call whose parens are
    open at the literal, skipping bare grouping parens; '' when none."""
    out = []
    i, n, line = 0, len(src), 1
    stack = []            # callee identifier per open paren ('' = grouping)
    last_ident = ""
    at_bol = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            at_bol = True
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == "/" and src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
            continue
        if c == "/" and src.startswith("//", i):
            while i < n and src[i] != "\n":
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    line += 1
                    i += 1
                i += 1
            continue
        if c == "#" and at_bol:
            m = re.match(r"#\s*include\b[^\n]*", src[i:])
            if m:
                i += m.end()
                continue
        at_bol = False
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'" and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            last_ident = ""
            continue
        if c == '"':
            start_line = line
            buf = bytearray()
            j = i + 1
            while j < n and src[j] != '"' and src[j] != "\n":
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    e = src[j + 1]
                    if e in ESC:
                        buf.append(ESC[e]); j += 2
                    elif e == "x":
                        m = re.match(r"[0-9a-fA-F]+", src[j + 2:])
                        if m:
                            buf.append(int(m.group(0)[:2], 16) & 0xFF)
                            j += 2 + len(m.group(0))
                        else:
                            j += 2
                    elif e in "01234567":
                        m = re.match(r"[0-7]{1,3}", src[j + 1:])
                        buf.append(int(m.group(0), 8) & 0xFF); j += 1 + len(m.group(0))
                    elif e in "uU":
                        k = 4 if e == "u" else 8
                        h = src[j + 2:j + 2 + k]
                        try:
                            buf.extend(chr(int(h, 16)).encode("utf-8"))
                        except ValueError:
                            pass
                        j += 2 + k
                    elif e == "\n":
                        line += 1; j += 2
                    else:
                        buf.append(ord(e) & 0x7F); j += 2
                else:
                    buf.extend(ch.encode("utf-8"))
                    j += 1
            callee = ""
            for k in range(len(stack) - 1, -1, -1):
                if stack[k]:
                    callee = stack[k]
                    break
            out.append((start_line, buf.decode("utf-8", "replace"), callee))
            i = j + 1
            last_ident = ""
            continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            last_ident = src[i:j]
            i = j
            continue
        if c == "(":
            kw = last_ident in ("if", "while", "for", "switch", "return", "sizeof")
            stack.append("" if kw else last_ident)
            last_ident = ""
        elif c == ")":
            if stack:
                stack.pop()
            last_ident = ""
        elif c in ";{}":
            stack.clear()
            last_ident = ""
        else:
            if not c.isdigit():
                last_ident = ""
        i += 1
    return out


def english_words(text):
    t = FMT_RE.sub(" ", text)
    words = []
    for tok in t.split():
        tok = tok.strip(",;()[]{}!?\"'…")
        if tok in ALLOW or CODEISH.search(tok):
            continue
        for w in re.findall(r"[A-Za-z]{2,}", tok):
            if w in ALLOW:
                continue
            words.append(w)
    return words


# --------------------------------------------------------------------------
# file selection

def source_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for dp, dn, fn in os.walk(p):
                dn[:] = sorted(d for d in dn if not d.startswith("."))
                for f in sorted(fn):
                    if f.endswith(SRC_EXT):
                        yield os.path.join(dp, f)
        elif os.path.isfile(p):
            yield p
        else:
            print("ui_glyph_check: no such path %s" % p, file=sys.stderr)
            raise SystemExit(2)


def added_lines(root, rev):
    """{relpath: set(line numbers)} added under c/ since `rev` (worktree vs rev)."""
    r = subprocess.run(["git", "-C", root, "diff", "-U0", rev, "--", "c/"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("ui_glyph_check: git diff %s failed: %s" % (rev, r.stderr.strip()))
    out, cur = {}, None
    for ln in r.stdout.splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:] if ln.startswith("+++ b/") else None
            if cur and cur.endswith(SRC_EXT):
                out.setdefault(cur, set())
            else:
                cur = None
        elif ln.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", ln)
            s = int(m.group(1))
            cnt = int(m.group(2)) if m.group(2) is not None else 1
            out[cur].update(range(s, s + cnt))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--diff", metavar="REV")
    ap.add_argument("--english", action="store_true")
    ap.add_argument("--root", default=ROOT, help="git root for --diff (default: this tree)")
    ap.add_argument("--font-root", default=ROOT,
                    help="tree whose text.c/Makefile/fonts define the chain (default: this tree)")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)

    targets = []            # (display path, real path, line filter or None)
    if a.diff:
        added = added_lines(root, a.diff)
        sel = [os.path.abspath(p) for p in a.paths]
        for rel, lines in sorted(added.items()):
            real = os.path.join(root, rel)
            if not os.path.exists(real):
                continue
            if sel and not any(real == s or real.startswith(s.rstrip("/") + "/") for s in sel):
                continue
            targets.append((rel, real, lines))
    else:
        if not a.paths:
            ap.error("give paths or --diff REV")
        for p in source_files(a.paths):
            targets.append((os.path.relpath(p, os.getcwd()) if not os.path.isabs(p) else p, p, None))

    chain = [(name, Cmap(path)) for name, path in font_chain(os.path.abspath(a.font_root))]
    fails = 0
    served = {}
    seen_cp = set()
    for disp, real, only in targets:
        src = open(real, errors="replace").read()
        for line, text, callee in literals(src):
            if only is not None and line not in only:
                continue
            for cp in sorted({ord(ch) for ch in text if ord(ch) > 127}):
                if cp not in served:
                    served[cp] = next((nm for nm, cm in chain if cm.has(cp)), None)
                if served[cp] is None:
                    print("FAIL glyph U+%04X %s:%d" % (cp, disp, line))
                    fails += 1
                elif cp not in seen_cp:
                    print("U+%04X %s" % (cp, served[cp]))
                seen_cp.add(cp)
            if a.english and TEXT_CALLS.match(callee):
                for w in english_words(text):
                    print("FAIL english %s %s:%d" % (w, disp, line))
                    fails += 1
    print("ui_glyph_check: %d file(s), %d codepoint(s), %d FAIL"
          % (len(targets), len(seen_cp), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
