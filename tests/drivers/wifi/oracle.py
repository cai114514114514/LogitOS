"""Independent AP wire oracle using hashlib and Python cryptography.

Wire offsets and selectors are intentionally literal here: importing product
constants would let the implementation and its oracle agree on the same error.
These fixtures model only ordinary station association and authenticated data.
"""

import hashlib, hmac, struct, sys
from pathlib import Path
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

station_mac = bytes.fromhex("020000000001")
ap_mac = bytes.fromhex("020000000002")
ssid = b"IEEE"
pmk = hashlib.pbkdf2_hmac("sha1", b"password", ssid, 4096, 32)
supplicant_nonce = bytes(range(1, 33))
authenticator_nonce = bytes(range(101, 133))
gtk = bytes(range(201, 217))
seed = (
    min(station_mac, ap_mac)
    + max(station_mac, ap_mac)
    + min(supplicant_nonce, authenticator_nonce)
    + max(supplicant_nonce, authenticator_nonce)
)
ptk = b"".join(
    (
        hmac.new(
            pmk, b"Pairwise key expansion\x00" + seed + bytes([i]), hashlib.sha1
        ).digest()
        for i in range(3)
    )
)[:48]
rsn = bytes.fromhex("30140100000fac040100000fac040100000fac020000")
llc = bytes.fromhex("aaaa03000000")


def mac_header(frame_control, destination=station_mac, source=ap_mac, third=ap_mac):
    return (
        struct.pack("<HH", frame_control, 0)
        + destination
        + source
        + third
        + b"\x00\x00"
    )


beacon = (
    mac_header(128, b"\xff" * 6)
    + b"\x00" * 8
    + struct.pack("<HH", 100, 17)
    + b"\x00\x04IEEE"
    + bytes.fromhex("010882848b960c121824030101")
    + rsn
)
auth = mac_header(176) + struct.pack("<HHH", 0, 2, 0)
assoc = mac_header(16) + struct.pack("<HHH", 17, 0, 49153)


def eapol(key_info, replay, nonce=None, data=b"", reply=False, signing_key=ptk):
    packet = bytearray(99 + len(data))
    packet[:2] = b"\x02\x03"
    struct.pack_into(">H", packet, 2, len(packet) - 4)
    packet[4] = 2
    struct.pack_into(">HHQ", packet, 5, key_info, 0 if reply else 16, replay)
    if nonce:
        packet[17:49] = nonce
    struct.pack_into(">H", packet, 97, len(data))
    packet[99:] = data
    if key_info & 256:
        packet[81:97] = hmac.new(signing_key[:16], packet, hashlib.sha1).digest()[:16]
    return bytes(packet)


m1 = eapol(138, 1, authenticator_nonce)
key_data = rsn + b"\xdd\x16\x00\x0f\xac\x01\x01\x00" + gtk + b"\xdd\x00"
m3 = eapol(5066, 2, authenticator_nonce, aes_key_wrap(ptk[16:32], key_data))
m2 = eapol(266, 1, supplicant_nonce, rsn, True)
m4 = eapol(778, 2, reply=True)


def wrap(packet):
    return mac_header(520) + llc + b"\x88\x8e" + packet


ethernet = station_mac + bytes.fromhex("020000000003") + b"\x08\x00" + bytes(range(40))
data_header = mac_header(16904, station_mac, ap_mac, ethernet[6:12])
aad = (
    bytes([data_header[0] & 143, data_header[1] & 199])
    + data_header[4:22]
    + b"\x00\x00"
)
nonce = b"\x00" + ap_mac + (1).to_bytes(6, "big")
plain = llc + ethernet[12:]
rx = (
    data_header
    + bytes.fromhex("0100002000000000")
    + AESCCM(ptk[32:48], tag_length=8).encrypt(nonce, plain, aad)
)
key = bytes(range(16))
nonce = bytes(range(13))
aad = bytes(range(22))
plain = bytes(range(47))
vectors = {
    "pmk": pmk,
    "ptk": ptk,
    "beacon": beacon,
    "auth": auth,
    "assoc": assoc,
    "m1": wrap(m1),
    "m3": wrap(m3),
    "m2": m2,
    "m4": m4,
    "ethernet": ethernet,
    "rx": rx,
    "ccm": AESCCM(key, tag_length=8).encrypt(nonce, plain, aad),
    "wrapped": aes_key_wrap(key, bytes(range(32))),
}

# Pairwise rekey keeps the old traffic key until M4 is transmitted.
next_authenticator_nonce = bytes(range(151, 183))
next_seed = (
    min(station_mac, ap_mac)
    + max(station_mac, ap_mac)
    + min(supplicant_nonce, next_authenticator_nonce)
    + max(supplicant_nonce, next_authenticator_nonce)
)
next_ptk = b"".join(
    hmac.new(
        pmk, b"Pairwise key expansion\0" + next_seed + bytes([index]), hashlib.sha1
    ).digest()
    for index in range(3)
)[:48]
next_gtk = bytes(range(31, 47))
group_data = b"\xdd\x16\x00\x0f\xac\x01\x02\x00" + next_gtk
vectors.update(
    {
        "rekey_m1": wrap(eapol(0x008A, 3, next_authenticator_nonce)),
        "rekey_m3": wrap(
            eapol(
                0x13CA,
                4,
                next_authenticator_nonce,
                aes_key_wrap(next_ptk[16:32], key_data),
                signing_key=next_ptk,
            )
        ),
        "rekey_ptk": next_ptk,
        "group_one": wrap(eapol(0x1382, 5, data=aes_key_wrap(ptk[16:32], group_data))),
        "group_again": wrap(
            eapol(0x1382, 6, data=aes_key_wrap(ptk[16:32], group_data))
        ),
        "group_key": next_gtk,
    }
)


def protected_frame(
    frame_control,
    destination,
    source,
    third,
    traffic_key,
    packet_number,
    payload,
    key_id=0,
):
    header = mac_header(frame_control, destination, source, third)
    associated = bytes([header[0] & 0x8F, header[1] & 0xC7]) + header[4:22] + b"\0\0"
    nonce = b"\0" + source + packet_number.to_bytes(6, "big")
    pn = packet_number.to_bytes(6, "little")
    iv = pn[:2] + bytes([0, 0x20 | key_id << 6]) + pn[2:]
    return (
        header
        + iv
        + AESCCM(traffic_key, tag_length=8).encrypt(nonce, payload, associated)
    )


vectors["rekey_m2_wire"] = protected_frame(
    0x4108,
    ap_mac,
    station_mac,
    ap_mac,
    ptk[32:],
    1,
    llc + b"\x88\x8e" + eapol(0x010A, 3, supplicant_nonce, rsn, True, next_ptk),
)
vectors["rekey_m4_wire"] = protected_frame(
    0x4108,
    ap_mac,
    station_mac,
    ap_mac,
    ptk[32:],
    2,
    llc + b"\x88\x8e" + eapol(0x030A, 4, reply=True, signing_key=next_ptk),
)
vectors["group_two_wire"] = protected_frame(
    0x4108,
    ap_mac,
    station_mac,
    ap_mac,
    ptk[32:],
    1,
    llc + b"\x88\x8e" + eapol(0x0302, 5, reply=True),
)
vectors["group_rx"] = protected_frame(
    0x4208, b"\xff" * 6, ap_mac, ethernet[6:12], next_gtk, 1, llc + ethernet[12:], 2
)
vectors["new_pair_rx"] = protected_frame(
    0x4208, station_mac, ap_mac, ethernet[6:12], next_ptk[32:], 1, llc + ethernet[12:]
)


vectors["rekey_m1_retry"] = wrap(eapol(0x008A, 4, next_authenticator_nonce))
vectors["rekey_m3_retry"] = wrap(
    eapol(
        0x13CA,
        5,
        next_authenticator_nonce,
        aes_key_wrap(next_ptk[16:32], key_data),
        signing_key=next_ptk,
    )
)
vectors["rekey_m3_later_retry"] = wrap(
    eapol(
        0x13CA,
        7,
        next_authenticator_nonce,
        aes_key_wrap(next_ptk[16:32], key_data),
        signing_key=next_ptk,
    )
)

output_path = Path(sys.argv[1])
output_path.parent.mkdir(parents=True, exist_ok=True)
output_path.write_text(
    "/* Generated by independent Python cryptography AP oracle. */\n"
    + "".join(
        (
            "static const unsigned char oracle_"
            + name
            + "[]={"
            + ",".join((str(x) for x in data))
            + "};\n"
            for name, data in vectors.items()
        )
    )
)
