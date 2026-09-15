# Wi-Fi station code

`station.h` owns the public station state and transport callbacks. Initialize an
unowned instance, scan the radio's permitted channel list, and join a returned
BSS. Feed complete FCS-checked MPDUs through `wifi_station_receive`, call
`wifi_station_tick` from the transport poll loop, and send Ethernet through
`wifi_station_transmit`. The Ethernet callback borrows its buffer only during
the callback. All callbacks must be bounded and must not reenter the station.

The internal entry points require the station lock. The public functions acquire
it once around a state transition and its synchronous transport callbacks.

- `station.c`: initialization, deadlines, failure cleanup, disconnect.
- `management/scan.c`: passive scan and beacon information elements.
- `management/association.c`: Open-System authentication and association requests.
- `management/receive.c`: frame dispatch and management response transitions.
- `security/rsn.c`: supported security policy; required PMF is refused.
- `security/handshake.c`: EAPOL envelope/MIC validation, M1/M3 processing, GTK
  validation, temporary PTK management and M3 retransmission without reinstallation.
- `security/keydata.c`: authenticated RSN/GTK information element parsing.
- `security/group.c`: group rotations with independent replay windows per key slot.
- `security/derivation.c`: HMAC-SHA1, PBKDF2 and WPA2 key derivation.
- `security/keywrap.c`: RFC3394 authenticated key-data unwrapping.
- `security/ccmp.c`: RFC3610 CCM with the CCMP-128 nonce/tag sizes.
- `data/frame.c`: CCMP framing, replay protection and Ethernet conversion.
- `adapter/netdev.c`: permanent kernel netdev, transport callback registration,
  bounded polling, Ethernet delivery and detach ownership.
- `protocol.h`: wire field offsets, security selectors and frame-control masks.

Association does not open the Ethernet port. An authenticated M3, parsed group
key and successful M4 submission are required. Repeated M3 sends M4 again without
resetting packet numbers, including retries with newer EAPOL replay counters.
Pairwise rekey derives a temporary PTK while the installed key continues carrying
traffic. An authenticated M3 and submitted M4 commit the candidate keys; a failed
submission can retry without publishing them. The PMK stays until disconnect so
later pairwise rekeys can derive a new PTK. Group rotations keep four receive
slots, preserving replay windows when identical keys are announced again.

Five seconds without an AP beacon closes the link and erases session keys.
A pending pairwise handshake also has a five-second deadline. Malformed or
unauthenticated key messages leave the active key unchanged while that deadline
runs. Transport failure and explicit detach close the link immediately. Recovery
is explicit scan/join; the adapter does not keep a plaintext password or pretend
to reassociate automatically. Failed data submission still consumes its packet
number, because the transport might already have handed the frame to hardware.

## Evidence and remaining work

The host gate links every production module above and runs an independent Python
`hashlib`/`cryptography` access-point oracle. It verifies exact M2/M4 bytes,
CCMP ciphertext, replay rejection, timeout and secret erasure. MIC bypass, GTK
reinstallation and fabricated-link negative controls are
prerequisites. The adapter gate includes the real kernel `netdev.c` and exercises
its transmit/receive entry points with the same authenticated AP frames. Run:

```
make -f tests/drivers/wifi/tests.mk BUILD=build/drivers/expansion/wifi CC=clang test-wifi-adapter-host
```

This is host protocol evidence, not a physical-radio or operating-system network
connection. Intel AX200/AX210 PCI transport, DMA firmware boot/ALIVE, PNVM,
firmware command queues and calibrated channels are not implemented here.
MAC ACK/retry/rate control belongs to that transport. PMF, WPA3 and roaming
remain unsupported.

The kernel adapter is a real consumer of station Ethernet and transport callbacks,
but it has no physical Intel transport to attach yet. `wifi_adapter_attach`
registers one permanent interface after `netdev_init`; it does not set RUNNING
until authentication completes. An existing primary NIC remains primary. The
legacy netdev vtable has no context pointer, so this adapter supports one radio.
It may reattach the same name/MAC after detach without consuming another slot.
`NETIF_MAX` includes this permanent slot and loopback.

If boot found no NIC, the radio owner first calls `wifi_adapter_poll` to scan and
associate, then invokes `net_init` from a control path to configure IP/DHCP.
The normal network poll subsequently drives the adapter. IP setup is deliberately
outside transport callbacks because DHCP may wait; registration is not IP setup.
No physical radio or guest Internet connection was exercised by the host gate.

## Protocol references

- [Linux v6.12 mac80211 CCMP AAD/nonce construction](https://github.com/torvalds/linux/blob/v6.12/net/mac80211/wpa.c)
- [Android's upstream hostap RSN supplicant](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/refs/heads/main/src/rsn_supp/wpa.c)
- [RFC3394 AES Key Wrap](https://www.rfc-editor.org/rfc/rfc3394)
- [RFC3610 CCM](https://www.rfc-editor.org/rfc/rfc3610)

The test oracle intentionally keeps independent literal wire offsets and
selectors: sharing the production constants would hide a common format error.
