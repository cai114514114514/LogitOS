# durcheck -- write or verify a file whose every byte is predictable, so a
# durability test can tell "still there" from "still the right bytes".
#
#   as /usr/as/examples/durcheck.as write  /dur/x.bin mid
#   as /usr/as/examples/durcheck.as verify /dur/x.bin mid
#   as /usr/as/examples/durcheck.as churn  /dur
#
# `wc` can only say a file is the right length. A filesystem with no journal and
# no write ordering fails by handing the same block to two files, and the symptom
# of that is a file of exactly the right size holding somebody else's bytes --
# which is why this compares content rather than counting it.
#
# Sizes are named rather than parsed: there is no int() builtin, and the three
# that matter are the three inode shapes.
#   tiny  one block
#   small several direct blocks
#   mid   past direct[12] into the single-indirect block
#   huge  past single-indirect into the double-indirect tree

# The builtins, not lib/sys.as: this script has to run on the host too, where
# there is no /usr/as/lib. Deleting goes straight through the syscall for the
# same reason (on the host it stubs to -1, which churn does not care about).

SIZE_TINY = 100
SIZE_SMALL = 30000
SIZE_MID = 120000
SIZE_BIG = 400000       # crashwrite's victim: big enough that the write is still
                        # in flight when the harness pulls the plug
SIZE_HUGE = 4400000     # 1075 blocks > 1036 (direct+single): exercises the
                        # double-indirect tree

def size_of(name):
    if name == "tiny":
        return SIZE_TINY
    if name == "small":
        return SIZE_SMALL
    if name == "big":
        return SIZE_BIG          # crashwrite's victim size
    if name == "huge":
        return SIZE_HUGE
    return SIZE_MID

# Byte i of the stream is a function of i alone, so verify never has to have
# seen the writer. Cheap, and it changes in every byte position.
#
# chr() takes the value mod 256, so a chunk's content only depends on
# (index*251) % 256 -- there are only 256 distinct chunks, and 251 is coprime
# with 256 so all of them occur. Cache them: at huge size the interpreter
# would otherwise rebuild the same 256 strings ~17k times (many minutes of
# VM churn) for content that is bit-identical to the cached version.
CHUNK_CACHE = []
def chunk(index):
    r = index % 256
    while len(CHUNK_CACHE) <= r:
        c = len(CHUNK_CACHE)
        out = []
        base = c * 251
        for i in range(256):
            out.append(chr((base + i * 37 + (i * i) % 97) % 256))
        CHUNK_CACHE.append("".join(out))
    return CHUNK_CACHE[r]

def build(size):
    parts = []
    n = 0
    i = 0
    while n + 256 <= size:
        parts.append(chunk(i))
        n = n + 256
        i = i + 1
    if n < size:                    # the last, partial chunk (no slicing in the language)
        c = chunk(i)
        tail = []
        for k in range(size - n):
            tail.append(c[k])
        parts.append("".join(tail))
    return "".join(parts)

def cmd_write(path, size):
    wrote = file_write(path, build(size))
    print("durcheck write", path, size, "->", wrote)

def cmd_verify(path, size):
    data = file_read(path)
    if data == nil:
        print("DURCHECK-FAIL", path, "unreadable")
        return nil
    if len(data) != size:
        print("DURCHECK-FAIL", path, "length", len(data), "expected", size)
        return nil
    want = build(size)
    if data != want:
        # Name the first divergence. Where it lands says a lot about whether this
        # was a lost write or a block handed to two files.
        for i in range(size):
            if data[i] != want[i]:
                print("DURCHECK-FAIL", path, "first bad byte", i,
                      "got", ord(data[i]), "want", ord(want[i]))
                return nil
    print("DURCHECK-OK", path, size)

# Allocate and free repeatedly. This is what should shake out a free-block bitmap
# that drifts out of step with the inode table: the damage does not show on the
# files being churned, it shows on a bystander written long before.
def cmd_churn(dir):
    for r in range(12):
        p = dir + "/churn.tmp"
        file_write(p, build(3000 + r * 900))
        syscall(SYS_DELETE_FILE, addr(p))
    print("DURCHECK-CHURN-DONE")

# Build the content FIRST, then announce, then write in a long loop. Everything
# after the announcement is disk I/O, so a harness that kills the machine once it
# sees the marker lands mid-write rather than mid-string-building.
#
# A LOOP, not one big write: a single write can finish inside the harness's
# polling interval, and then the crash test tested nothing while still looking
# green. Hundreds of write/delete rounds make the window seconds wide, and they
# keep the free-block bitmap and the inode table in motion the whole time --
# which is the state a crash has to be survivable from.
#
# The counter it prints is how boot 3 proves the kill actually landed inside the
# loop: a run where CRASH-WRITE-DONE appears tested a clean shutdown, not a crash.
def cmd_crashwrite(path):
    # SIZE_BIG, not SIZE_MID: run-fscrash-test.sh verifies this file with the
    # "big" spec, so writing a "mid" one made every surviving victim look TORN
    # ("length 120000 expected 400000") on a filesystem that had done nothing
    # wrong. The test therefore only passed when the SIGKILL happened to land
    # while the victim was absent -- which also means its whole-and-intact
    # branch had never once been exercised. Pre-existing since the v4 journal
    # landed (95734ad); found by re-running the harness for the crash-
    # consistency work.
    data = build(SIZE_BIG)
    other = path + ".b"
    print("CRASH-WRITE-ARMED")
    for i in range(300):
        file_write(path, data)
        file_write(other, data)
        syscall(SYS_DELETE_FILE, addr(path))
        if i % 10 == 0:
            print("CRASH-WRITE-ROUND", i)
    print("CRASH-WRITE-DONE")       # not expected to appear: the plug comes first

a = args()
if len(a) < 3:
    print("usage: durcheck.as <write|verify|churn> <path> [tiny|small|mid|big|huge]")
else:
    op = a[1]
    path = a[2]
    sz = size_of(a[3] if len(a) > 3 else "mid")
    if op == "write":
        cmd_write(path, sz)
    elif op == "verify":
        cmd_verify(path, sz)
    elif op == "churn":
        cmd_churn(path)
    elif op == "crashwrite":
        cmd_crashwrite(path)
    else:
        print("durcheck: unknown op", op)
