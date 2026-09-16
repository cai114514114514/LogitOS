# aether: 3.0
# Counters observe real resource lifetimes. They are snapshots, so neither GC
# nor editing a returned dictionary can silently alter the underlying state.
def main() -> None:
    path = args()[1] + "/port-stats.txt"
    file_write(path, Bytes("content\n"))
    initial = port_stats()
    assert len(initial) == 5 and initial["open"] == 0
    assert initial["finalized"] == 0 and initial["orphans"] == 0
    assert initial["close_errors"] == 0
    with file = open(path):
        assert port_stats()["open"] == 1
        assert initial["open"] == 0
        gc_collect()
        assert port_stats()["open"] == 1 and file.readall() == Bytes("content\n")
        file.close()
        assert port_stats()["open"] == 0
        assert port_stats()["closed"] == initial["closed"] + 1
        file.close()
    assert port_stats()["closed"] == initial["closed"] + 1

    with view = port(1):
        assert port_stats()["open"] == 0
        view.close()
    assert port_stats()["closed"] == initial["closed"] + 1
    with reader, writer = pipe():
        assert port_stats()["open"] == 2
        writer.close()
        assert port_stats()["open"] == 1
        assert reader.readall() == Bytes("")
    assert port_stats()["closed"] == initial["closed"] + 3

    try:
        with file = open(path):
            raise ValueError("close on exception")
    except ValueError:
        pass
    assert port_stats()["open"] == 0
    assert port_stats()["closed"] == initial["closed"] + 4
    try:
        with file = open(path + ".absent"):
            assert False
    except IOError:
        pass
    assert port_stats()["closed"] == initial["closed"] + 4

    snapshot = port_stats()
    snapshot["open"] = 99
    for index in range(100):
        snapshot["extra" + str(index)] = index
    gc_collect()
    assert snapshot["open"] == 99 and port_stats()["open"] == 0
    for index in range(100):
        assert snapshot["extra" + str(index)] == index
    assert port_stats()["close_errors"] == 0
    assert port_stats()["finalized"] == 0 and port_stats()["orphans"] == 0
    print("native port statistics ok")
