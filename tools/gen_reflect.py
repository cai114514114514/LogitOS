#!/usr/bin/env python3
"""gen_reflect.py -- the IDL-attribute reflection table, generated from WPT's own data.

WHY GENERATED. html/dom/reflection-*.html is 64% of every failing subtest in the
WPT baseline, and it is one mechanism repeated a few hundred times: an IDL
attribute reflects a content attribute through a typed coercion. The type list
is short (DOMString, URL, enumerated, boolean, long, unsigned long, double, and
their limited/clamped variants); the attribute/element pairs are not, and the
corpus already carries them -- reflection-text.html loads elements-text.js, whose
whole content is

    a: { target: "string", rel: "string", href: "url",
         referrerPolicy: {type: "enum", keywords: [...]}, ... }

Typing that table into C by hand would create a second copy of the answer key
that can disagree with the questions, silently, one attribute at a time. This is
the same call tools/mkfont.py, genroots.py and gen_encoding_tables.py all made:
the upstream DATA is the input, ours is the code that reads it.

WHAT IT READS. The ten elements-*.js files that the ten reflection-*.html files
load, and nothing else -- notably NOT elements-aria-enumerated.js, which belongs
to a .tentative.html the ratchet does not count. Each is a JavaScript object
literal, so this parses a small subset of JS (identifier or quoted keys, string /
number / boolean / null / array / object values, both comment forms, trailing
commas). It is not a JS engine and does not need to be: any construct outside
that subset is a hard error rather than a silent skip, because a silently
dropped attribute would look exactly like a passing implementation of it.

WHAT IT WRITES. c/apps/browser/js_reflect.inc -- a flat table of
(element, IDL name, content name, type, per-type parameters), plus the string
pools it points into. js_reflect.c installs an accessor pair per row.

Usage:  python3 tools/gen_reflect.py [WPT_ROOT] [-o OUT]
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The ten files reflection-*.html load, by the name of the .html that loads them.
SUBSETS = ["text", "embedded", "forms", "forms-weekmonth", "grouping",
           "metadata", "misc", "obsolete", "sections", "tabular"]

# --------------------------------------------------------------------------
# A JS object-literal reader.
# --------------------------------------------------------------------------


class ParseError(Exception):
    pass


class JSObjReader:
    def __init__(self, text, where):
        self.s = text
        self.i = 0
        self.where = where

    def fail(self, msg):
        line = self.s.count("\n", 0, self.i) + 1
        raise ParseError("%s:%d: %s" % (self.where, line, msg))

    def ws(self):
        while self.i < len(self.s):
            c = self.s[self.i]
            if c in " \t\r\n":
                self.i += 1
            elif self.s.startswith("//", self.i):
                nl = self.s.find("\n", self.i)
                self.i = len(self.s) if nl < 0 else nl + 1
            elif self.s.startswith("/*", self.i):
                end = self.s.find("*/", self.i + 2)
                if end < 0:
                    self.fail("unterminated block comment")
                self.i = end + 2
            else:
                return

    def peek(self):
        self.ws()
        return self.s[self.i] if self.i < len(self.s) else ""

    def eat(self, c):
        if self.peek() != c:
            self.fail("expected %r, got %r" % (c, self.peek()))
        self.i += 1

    def string(self):
        q = self.s[self.i]
        self.i += 1
        out = []
        while True:
            if self.i >= len(self.s):
                self.fail("unterminated string")
            c = self.s[self.i]
            if c == "\\":
                self.i += 1
                e = self.s[self.i]
                self.i += 1
                out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0",
                            "\\": "\\", '"': '"', "'": "'", "/": "/"}.get(e, e))
            elif c == q:
                self.i += 1
                return "".join(out)
            else:
                out.append(c)
                self.i += 1

    def value(self):
        c = self.peek()
        if c in "\"'":
            return self.string()
        if c == "{":
            return self.obj()
        if c == "[":
            return self.arr()
        m = re.match(r"-?[0-9][0-9a-zA-Z_.+\-]*|true|false|null|undefined",
                     self.s[self.i:])
        if not m:
            self.fail("unhandled value at %r" % self.s[self.i:self.i + 24])
        tok = m.group(0)
        self.i += len(tok)
        if tok == "true":
            return True
        if tok == "false":
            return False
        if tok in ("null", "undefined"):
            return None
        try:
            return int(tok, 0)
        except ValueError:
            return float(tok)

    def arr(self):
        self.eat("[")
        out = []
        while True:
            if self.peek() == "]":
                self.i += 1
                return out
            out.append(self.value())
            if self.peek() == ",":
                self.i += 1
            elif self.peek() != "]":
                self.fail("expected , or ] in array")

    def obj(self):
        self.eat("{")
        out = {}
        while True:
            c = self.peek()
            if c == "}":
                self.i += 1
                return out
            if c in "\"'":
                key = self.string()
            else:
                m = re.match(r"[A-Za-z_$][A-Za-z0-9_$\-]*", self.s[self.i:])
                if not m:
                    self.fail("expected a key at %r" % self.s[self.i:self.i + 24])
                key = m.group(0)
                self.i += len(key)
            self.eat(":")
            out[key] = self.value()
            if self.peek() == ",":
                self.i += 1
            elif self.peek() != "}":
                self.fail("expected , or } in object")


def read_elements(path):
    """Pull the single `var <name> = { ... };` object literal out of an
    elements-*.js. The files each declare exactly one and then call
    mergeElements() on it; anything else in there is a comment."""
    src = open(path, encoding="utf-8").read()
    m = re.search(r"^var\s+[A-Za-z_$][A-Za-z0-9_$]*\s*=\s*\{", src, re.M)
    if not m:
        raise ParseError("%s: no `var x = {` table found" % path)
    r = JSObjReader(src, os.path.basename(path))
    r.i = m.end() - 1
    return r.obj()


# --------------------------------------------------------------------------
# The type map. Names are WPT's (reflection.js typeMap keys); the C enum is
# ours. A type WPT itself does not implement is recorded as RT_SKIP so the
# generated table still names the attribute -- an attribute that exists with no
# tests is a different fact from an attribute nobody listed.
# --------------------------------------------------------------------------

TYPES = [
    ("string", "RT_STRING"),
    ("url", "RT_URL"),
    ("enum", "RT_ENUM"),
    ("boolean", "RT_BOOL"),
    ("long", "RT_LONG"),
    ("limited long", "RT_LIMLONG"),
    ("unsigned long", "RT_ULONG"),
    ("limited unsigned long", "RT_LIMULONG"),
    ("limited unsigned long with fallback", "RT_LIMULONG_FB"),
    ("clamped unsigned long", "RT_CLAMPED_ULONG"),
    ("double", "RT_DOUBLE"),
    ("limited double", "RT_LIMDOUBLE"),
    # Not in WPT's typeMap: reflection.js puts these on its `unimplemented`
    # list and runs no subtest for them. They stay in the table because the
    # BROWSER still wants them; they simply score nothing.
    ("tokenlist", "RT_TOKENLIST"),
    ("settable tokenlist", "RT_TOKENLIST"),
]
TYPEMAP = dict(TYPES)

# The IDL name is camelCase; the content attribute name is its ASCII-lowercase
# unless domAttrName says otherwise. That is the spec's own rule and it holds
# for every row in this corpus -- assert it rather than assume it.


def content_name(idl, data):
    if isinstance(data, dict) and "domAttrName" in data:
        return data["domAttrName"]
    return idl.lower()


def merge_attr(tag, idl, a, b):
    """One attribute described by two subsets.

    This happens exactly once and it is not a corpus bug: elements-forms.js and
    elements-forms-weekmonth.js both describe `input.type`, with DIFFERENT
    keyword lists -- the second is a separate page because <input type=week|month>
    is feature-gated upstream, so the first page's list deliberately omits them.
    An implementation has to satisfy both, and the UNION does: the keywords one
    page omits are never SET by that page's tests, and every value it does set is
    classified the same way against either list. Anything else differing between
    two descriptions of one attribute is a real disagreement and stops here --
    silently picking a winner is how the table and the corpus drift apart."""
    if not (isinstance(a, dict) and isinstance(b, dict)):
        raise ParseError("%s.%s: described twice, incompatibly" % (tag, idl))
    ka, kb = a.get("keywords"), b.get("keywords")
    ra = {k: v for k, v in a.items() if k != "keywords"}
    rb = {k: v for k, v in b.items() if k != "keywords"}
    if ra != rb:
        raise ParseError("%s.%s: described twice, differing beyond keywords: %r vs %r"
                         % (tag, idl, ra, rb))
    out = dict(a)
    seen, merged = set(), []
    for k in list(ka or []) + list(kb or []):
        if k not in seen:
            seen.add(k)
            merged.append(k)
    out["keywords"] = merged
    return out


class Row:
    __slots__ = ("tag", "idl", "attr", "type", "flags", "keywords", "noncanon",
                 "dstr", "istr", "dnum", "vmin", "vmax")


def build_rows(tables):
    rows = []
    for tag in sorted(tables):
        for idl in sorted(tables[tag]):
            data = tables[tag][idl]
            if isinstance(data, str):
                data = {"type": data}
            if not isinstance(data, dict) or "type" not in data:
                raise ParseError("%s.%s: no type" % (tag, idl))
            if isinstance(data["type"], dict):
                # An upstream typo, and one worth naming rather than routing
                # around silently: elements-obsolete.js writes marquee.behavior
                # and marquee.direction as {type: {type: "enum", ...}}. WPT's own
                # reflects() looks up typeMap[data.type], gets undefined for an
                # object, files the attribute under `unimplemented` and runs ZERO
                # subtests for it -- so these two score nothing either way. The
                # nesting is unwrapped because the BROWSER still wants a working
                # marquee.behavior; the corpus simply never asks about it.
                inner = dict(data["type"])
                for k, v in data.items():
                    if k != "type":
                        inner.setdefault(k, v)
                data = inner
            t = data["type"]
            if t not in TYPEMAP:
                raise ParseError("%s.%s: unknown type %r" % (tag, idl, t))
            r = Row()
            r.tag = tag
            r.idl = idl
            r.attr = content_name(idl, data)
            r.type = TYPEMAP[t]
            r.flags = []
            r.keywords = data.get("keywords") or []
            r.noncanon = sorted((data.get("nonCanon") or {}).items())
            r.dstr = None
            r.istr = None
            r.dnum = 0.0
            r.vmin = 0
            r.vmax = 0

            if data.get("isNullable"):
                r.flags.append("RF_NULLABLE")
            if "treatNullAsEmptyString" in data:
                r.flags.append("RF_NULLEMPTY")
            if data.get("customGetter"):
                r.flags.append("RF_CUSTOMGET")

            has_def = "defaultVal" in data
            dv = data.get("defaultVal")
            if t == "enum":
                # The missing-value default, then the invalid-value default
                # (which falls back to the missing-value default when absent --
                # they are NOT the same knob and conflating them is exactly the
                # bug the negative control below reproduces).
                #
                # defaultVal may be an ARRAY of acceptable answers (preload):
                # WPT asserts membership, so any element passes. Take the first.
                if has_def and dv is None:
                    r.flags.append("RF_DEFNULL")
                    r.dstr = None
                else:
                    r.dstr = (dv[0] if isinstance(dv, list) else dv) if has_def else ""
                if "invalidVal" in data:
                    iv = data["invalidVal"]
                    if iv is None:
                        r.flags.append("RF_INVNULL")
                        r.istr = None
                    else:
                        r.istr = iv[0] if isinstance(iv, list) else iv
                else:
                    r.istr = r.dstr
                    if "RF_DEFNULL" in r.flags:
                        r.flags.append("RF_INVNULL")
            elif t in ("long", "limited long", "unsigned long",
                       "limited unsigned long",
                       "limited unsigned long with fallback",
                       "clamped unsigned long", "double", "limited double"):
                if has_def and dv is None:
                    # tabIndex: "the default is too complicated" -- WPT skips
                    # every subtest whose expectation would be the default.
                    r.flags.append("RF_DEFNULL")
                elif has_def:
                    r.flags.append("RF_HASDEF")
                    r.dnum = float(dv)
                if t == "clamped unsigned long":
                    if "min" not in data or "max" not in data:
                        raise ParseError("%s.%s: clamped without min/max" % (tag, idl))
                    r.vmin = int(data["min"])
                    r.vmax = int(data["max"])
            else:
                if has_def and dv is not None:
                    r.flags.append("RF_HASDEF")
                    r.dstr = dv if isinstance(dv, str) else str(dv)
            rows.append(r)
    return rows


# --------------------------------------------------------------------------
# Emit
# --------------------------------------------------------------------------

def cstr(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20 or ord(ch) == 0x7f:
            out.append("\\%03o" % ord(ch))
        elif ord(ch) > 0x7f:
            out.extend("\\%03o" % b for b in ch.encode("utf-8"))
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


def emit(rows, out, sources):
    kw_pool = []        # flat keyword list
    can_pool = []       # flat (from, to) list

    L = []
    w = L.append
    w("/* js_reflect.inc -- GENERATED by tools/gen_reflect.py. DO NOT EDIT.\n"
      " *\n"
      " * Source: the WPT corpus's own reflection tables,\n")
    for s in sources:
        w(" *   %s\n" % s)
    w(" *\n"
      " * Regenerate with `make reflect-table` (or python3 tools/gen_reflect.py).\n"
      " * The generator's header comment explains why this is data and not code.\n"
      " */\n\n")

    w("static const char *const RFL_KW[] = {\n")
    kw_index = {}
    for r in rows:
        if not r.keywords:
            continue
        key = tuple(r.keywords)
        if key in kw_index:
            continue
        kw_index[key] = len(kw_pool)
        for k in r.keywords:
            kw_pool.append(k)
    for i in range(0, len(kw_pool), 4):
        w("    " + " ".join(cstr(k) + "," for k in kw_pool[i:i + 4]) + "\n")
    w("};\n\n")

    w("/* non-canonical value map: pairs, [from, to]; a NULL `to` means the\n"
      " * canonical answer is null (a nullable enum reading as absent). */\n")
    w("static const char *const RFL_CANON[] = {\n")
    can_index = {}
    for r in rows:
        if not r.noncanon:
            continue
        key = tuple(r.noncanon)
        if key in can_index:
            continue
        can_index[key] = len(can_pool) // 2
        for a, b in r.noncanon:
            can_pool.append(a)
            can_pool.append(b)
    for i in range(0, len(can_pool), 2):
        a, b = can_pool[i], can_pool[i + 1]
        w("    %s, %s,\n" % (cstr(a), cstr(b) if b is not None else "0"))
    w("};\n\n")

    w("static const struct refl_attr RFL_ATTRS[] = {\n")
    per_tag = {}
    for r in rows:
        per_tag.setdefault(r.tag, []).append(r)
    order = []
    idx = 0
    tag_spans = []
    for tag in sorted(per_tag):
        first = idx
        for r in per_tag[tag]:
            flags = " | ".join(r.flags) if r.flags else "0"
            kw = kw_index.get(tuple(r.keywords), 0) if r.keywords else 0
            can = can_index.get(tuple(r.noncanon), 0) if r.noncanon else 0
            w("    { %s, %s, %s, %d, %s, %d, %s, %s, %s, %s, %d,%d, %s, RTGT_SELF },\n"
              % (cstr(r.idl), cstr(r.attr), r.type,
                 len(r.keywords), ("RFL_KW + %d" % kw) if r.keywords else "0",
                 len(r.noncanon),
                 ("RFL_CANON + %d" % (2 * can)) if r.noncanon else "0",
                 cstr(r.dstr) if r.dstr is not None else "0",
                 cstr(r.istr) if r.istr is not None else "0",
                 repr(r.dnum), r.vmin, r.vmax, flags))
            idx += 1
            order.append(r)
        tag_spans.append((tag, first, idx - first))
    w("};\n\n")

    w("static const struct refl_elem RFL_ELEMS[] = {\n")
    for tag, first, n in tag_spans:
        w("    { %-16s %4d, %3d },\n" % (cstr(tag) + ",", first, n))
    w("};\n\n")
    w("#define RFL_NATTRS %d\n" % len(order))
    w("#define RFL_NELEMS %d\n" % len(tag_spans))
    text = "".join(L)
    with open(out, "w", newline="\n", encoding="utf-8") as f:
        f.write(text)
    return len(order), len(tag_spans)


def main():
    args = [a for a in sys.argv[1:]]
    out = os.path.join(ROOT, "c", "apps", "browser", "js_reflect.inc")
    if "-o" in args:
        k = args.index("-o")
        out = args[k + 1]
        del args[k:k + 2]
    wpt = args[0] if args else os.path.join(ROOT, "third_party", "wpt")
    base = os.path.join(wpt, "html", "dom")
    if not os.path.isdir(base):
        sys.stderr.write(
            "gen_reflect.py: no corpus at %s -- nothing to regenerate.\n"
            "  The committed js_reflect.inc stands; `make wpt-fetch` brings the\n"
            "  data back if you need to rebuild it.\n" % base)
        return 1

    tables = {}
    sources = []
    for name in SUBSETS:
        path = os.path.join(base, "elements-%s.js" % name)
        if not os.path.exists(path):
            sys.stderr.write("gen_reflect.py: missing %s\n" % path)
            return 1
        sources.append("html/dom/elements-%s.js" % name)
        one = read_elements(path)
        for tag, attrs in one.items():
            # Two subsets naming the same element is a UNION, not a conflict;
            # the corpus never gives one attribute two different types, so a
            # collision with a different value is a real error worth stopping on.
            dst = tables.setdefault(tag, {})
            for k, v in attrs.items():
                if k in dst and dst[k] != v:
                    dst[k] = merge_attr(tag, k, dst[k], v)
                else:
                    dst[k] = v

    rows = build_rows(tables)
    n, e = emit(rows, out, sources)
    sys.stderr.write("gen_reflect.py: %d attributes over %d elements -> %s\n"
                     % (n, e, os.path.relpath(out, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
