#!/usr/bin/env python3
"""Judge one run of tests/boot/run-sysroot-device.sh: the serial log, the
times file, and the files tcc left on the image.

usage: sysroot_dev_verify.py <outdir> <sysroot> <host-cross-tcc> <repo> <full|readback>

Every check is one line, `ok`/`FAIL`, and the exit status is the FAIL count.
In `readback` mode the image under test carries ONE CORRUPTED BYTE in
/usr/include/stdio.h (the negative control), and the checks that compare the
device's readback against the host INVERT: the head line must DIFFER, the
crcwalk must disagree on exactly that one file and agree on the other 75.
A control whose readback still matched would mean the device was serving
something other than the disk (a cache of the host file cannot exist here,
but a harness that compared host-to-host by mistake would look identical).

THE SERIAL LOG IS SPLIT AT THE PROMPT. Each command's output is the text
between its echo (`/ $ cmd`) and the next `/ $ `; lines beginning with `[`
are the kernel's ([sched], [execve], [aex], [exec], [waitq] ...) and are
dropped, because they interleave with user output at the serial port.
"""
import os
import re
import subprocess
import sys
import zlib
import importlib.util

outdir, sysroot, hosttcc, repo, mode = sys.argv[1:6]
sysroot = os.path.abspath(sysroot)
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails += 1


# ---- the serial log, matched against the commands the driver SENT -----------
# The prompt (`/ $ `, no newline) and the echo of the typed command are not
# reliably on one line: the kernel prints between them ([mm] fork:, [sched]
# first-run, ...), so splitting at the prompt mis-assigns a command's output
# to a kernel line. Instead the SENT commands are read from times.txt (the
# driver logs every START), each one's echo line is found in order, and its
# output is everything up to the next line that begins with the prompt.
# User-program lines arrive with \r\n through the tty (208 of this log's
# lines) while kprintf lines are \n, so \r is stripped first.
log = open(os.path.join(outdir, "serial.log"), "rb").read().decode("utf-8", "replace").replace("\r", "")
lines = log.split("\n")
sent = []
for l in open(os.path.join(outdir, "times.txt")):
    if l.startswith("START "):
        sent.append(l.rstrip("\n").split(" ", 2)[2])
outputs = {}          # command -> list of its output lines (kernel lines dropped)
order = []
pos = 0
nprompt = sum(1 for l in lines if l.startswith("/ $ "))
for cmd in sent:
    i = pos
    while i < len(lines) and lines[i] != cmd and lines[i] != "/ $ " + cmd:
        i += 1
    if i >= len(lines):
        continue          # echo never appeared; out() returns None and the check fails
    j = i + 1
    body = []
    while j < len(lines) and not lines[j].startswith("/ $ "):
        if lines[j] and not lines[j].startswith("["):
            body.append(lines[j])
        j += 1
    outputs.setdefault(cmd, []).append(body)
    order.append(cmd)
    pos = j


def out(cmd, which=0):
    if cmd not in outputs:
        return None
    return outputs[cmd][which]


print("-- %s mode: %d commands sent, %d echoes found, %d prompt lines" % (mode, len(sent), len(order), nprompt))
check(out("echo SYS-START") == ["SYS-START"], "the shell echoed SYS-START (the drive reached the shell)")
check(out("echo SYS-END") == ["SYS-END"], "the shell echoed SYS-END (the drive completed)")

# ---- 1. the listing: every directory under /usr, counts against the host ----
expect = {}           # device dir -> sorted names (dirs with trailing /)
total_files = 0
for dirpath, dirnames, filenames in os.walk(sysroot):
    rel = "/" + os.path.relpath(dirpath, sysroot).replace(os.sep, "/")
    names = sorted([d + "/" for d in dirnames] + filenames)
    expect[rel] = names
    total_files += len(filenames)
listed_files = 0
listed_dirs = 0
all_dirs_ok = True
for d in sorted(expect):
    if d == "/" or d == "/usr":
        continue
    got = out("ls " + d)
    if got is None:
        check(False, "ls %s: no output captured" % d)
        all_dirs_ok = False
        continue
    got = sorted(got)
    ok = got == expect[d]
    nf = sum(1 for g in got if not g.endswith("/"))
    listed_files += nf
    listed_dirs += 1
    if not ok:
        all_dirs_ok = False
        missing = sorted(set(expect[d]) - set(got))
        extra = sorted(set(got) - set(expect[d]))
        print("       %s: missing %s extra %s" % (d, missing[:5], extra[:5]))
    check(ok, "ls %-22s %2d files, names identical to build/sysroot%s" % (d, nf, d))
check(listed_files == total_files,
      "DEVICE listing: %d files across %d directories == %d installed by mksysroot.py (host)"
      % (listed_files, listed_dirs, total_files))

# ---- 2. the first 64 bytes of stdio.h through the shell ----------------------
host_stdio = open(os.path.join(sysroot, "usr/include/stdio.h"), "rb").read()
host_head = host_stdio.decode().split("\n")[:4]
head_bytes = len("\n".join(host_head)) + 1
dev_head = out("cat /usr/include/stdio.h | head -n 4")
same_head = dev_head == host_head
if mode == "full":
    check(head_bytes >= 64, "4 lines of stdio.h cover %d bytes (>= 64)" % head_bytes)
    check(same_head, "cat stdio.h | head -n 4 on the device == the host file's first 4 lines (%d bytes)" % head_bytes)
else:
    check(not same_head and dev_head is not None,
          "CONTROL: cat stdio.h | head -n 4 on the device DIFFERS from the host (corrupted byte read back)")
    if dev_head is not None:
        for a, b in zip(host_head, dev_head):
            if a != b:
                print("       host: %r" % a)
                print("       dev:  %r" % b)
wc = out("wc /usr/include/stdio.h")
m = re.match(r"(\d+) (\d+) (\d+) ", (wc or [""])[0])
check(m is not None and int(m.group(3)) == len(host_stdio),
      "wc stdio.h on the device: %s bytes == %d on the host" % (m.group(3) if m else "?", len(host_stdio)))

# ---- 3. crcwalk: compiled on the device, every file under /usr ---------------
crc_expect = []
for top in ("usr/include", "usr/lib"):
    for dirpath, dirnames, filenames in os.walk(os.path.join(sysroot, top)):
        dirnames.sort()
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            data = open(p, "rb").read()
            rel = "/" + os.path.relpath(p, sysroot).replace(os.sep, "/")
            crc_expect.append("%08x %d %s" % (zlib.crc32(data) & 0xffffffff, len(data), rel))
# crcwalk walks the arguments in order and each directory sorted, which is
# os.walk with sorted dirnames EXCEPT that crcwalk interleaves files and
# subdirectories in one sorted pass. Re-sort both sides by path for the diff.
crc_expect_by_path = {l.split(" ", 2)[2]: l for l in crc_expect}
comp = out("/bin/tcc -o /crcwalk /src/crcwalk.c")
check(comp == [], "tcc compiled /src/crcwalk.c ON THE DEVICE with no diagnostics (got %r)" % (comp,))
walk = out("/crcwalk /usr/include /usr/lib") or []
crc_lines = [l for l in walk if re.match(r"^[0-9a-f]{8} \d+ /", l)]
total = [l for l in walk if l.startswith("CRCWALK ")]
errs = [l for l in walk if l.startswith("ERR")]
got_by_path = {l.split(" ", 2)[2]: l for l in crc_lines}
same = [p for p in crc_expect_by_path if got_by_path.get(p) == crc_expect_by_path[p]]
differ = [p for p in crc_expect_by_path if p in got_by_path and got_by_path[p] != crc_expect_by_path[p]]
missing = [p for p in crc_expect_by_path if p not in got_by_path]
extra = [p for p in got_by_path if p not in crc_expect_by_path]
check(errs == [], "crcwalk reported no errors (%d ERR lines)" % len(errs))
check(not missing and not extra,
      "crcwalk saw exactly the %d files the host packed (%d missing, %d extra)" % (len(crc_expect), len(missing), len(extra)))
want_total = "CRCWALK files=%d dirs=%d bytes=%d errors=0" % (
    len(crc_expect), sum(1 for _ in os.walk(os.path.join(sysroot, "usr/include"))) + sum(1 for _ in os.walk(os.path.join(sysroot, "usr/lib"))),
    sum(int(l.split(" ")[1]) for l in crc_expect))
if mode == "full":
    check(len(same) == len(crc_expect) and not differ,
          "DEVICE CRC-32 of every file == host zlib.crc32: %d of %d identical, %d differ" % (len(same), len(crc_expect), len(differ)))
    check(total == [want_total], "crcwalk total line: %s" % (total[0] if total else "missing"))
else:
    check(differ == ["/usr/include/stdio.h"] and len(same) == len(crc_expect) - 1,
          "CONTROL: crcwalk disagrees on EXACTLY /usr/include/stdio.h and agrees on the other %d (differ=%s)"
          % (len(crc_expect) - 1, differ))
    for p in differ:
        print("       host: %s" % crc_expect_by_path[p])
        print("       dev:  %s" % got_by_path[p])

# ---- 4. tcc -E on the device vs the host cross-tcc over build/sysroot --------
if mode == "full":
    spec = importlib.util.spec_from_file_location("lfs", os.path.join(repo, "tests/boot/lfs_extract.py"))
    lfs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lfs)
    img = bytearray(open(os.path.join(outdir, "run.img"), "rb").read())
    g = lfs.sb(img)

    def dev_file(path):
        try:
            ino = lfs.resolve(img, g, path)
        except SystemExit:
            return None
        if lfs.itype(img, g, ino) != lfs.T_FILE:
            return None
        blks, size = lfs.blocks_of(img, g, ino)
        return b"".join(bytes(img[b * lfs.BS:(b + 1) * lfs.BS]) for b in blks)[:size]

    INC = ["-nostdinc", "-I" + os.path.join(sysroot, "usr/include"), "-I" + os.path.join(sysroot, "usr/lib/tcc/include")]

    def host_E(src, extra):
        # cwd = the source's directory and a RELATIVE source name, so the main
        # file's marker is "hdrs.c" on the host where it is "/src/hdrs.c" on
        # the device: one rewrite, applied below and counted.
        r = subprocess.run([os.path.abspath(hosttcc)] + INC + extra + ["-E", os.path.basename(src)],
                           cwd=os.path.dirname(os.path.abspath(src)), capture_output=True)
        if r.returncode != 0:
            sys.exit("host tcc -E failed: %s" % r.stderr.decode())
        return r.stdout

    def normalise(host_out, devname, hostname):
        """Rewrite the host's # line markers to the device's paths; return
        (text, markers rewritten). Only the marker lines are touched."""
        n = 0
        lines = host_out.split(b"\n")
        for i, l in enumerate(lines):
            if l.startswith(b"# "):
                l2 = l.replace(sysroot.encode() + b"/usr/", b"/usr/").replace(b'"' + hostname.encode() + b'"', b'"' + devname.encode() + b'"')
                if l2 != l:
                    n += 1
                    lines[i] = l2
        return b"\n".join(lines), n

    for dev_out, src, devname, extra, label in (
            ("/out1.i", "tests/unit/sysroot_dev_hdrs.c", "/src/hdrs.c", [], "hdrs.c, default paths"),
            ("/out2.i", "tests/unit/sysroot_dev_hdrs.c", "/src/hdrs.c", [], "hdrs.c, -nostdinc -I/usr/include -I/usr/lib/tcc/include"),
            ("/out3.i", "tests/unit/sysroot_dev_hdrs_all.c", "/src/hdrs_all.c", [], "hdrs_all.c (14 headers), default paths"),
            ("/out1p.i", "tests/unit/sysroot_dev_hdrs.c", "/src/hdrs.c", ["-P"], "hdrs.c, -P (no line markers)"),
            ("/out3p.i", "tests/unit/sysroot_dev_hdrs_all.c", "/src/hdrs_all.c", ["-P"], "hdrs_all.c, -P (no line markers)")):
        dev = dev_file(dev_out)
        host = host_E(os.path.join(repo, src), extra)
        if dev is None:
            check(False, "tcc -E %s: %s is not on the image" % (label, dev_out))
            continue
        if "-P" in extra:
            check(dev == host, "tcc -E %s: device %s == host, BYTE FOR BYTE (%d B)" % (label, dev_out, len(dev)))
        else:
            normd, n = normalise(host, devname, os.path.basename(src))
            nmark = sum(1 for l in dev.split(b"\n") if l.startswith(b"# "))
            check(dev == normd,
                  "tcc -E %s: device %s == host after rewriting %d of %d # line markers to device paths (%d B)"
                  % (label, dev_out, n, nmark, len(dev)))
            if dev != normd:
                dl, hl = dev.split(b"\n"), normd.split(b"\n")
                for i, (a, b) in enumerate(zip(dl, hl)):
                    if a != b:
                        print("       first difference at line %d:" % (i + 1))
                        print("         dev:  %r" % a[:120])
                        print("         host: %r" % b[:120])
                        break
                else:
                    print("       lengths differ: dev %d lines, host %d lines" % (len(dl), len(hl)))
        # the device's own wc of the file must agree with what the image holds
        wcl = out("wc /out1.i /out1p.i /out2.i /out3.i /out3p.i") or []
        for l in wcl:
            mm = re.match(r"(\d+) (\d+) (\d+) (/out\w+\.i)$", l)
            if mm and mm.group(4) == dev_out:
                check(int(mm.group(3)) == len(dev), "  wc on the device says %s B for %s, the image holds %d" % (mm.group(3), dev_out, len(dev)))

    # ---- 5. the cost: timeit (TSC, on the device) and the host stopwatch ----
    print("-- timing (DEVICE, TSC via /timeit; then the host's prompt-to-prompt stopwatch)")
    for cmd in order:
        if cmd.startswith("/timeit"):
            for l in out(cmd) or []:
                if l.startswith("TIMEIT"):
                    print("     " + l)
    times = open(os.path.join(outdir, "times.txt")).read().split("\n")
    starts = []
    for l in times:
        if l.startswith("START "):
            _, t, cmd = l.split(" ", 2)
            starts.append([float(t), cmd, None])
        elif l.startswith("PROMPT ") and starts and starts[-1][2] is None:
            starts[-1][2] = float(l.split(" ")[1])
    for t0, cmd, t1 in starts:
        if t1 is not None and (cmd.startswith("/bin/tcc") or cmd.startswith("/timeit") or cmd.startswith("/crcwalk") or cmd.startswith("/openbench")):
            print("     host stopwatch %7.2f s  %s" % (t1 - t0, cmd))
    print("-- open() cost (DEVICE, /openbench)")
    ob = out("/openbench") or []
    for l in ob:
        if l.startswith("OPENBENCH"):
            print("     " + l)
    check(any(l.startswith("OPENBENCH-DONE ok") for l in ob), "openbench ran to completion with every open's outcome as expected")

print("sysroot_dev_verify: %d checks, %d failed (%s mode)" % (checks, fails, mode))
sys.exit(1 if fails else 0)
