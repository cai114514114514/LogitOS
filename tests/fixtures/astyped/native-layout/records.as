# aether: 3.0
# The deliberately unaligned and overlapping fields distinguish explicit ABI
# layout from LLVM's ordinary struct padding. A second nominal type has the
# same bytes so accidental structural type compatibility is observable.
Packet = layout("packet", 24, [
    ["small", 0, 1, "i"],
    ["signed", 1, 2, "i"],
    ["word", 3, 4, "u"],
    ["pointer", 7, 8, "p"],
    ["text", 15, 8, "s"],
    ["overlay", 3, 1, "u"]
])

Other = layout("other", 24, [["text", 15, 8, "s"]])
Pair = layout("pair", 2, [["first", 0, 1, "u"], ["second", 1, 1, "u"]])

def make_packet() -> Packet:
    packet = Packet()
    packet.small = -7
    packet.signed = -1234
    packet.word = 0x12345678
    packet.pointer = 0x0102030405060708
    packet.text = Bytes("hi")
    return packet
