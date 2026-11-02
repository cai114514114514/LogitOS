# Wi-Fi support

The code in `c/drivers/net/wifi/` implements an 802.11 infrastructure station with passive scanning, Open System authentication, association, WPA2-PSK key exchange and CCMP-128 data protection. A transport supplies radio operations; the station handles management frames, session keys and Ethernet conversion. The kernel adapter exposes authenticated station traffic through the network-device interface.

## Station and transport

`struct wifi_station` in `c/drivers/net/wifi/station.h` holds scan results, the selected access point, connection state, keys and replay counters. `struct wifi_station_ops` supplies channel selection, MPDU transmission, cryptographic random bytes, time in milliseconds and an Ethernet receive callback.

The transport supplies complete 802.11 MAC protocol data units, or MPDUs, with the frame check sequence checked and removed. It preserves CCMP headers, ciphertext and authentication tags for software processing. MAC acknowledgements, retries, rate control and regulatory channel selection belong to the transport. These contracts appear in `c/drivers/net/wifi/station.h` and `c/drivers/net/wifi/adapter/netdev.h`.

Station entry points serialize protocol transitions with an atomic try-lock. Transport callbacks run synchronously while the station gate is held. Callers must serialize transport access, keep callback context valid and provide bounded callbacks that avoid reentering the station. Frame buffers are borrowed for the duration of a callback. `c/drivers/net/wifi/internal.h` defines the gate and internal calling rules.

| Operation | Entry point |
| --- | --- |
| Initialise station state and callbacks | `wifi_station_init` |
| Start scanning supplied channels | `wifi_station_scan` |
| Join a stored scan result | `wifi_station_join` |
| Process an incoming MPDU | `wifi_station_receive` |
| Advance scan and connection deadlines | `wifi_station_tick` |
| Submit an Ethernet frame | `wifi_station_transmit` |
| Query authenticated link readiness | `wifi_station_link_up` |
| Report transport loss | `wifi_station_transport_lost` |
| Clear the session and return to idle | `wifi_station_disconnect` |

## Scanning and security selection

`wifi_station_scan` accepts 1–64 nonzero channel numbers while the station is idle or failed. It starts a new result set, tunes to the first channel and enters `WIFI_SCANNING`. Each channel receives a 150 ms dwell deadline. Calls to `wifi_station_tick` advance the channel list and return the station to `WIFI_IDLE` after the final dwell. The implementation is in `c/drivers/net/wifi/management/scan.c` and `c/drivers/net/wifi/station.c`; timing constants are in `c/drivers/net/wifi/protocol.h`.

During scanning, beacons and probe responses supply candidate basic service set (BSS) records. The parser checks information-element lengths and records the BSSID, SSID, current channel, capabilities, supported rates, RSN information and transport-supplied signal strength. The table holds 16 records and updates an existing record when its BSSID appears again.

Candidates must advertise an infrastructure network with privacy enabled, a 1–32 byte SSID, a supported-rates element containing 1–8 rates and supported RSN information. A channel element, when present, must match the channel being scanned. The parser also checks that the transmitter address matches the BSSID.

`wifi_rsn_supported` in `c/drivers/net/wifi/security/rsn.c` accepts RSN version 1 with CCMP as the group cipher, CCMP among the pairwise ciphers and PSK among the authentication methods. The station advertises its selected CCMP/PSK profile through `wifi_rsn_information_element` in `c/drivers/net/wifi/protocol.h`.

## Authentication and association

`wifi_station_join` selects a stored BSS while the station is idle or scanning. It clears session secrets, resets pairwise handshake replay counters, derives the master key from the passphrase and SSID, tunes to the AP’s channel and sends an Open System authentication request. This path is implemented in `c/drivers/net/wifi/management/association.c`.

The authentication response must address the station and identify the selected AP as both transmitter and BSSID. Its body must contain the Open System algorithm, response sequence 2 and success status. The station then sends an association request containing selected AP capability bits, a listen interval of 10, the SSID, supported rates and the station’s RSN element.

A successful association response supplies an association identifier from 1 through 2007 and moves the station into `WIFI_KEY_EXCHANGE`. Authentication and association each receive a 2,000 ms deadline. Association success starts a 5,000 ms key-exchange deadline. Response checks and transitions are in `c/drivers/net/wifi/management/receive.c`.

Ethernet readiness follows key establishment. The handshake publishes the link flag after validated M3 processing, successful M4 submission and session-key installation. `wifi_station_link_up` reads that flag.

## WPA2 key exchange

`wifi_psk` in `c/drivers/net/wifi/security/derivation.c` derives a 32-byte pairwise master key, or PMK, using PBKDF2-HMAC-SHA1 with 4,096 iterations and the SSID as salt. It accepts passphrases of 8–63 bytes.

`wifi_ptk` derives a 48-byte pairwise transient key, or PTK, from the PMK, both MAC addresses and both nonces. Sorting the addresses and nonces gives each peer the same derivation input. The PTK contains three 16-byte components:

| Component | Use |
| --- | --- |
| Key confirmation key | Authenticate EAPOL-Key messages |
| Key encryption key | Unwrap encrypted key data |
| Temporal key | Protect data frames with CCMP |

EAPOL-Key packets reach `wifi_receive_key_exchange` through the LLC/SNAP receive path in `c/drivers/net/wifi/data/frame.c`. Envelope checks in `c/drivers/net/wifi/security/handshake.c` validate packet lengths, EAPOL version 1 or 2, the RSN descriptor and an incoming key length of 16 bytes. Station replies set the key-length field to zero. Message dispatch matches the Key Information values defined in `c/drivers/net/wifi/protocol.h`.

| Message | Station processing |
| --- | --- |
| M1, AP to station | Record the AP nonce and replay counter, obtain a nonzero station nonce from the random callback, and derive a candidate PTK. Enter `WIFI_WAIT_M3`, or `WIFI_REKEYING` for an active link. |
| M2, station to AP | Return the station nonce and selected RSN element, authenticated with HMAC-SHA1 truncated to 16 bytes. |
| M3, AP to station | Check the AP nonce, MIC and replay counter; unwrap key data; compare its RSN element with the scanned advertisement; and parse any group key. |
| M4, station to AP | Submit the authenticated reply, then commit the candidate keys and enter `WIFI_CONNECTED`. |

Starting a candidate handshake from M1 sets a new 5,000 ms deadline. For its M3, the replay counter must exceed both the pending M1 counter and the committed EAPOL counter. Initial establishment requires a group temporal key, or GTK. A pairwise rekey can retain the installed group keys when M3 omits a GTK.

The authenticated RSN comparison binds M3 to the advertisement selected during scanning. `c/drivers/net/wifi/security/keydata.c` requires the RSN element and compares its bytes with the stored advertisement. It also validates the GTK element when present. `c/drivers/net/wifi/security/keywrap.c` implements AES key unwrapping and checks the RFC3394 integrity register before publishing the unwrapped bytes.

### Retransmission and key rotation

While waiting for M3, a repeated M1 with the same AP nonce and an equal or greater replay counter reuses the station nonce and candidate PTK. This preserves agreement with an M3 the AP may already have prepared.

After installation, a validated M3 retransmission triggers another M4. The station compares a fingerprint that excludes the replay counter and MIC, allowing matching retries with newer counters to retain the installed keys and packet-number history. Sending the reply consumes a fresh transmit packet number. Resetting packet numbers under the same traffic key would permit nonce reuse, so a candidate PTK matching the installed PTK is rejected during rekey. These checks are in `c/drivers/net/wifi/security/handshake.c`.

During pairwise rekey, the installed key continues carrying traffic while the candidate PTK is prepared. Replies use the candidate EAPOL MIC key and the installed CCMP traffic key. Successful M4 submission commits the replacement PTK. The PMK remains available for later rekeys until disconnect or failure.

Group rotation in `c/drivers/net/wifi/security/group.c` verifies the MIC and replay counter, unwraps the key data, submits the reply and installs the GTK. Four group-key slots support rotation. Reannouncing the same key in its current slot preserves its receive counters. Installing matching material under another key ID takes the greater of the AP’s receive sequence counter and the stored counter for each priority among matching installed slots. The key ID is absent from the CCMP nonce, so moving the same key between slots must preserve replay history.

## CCMP and Ethernet conversion

`c/drivers/net/wifi/security/ccmp.c` implements CCM with AES-128, a 13-byte nonce and an 8-byte authentication tag through the shared AES backend. Decryption keeps plaintext in scratch storage until tag verification succeeds.

`c/drivers/net/wifi/data/frame.c` builds the nonce from the QoS priority, transmitter address and 48-bit packet number. Traffic without QoS uses priority zero in the nonce. Additional authenticated data covers masked frame-control fields, the three MAC addresses, fragment information and, for QoS frames, the priority. The masks accommodate header fields that the MAC may change during retransmission.

Transmit processing converts Ethernet into a protected data frame sent through the AP. It adds LLC/SNAP, encrypts with the pairwise temporal key and submits the MPDU. The public interface accepts Ethernet frames of 14–1,514 bytes, requires the station MAC as the source and checks for an EtherType of at least 1,536.

The transmit packet number is consumed before submission because a failing transport callback may already have handed the frame to hardware. Transmission fails when the 48-bit counter is exhausted.

Receive processing checks the AP transmitter address, frame direction and destination. Unicast uses the pairwise key; multicast uses the group-key slot named by the CCMP header. The pairwise key and each group slot have 17 receive counters: one for each QoS priority from 0 through 15 and one for traffic without QoS.

A protected packet must carry a nonzero packet number greater than its stored counter. Tag verification precedes the counter update, preventing a forged packet with a high number from advancing receive history. Protected application data becomes Ethernet for the receive callback. EAPOL traffic enters the key-exchange handler.

## Kernel network integration

`c/drivers/net/wifi/adapter/netdev.c` connects the station to the kernel network-device interface. After `netdev_init`, a transport owner calls `wifi_adapter_attach`, starts a scan and joins a result. `wifi_adapter_scan_results` copies the BSS table, while `wifi_adapter_status` reports connection state, disconnect reason and link readiness.

A poll processes up to 16 MPDUs before advancing station deadlines. Received Ethernet frames enter an eight-slot queue. Network-stack delivery occurs after releasing the adapter and station gates because Ethernet input can synchronously transmit a reply. Link loss clears queued frames from the previous session.

The adapter updates `NETIF_F_RUNNING` from authenticated station readiness. Its interface slot persists across detach, and reattachment uses the same name and MAC address. `wifi_adapter_detach` returns zero when transport storage can be released; a busy result of `-2` leaves ownership with the caller. The contracts are in `c/drivers/net/wifi/adapter/netdev.h`.

`c/drivers/net/netdev.c` makes an attached transport primary when the interface table has no primary device. When boot found no NIC, the transport owner drives `wifi_adapter_poll` through connection establishment, then invokes `net_init` from a control path to configure IP. DHCP can wait, so configuration belongs outside transport callbacks. Subsequent network receive polling reaches the adapter through `netdev_rx_poll`; see `c/net/core/net.c`.

### Address acquisition and link changes

`net_init` delegates address selection to `net_cfg_acquire` in `c/net/core/netcfg.c`. A static configuration selected through the `net.dhcp` setting with value zero takes precedence. Automatic configuration starts DHCP. Following an unsuccessful initial DHCP attempt, a detected hypervisor selects the SLIRP fallback configuration. On physical hardware, the initial address stays `0.0.0.0` while the client continues acquisition.

`netdev_link_up` in `c/drivers/net/netdev.c` reads the primary interface’s `NETIF_F_RUNNING` flag under the network lock. When Wi-Fi is primary, DHCP follows authenticated station readiness. `dhcp_run` in `c/net/core/dhcp.c` polls for link readiness and shares one four-second monotonic budget between that wait and the DHCP exchange. The exchange receives up to three seconds, capped by the remaining budget.

`net_poll` calls `dhcp_poll` to advance the background client. DISCOVER retries use nominal delays of four, eight, 16, 32 and then 64 seconds, with random jitter of up to one second in either direction. Requests and renewal have their own retry rules. Background retry scheduling pauses while the link is down. On link recovery, a client holding a lease requests confirmation through INIT-REBOOT; a client acquiring its first lease sends DISCOVER.

A DHCPNAK matching the transaction and client MAC, received while a request is outstanding, clears the IPv4 address, mask, gateway, DNS address and primary interface routes before restarting DISCOVER. These transitions are implemented in `c/net/core/dhcp.c`.

## Connection loss and cleanup

`wifi_station_tick` checks deadlines and detects time moving backwards. Once the link is active, five seconds from connection establishment or the most recent matching AP beacon causes beacon-loss failure. Pending authentication, association and pairwise key exchange also expire through this function.

Deauthentication or disassociation addressed to the station from the selected AP, and reported transport loss, enter the failure path. `wifi_station_fail` clears link readiness and erases the PMK, installed and candidate PTKs, group keys and nonces. `wifi_station_disconnect` performs cleanup, records a requested disconnect and returns to `WIFI_IDLE`. Recovery proceeds through another scan and join. These lifecycle operations are in `c/drivers/net/wifi/station.c`.

## Validation

`tests/drivers/wifi/oracle.py` generates AP fixtures with Python `hashlib` and `cryptography`. It supplies expected derivation results, EAPOL messages, wrapped keys and CCMP ciphertext. Its literal wire offsets and selectors help expose format errors shared by production constants.

`tests/drivers/wifi/station_test.c` checks handshake replies, pairwise and group rekey, retransmission, replay rejection, failed submissions, timeouts and secret erasure. `tests/drivers/wifi/adapter_test.c` includes the production network-device implementation through its fixture and checks Ethernet delivery, synchronous replies, link readiness and detach ownership.

In `tests/drivers/wifi/tests.mk`, `test-wifi-station-host` requires controls that bypass CCMP tag verification and reset GTK replay counters. `test-wifi-adapter-host` also requires the fabricated-link control.

The retained `docs/reports/drivers/expansion/2026-09-15/wifi-host.log` records 312 station checks and 53 adapter checks with zero failures for that run.

## Limitations

- Physical radio transport remains unimplemented. `c/drivers/net/wifi/README.md` identifies Intel AX200/AX210 firmware boot, DMA queues, firmware commands and calibrated channels as remaining work. Recorded Wi-Fi validation covers host fixtures; physical association and a guest Internet connection remain unverified.
- The security profile is infrastructure WPA2-PSK with CCMP-128. AP mode, WEP, TKIP, WPA3/SAE, fast transition, protected management frames and roaming are unsupported. Networks requiring protected management frames are rejected.
- Scanning is passive and requires an advertised SSID of nonzero length. Active probe transmission is unimplemented.
- Fragmented frames are rejected. The data receive path also rejects aggregated MSDUs and frames with the Order bit set.
- The kernel adapter supports one radio because the network-device callbacks have no context argument.
- `TRANSPARENCY.md` describes the cryptographic code as educational project code without certification or an independent production security audit.

## See also

- [Driver overview](overview.md)
- [Ethernet drivers](network.md)
- [Network stack](../architecture/network-stack.md)
- [Cryptography overview](../crypto/overview.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
| --- | --- |
| `c/drivers/net/wifi/station.h` | Public station API, state, scan records, keys and transport contract |
| `c/drivers/net/wifi/station.c` | Initialisation, deadlines, link queries and session cleanup |
| `c/drivers/net/wifi/internal.h` | Station gate, buffer ownership and internal helper contracts |
| `c/drivers/net/wifi/protocol.h` | Wire fields, RSN selection and protocol constants |
| `c/drivers/net/wifi/management/scan.c` | Passive scanning and BSS parsing |
| `c/drivers/net/wifi/management/association.c` | Join, authentication and association requests |
| `c/drivers/net/wifi/management/receive.c` | Frame dispatch and management response transitions |
| `c/drivers/net/wifi/security/rsn.c` | Accepted cipher and authentication profile |
| `c/drivers/net/wifi/security/derivation.c` | HMAC-SHA1, PBKDF2 and PTK derivation |
| `c/drivers/net/wifi/security/internal.h` | Cryptographic parameters and secret-erasure helper |
| `c/drivers/net/wifi/security/handshake.c` | EAPOL validation, pairwise handshake and retransmission |
| `c/drivers/net/wifi/security/keywrap.c` | AES key unwrapping and integrity checking |
| `c/drivers/net/wifi/security/keydata.c` | Authenticated RSN and GTK parsing |
| `c/drivers/net/wifi/security/group.c` | Group rotation and replay-counter preservation |
| `c/drivers/net/wifi/security/ccmp.c` | CCM encryption and authenticated decryption |
| `c/drivers/net/wifi/data/frame.c` | CCMP framing, replay checks and Ethernet conversion |
| `c/drivers/net/wifi/adapter/netdev.h` | Radio transport and adapter ownership contracts |
| `c/drivers/net/wifi/adapter/netdev.c` | Polling, Ethernet queue, link updates and detach |
| `c/drivers/net/netdev.h` | Network-device callbacks and interface flags |
| `c/drivers/net/netdev.c` | Transport registration, link queries and network-device dispatch |
| `c/net/core/net.c` | IP initialisation and receive polling |
| `c/net/core/netcfg.c` | Static settings, DHCP acquisition and hypervisor fallback policy |
| `c/net/core/dhcp.c` | Link tracking, initial deadline, background acquisition and lease handling |
| `tests/drivers/wifi/oracle.py` | AP and cryptographic fixtures |
| `tests/drivers/wifi/station_test.c` | Station and cryptographic checks |
| `tests/drivers/wifi/adapter_test.c` | Adapter integration checks |
| `tests/drivers/wifi/tests.mk` | Host targets and prerequisite negative controls |
| `c/drivers/net/wifi/README.md` | Integration guidance and hardware support status |
| `docs/reports/drivers/expansion/2026-09-15/wifi-host.log` | Retained host validation results |
| `TRANSPARENCY.md` | Project claim rules and security scope |