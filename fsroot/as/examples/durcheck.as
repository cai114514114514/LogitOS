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

# The builtins, not lib/sys.as: this script has to run on the host too, where
# there is no /usr/as/lib. Deleting goes straight through the syscall for the
# same reason (on the host it stubs to -1, which churn does not care about).

SIZE_TINY = 100
SIZE_SMALL = 30000
SIZE_MID = 120000

def size_of(name):
    if name == "tiny":
        return SIZE_TINY
    if name == "small":
        return SIZE_SMALL
    return SIZE_MID

# Byte i of the stream is a function of i alone, so verify never has to have
# seen the writer. Cheap, and it changes in every byte position.
def chunk(index):
    out = []
    base = index * 251
    for i in range(256):
        out.append(chr((base + i * 37 + (i * i) % 97) % 256))
    return "".join(out)

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

a = args()
if len(a) < 3:
    print("usage: durcheck.as <write|verify|churn> <path> [tiny|small|mid]")
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
    else:
        print("durcheck: unknown op", op)
