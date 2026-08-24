#!/usr/bin/env bash
# THE IDENTITY GATE: the compiler on the device is the compiler on the host,
# proven on BYTES, not on version strings.
#
# The same sources are compiled to OBJECTS twice -- by /bin/tcc (tcc.aex,
# running under mini-libc on LogitOS inside QEMU) and by build/tcc/host/tcc
# (the same vendored+patched source built natively with clang against glibc)
# -- and the .o files must be BYTE-IDENTICAL. tcc is deterministic and -c
# embeds no date, no path (only the input's basename, which both sides see
# as the same string because both compile from a directory containing it)
# and no address, so any difference is a real divergence between the two
# builds: a libc function they disagree on, a float parse, a codegen bug.
#
# Two sources, deliberately different in kind:
#   hello_id.c   NO includes, its own write() prototype: isolates the
#                COMPILER -- preprocessor trivia, codegen, ELF writer.
#   tccpp.c      3,903 lines of tcc's own preprocessor, includes tcc.h which
#                pulls 11 mini-libc headers + 3 of tcc's own: adds the whole
#                header set and a large token stream to the claim.
#
# The .o files come back off the disk image via the embedded LogitFS reader
# below (NOT tools/mkfs.py, which only writes and is owned by another line;
# NOT c/fs/lfsro.c, which needs the kernel around it) -- so the run must NOT
# use -snapshot: the whole point is that the guest's writes reach the image.
#
# NEGATIVE CONTROL (the gate's own): hello_id compiled on the host with one
# extra -D (-DTCC_ID_NEGCTL flips a constant) must DIFFER from the device .o.
# A gate that reports identical for different inputs is comparing the wrong
# files. TCC_ID_SABOTAGE=1 simulates exactly that wrong-file bug (points the
# device path at the negctl file itself) and the gate must then FAIL -- run
# once, watched, before this harness was trusted.
#
# TIMING: the harness stamps wall-clock marks around every command it sends;
# compile time = prompt-return minus command-sent. Reported for the TCG pass
# and, when /dev/kvm exists, for a second KVM pass -- the identity cmp runs
# against BOTH passes' extracted objects.
#
# There is NO kernel open()/VFS-resolution counter to read (checked: c/fs/
# vfs.c counts nothing, fsbench.c measures latency not call counts), so the
# open census is taken on the HOST with an LD_PRELOAD shim around the same
# tcc binary given the device's exact include configuration: same tcc code +
# same search paths = same probe SEQUENCE; only the cost per probe differs,
# which is what the device wall time is for.
set -u
cd "$(dirname "$0")/../.."      # repo root, like every tests/boot harness
. tests/boot/bootwait.sh

ISO="${1:?usage: run-tcc-identity.sh <iso>}"
QEMU="${QEMU:-qemu-system-x86_64}"
OUT=build/tcc/id
HOSTTCC=build/tcc/host/tcc
SYSROOT_LIVE=build/sysroot
SNAP=$OUT/sysroot
fail=0

say()  { echo "$@"; }
need_file() { [ -f "$1" ] || { echo "FAIL: missing $1 -- $2"; exit 1; }; }

need_file "$HOSTTCC" "build it: make -f Makefile -f tests/tcc.mk build/tcc/host/tcc"
need_file build/tcc/tcc.aex "build it: make -f Makefile -f tests/tcc.mk build/tcc/tcc.aex"
[ -d "$SYSROOT_LIVE/usr/include" ] || {
    echo "FAIL: $SYSROOT_LIVE does not exist -- that is the sysroot line's output"
    echo "      (workflow 2): make -f Makefile -f tests/sysroot.mk sysroot"
    exit 1; }

mkdir -p "$OUT/src" "$OUT/hostout" "$OUT/dev"
rm -f "$OUT"/src/*.o "$OUT"/src/*.d "$OUT"/hostout/*.o

# --- 1. the sysroot SNAPSHOT -------------------------------------------------
# build/sysroot is workflow 2's LIVE output and can be regenerated under this
# gate mid-run; the identity claim needs the image and the host compile to
# read the SAME header bytes, so both read a copy taken here, atomically
# (same argument as tests/sysroot.mk's own snapshot of third_party/tcc).
rm -rf "$SNAP"
cp -r "$SYSROOT_LIVE" "$SNAP"

# --- 2. /src: the sources both compilers see ---------------------------------
# The 12-file closure was measured with `host/tcc -MD` (tccpp.c pulls tcc.h,
# which pulls the target-defs sources under TARGET_DEFS_ONLY and the token
# tables); a file missing here fails the compile by name on both sides.
for f in tccpp.c tcc.h libtcc.h elf.h stab.h stab.def tcctok.h i386-tok.h \
         x86_64-asm.h x86_64-gen.c x86_64-link.c; do
    cp "third_party/tcc/$f" "$OUT/src/$f"
done

cat > "$OUT/src/config.h" <<'EOF'
/* identity-gate config.h: the SAME file is read by the device tcc (at
 * /src/config.h) and the host cross-tcc, so the defines ride the file, not
 * two command lines that could drift. ONE_SOURCE=0: tccpp.c as its own TU
 * (tcc.h defaults it to 1, which makes every ST_FUNC static and the TU
 * unlinkable alone). CONFIG_TCC_STATIC: without it tcc.h reaches for
 * <dlfcn.h>, which mini-libc does not have. TCC_LOGIT: the build the
 * device tcc was compiled with, mirrored so the preprocessed source is
 * identical on both sides. */
#define ONE_SOURCE 0
#define CONFIG_TCC_STATIC 1
#define TCC_LOGIT 1
EOF

cat > "$OUT/src/hello_id.c" <<'EOF'
/* The identity-gate source: compiled to an OBJECT by the device tcc and by
 * the host-built tcc from the same vendored source, and the two .o files
 * must be byte-identical. NO includes on purpose: an #include would make the
 * gate measure the header set as well as the compiler; write()'s prototype
 * is spelled here instead. The TCC_ID_NEGCTL branch exists for the gate's
 * negative control: one -D on one side must change the bytes, or the gate
 * is comparing the wrong files. */
long write(long fd, const void *buf, unsigned long n);

#ifdef TCC_ID_NEGCTL
enum { ID_SALT = 0x5eed };
#else
enum { ID_SALT = 0x1d };
#endif

static unsigned int mix(unsigned int h, unsigned int c)
{
    h ^= c;
    h *= 16777619u;
    return h ^ (h >> 13);
}

static const char msg[] = "TCC-ID ok\n";

int main(void)
{
    unsigned int h = 2166136261u ^ ID_SALT;
    int i;
    for (i = 0; msg[i]; i++)
        h = mix(h, (unsigned char)msg[i]);
    write(1, msg, sizeof msg - 1);
    return (int)(h & 63);
}
EOF

# --- 3. the LogitFS reader (how the .o comes back off the image) -------------
cat > "$OUT/lfsread.py" <<'EOF'
#!/usr/bin/env python3
"""Read ONE file out of a LogitFS v4 image, on the host, after a run.

Why it exists: the identity gate writes .o files ON the device and must get
their exact bytes back. The alternatives were rejected for cause: tools/mkfs.py
only WRITES images (owned by another line, and growing a reader there is scope
creep into a contended file); c/fs/lfsro.c is the kernel's reader and needs the
kernel around it. This is ~60 lines against the format's single definition
site (c/fs/logitfs_fmt.h; layout mirrored from tools/mkfs.py serialize()), and
it REFUSES anything it does not recognise rather than guessing -- a truncated
read here would quietly turn "the compilers disagree" into "the gate passed".

Free dirent slots are name[0]==0, the same test c/fs/logitfs.c:dir_lookup uses.

usage: lfsread.py <image> </abs/path>   (file bytes on stdout, errors on stderr)
"""
import struct, sys

BS, INODE_SIZE, DIRENT, PPB, NDIRECT = 4096, 128, 64, 1024, 12
MAGIC = 0x4C4F4749

img = open(sys.argv[1], "rb").read()
sb = struct.unpack_from("<13I", img, 0)
(magic, ver, bs, total, icount, bstart, bblocks,
 istart, iblocks, dstart, root, lstart, lblocks) = sb
if magic != MAGIC or bs != BS:
    sys.exit("lfsread: not a LogitFS image (magic %#x bs %d)" % (magic, bs))
if len(img) != total * BS:
    sys.exit("lfsread: image is %d bytes, superblock says %d" % (len(img), total * BS))

def inode(i):
    if i >= icount:
        sys.exit("lfsread: inode %d out of range" % i)
    off = istart * BS + i * INODE_SIZE
    t, _, size = struct.unpack_from("<HHI", img, off)
    direct = struct.unpack_from("<%dI" % NDIRECT, img, off + 8)
    ind, dind = struct.unpack_from("<II", img, off + 8 + NDIRECT * 4)
    return t, size, direct, ind, dind

def content(i):
    t, size, direct, ind, dind = inode(i)
    nblk = (size + BS - 1) // BS
    blks = list(direct[:min(nblk, NDIRECT)])
    if nblk > NDIRECT:
        blks += list(struct.unpack_from("<%dI" % min(nblk - NDIRECT, PPB), img, ind * BS))
    if nblk > NDIRECT + PPB:
        dp = struct.unpack_from("<%dI" % PPB, img, dind * BS)
        left = nblk - NDIRECT - PPB
        for k in range((left + PPB - 1) // PPB):
            n = min(left - k * PPB, PPB)
            blks += list(struct.unpack_from("<%dI" % n, img, dp[k] * BS))
    out = bytearray()
    for b in blks:
        if b == 0 or b >= total:
            sys.exit("lfsread: inode %d has block pointer %d out of range" % (i, b))
        out += img[b * BS:(b + 1) * BS]
    return t, bytes(out[:size])

cur = root
walked = ""
for comp in [c for c in sys.argv[2].split("/") if c]:
    t, data = content(cur)
    if t != 2:
        sys.exit("lfsread: %s is not a directory" % (walked or "/"))
    hit = 0
    for off in range(0, len(data) - DIRENT + 1, DIRENT):
        ino = struct.unpack_from("<I", data, off)[0]
        name = data[off + 4:off + DIRENT].split(b"\0")[0].decode("utf-8", "replace")
        if name and name == comp:
            hit = ino
            break
    if not hit:
        sys.exit("lfsread: no entry '%s' in %s" % (comp, walked or "/"))
    cur, walked = hit, walked + "/" + comp
t, data = content(cur)
if t != 1:
    sys.exit("lfsread: %s is not a regular file" % walked)
sys.stdout.buffer.write(data)
EOF

# --- 4. host compiles --------------------------------------------------------
# From INSIDE $OUT/src, with the input as a bare basename, because that
# basename is the one string tcc writes into the .o (the STT_FILE symbol);
# the device side cd's to /src for the same reason. The -I order is the
# device's baked search order verbatim (tcc.h TCC_LOGIT block:
# CONFIG_TCC_SYSINCLUDEPATHS "/usr/include:{B}/include", {B}=/usr/lib/tcc);
# -nostdinc because without it the HOST tcc would fall through to the WSL's
# real /usr/include and the two sides would read different headers.
HT=$(cd "$(dirname "$HOSTTCC")" && pwd)/$(basename "$HOSTTCC")
SNAPABS=$(cd "$SNAP" && pwd)
(cd "$OUT/src" && "$HT" -nostdinc -c hello_id.c -o ../hostout/hello_id.o) || \
    { echo "FAIL: host tcc could not compile hello_id.c"; exit 1; }
(cd "$OUT/src" && "$HT" -nostdinc -DTCC_ID_NEGCTL -c hello_id.c -o ../hostout/hello_id.negctl.o) || \
    { echo "FAIL: host tcc could not compile hello_id.c (negctl)"; exit 1; }
(cd "$OUT/src" && "$HT" -nostdinc -I"$SNAPABS/usr/include" -I"$SNAPABS/usr/lib/tcc/include" \
    -c tccpp.c -o ../hostout/tccpp.o) || \
    { echo "FAIL: host tcc could not compile tccpp.c against the sysroot snapshot"; exit 1; }
say "HOST: hello_id.o $(stat -c%s "$OUT/hostout/hello_id.o") B," \
    "negctl $(stat -c%s "$OUT/hostout/hello_id.negctl.o") B," \
    "tccpp.o $(stat -c%s "$OUT/hostout/tccpp.o") B"

# --- 5. the open census (HOST, LD_PRELOAD) -----------------------------------
# Counts open()/open64() from tcc under the device's include configuration.
# fopen() bypasses the shim (glibc-internal calls do not go through the
# PLT); tcc's include probes and input reads all use open() via tcc_open,
# which is the population being counted.
if command -v cc >/dev/null 2>&1; then
    cat > "$OUT/opencount.c" <<'EOF'
/* LD_PRELOAD open() census. Counts land in the file named by $OPENLOG:
 * one line per call, "ok/fail FN PATH", so the harness can total and
 * bucket them. dlsym(RTLD_NEXT) is the standard interposition; the
 * rejected alternative was strace, which this WSL does not have. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dlfcn.h>

static FILE *out;
static FILE *census(void)
{
    if (!out) {
        const char *p = getenv("OPENLOG");
        out = fopen(p ? p : "/dev/null", "a");
    }
    return out;
}
static int note(const char *fn, const char *path, int r)
{
    FILE *f = census();
    if (f) { fprintf(f, "%s %s %s\n", r < 0 ? "fail" : "ok", fn, path); fflush(f); }
    return r;
}
#define WRAP(name)                                                        \
    int name(const char *path, int flags, ...)                            \
    {                                                                     \
        static int (*real)(const char *, int, ...);                       \
        va_list ap; int m = 0;                                            \
        if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name); \
        if (flags & O_CREAT) { va_start(ap, flags); m = va_arg(ap, int); va_end(ap); } \
        return note(#name, path, real(path, flags, m));                   \
    }
WRAP(open)
WRAP(open64)
EOF
    if cc -shared -fPIC -o "$OUT/opencount.so" "$OUT/opencount.c" -ldl 2>/dev/null; then
        rm -f "$OUT/openlog"
        (cd "$OUT/src" && OPENLOG=../openlog LD_PRELOAD="$(cd "$OUT" && pwd)/opencount.so" \
            "$HT" -nostdinc -I"$SNAPABS/usr/include" -I"$SNAPABS/usr/lib/tcc/include" \
            -c tccpp.c -o ../hostout/tccpp.census.o) >/dev/null 2>&1
        if [ -s "$OUT/openlog" ]; then
            tot=$(wc -l < "$OUT/openlog")
            bad=$(grep -c '^fail' "$OUT/openlog" || true)
            say "HOST(census): tcc -c tccpp.c performs $tot open() calls, $bad failed probes"
            say "HOST(census): failed probes by directory:"
            grep '^fail' "$OUT/openlog" | awk '{p=$3; sub("/[^/]*$", "", p); print p}' \
                | sort | uniq -c | sort -rn | sed 's/^/    /'
            cmp -s "$OUT/hostout/tccpp.census.o" "$OUT/hostout/tccpp.o" || \
                say "  note: census-run tccpp.o differs from the plain run -- shim interfered"
        else
            say "HOST(census): shim produced no log -- census skipped"
        fi
    else
        say "HOST(census): cc could not build the shim -- census skipped"
    fi
else
    say "HOST(census): no cc on this host -- census skipped"
fi

# --- 6. the disk image -------------------------------------------------------
# The Makefile's own file list via make -n (continuations joined by
# mk-tcc-disk.py), plus: tcc.aex at /bin/tcc, the staged /src, and the
# sysroot snapshot's usr/ tree at /usr. mkfs exits loudly on a duplicate
# path, so the day the sysroot line adds /usr/include to $(DISK) this gate
# refuses instead of silently packing two header sets.
make -f Makefile -n -W tools/mkfs.py build/disk.img > "$OUT/disk.mk-n" || \
    { echo "FAIL: make -n for the disk file list failed"; exit 1; }
python3 tests/boot/mk-tcc-disk.py . "$OUT/disk.mk-n" "$OUT/disk.img" \
    build/tcc/tcc.aex:/bin/tcc \
    "$OUT/src":/src \
    "$SNAP/usr":/usr | tee "$OUT/mkdisk.out" || { echo "FAIL: image build failed"; exit 1; }

# --- 7. boot passes ----------------------------------------------------------
# ' $ ' (not '/ $ ') because the prompt is "<cwd> $ " and the drive cd's to
# /src; measured on run-tcg.log that no kernel line contains the pattern.
LASTP=0
LOG=; MARKS=
prompts() { grep -aoF -- ' $ ' "$LOG" 2>/dev/null | wc -l; }
count_of() { grep -ac -- "$1" "$LOG" 2>/dev/null || echo 0; }
send() {  # send <cmd> [budget-s: how long the PREVIOUS command may run]
    local i n=$(( ${2:-60} * 10 ))
    for ((i = 0; i < n; i++)); do [ "$(prompts)" -gt "$LASTP" ] && break; sleep 0.1; done
    LASTP=$(prompts)
    echo "PROMPT $(date +%s.%N) before |$1|" >> "$MARKS"
    sleep 1
    printf '%s\n' "$1"
    echo "SENT $(date +%s.%N) |$1|" >> "$MARKS"
}
drive() {
    logit_wait_for_shell "$LOG" 240
    send 'echo ID-START' 60
    send 'mkdir /out' 60
    send 'cd /src' 60
    send '/bin/tcc -c hello_id.c -o /out/hello_id.o' 60
    send '/bin/tcc -c tccpp.c -o /out/tccpp.o' 300
    send 'ls /out' 600
    send 'echo TCC-ID-END' 60
    send 'exit' 30
    sleep 2
}
NET="-netdev user,id=n0 -device e1000,netdev=n0"

boot_pass() {  # boot_pass <tcg|kvm>  -> 0 ok, 1 failed
    local accel=$1
    local img="$OUT/disk.$accel.img"
    LOG="$OUT/run-$accel.log"; MARKS="$OUT/marks-$accel"
    rm -f "$LOG" "$MARKS"; touch "$LOG" "$MARKS"
    cp "$OUT/disk.img" "$img"          # each pass mutates its own copy
    local aflags
    if [ "$accel" = kvm ]; then aflags="-accel kvm"; else aflags="-accel tcg,thread=multi"; fi
    LASTP=0
    # NO -snapshot: the .o files written by the guest MUST reach this image.
    drive | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$img",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d \
        -m 512M -smp 4 $aflags -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$LOG" 2>"$OUT/qemu-$accel.err" &
    local qpid=$!
    local i
    for i in $(seq 1 "${WAIT:-12000}"); do
        [ "$(count_of 'TCC-ID-END')" -ge 2 ] && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    if [ "$(count_of 'TCC-ID-END')" -lt 2 ]; then
        echo "  $accel pass: the drive script never completed"
        return 1
    fi
    return 0
}

report_times() {  # report_times <accel> -- compile wall time from the marks
    python3 - "$OUT/marks-$1" "$1" <<'EOF'
import sys, re
res = []                      # [cmd, t_sent, t_done]
for ln in open(sys.argv[1]):
    m = re.match(r"(PROMPT|SENT) ([0-9.]+) (?:before )?\|(.*)\|", ln)
    if not m:
        continue
    kind, t, cmd = m.group(1), float(m.group(2)), m.group(3)
    if kind == "SENT":
        res.append([cmd, t, None])
    elif res and res[-1][2] is None:
        res[-1][2] = t        # a PROMPT closes the most recently sent command
for cmd, t0, done in res:
    if done is not None and "tcc -c" in cmd:
        print("DEVICE(%s): %-44s %6.1f s wall" % (sys.argv[2], cmd, done - t0))
EOF
}

extract_and_compare() {  # extract_and_compare <accel>
    local accel=$1 img="$OUT/disk.$accel.img" broken=0
    mkdir -p "$OUT/dev/$accel"
    local f
    for f in hello_id.o tccpp.o; do
        if ! python3 "$OUT/lfsread.py" "$img" "/out/$f" > "$OUT/dev/$accel/$f"; then
            echo "  FAIL($accel): could not read /out/$f back off the image"
            grep -a 'tcc\|error' "$OUT/run-$accel.log" | tail -5 | sed 's/^/    /'
            fail=1; broken=1; continue
        fi
        say "DEVICE($accel): /out/$f extracted, $(stat -c%s "$OUT/dev/$accel/$f") B"
    done
    [ "$broken" -ne 0 ] && return 1
    local dev_hello="$OUT/dev/$accel/hello_id.o"
    # TCC_ID_SABOTAGE simulates the wrong-file bug the negative control
    # exists to catch: the "device" object is silently the host negctl file.
    [ "${TCC_ID_SABOTAGE:-0}" = 1 ] && dev_hello="$OUT/hostout/hello_id.negctl.o"
    if cmp -s "$dev_hello" "$OUT/hostout/hello_id.o"; then
        say "  ok($accel): hello_id.o -- device and host bytes IDENTICAL"
    else
        echo "  FAIL($accel): hello_id.o differs between device and host:"
        cmp "$dev_hello" "$OUT/hostout/hello_id.o" 2>&1 | sed 's/^/    /'
        fail=1
    fi
    if cmp -s "$OUT/dev/$accel/tccpp.o" "$OUT/hostout/tccpp.o"; then
        say "  ok($accel): tccpp.o   -- device and host bytes IDENTICAL"
    else
        echo "  FAIL($accel): tccpp.o differs between device and host:"
        cmp "$OUT/dev/$accel/tccpp.o" "$OUT/hostout/tccpp.o" 2>&1 | sed 's/^/    /'
        fail=1
    fi
    # the NEGATIVE CONTROL: one -D on one side must change the bytes.
    if cmp -s "$dev_hello" "$OUT/hostout/hello_id.negctl.o"; then
        echo "  FAIL($accel): NEGATIVE CONTROL -- the -DTCC_ID_NEGCTL object is"
        echo "        IDENTICAL to the device object: the gate is comparing the"
        echo "        wrong files (or the define never reached the compiler)"
        fail=1
    else
        say "  ok($accel): control -- one -D on the host side changes the bytes"
    fi
    return 0
}

echo "--- TCG pass ---"
if boot_pass tcg; then
    extract_and_compare tcg
    report_times tcg
else
    echo "FAIL: TCG pass did not complete"; tail -30 "$OUT/run-tcg.log"; fail=1
fi

if [ "${TCC_ID_KVM:-1}" = 1 ] && [ -e /dev/kvm ]; then
    echo "--- KVM pass ---"
    if boot_pass kvm; then
        extract_and_compare kvm
        report_times kvm
    else
        echo "  note: KVM pass did not complete (not fatal -- TCG is the gate);"
        echo "        log tail:"; tail -10 "$OUT/run-kvm.log" | sed 's/^/    /'
    fi
else
    echo "KVM pass skipped ($([ -e /dev/kvm ] && echo TCC_ID_KVM=0 || echo 'no /dev/kvm'))"
fi

grep '^mk-tcc-disk' "$OUT/mkdisk.out"
if [ "$fail" -ne 0 ]; then
    echo "FAIL: tcc identity gate"
    exit 1
fi
echo "PASS: the device tcc and the host tcc produce byte-identical objects (hello_id.c, tccpp.c), and the one-define control differs"
