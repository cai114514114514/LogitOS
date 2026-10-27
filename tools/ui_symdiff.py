#!/usr/bin/env python3
"""ui_symdiff.py -- prove that two builds of an object differ only in LAYOUT.

A pure code move (ui-aui-core split aui.c into unity-included modules) changes
the order of functions and data in aui.o, so a hash of the stripped sections
changes although no instruction or byte of data did. This tool compares two
ELF64 relocatable objects SYMBOL BY SYMBOL, with everything that depends on
placement normalised away:

  functions  `llvm-objdump -d --no-show-raw-insn -r`, per function: addresses
             stripped; a branch or call keeps only its `<symbol+off>` target;
             each relocation is rewritten from `section+addend` to what it
             actually names -- `symbol+offset` (the containing symbol of the
             target, PC-relative addends corrected by the instruction end), the
             string at that offset of a .rodata.str* section, the bytes of a
             constant-pool entry (.LCPIn_m), or `anon:<sha>+off` for unnamed
             data (jump tables), keyed by its normalised content;
  data       each OBJECT symbol by name: section kind, size, bytes, and its own
             relocations normalised the same way (.bss: kind and size only);
  strings    every NUL-terminated string of the .rodata.str* sections, compared
             as a MULTISET (merge order is placement);
  symbols    the sets of defined functions, objects and undefined references.

usage: ui_symdiff.py BASE.o BRANCH.o
       ui_symdiff.py --dirs BASE_DIR BRANCH_DIR NAME.o [NAME.o ...]
Prints `ok symdiff <name>: N functions, M objects, K strings identical` or one
`DIFF <name> <what>` line per difference, and exits 1 on any difference (2 on a
usage or tool error). LLVM_OBJDUMP names the disassembler (default: llvm-objdump
on PATH, then Homebrew's).
"""
import collections
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
STT_OBJECT, STT_FUNC, STT_SECTION = 1, 2, 3


def objdump_bin():
    env = os.environ.get('LLVM_OBJDUMP')
    if env:
        return env
    for cand in ('llvm-objdump', '/opt/homebrew/opt/llvm/bin/llvm-objdump',
                 '/usr/local/opt/llvm/bin/llvm-objdump'):
        p = shutil.which(cand) if '/' not in cand else (cand if os.path.exists(cand) else None)
        if p:
            return p
    sys.exit('ui_symdiff: no llvm-objdump (set LLVM_OBJDUMP)')


class Elf:
    def __init__(self, path):
        self.path = path
        b = self.b = open(path, 'rb').read()
        if b[:4] != b'\x7fELF' or b[4] != 2 or b[5] != 1:
            sys.exit('ui_symdiff: %s is not an ELF64 little-endian object' % path)
        shoff, = struct.unpack_from('<Q', b, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x3A)
        self.sec = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shentsize)
            self.sec.append(dict(name_off=name, type=typ, off=off, size=size, link=link,
                                 info=info, entsize=entsize))
        strtab = self.sec[shstrndx]
        for s in self.sec:
            s['name'] = self._str(strtab['off'] + s['name_off'])
        self.syms = []
        for s in self.sec:
            if s['type'] != SHT_SYMTAB:
                continue
            st = self.sec[s['link']]
            for k in range(s['size'] // 24):
                name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', b, s['off'] + 24 * k)
                self.syms.append(dict(name=self._str(st['off'] + name), type=info & 15,
                                      bind=info >> 4, shndx=shndx, value=value, size=size))
        self.rela = collections.defaultdict(list)   # target section index -> [(off, type, sym, addend)]
        for s in self.sec:
            if s['type'] != SHT_RELA:
                continue
            for k in range(s['size'] // 24):
                off, info, add = struct.unpack_from('<QQq', b, s['off'] + 24 * k)
                self.rela[s['info']].append((off, info & 0xFFFFFFFF, info >> 32, add))
        for v in self.rela.values():
            v.sort()
        self.by_sec = collections.defaultdict(list)
        for i, sy in enumerate(self.syms):
            if sy['type'] in (STT_OBJECT, STT_FUNC) or (sy['type'] == 0 and sy['name'].startswith('.LCPI')):
                if 0 < sy['shndx'] < len(self.sec):
                    self.by_sec[sy['shndx']].append(sy)
        for v in self.by_sec.values():
            v.sort(key=lambda s: s['value'])

    def _str(self, off):
        e = self.b.index(b'\0', off)
        return self.b[off:e].decode('latin-1')

    def data(self, si, off, n):
        s = self.sec[si]
        if s['type'] == SHT_NOBITS:
            return b'\0' * n
        return self.b[s['off'] + off: s['off'] + off + n]

    def containing(self, si, off):
        """the named symbol of section si that contains offset off"""
        best = None
        for sy in self.by_sec.get(si, ()):
            if sy['value'] <= off and (off < sy['value'] + max(sy['size'], 1)):
                best = sy
        return best

    def gap(self, si, off):
        """the unnamed byte range of section si around off: [start, end)"""
        start, end = 0, self.sec[si]['size']
        for sy in self.by_sec.get(si, ()):
            e = sy['value'] + sy['size']
            if e <= off and e > start:
                start = e
            if sy['value'] > off and sy['value'] < end:
                end = sy['value']
        return start, end


class Norm:
    def __init__(self, elf):
        self.e = elf
        self.anon = {}

    def target(self, symidx, addend):
        """what a relocation (symbol, addend) names, placement-free"""
        e = self.e
        sy = e.syms[symidx]
        if sy['type'] != STT_SECTION and sy['shndx'] == 0:
            return '%s%+d' % (sy['name'], addend)            # undefined: external
        if sy['name'].startswith('.LCPI'):
            sec = e.sec[sy['shndx']]
            n = sec['entsize'] or 16
            return 'cst:%s%+d' % (e.data(sy['shndx'], sy['value'], n).hex(), addend)
        if sy['type'] != STT_SECTION:
            return '%s%+d' % (sy['name'], addend)
        si = sy['shndx']
        sec = e.sec[si]
        off = sy['value'] + addend
        if sec['name'].startswith('.rodata.str'):
            raw = e.data(si, 0, sec['size'])
            end = raw.index(b'\0', off) if off < len(raw) else off
            return 'str:%r' % raw[off:end].decode('latin-1')
        if sec['name'].startswith('.rodata.cst'):
            n = sec['entsize'] or 16
            base = off - off % n
            return 'cst:%s+%d' % (e.data(si, base, n).hex(), off - base)
        c = e.containing(si, off)
        if c is not None:
            return '%s+%d' % (c['name'], off - c['value'])
        a, b = e.gap(si, off)
        return 'anon:%s+%d' % (self.anon_key(si, a, b), off - a)

    def anon_key(self, si, a, b):
        k = (si, a, b)
        if k not in self.anon:
            self.anon[k] = '?'                                # recursion guard
            body = self.blob(si, a, b)
            self.anon[k] = hashlib.sha256(repr(body).encode()).hexdigest()[:12]
        return self.anon[k]

    def blob(self, si, a, b):
        """bytes of [a, b) of section si with its relocations normalised"""
        e = self.e
        out, pos = [], a
        for off, typ, sym, add in e.rela.get(si, ()):
            if off < a or off >= b:
                continue
            out.append(e.data(si, pos, off - pos).hex())
            out.append('<R%d %s>' % (typ, self.target(sym, add)))
            pos = off + (4 if typ in (2, 4, 10, 11, 24) else 8)
        out.append(e.data(si, pos, b - pos).hex())
        return out


INS_RE = re.compile(r'^\s*([0-9a-f]+):\s*(.*)$')
FN_RE = re.compile(r'^([0-9a-f]+) <(.+)>:$')
RELOC_RE = re.compile(r'^\s+([0-9a-f]+):\s+(R_\S+)\s+(.*)$')
ADDR_TGT_RE = re.compile(r'\b0x[0-9a-f]+ (<[^>]+>)')
SEC_RE = re.compile(r'^Disassembly of section (\S+):$')
PCREL = {2, 4, 24}       # R_X86_64_PC32, PLT32, PC64


def functions(elf, objdump):
    """{name: [normalised lines]} for every function of every code section"""
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', '-r', elf.path],
                         capture_output=True, text=True)
    if out.returncode:
        sys.exit('ui_symdiff: %s failed on %s: %s' % (objdump, elf.path, out.stderr.strip()))
    norm = Norm(elf)
    secidx = {s['name']: i for i, s in enumerate(elf.sec)}
    relmap = {}
    fns = collections.OrderedDict()
    cur, cursec, insns = None, None, []   # insns: [(addr, text)]

    raw = []                              # (section, name, [(addr, text)])

    def flush():
        if cur is not None:
            raw.append((cursec, cur, insns))

    for line in out.stdout.splitlines():
        m = SEC_RE.match(line)
        if m:
            flush(); cur = None; insns = []
            cursec = secidx.get(m.group(1))
            if cursec is not None and cursec not in relmap:
                relmap[cursec] = {o: (o, t, s, a) for (o, t, s, a) in elf.rela.get(cursec, ())}
            continue
        m = FN_RE.match(line)
        if m:
            flush()
            cur, insns = m.group(2), []
            continue
        if RELOC_RE.match(line):
            continue                      # relocations come from the ELF itself
        m = INS_RE.match(line)
        if m and cur is not None:
            insns.append((int(m.group(1), 16), m.group(2)))
    flush()
    for k, (si, name, ins) in enumerate(raw):
        rel = relmap.get(si, {})
        # an instruction ends where the next one starts; the last one where
        # the next function (or the section) starts
        nxt = raw[k + 1][2][0][0] if k + 1 < len(raw) and raw[k + 1][0] == si and raw[k + 1][2] else \
            (elf.sec[si]['size'] if si is not None else 0)
        addrs = [a for a, _ in ins]
        lines = []
        for j, (a, t) in enumerate(ins):
            end = addrs[j + 1] if j + 1 < len(addrs) else nxt
            t = t.split('#')[0].rstrip()
            t = ADDR_TGT_RE.sub(r'\1', t)
            t = re.sub(r'\s+', ' ', t)
            ann = []
            for o in sorted(o for o in rel if a <= o < end):
                _, typ, sym, add = rel[o]
                if typ in PCREL:
                    add += end - o
                ann.append('R%d %s' % (typ, norm.target(sym, add)))
            lines.append(t + (' {' + '; '.join(ann) + '}' if ann else ''))
        fns[name] = lines
    return fns, norm


def objects(elf, norm):
    objs = {}
    for sy in elf.syms:
        if sy['type'] != STT_OBJECT or not (0 < sy['shndx'] < len(elf.sec)):
            continue
        sec = elf.sec[sy['shndx']]
        kind = sec['name'].split('.')[1] if '.' in sec['name'] else sec['name']
        if sec['type'] == SHT_NOBITS:
            body = ['bss']
        else:
            body = norm.blob(sy['shndx'], sy['value'], sy['value'] + sy['size'])
        objs[sy['name']] = (kind, sy['size'], sy['bind'], tuple(body))
    return objs


def strings(elf):
    c = collections.Counter()
    for i, s in enumerate(elf.sec):
        if s['name'].startswith('.rodata.str'):
            for part in elf.data(i, 0, s['size']).split(b'\0')[:-1]:
                c[part] += 1
    return c


def symsets(elf):
    d = set(); u = set()
    for sy in elf.syms:
        if sy['type'] == STT_SECTION or not sy['name'] or sy['name'].startswith('.L'):
            continue
        if sy['shndx'] == 0:
            u.add(sy['name'])
        elif sy['type'] in (STT_FUNC, STT_OBJECT):
            d.add((sy['name'], sy['type'], sy['bind']))
    return d, u


def compare(a_path, b_path, label, objdump):
    A, B = Elf(a_path), Elf(b_path)
    fa, na = functions(A, objdump)
    fb, nb = functions(B, objdump)
    diffs = []
    for n in sorted(set(fa) | set(fb)):
        if n not in fa or n not in fb:
            diffs.append('function %s only in %s' % (n, 'branch' if n in fb else 'base'))
        elif fa[n] != fb[n]:
            k = next(i for i, (x, y) in enumerate(zip(fa[n] + [None], fb[n] + [None])) if x != y)
            diffs.append('function %s differs at insn %d: %r vs %r' % (
                n, k, fa[n][k] if k < len(fa[n]) else None, fb[n][k] if k < len(fb[n]) else None))
    oa, ob = objects(A, na), objects(B, nb)
    for n in sorted(set(oa) | set(ob)):
        if oa.get(n) != ob.get(n):
            diffs.append('object %s: %s vs %s' % (n, str(oa.get(n))[:120], str(ob.get(n))[:120]))
    sa, sb = strings(A), strings(B)
    if sa != sb:
        diffs.append('strings: only base %s, only branch %s' % (list((sa - sb).elements())[:5],
                                                               list((sb - sa).elements())[:5]))
    (da, ua), (db, ub) = symsets(A), symsets(B)
    if da != db:
        diffs.append('defined symbols: only base %s, only branch %s' % (sorted(da - db)[:6], sorted(db - da)[:6]))
    if ua != ub:
        diffs.append('undefined symbols: only base %s, only branch %s' % (sorted(ua - ub), sorted(ub - ua)))
    for d in diffs:
        print('DIFF %s %s' % (label, d))
    if not diffs:
        print('ok symdiff %s: %d functions, %d objects, %d strings identical' % (
            label, len(fa), len(oa), sum(sa.values())))
    return not diffs


def main(argv):
    objdump = objdump_bin()
    if len(argv) >= 4 and argv[0] == '--dirs':
        ok = True
        for name in argv[3:]:
            ok &= compare(os.path.join(argv[1], name), os.path.join(argv[2], name), name, objdump)
        return 0 if ok else 1
    if len(argv) == 2:
        return 0 if compare(argv[0], argv[1], os.path.basename(argv[1]), objdump) else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
