# aether: 3.0
# durcheck -- generate and verify the original filesystem durability fixtures.
# Run /usr/as/bin/durcheck.aex <write|verify|churn|crashwrite> PATH [SIZE]
#
# Length alone cannot detect one file receiving another file's block. Every
# byte below depends on its position, and verification reports the first wrong
# byte. The formula and sizes are unchanged from the A2 tool so existing disks
# remain verifiable. Arbitrary octets now use Bytes, never Unicode chr()/str.

SIZE_TINY: i64 = 100
SIZE_SMALL: i64 = 30000
SIZE_MID: i64 = 120000
SIZE_BIG: i64 = 400000
# 1075 blocks exceed direct[12] plus 1024 single-indirect slots, so this
# original fixture reaches LogitFS's double-indirect tree.
SIZE_HUGE: i64 = 4400000
CHUNK_CACHE: List[Bytes] = []


def size_of(name: str) -> i64:
    if name == "tiny":
        return SIZE_TINY
    if name == "small":
        return SIZE_SMALL
    if name == "big":
        return SIZE_BIG
    if name == "huge":
        return SIZE_HUGE
    # Retain the original default, including unknown size names.
    return SIZE_MID


def chunk(index: i64) -> Bytes:
    if index < 0:
        raise ValueError("Chunk index must be nonnegative")
    residue = index % 256
    # The 256-byte chunks repeat every 256 chunks. Cache immutable copies;
    # reusing a mutable buffer would silently change previously cached chunks.
    while len(CHUNK_CACHE) <= residue:
        base = len(CHUNK_CACHE) * 251
        data = buffer(256)
        for offset in range(256):
            data[offset] = (base + offset * 37 + (offset * offset) % 97) % 256
        CHUNK_CACHE.append(Bytes(data))
    return CHUNK_CACHE[residue]


def build(size: i64) -> Bytes:
    if size < 0:
        raise ValueError("Fixture size must be nonnegative")
    data = buffer(size)
    position = 0
    index = 0
    while position < size:
        part = chunk(index)
        count = size - position
        if count > 256:
            count = 256
        for offset in range(count):
            data[position + offset] = part[offset]
        position += count
        index += 1
    return Bytes(data)


def cmd_write(path: str, size: i64) -> i64:
    data = build(size)
    try:
        wrote = file_write(path, data)
    except IOError as error:
        print("DURCHECK-FAIL", path, "write")
        return 1
    print("durcheck write", path, size, "->", wrote)
    return 0


def cmd_verify(path: str, size: i64) -> i64:
    try:
        data = file_read(path)
    except IOError as error:
        print("DURCHECK-FAIL", path, "unreadable")
        return 1
    if len(data) != size:
        print("DURCHECK-FAIL", path, "length", len(data), "expected", size)
        return 1
    want = build(size)
    if data != want:
        for offset in range(size):
            if data[offset] != want[offset]:
                print("DURCHECK-FAIL", path, "first bad byte", offset,
                      "got", data[offset], "want", want[offset])
                return 1
    print("DURCHECK-OK", path, size)
    return 0


def _remove(path: str) -> None:
    # A3 text can be an interior view. The syscall needs a terminated copy
    # whose owner remains live until it returns.
    terminated = Bytes(path + "\0")
    unsafe:
        result = syscall(SYS_DELETE_FILE, addr(terminated))
    if result < 0:
        raise IOError("Cannot remove durability fixture: " + path)


def cmd_churn(directory: str) -> None:
    # Bystander files must survive these ordinary create/delete cycles. Errors
    # now abort instead of printing DONE after an unsuccessful deletion.
    path = directory + "/churn.tmp"
    for round in range(12):
        file_write(path, build(3000 + round * 900))
        _remove(path)
    print("DURCHECK-CHURN-DONE")


def cmd_crashwrite(path: str) -> None:
    # Prebuild before ARMED: the existing recovery harness interrupts I/O, not
    # fixture generation. Keep the original 300 rounds and progress markers.
    # A former SIZE_MID implementation disagreed with the harness's "big"
    # verifier and called intact victims torn; use SIZE_BIG here.
    data = build(SIZE_BIG)
    other = path + ".b"
    print("CRASH-WRITE-ARMED")
    for round in range(300):
        file_write(path, data)
        file_write(other, data)
        _remove(path)
        if round % 10 == 0:
            print("CRASH-WRITE-ROUND", round)
    print("CRASH-WRITE-DONE")


def main() -> i64:
    arguments = args()
    if len(arguments) < 3:
        print("usage: durcheck.as <write|verify|churn|crashwrite> <path> [tiny|small|mid|big|huge]")
        return 2
    operation = arguments[1]
    path = arguments[2]
    size = size_of(arguments[3] if len(arguments) > 3 else "mid")
    if operation == "write":
        return cmd_write(path, size)
    if operation == "verify":
        return cmd_verify(path, size)
    if operation == "churn":
        cmd_churn(path)
        return 0
    if operation == "crashwrite":
        cmd_crashwrite(path)
        return 0
    print("durcheck: unknown op", operation)
    return 2
