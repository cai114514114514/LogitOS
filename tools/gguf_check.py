#!/usr/bin/env python3
"""gguf_check.py -- a SECOND implementation of the GGUF reader in tools/gguf.c,
written in numpy, whose only job is to disagree with it.

HOST ONLY. Nothing here is linked into anything that boots, and nothing in the
build graph depends on it: tests/nn.mk calls it and SKIPS LOUDLY when numpy is
absent, the same rule test-lm-os already applies to a missing model.

WHY A SECOND IMPLEMENTATION AT ALL, given that tools/lmshape.c's own header
argues at length for C over Python.  That argument is about the WRITER -- the
quantiser must be the real one, because a Python reimplementation of "round to
nearest multiple of scale" is a second rounding rule that agrees almost always.
It does not apply to a READER used as an ORACLE.  The failure this file exists
to catch is the one named in the task ground:

    A dequantiser that is wrong by a constant factor produces a model that
    runs and talks nonsense.

A constant factor is invisible to every structural check in the tree.  The file
opens, lm_expected_size agrees, the logits are finite, the tok/s is unchanged,
and the model is worthless.  Nothing but a second implementation of the same
arithmetic sees it, so this is the one place where a second implementation is
the point rather than the cost.

gguf-py and llama-cpp-python were the first choice and are NOT INSTALLED on
this machine (measured: `python -c "import gguf"` -> ModuleNotFoundError,
likewise llama_cpp, on both the Windows interpreter and WSL's).  numpy is on
both (2.4.4 Windows / 2.3.5 WSL) and torch only on Windows (2.13.0+cu132), so
the numpy path is what a Makefile target can rely on and the torch path is an
extra this file uses when it is there and names when it is not.

THREE MODES:
  --info FILE                 dump metadata + the tensor table.  This is the
                              ground truth the name map in tools/gguf.c was
                              written FROM, so it is worth being able to
                              reproduce it rather than trusting a comment.
  --dequant FILE --elems CSV  read the (tensor,row,col,value) quadruples
                              tools/lmshape.c --dump-elems printed, dequantise
                              the SAME elements here, report the worst
                              difference.  Exact equality is the bar: both
                              sides compute scale*q in f32 from identical
                              bytes, so anything nonzero is a real difference
                              in the arithmetic and not rounding.
  --matvec FILE --tensor NAME --xy FILE
                              read the x and y that tools/lmshape.c --matvec
                              wrote, recompute y from THIS reader's copy of the
                              weights in float64, and report max|dy|.  This is
                              the orientation proof: it is a matvec against an
                              independent load of the same tensor, which is the
                              cheapest thing that can tell [n,k] from [k,n].
"""
import sys, struct, os

try:
    import numpy as np
except ImportError:
    sys.stderr.write("gguf_check: numpy is not installed -- this is the ORACLE, "
                     "so there is nothing to check against. Not a pass.\n")
    sys.exit(77)          # 77 = the skip code tests/nn.mk looks for

# GGUF value types, from the spec.  Spelled out rather than computed from an
# enum because a wrong width here does not fail -- it silently shifts every
# following key-value pair, and the tensor table after them.
GT_U8, GT_I8, GT_U16, GT_I16, GT_U32, GT_I32, GT_F32, GT_BOOL, \
    GT_STR, GT_ARR, GT_U64, GT_I64, GT_F64 = range(13)

_FIX = {GT_U8: ("<B", 1), GT_I8: ("<b", 1), GT_U16: ("<H", 2), GT_I16: ("<h", 2),
        GT_U32: ("<I", 4), GT_I32: ("<i", 4), GT_F32: ("<f", 4),
        GT_BOOL: ("<B", 1), GT_U64: ("<Q", 8), GT_I64: ("<q", 8),
        GT_F64: ("<d", 8)}

# ggml tensor types.  Only the three this model uses are named; anything else
# is REFUSED by name rather than skipped, because a skipped tensor is a hole in
# the model that every structural check still passes over.
GGML_F32, GGML_F16, GGML_Q8_0 = 0, 1, 8
GGML_NAME = {0: "F32", 1: "F16", 8: "Q8_0"}


class Reader:
    def __init__(self, path):
        # memmap, not read(): the file is 610 MB and this reads a few thousand
        # scattered elements out of it. A slice of a memmap is a view, so
        # nothing below copies a tensor it does not need.
        self.buf = np.memmap(path, dtype=np.uint8, mode="r")
        self.p = 0
        self.path = path
        self._parse()

    # -- primitive readers.  self.buf is a memmap, so a slice is not a copy.
    def _bytes(self, n):
        b = self.buf[self.p:self.p + n].tobytes()
        self.p += n
        return b

    def _fix(self, t):
        f, n = _FIX[t]
        return struct.unpack(f, self._bytes(n))[0]

    def _str(self):
        n = struct.unpack("<Q", self._bytes(8))[0]
        return self._bytes(n).decode("utf-8", "replace")

    def _val(self, t):
        if t == GT_STR:
            return self._str()
        if t == GT_ARR:
            et = struct.unpack("<I", self._bytes(4))[0]
            n = struct.unpack("<Q", self._bytes(8))[0]
            if et == GT_STR:
                # Not materialised: tokenizer.ggml.tokens is 151,936 strings and
                # this file never reads one.  The COUNT is what the name map
                # needs (it is the vocabulary size), so the elements are walked
                # and dropped rather than kept.
                for _ in range(n):
                    ln = struct.unpack("<Q", self._bytes(8))[0]
                    self.p += ln
                return ("<%d strings>" % n, n)
            f, w = _FIX[et]
            if n * w > len(self.buf) - self.p:
                raise SystemExit("gguf_check: an array claims %d x %d bytes, "
                                 "past the end of the file" % (n, w))
            self.p += n * w
            return ("<%d x type %d>" % (n, et), n)
        return self._fix(t)

    def _parse(self):
        if self._bytes(4) != b"GGUF":
            raise SystemExit("gguf_check: %s is not a GGUF file" % self.path)
        self.version = struct.unpack("<I", self._bytes(4))[0]
        ntensor = struct.unpack("<Q", self._bytes(8))[0]
        nkv = struct.unpack("<Q", self._bytes(8))[0]
        self.kv = {}
        for _ in range(nkv):
            k = self._str()
            t = struct.unpack("<I", self._bytes(4))[0]
            self.kv[k] = self._val(t)
        self.tensors = {}
        self.order = []
        for _ in range(ntensor):
            name = self._str()
            nd = struct.unpack("<I", self._bytes(4))[0]
            dims = [struct.unpack("<Q", self._bytes(8))[0] for _ in range(nd)]
            tt = struct.unpack("<I", self._bytes(4))[0]
            off = struct.unpack("<Q", self._bytes(8))[0]
            self.tensors[name] = (dims, tt, off)
            self.order.append(name)
        align = self.kv.get("general.alignment", 32)
        if isinstance(align, tuple):
            align = 32
        self.align = align
        self.data0 = (self.p + align - 1) // align * align

    # ---- dequantisation.  THE WHOLE POINT OF THE FILE.
    #
    # Q8_0 is blocks of 32 along the CONTIGUOUS dimension (dims[0]): one f16
    # scale then 32 int8, 34 bytes a block, value = scale * q.  There is no
    # zero point and no minimum -- that is Q4_K's shape, not this one, and
    # adding one here would be a constant offset, i.e. exactly the silent
    # failure this file exists to catch.
    def rows(self, name, r0, nrow):
        """Rows [r0, r0+nrow) of `name`, as float32 [nrow, ne0]."""
        dims, tt, off = self.tensors[name]
        ne0 = dims[0]
        base = self.data0 + off
        if tt == GGML_F32:
            n = ne0 * nrow
            a = self.buf[base + r0 * ne0 * 4: base + (r0 * ne0 + n) * 4]
            return a.view(np.float32).reshape(nrow, ne0).astype(np.float32)
        if tt == GGML_F16:
            n = ne0 * nrow
            a = self.buf[base + r0 * ne0 * 2: base + (r0 * ne0 + n) * 2]
            return a.view(np.float16).reshape(nrow, ne0).astype(np.float32)
        if tt != GGML_Q8_0:
            raise SystemExit("gguf_check: %s is ggml type %d, which this reader "
                             "REFUSES rather than skipping" % (name, tt))
        if ne0 % 32:
            raise SystemExit("gguf_check: %s has ne0=%d, not a multiple of the "
                             "Q8_0 block (32)" % (name, ne0))
        nb = ne0 // 32                       # blocks per row
        b0 = base + r0 * nb * 34
        raw = self.buf[b0: b0 + nrow * nb * 34].reshape(nrow * nb, 34)
        # THE SCALE IS f16 AND THE PRODUCT IS TAKEN IN f32.  Promoting the
        # scale to f64 first would give a different last bit than tools/gguf.c,
        # which converts f16->f32 and multiplies in f32 -- and this check
        # demands EXACT equality, so the oracle has to make the same promotion
        # the implementation does, not a better one.
        sc = raw[:, 0:2].copy().view(np.float16).reshape(-1, 1).astype(np.float32)
        q = raw[:, 2:34].view(np.int8).astype(np.float32)
        return (q * sc).reshape(nrow, ne0)

    def full(self, name):
        dims, tt, off = self.tensors[name]
        nrow = 1
        for d in dims[1:]:
            nrow *= d
        return self.rows(name, 0, nrow)


def cmd_info(path):
    g = Reader(path)
    print("gguf   %s" % path)
    print("  version %d   tensors %d   kv %d   alignment %d   data at %d"
          % (g.version, len(g.tensors), len(g.kv), g.align, g.data0))
    for k in sorted(g.kv):
        v = g.kv[k]
        if isinstance(v, tuple):
            v = v[0]
        s = str(v)
        if len(s) > 60:
            s = s[:57] + "..."
        print("    %-44s %s" % (k, s))
    print("  tensors (name, dims as GGUF states them = [ne0 contiguous, ne1], type):")
    # Collapsed by name pattern: `blk.7.attn_q.weight` and `blk.8.attn_q.weight`
    # are one row with a count. 310 rows is a scroll, not a check.
    seen = {}
    for n in g.order:
        dims, tt, off = g.tensors[n]
        key = "blk.N." + ".".join(n.split(".")[2:]) if n.startswith("blk.") else n
        if key in seen:
            seen[key][0] += 1
            continue
        seen[key] = [1, dims, tt]
    for k in seen:
        c, dims, tt = seen[k]
        print("    %-28s %-16s %-6s  x%d" % (k, dims, GGML_NAME.get(tt, tt), c))
    return 0


def cmd_dequant(path, elems):
    """elems: a file of `name,row,col,value` lines that the C side printed."""
    g = Reader(path)
    worst, worst_at, n = 0.0, "", 0
    cache = {}
    bad = 0
    for line in open(elems):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, r, c, v = line.split(",")
        r, c, v = int(r), int(c), float(v)
        key = (name, r)
        if key not in cache:
            if len(cache) > 4096:
                cache.clear()
            cache[key] = g.rows(name, r, 1)[0]
        got = float(cache[key][c])
        d = abs(got - v)
        if d > worst:
            worst, worst_at = d, "%s[%d,%d] C %.9g numpy %.9g" % (name, r, c, v, got)
        if d != 0.0:
            bad += 1
        n += 1
    print("  dequant cross-check  %d elements, %d differ, worst |C - numpy| %.9g" % (n, bad, worst))
    if worst_at:
        print("    worst at %s" % worst_at)
    if bad:
        print("  FAIL: the two dequantisers disagree. The bar is EXACT: both compute")
        print("    scale*q in f32 from the same bytes, so a nonzero difference is a")
        print("    difference in the arithmetic, not in rounding.")
        return 1
    print("  dequant OK (exact)")
    return 0


def cmd_matvec(path, tensor, xy):
    """xy: [i32 n][i32 k][k f32 x][n f32 y] as tools/lmshape.c --matvec wrote it."""
    g = Reader(path)
    raw = open(xy, "rb").read()
    n, k = struct.unpack("<ii", raw[:8])
    x = np.frombuffer(raw[8:8 + 4 * k], dtype=np.float32)
    y = np.frombuffer(raw[8 + 4 * k: 8 + 4 * k + 4 * n], dtype=np.float32)
    dims, tt, off = g.tensors[tensor]
    ne0, ne1 = dims[0], dims[1]
    print("  matvec  %s  GGUF dims [ne0=%d, ne1=%d] type %s" % (tensor, ne0, ne1, GGML_NAME.get(tt, tt)))
    print("          C read it as [n=%d, k=%d]" % (n, k))
    if (n, k) != (ne1, ne0):
        print("  NOTE: C's (n,k) is NOT (ne1,ne0) -- it transposed. That is either the")
        print("        negative control or the bug the control exists to catch.")
    W = g.full(tensor)                    # [ne1, ne0], k = ne0 contiguous
    if (n, k) == (ne1, ne0):
        ref = W.astype(np.float64) @ x.astype(np.float64)
    elif (n, k) == (ne0, ne1):
        ref = W.T.astype(np.float64) @ x.astype(np.float64)
    else:
        print("  FAIL: (n,k)=(%d,%d) matches neither orientation of [%d,%d]" % (n, k, ne1, ne0))
        return 1
    d = np.abs(ref - y.astype(np.float64))
    scale = float(np.abs(ref).max()) or 1.0
    print("          max|dy| %.6g   rel %.3g   (|y| up to %.6g)"
          % (float(d.max()), float(d.max()) / scale, scale))
    # THE BOUND IS DERIVED, NOT FITTED.  C accumulates a k-term dot product in
    # f32; numpy does it in f64.  The f32 accumulation's error is bounded in
    # practice by ~sqrt(k) * eps32 * sum|w_i x_i|; sum|w x| <= k * max|w| *
    # max|x|, and the row norms here are O(1e-2), so a bound of 1e-3 relative
    # is loose by orders of magnitude for agreement and tight by orders of
    # magnitude against a transposition, which moves every element by O(1).
    ok = float(d.max()) / scale < 1e-3
    try:
        import torch
        Wt = torch.from_numpy(np.ascontiguousarray(W if (n, k) == (ne1, ne0) else W.T))
        yt = torch.mv(Wt.double(), torch.from_numpy(x.astype(np.float64)))
        dt = float(torch.max(torch.abs(yt - torch.from_numpy(y.astype(np.float64)))))
        print("          torch %s: max|dy| %.6g" % (torch.__version__, dt))
    except ImportError:
        print("          torch: NOT INSTALLED on this interpreter -- numpy float64 is")
        print("                 the reference above. (torch is on the Windows python,")
        print("                 not WSL's; run this file there to add its line.)")
    if not ok:
        print("  FAIL: the matvec disagrees by %.3g relative, bound 1e-3." % (float(d.max()) / scale))
        return 1
    print("  matvec OK")
    return 0


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    if argv[1] == "--info":
        return cmd_info(argv[2])
    if argv[1] == "--dequant":
        return cmd_dequant(argv[2], argv[4])
    if argv[1] == "--matvec":
        return cmd_matvec(argv[2], argv[4], argv[6])
    sys.stderr.write("gguf_check: unknown mode %s\n" % argv[1])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
