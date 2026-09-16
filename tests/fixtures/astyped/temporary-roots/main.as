# aether: 3.0
# Distinct expression sites used to retain every old buffer until return,
# although the only source owner had already been overwritten.
def check_buffers() -> None:
    data = buffer(1048576)
    data = buffer(1048576)
    data = buffer(1048576)
    data = buffer(1048576)
    data = buffer(1048576)
    data = buffer(1048576)
    gc_collect()
    assert len(data) == 1048576
    assert gc_live_bytes() < 1100000

class Holder:
    data: Buffer

def check_construction() -> None:
    owner = Holder(buffer(1048576))
    owner = Holder(buffer(1048576))
    owner = Holder(buffer(1048576))
    gc_collect()
    assert len(owner.data) == 1048576
    assert gc_live_bytes() < 1100000

def collect() -> i64:
    gc_collect()
    return 7

def consume(data: Buffer, number: i64) -> None:
    assert len(data) == 256 and number == 7

def check_live_values() -> None:
    consume(buffer(256), collect())
    count = 0
    for data in [buffer(4), buffer(4), buffer(4)]:
        gc_collect()
        data[0] = 42
        assert data[0] == 42
        count += 1
    assert count == 3
    for key in {"first": Bytes("one"), "second": Bytes("two")}:
        gc_collect()
        assert key == "first" or key == "second"
    try:
        raise ValueError("retained " + "message")
    except Error as error:
        gc_collect()
        assert error.message == "retained message"

def main() -> None:
    check_buffers()
    check_construction()
    check_live_values()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native temporary roots ok")
