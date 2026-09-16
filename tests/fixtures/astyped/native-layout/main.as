# aether: 3.0
import records
from records import Packet, Pair, make_packet

struct PacketBox:
    record: Packet

class Holder:
    record: Packet

saved_global: Optional[Packet] = None

def same[T: Equatable](left: T, right: T) -> bool:
    return left == right

def captured() -> Callable[[], Packet]:
    packet = make_packet()
    def read() -> Packet:
        return packet
    return read

def exercise() -> None:
    global saved_global
    packet: Packet = make_packet()
    assert packet.small == -7 and packet.signed == -1234
    assert packet.word == 0x12345678 and packet.overlay == 0x78
    assert packet.pointer == 0x0102030405060708
    assert len(packet) == 24 and packet[1] == 46 and packet[2] == 251
    assert packet[7] == 8 and packet[14] == 1
    saved = packet.text
    assert len(saved) == 8 and saved[0] == 104 and saved[2] == 0
    alias = packet
    alias.signed += 1
    assert packet.signed == -1233
    alias.overlay = 0x55
    assert packet.word == 0x12345655
    alias.text = Bytes("A")
    assert saved[0] == 104 and packet.text[0] == 65 and packet.text[1] == 0
    assert same(packet, alias) and not same(packet, Packet())

    # Immutable copies, byte loops and byte writes use the same bounded
    # storage contract as Buffer, including negative indices.
    copied = Bytes(packet)
    packet[-1] = 42
    packet[-1] += 1
    assert copied[-1] == 0 and packet[-1] == 43
    assert 43 in packet and 999 not in packet
    collected = [x for x in packet]
    assert len(collected) == 24
    for index in range(len(packet)):
        assert collected[index] == packet[index]
    pair = Pair()
    pair.first = 65
    pair.second = 66
    assert str(pair) == "pair(first=65, second=66)"
    assert str(Any(pair)) == str(pair)
    assert str([pair]) == "[pair(first=65, second=66)]"
    formatted = str(packet)
    assert "packet(small=-7, signed=-1233, word=305419861" in formatted
    assert "text=" + str(packet.text) in formatted
    assert Any(pair) == Any(pair) and Any(pair) != Any(Pair())
    a, b = pair
    assert a == 65 and b == 66
    unsafe:
        assert mem2str(pair, 2) == "AB"
        assert peek8(addr(pair)) == 65
        assert mem2cstr(packet.text) == "A"

    errors = 0
    try:
        packet.text = Bytes("123456789")
    except ValueError as error:
        assert error.line > 0 and error.column > 0
        errors += 1
    assert packet.text[0] == 65 and packet.text[1] == 0
    try:
        packet[24] = 1
    except IndexError:
        errors += 1
    try:
        packet[0] = 256
    except ValueError:
        errors += 1
    try:
        packet.small = 127
        packet.small += 1
    except OverflowError:
        errors += 1
    assert errors == 4 and packet.small == 127

    # Each path must keep the shared record alive through explicit collection.
    items: List[Packet] = [make_packet()]
    table: Dict[str, Packet] = {"packet": make_packet()}
    boxed: Any = Any(make_packet())
    optional: Optional[Packet] = make_packet()
    read = captured()
    boxes: List[PacketBox] = [PacketBox(make_packet())]
    holder = Holder(make_packet())
    saved_global = make_packet()
    gc_collect()
    assert items[0].signed == -1234 and table["packet"].word == 0x12345678
    assert cast[Packet](boxed).small == -7
    assert optional is not None
    assert optional.pointer == 0x0102030405060708
    assert read().text[0] == 104
    assert boxes[0].record.small == -7 and holder.record.signed == -1234
    read().signed += 2
    assert read().signed == -1232
    assert records.Packet().small == 0

def verify_global() -> None:
    global saved_global
    recovered = saved_global
    assert recovered is not None and recovered.word == 0x12345678
    saved_global = None

def main() -> None:
    exercise()
    gc_collect()
    verify_global()
    # The narrowed local in verify_global owned the record until its frame
    # exited. This caller holds no remaining managed values.
    gc_collect()
    assert gc_live_bytes() == 0
    print("native layouts ok")
