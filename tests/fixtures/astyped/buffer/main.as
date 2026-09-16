# aether: 3.0
# Bytes occupy contiguous one-byte storage even though the buffer API uses
# i64 values. A write to the last byte must not overwrite neighboring memory.
class Packet:
    data: Buffer


def payload() -> Buffer:
    data = buffer(8)
    data[0] = 1
    data[-1] = 255
    return data


def collect() -> i64:
    gc_collect()
    return 1


def consume(data: Buffer, count: i64) -> None:
    assert count == 1 and data[-1] == 255


def worker() -> None:
    empty = buffer(0)
    assert len(empty) == 0 and 0 not in empty
    data = payload()
    alias = data
    assert alias == data and data != buffer(8)
    assert len(data) == 8 and data[-8] == 1
    assert data[1] == 0 and data[-1] == 255
    data[1] = 10
    data[1] += 2
    data[1] *= 3
    data[1] /= 4
    data[1] %= 4
    data[1] -= 1
    assert data[1] == 0 and data[2] == 0
    assert 255 in data and -1 not in data and 256 not in data
    # List equality is reference equality; compare the extracted values.
    selected = [value for value in data if value > 0]
    assert len(selected) == 2 and selected[0] == 1 and selected[1] == 255
    total = 0
    for value in data:
        gc_collect()
        total += value
    assert total == 256
    short = Buffer(2)

    first, last = short
    assert first == 0 and last == 0
    retained = Packet(payload())
    boxed = Any(retained.data)
    gc_collect()
    assert cast[Buffer](boxed)[-1] == 255
    assert Any(retained.data) == boxed
    assert Any(data) != boxed
    consume(payload(), collect())
    assert str(data) == "<buffer 8>"
    assert str(retained) == "Packet(data=<buffer 8>)"

    rejected = 0
    try:
        data[8] = 4
    except IndexError:
        rejected += 1
    try:
        print(data[-9])
    except IndexError:
        rejected += 1
    try:
        print(empty[0])
    except IndexError:
        rejected += 1
    try:
        data[0] = 256
    except ValueError:
        rejected += 1
    try:
        data[0] = -1
    except ValueError:
        rejected += 1
    try:
        data[0] += 255
    except ValueError:
        rejected += 1
    try:
        buffer(-1)
    except ValueError:
        rejected += 1
    try:
        buffer(67108865)
    except ValueError:
        rejected += 1
    try:
        first, last = data
    except ValueError:
        rejected += 1
    assert rejected == 9 and data[0] == 1


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native buffers ok")
