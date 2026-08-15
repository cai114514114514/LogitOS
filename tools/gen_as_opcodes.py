#!/usr/bin/env python3
"""Check (or regenerate) the constant tables that AetherScript hand-mirrors.

Three separate implementations carry copies of the same numbers:

  authority                     mirror
  ------------------------------------------------------------------
  c/apps/as/as.h  OpCode        fsroot/as/lib/asc.as   OP_*      (order IS the ABI)
  c/apps/as/as.h  AS_BC_VERSION fsroot/as/lib/asc.as   AS_BC_VERSION
  c/apps/as/as_bc.c  K_*        fsroot/as/lib/asc.as   K_*
  c/apps/as/lexer.h  TokType    fsroot/as/lib/aslex.as T_*  + asc.as T_*
  c/apps/as/lexer.c  keywords   fsroot/as/lib/aslex.as KEYWORDS
  c/apps/as/vm.c + as_native.c  c/apps/as/complete.c   BUILTINS / SYSCONSTS / *_METHODS
  c/apps/as/as_port.c (M27)     c/apps/as/complete.c   BUILTINS / PORT_ / PROC_METHODS

A mismatch in the first four is a *silent* miscompile: the self-hosted compiler
emits bytecode the C VM reads as a different instruction. Nothing in the build
catches it today -- `make check-asops` is that gate.

M28 added an eighth case that is not cross-language but is the same failure
shape and was, until then, checked by nothing at all: vm.c's computed-goto
`dispatch[]` is positionally matched to the as.h `OpCode` enum BY HAND (see
`c_dispatch_order` below for exactly how a mismatch there used to compile
clean and silently misdispatch every opcode after the one that moved).

Default mode is --check: it only reads, never writes, so it is safe to wire into
the build. --write is for the milestone that actually renumbers opcodes; it is
deliberately not implemented until an opcode renumber is on the table. (An
earlier version of this docstring named a specific AS_BC_VERSION as "frozen" --
that number drifts every time a milestone appends opcodes, as.h is the
authority on the current one, and repeating it here would just go stale again.)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FAILURES = []


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def fail(table, msg):
    FAILURES.append((table, msg))


def c_enum(text, enum_name, member_prefix=None):
    """Ordered [(name, value)] of a C enum, honouring explicit `= N` members.

    The brace class is [^{}] on purpose: a `.*?` here happily spans from an
    earlier enum through intervening macros to the closing brace of this one.
    """
    body = None
    if enum_name:
        m = re.search(r"typedef\s+enum\s*\{([^{}]*)\}\s*" + enum_name + r"\s*;", text, re.S)
        if m:
            body = m.group(1)
    if body is None and member_prefix:
        for m in re.finditer(r"\benum\s*\{([^{}]*)\}\s*;", text, re.S):
            if member_prefix in m.group(1):
                body = m.group(1)
                break
    if body is None:
        return []
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out, nxt = [], 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, val = item.split("=", 1)
            nxt = int(val.strip(), 0)
            item = name.strip()
        out.append((item, nxt))
        nxt += 1
    return out


def as_consts(text, prefix):
    """Ordered [(name, value)] of top-level `NAME = <int>` in an .as file."""
    out = []
    for line in text.splitlines():
        m = re.match(r"^(" + prefix + r"\w*)\s*=\s*(-?\d+)\s*(?:#.*)?$", line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


def c_string_list(text, var):
    """The string literals of a `static const char *const VAR[] = {...}`."""
    m = re.search(r"\b" + var + r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        return None
    return re.findall(r'"([^"]*)"', m.group(1))


def c_dispatch_order(text):
    """vm.c's computed-goto `dispatch[]`, as an ordered list of OP_* names.

    M28 D8: this table is a THIRD hand-mirrored copy of the OpCode enum (after
    asc.as and OPNAMES) and, unlike those two, nothing but a human ever read it
    before -- a label in the wrong position compiles cleanly and silently
    misdispatches every opcode after it. Most entries are `&&op_NAME`, which
    names the opcode by construction (op_CONST -> OP_CONST); the deliberate
    holes are `&&op_BAD /* OP_REALNAME: ... */`, where the comment -- not the
    label -- says which opcode the position stands in for. Returns None if the
    array can't be found (kept optional, like the other c_string_list-based
    checks, so a refactor of vm.c fails loudly here rather than raising).
    """
    m = re.search(r"dispatch\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        return None
    body = m.group(1)
    out = []
    for entry in re.finditer(r"&&\s*(op_\w+)\s*(/\*\s*(OP_\w+)\s*:[^*]*\*/)?", body):
        label, real = entry.group(1), entry.group(3)
        out.append(real if real else "OP_" + label[3:])
    return out


def cmp_ordered(table, authority, mirror, auth_src, mir_src):
    """Both name AND value AND order must agree -- order is the wire format."""
    if authority == mirror:
        return
    amap, mmap = dict(authority), dict(mirror)
    for name, val in authority:
        if name not in mmap:
            fail(table, "%s missing from %s (%s has it = %d)" % (name, mir_src, auth_src, val))
        elif mmap[name] != val:
            fail(table, "%s = %d in %s but = %d in %s" % (name, val, auth_src, mmap[name], mir_src))
    for name, val in mirror:
        if name not in amap:
            fail(table, "%s = %d in %s but does not exist in %s" % (name, val, mir_src, auth_src))
    if [n for n, _ in authority] != [n for n, _ in mirror] and not FAILURES:
        fail(table, "same members but different order (%s vs %s)" % (auth_src, mir_src))


def cmp_subset(table, authority, mirror, auth_src, mir_src):
    """The mirror may list a subset (it only needs the members it consumes), but
    every value it does list must be right -- a wrong number is the silent bug."""
    amap = dict(authority)
    for name, val in mirror:
        if name not in amap:
            fail(table, "%s = %d in %s but does not exist in %s" % (name, val, mir_src, auth_src))
        elif amap[name] != val:
            fail(table, "%s = %d in %s but = %d in %s" % (name, amap[name], auth_src, val, mir_src))


def cmp_set(table, authority, mirror, auth_src, mir_src):
    a, m = set(authority), set(mirror)
    for name in sorted(a - m):
        fail(table, "'%s' is in %s but missing from %s" % (name, auth_src, mir_src))
    for name in sorted(m - a):
        fail(table, "'%s' is in %s but no longer exists in %s" % (name, mir_src, auth_src))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--check"
    if mode == "--write":
        sys.exit("gen_as_opcodes.py: --write is not implemented (phase 1 freezes "
                 "AS_BC_VERSION and does not renumber opcodes; add it with the "
                 "renumber milestone so asc.as/aslex.as regenerate atomically)")
    if mode != "--check":
        sys.exit("usage: gen_as_opcodes.py [--check]")

    as_h = read("c/apps/as/as.h")
    lexer_h = read("c/apps/as/lexer.h")
    lexer_c = read("c/apps/as/lexer.c")
    as_bc_c = read("c/apps/as/as_bc.c")
    vm_c = read("c/apps/as/vm.c")
    native_c = read("c/apps/as/as_native.c")
    complete_c = read("c/apps/as/complete.c")
    port_c = read("c/apps/as/as_port.c")
    asc_as = read("fsroot/as/lib/asc.as")
    aslex_as = read("fsroot/as/lib/aslex.as")

    # 1. opcodes: order is the ABI.
    # M28 D8: OP__COUNT is a C-only dispatch-table sentinel (as.h), not a real
    # opcode -- it carries no wire-format meaning, asc.as never dispatches
    # bytecode (only emits it) so it has no reason to name a sentinel, and
    # OPNAMES is indexed by REAL opcode values only. Every comparison below that
    # walks "the opcodes" therefore filters it out once, here, rather than
    # teaching cmp_ordered (also used for tables that have no such sentinel)
    # to special-case one name.
    opcodes_full = c_enum(as_h, "OpCode")
    opcodes = [t for t in opcodes_full if t[0] != "OP__COUNT"]
    if len(opcodes) == len(opcodes_full):
        fail("opcodes", "as.h OpCode has no OP__COUNT sentinel (expected by the M28 dispatch[] check)")

    cmp_ordered("opcodes", opcodes, as_consts(asc_as, "OP_"), "as.h OpCode", "asc.as")

    # 1b. the disassembler's mnemonic table is indexed by opcode, so a missing
    # or reordered entry silently mislabels every instruction after it.
    opnames = c_string_list(as_bc_c, "OPNAMES")
    if opnames is not None:
        auth = [n[3:] for n, _ in opcodes]   # strip the OP_ prefix
        if auth != opnames:
            for i in range(max(len(auth), len(opnames))):
                a = auth[i] if i < len(auth) else "<missing>"
                b = opnames[i] if i < len(opnames) else "<missing>"
                if a != b:
                    fail("opnames", "index %d: as.h has OP_%s, as_bc.c OPNAMES has \"%s\"" % (i, a, b))

    # 1c. M28 D8: vm.c's computed-goto dispatch[] must list the SAME opcodes in
    # the SAME order as the enum -- previously asserted by nothing at all (see
    # c_dispatch_order's docstring). A mismatched length is reported as
    # <missing> pairs rather than silently truncating the shorter list, the
    # same style as the opnames check above.
    dispatch = c_dispatch_order(vm_c)
    if dispatch is not None:
        auth = [n for n, _ in opcodes]
        if auth != dispatch:
            for i in range(max(len(auth), len(dispatch))):
                a = auth[i] if i < len(auth) else "<missing>"
                b = dispatch[i] if i < len(dispatch) else "<missing>"
                if a != b:
                    fail("dispatch", "index %d: as.h enum has %s, vm.c dispatch[] has %s" % (i, a, b))

    # 2. bytecode version.
    m = re.search(r"#define\s+AS_BC_VERSION\s+(\d+)", as_h)
    auth_ver = int(m.group(1)) if m else None
    m = re.search(r"^AS_BC_VERSION\s*=\s*(\d+)", asc_as, re.M)
    mir_ver = int(m.group(1)) if m else None
    if auth_ver != mir_ver:
        fail("bc-version", "as.h says %s, asc.as says %s" % (auth_ver, mir_ver))

    # 3. constant tags.
    ktags = [(n, v) for n, v in c_enum(as_bc_c, None, "K_NIL") if n.startswith("K_")]
    cmp_ordered("const-tags", ktags, as_consts(asc_as, "K_"), "as_bc.c", "asc.as")

    # 4/5. token types. aslex.as IS the lexer, so it must be able to emit every
    # kind -> exact match. asc.as only consumes tokens and omits the ones it never
    # names (T_ERROR) -> subset, but every number it does carry must be right.
    toks = c_enum(lexer_h, "TokType")
    cmp_ordered("tokens(aslex.as)", toks, as_consts(aslex_as, "T_"), "lexer.h", "aslex.as")
    cmp_subset("tokens(asc.as)", toks, as_consts(asc_as, "T_"), "lexer.h", "asc.as")

    # 6. keywords: lexer.c maps literals to T_*; aslex.as has a KEYWORDS dict.
    kw_c = set(re.findall(r'memcmp\(s,\s*"(\w+)"', lexer_c))
    m = re.search(r"KEYWORDS\s*=\s*\{(.*?)\}", aslex_as, re.S)
    kw_as = set(re.findall(r'"(\w+)"\s*:', m.group(1))) if m else set()
    cmp_set("keywords", kw_c, kw_as, "lexer.c", "aslex.as")
    kw_complete = c_string_list(complete_c, "KEYWORDS")
    if kw_complete is not None:
        cmp_set("keywords(complete.c)", kw_c, kw_complete, "lexer.c", "complete.c")

    # 7. builtins registered at runtime vs. what the IDE offers.
    natives = set(re.findall(r'as_define_native\("([^"]+)"', vm_c))
    natives |= set(re.findall(r'as_define_native\("([^"]+)"', native_c))
    natives |= set(re.findall(r'as_define_native\("([^"]+)"', port_c))
    builtins_complete = c_string_list(complete_c, "BUILTINS")
    if builtins_complete is not None:
        cmp_set("builtins", natives, builtins_complete, "vm.c+as_native.c", "complete.c")

    # 8. SYS_*/EV_* integer globals.
    sysconsts = set(re.findall(r'as_define_int\("([^"]+)"', native_c))
    sys_complete = c_string_list(complete_c, "SYSCONSTS")
    if sys_complete is not None:
        cmp_set("sysconsts", sysconsts, sys_complete, "as_native.c", "complete.c")

    # 9. method names dispatched by OP_INVOKE vs. the IDE's per-type tables.
    methods = set(re.findall(r'name_eq\(name,\s*"([^"]+)"', vm_c))
    # M27: port/proc methods are dispatched in as_port.c, through name_is().
    methods |= set(re.findall(r'name_is\(name,\s*"([^"]+)"', port_c))
    mir = []
    for var in ("LIST_METHODS", "DICT_METHODS", "STR_METHODS",
                "PORT_METHODS", "PROC_METHODS", "CAP_METHODS"):
        got = c_string_list(complete_c, var)
        if got:
            mir += got
    if mir:
        cmp_set("methods", methods, mir, "vm.c OP_INVOKE", "complete.c")

    if FAILURES:
        print("check-asops: FAIL -- hand-mirrored constant tables have drifted\n")
        width = max(len(t) for t, _ in FAILURES)
        for table, msg in FAILURES:
            print("  [%-*s] %s" % (width, table, msg))
        print("\n%d mismatch(es). These tables are copied by hand; fix the mirror "
              "to match the authority." % len(FAILURES))
        return 1
    print("check-asops: ok (opcodes, opnames, dispatch, bc-version, const-tags, tokens x2, "
          "keywords, builtins, sysconsts, methods)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
