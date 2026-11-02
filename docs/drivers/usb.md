# USB

LogitOS provides a shared USB core, EHCI and xHCI host controllers, and class drivers for hubs, HID input and mass storage. The core reads descriptors, tracks topology and binds interfaces. Controller backends manage device addresses, endpoints, DMA and transfer completion. The implementation lives in `c/drivers/usb/`.

## Stack organisation

The contracts in `c/drivers/usb/usb.h` and `c/drivers/usb/usb_hc.h` divide responsibility across three layers.

| Layer | Responsibility |
|---|---|
| Host controller | Manage root ports, address devices, configure endpoints and execute transfers |
| USB core | Enumerate devices, retain topology, bind interfaces and coordinate removal |
| Class driver | Interpret interface protocols and connect devices to kernel services |

`struct usb_hc_ops` supplies the controller operations. Control and bulk transfers return a transferred byte count or an error. Interrupt input uses an arm operation followed by completion retrieval.

`struct usb_device` records the controller, parent hub, root port, downstream port, speed and descriptors. Each interface has its own driver binding and private state, allowing composite devices to expose several functions.

Class drivers register a `struct usb_driver` with an interface match table and callbacks for probing, polling and removal. An optional `service` callback runs on the core worker and returns the delay before its next invocation.

In `c/drivers/usb/usb_bind.c`, `usb_bind_interfaces()` matches interface class, subclass and protocol. Endpoint configuration precedes HID, storage and xHCI hub probes. EHCI hubs configure their status endpoint inside the hub probe, after establishing hub state. A declined probe lets the binder try another candidate and continue with sibling interfaces. Successful bindings remain active.

## Host controllers

EHCI registers its PCI driver in `c/drivers/usb/ehci.c`; the xHCI adapter registers in `c/drivers/usb/usb_core.c`. Both use `DRIVER_DECLARE`.

| Controller | PCI class | Subclass | Programming interface |
|---|---|---|---|
| EHCI | `0x0c` | `0x03` | `0x20` |
| xHCI | `0x0c` | `0x03` | `0x30` |

After hardware initialisation, each probe calls `usb_hc_register()`. Registration installs the built-in class drivers once, enumerates connected root ports, requests interrupt delivery and installs the root port watcher.

### EHCI schedules

`c/drivers/usb/ehci.c` uses an asynchronous schedule for control and bulk transfers and a periodic schedule for interrupt input. Queue heads and transfer descriptors are defined in `c/drivers/usb/ehci_hw.h`.

Control and bulk transfers share a serialised queue and a DMA buffer below 4 GiB. The backend copies payloads between this buffer and caller memory. After a transfer completes or times out, it waits for the asynchronous schedule to stop before reading the result or reusing descriptors.

Earlier documentation described whole-frame reservations and stopping the periodic schedule on every rearm. The current scheduler accounts for bus time over an eight-frame window and links queue heads into a tree with shared tails. Its frame list has 1,024 entries, and admission permits up to 100 microseconds of periodic traffic in each 125-microsecond microframe. `c/drivers/usb/ehci_sched.h` also accounts for downstream transaction-translator time. A hub with one translator shares its budget across ports; a hub with multiple translators accounts for each port.

Rearming fills an inactive dummy transfer descriptor, publishes its successor and buffer pointers, then sets Active after a DMA write barrier. The queue head carries the hardware data toggle across completions. The progress timer requests a 1 ms period; callbacks are delivered on the current 100 Hz PIT tick, giving a 10 ms cadence.

Transfer errors unlink the affected queue head while retaining its bandwidth reservation. After a grace period of at least 1,125 microseconds, recovery refreshes the overlay and relinks it. A STALL adds a worker step that clears the endpoint halt and restores DATA0. Clear-halt recovery allows three attempts, with a 100 ms delay before each attempt. Interrupt and timer callbacks advance recovery through timestamps; the worker performs the control request.

For low and full speed devices beneath a high speed hub, the queue head carries the translator hub’s USB address and the downstream port serving the device’s branch.

### xHCI rings and contexts

`c/drivers/usb/xhci.c` uses a command ring, an event ring and endpoint transfer rings. Device addressing allocates a slot, prepares contexts and submits an Address Device command. The controller writes the assigned USB address into the output device context.

Ring arithmetic lives in `c/drivers/usb/xhci_ring.c`. Command and transfer rings wrap through a Link TRB; the event ring wraps at its segment boundary. Cycle bits identify entries ready for the consumer. Host fixtures compile the same ring implementation.

DMA allocations retain both a CPU pointer and a device address. The controller’s address capability selects a 32-bit or 64-bit DMA mask. Ring and context allocations use page alignment and a 64 KiB boundary constraint.

Synchronous command and transfer waits drain the event ring, allowing enumeration to progress before controller interrupts are enabled. During operation, the core’s interrupt and timer paths also drain events and dispatch class callbacks.

### Firmware ownership and DMA lifetime

Both backends disable PCI bus mastering and check the register readback before taking firmware ownership. When a legacy USB capability is present, they request OS ownership and wait for firmware release. Successful handoff disables legacy USB SMI delivery. The xHCI source explains how this stops firmware keyboard emulation from feeding the PS/2 input path alongside USB input.

An older EHCI handoff comment says BIOS release is never forced. The current timeout path waits up to one second, also bounded by a spin count, then clears the BIOS semaphore and checks its release. It disables legacy SMI delivery and ends capability traversal at the legacy-support entry. `c/drivers/usb/ehci.c` records the firmware behaviour that prompted these changes.

Shutdown releases DMA allocations after controller stop acknowledgement and PCI bus-master isolation succeed. Failed checks quarantine the allocations because hardware may still retain their addresses.

Device removal follows backend retirement rules. xHCI releases a slot’s DMA allocations after a successful Disable Slot command. EHCI keeps endpoint DMA allocations in the controller ledger until a successful controller shutdown.

## Enumeration and binding

Root devices and hub children share the descriptor sequence in `c/drivers/usb/usb_core.c`. Their port reset and topology setup precede these steps:

1. Open and address the device through the backend.
2. Read the first eight device-descriptor bytes.
3. Adjust endpoint zero’s packet size for a full speed device when required.
4. Read and parse the complete device descriptor.
5. Read the configuration header, then fetch the configuration using its reported total length.
6. Select the configuration value and bind matching interfaces.

The configuration header determines the size of the second configuration request. `c/drivers/usb/usb_desc.c` checks descriptor lengths and buffer bounds before recording interfaces and endpoints. Rejection paths include truncated configurations, descriptor lengths below two bytes, endpoint zero in an endpoint descriptor and zero endpoint packet sizes.

The core publishes callback readiness after binding. Serial records distinguish descriptor discovery from interface activation, which helps diagnose a device that enumerates successfully but fails to bind a class driver.

## Hubs and topology

`c/drivers/usb/usb_hub.c` uses USB 2 hub-class requests. A probe reads the hub descriptor, configures the backend’s hub state, powers downstream ports and waits for power stabilisation. Each connected port is reset, checked for enable and given recovery time. The driver then determines its speed and calls `usb_enumerate_child()`.

On EHCI, the probe configures and arms the hub’s interrupt status endpoint before discovering children. This gives the hub an early scheduling phase. Status endpoint admission is advisory, so initial discovery can proceed through control requests. The hub poll callback logs nonzero change-bitmap bytes and rearms the endpoint.

The core records topology before addressing each child. Its xHCI route string encodes downstream hops. Transaction-translator state identifies the nearest high speed hub and the port leading into the child’s branch.

Children beneath an intervening full speed hub inherit that translator port. This preserves the upstream branch even when the child’s immediate port number differs. EHCI obtains the translator’s USB address from the retained hub device; xHCI obtains its slot ID.

Discovery runs whenever a hub is probed, including when a hub arrives through root port attachment.

## HID input

`c/drivers/usb/usb_hid.c` matches HID interfaces and reads their declared report descriptors. `c/drivers/usb/hid_report.c` parses fields and decodes keyboard arrays, keyboard bitmaps, report IDs, and relative or absolute pointer axes.

The field table in `c/drivers/usb/hid_report.h` grew from 96 to 320 entries to accommodate keyboards that describe keys as individual bitmap fields. `tests/unit/usb_hid_test.c` includes a descriptor with 240 fields and checks modifier and key decoding.

### Protocol selection and bring-up

The class driver derives keyboard, pointer, gamepad and touchpad roles from parsed fields. Boot fallback selects a fixed keyboard or mouse layout after a successful request for boot protocol.

Earlier documentation described a report-protocol request for every parsed interface supporting boot protocol. Parsed keyboards now rely on the report protocol selected at reset. Parsed interfaces supporting boot protocol whose roles exclude keyboard still receive the report-protocol request.

Keyboard bring-up sends an LED Output report with a zero body when a suitable report is available. The parsed path selects it from the descriptor; boot fallback uses a one-byte report. The source documents keyboards that wait for LED initialisation before producing input.

The probe requests keyboard idle reports every 24 units of 4 ms, or 96 ms. Other roles receive an idle duration of zero. After arming interrupt input, it reads GET_CONFIGURATION and records the result for diagnostics.

### Event delivery

Keyboard handling compares held-key sets and posts newly pressed mapped keys through `wm_key()`. Modifier and held-key snapshots support concurrent queries from `c/drivers/char/keyboard.c`. Repeated idle reports drive key repeat.

Pointer handling integrates relative motion or scales absolute coordinates using descriptor ranges. It delivers screen coordinates, button levels and wheel movement through `wm_mouse_event()`. Both window-manager entry points enqueue input in `c/kernel/gui/wm.c`.

`c/drivers/usb/hid/generic.c` maps gamepad directions to arrow keys and buttons one through four to Space, Enter, Escape and Tab. Its touchpad path converts one-finger motion into pointer movement and two-finger vertical motion into wheel events.

When a touchpad declares an input-mode feature, `c/drivers/usb/hid/feature.c` checks contact capacity, reads the feature report, changes the mode field, writes it and confirms the selected mode by reading it back.

The HID poll callback retrieves completed interrupt input, runs the applicable decoders and rearms the endpoint. Removal clears held input snapshots and releases interface allocations.

### Control input fallback

For a keyboard-role interface declaring the boot keyboard subclass and protocol, three seconds of interrupt silence enables a GET_REPORT probe through the control pipe. Report IDs and lengths come from keyboard input fields, or from the fixed eight-byte boot layout.

A decodable response activates polling. The `service` callback requests another pass after 16 ms; the core queues it on a watcher tick after the deadline. Changed reports enter the keyboard decoder immediately. Unchanged reports containing nonzero bytes are fed at intervals of at least 96 ms to drive repeat handling.

An interrupt report on the same interface retires the fallback in the input callback. A service pass also retires it when a bound HID sibling on the device has delivered an interrupt report. The first failed or undecodable GET_REPORT ends the fallback; an active fallback clears its held keys before entering the failed state.

The worker takes the controller callback lock when feeding keyboard state. It checks fallback state again after each successful transfer so an interrupt report received during the transfer takes precedence over the control response.

## Mass storage

`c/drivers/usb/usb_storage.c` matches this interface triple:

| Class | Subclass | Protocol | Transport |
|---|---|---|---|
| `0x08` | `0x06` | `0x50` | SCSI transparent commands over Bulk-Only Transport, or BOT |

The probe requires one bulk IN endpoint and one bulk OUT endpoint.

`c/drivers/usb/usb_bot.c` implements the command wrapper, optional data phase and status wrapper. It validates status length, signature, command tag, status code and residue. Transport recovery issues the BOT reset request and clears both endpoint halts. After the USB halt-clear request succeeds, `usb_clear_halt()` invokes the backend operation that restores endpoint transport state.

Discovery queries logical units, sends INQUIRY and TEST UNIT READY, and reads capacity. Each successfully discovered medium becomes a block device such as `usb0` through `blk_register()`, followed by `blk_probe_partitions()`.

Reads and writes select SCSI command descriptors of 10 or 16 bytes according to the requested range. Requests are split into chunks of at most 64 sectors. Flush sends SYNCHRONIZE CACHE with IMMED clear and waits for the command result.

A shared gate serialises complete BOT commands across an interface’s logical units, including sense retrieval and transport recovery. Discovery retries selected transient sense conditions. Read and write failures propagate to the caller.

## Root port changes and removal

In `c/drivers/usb/usb_core.c`, interrupt handling records completions, dispatches class callbacks and queues root port work. A watcher requested every 10 ms also checks for changes. The worker performs enumeration and removal in a context that can wait.

The same watcher queues class service work when a requested deadline arrives. The service worker holds the enumeration domain and a controller reference across callbacks, keeping device state valid during control transfers.

Connection-change bits authorise replacement or removal. Checking these bits distinguishes cable transitions from port events generated by reset activity.

On disconnect, the core withdraws callback readiness while holding the controller callback lock. It removes descendants before their hub, retaining topology and translator state until children have closed.

Controller unregistration closes transfer admission and drains active users before removing class state and shutting down the backend. Mass-storage removal marks block devices offline, drains their operations and clears the USB pointer under the BOT gate. Storage contexts remain allocated because callers may retain block registry entries.

## Diagnostics and test coverage

Serial markers include `USB_DEV`, `USB_IF`, `USB_HID_BIND`, `USB_HUB` and `USB_HOTPLUG`. Storage discovery reports disk name, logical unit and capacity.

EHCI emits `EHCI_EP` records for endpoint publication, first completion and errors. `EHCI_PERIODIC` reports scheduler counters after eight successful interrupt transfers. An armed endpoint with no successful completion after three seconds produces an observational `EHCI_STALLED` record with overlay state and frame-index progress. `EHCI_IRQ` pairs report counts with dispatcher delivery sampled from the current INTx binding.

The `make HWHUD=1` build option enables the hardware panel in `c/kernel/diag/hwhud.c`. Static HID diagnostic slots retain bring-up request outcomes and input counters. EHCI snapshots expose scheduling masks, phase, error counters and queue-head state. `ehci_diag_get()` samples DMA under the controller gate; its reach count reports how many frame-list entries lead to an endpoint’s queue head.

The tree defines these focused targets:

| Target | Scope asserted by the fixtures |
|---|---|
| `test-usb-host` | Ring arithmetic, descriptor parsing and HID decoding |
| `test-usb-os` | USB keyboard and mouse events reaching an application in QEMU with the PS/2 controller absent |
| `test-usb-hid-generic` | Class binding, gamepad input, touchpad processing, feature negotiation and removal |
| `test-usb-hub-host`, `test-usb-hub-guest` | Hub topology, translator state and downstream input |
| `test-ehci-host`, `test-ehci-hid`, `test-ehci-multi` | EHCI controller behaviour and input through one or two controllers |
| `test-usb-storage-host`, `test-usb-storage-dual` | BOT validation, block operations, flush and persistence across two QEMU cold boots |
| `test-usb-hotplug` | Storage attachment and removal after boot through EHCI and xHCI |

The EHCI guest runner checks interrupt delivery for every controller’s final binding and tracks vector sharing across the boot. Its assertions require exclusive ownership of the vector used as delivery evidence.

## Limitations

- The core has capacity for eight controllers and 32 devices. The backends provide one xHCI instance and four EHCI controller slots. UHCI and OHCI backends are absent; EHCI root ports require high speed devices.
- Hub children are discovered during probing. Runtime downstream enumeration and removal, and SuperSpeed hub requests, are unimplemented. Topology depth is bounded to five downstream hops.
- Isochronous transfers are unimplemented. EHCI also lacks high-bandwidth interrupt transfers, companion-controller routing and suspend support. Each EHCI controller has 32 endpoint slots and eight transaction-translator accounting slots.
- Enumeration reads configuration descriptor index zero and retains alternate setting zero. Configuration data is bounded to 4,096 bytes, with eight interfaces and eight endpoints per interface.
- HID input reports must fit one interrupt packet. The parser has bounded field and report tables, and keyboard decoding accepts up to 32 held keys. Touchpad processing supports parallel contact frames; hybrid contact-frame assembly is absent. LED initialisation and keyboard control polling accept report bodies, including any report ID, of at most 64 bytes.
- HID control polling shares the core worker with hotplug. EHCI asynchronous transfers use a three-second completion deadline and a spin bound, with additional schedule-stop and recovery work. Slow requests delay other work on that worker.
- Storage supports BOT direct-access media with 512-byte logical blocks. UAS and optical media fall outside its match and discovery paths. The 16 storage-interface contexts remain reserved for the boot lifetime.
- Fixture assertions apply to their named environments. Physical hardware compatibility requires device and environment evidence under `TRANSPARENCY.md`. Source comments mark the EHCI scheduler, hub status monitoring and HID bring-up changes as unverified on X79-VG4 hardware.

## See also

- [Driver overview](overview.md)
- [Device and driver model](../architecture/device-model.md)
- [Storage stack](../architecture/storage-stack.md)
- [Text and input](../architecture/text-and-input.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/usb/usb.h` | Device topology, interface bindings and class-driver contract |
| `c/drivers/usb/usb_hc.h` | Controller operations and class-facing transfer APIs |
| `c/drivers/usb/usb_core.c` | Enumeration, root port changes, service work, removal and xHCI PCI registration |
| `c/drivers/usb/usb_bind.c` | Interface matching and callback dispatch |
| `c/drivers/usb/usb_desc.h` | Descriptor structures and interface limits |
| `c/drivers/usb/usb_desc.c` | Device and configuration descriptor parsing |
| `c/drivers/usb/ehci.c` | EHCI ownership, schedules, transfers, recovery and shutdown |
| `c/drivers/usb/ehci_hw.h` | Queue-head and transfer-descriptor layouts and helpers |
| `c/drivers/usb/ehci_sched.h` | Bandwidth admission, translator accounting and periodic schedule tree |
| `c/drivers/usb/ehci_diag.h` | EHCI endpoint and scheduler diagnostic records |
| `c/drivers/usb/xhci.c` | xHCI ownership, contexts, transfers, events and DMA retirement |
| `c/drivers/usb/xhci_ring.c` | TRB publication, cycle handling and ring wraparound |
| `c/drivers/usb/usb_hub.c` | Hub probing, status monitoring and downstream enumeration |
| `c/drivers/usb/usb_hid.c` | HID bring-up, input delivery, control polling and removal |
| `c/drivers/usb/hid_report.c` | Report parsing and keyboard/pointer decoding |
| `c/drivers/usb/hid_report.h` | HID field structures and parser/decoder capacities |
| `c/drivers/usb/hid_diag.h` | HID request outcomes, input counters and fallback state |
| `c/drivers/usb/hid/generic.c` | Gamepad mapping and touch-contact processing |
| `c/drivers/usb/hid/feature.c` | Touchpad feature negotiation |
| `c/drivers/usb/usb_storage.c` | SCSI discovery, block operations and medium removal |
| `c/drivers/usb/usb_bot.c` | BOT framing, validation, recovery and SCSI helpers |
| `c/drivers/block/blkdev.c` | Block registration, partition publication and offline handling |
| `c/drivers/char/keyboard.c` | USB modifier and held-key queries |
| `c/kernel/gui/wm.c` | Keyboard and pointer event entry points |
| `c/drivers/timer/pit.h` | PIT frequency and timer granularity |
| `c/kernel/core/ktime.c` | Timer deadlines and callback dispatch |
| `c/kernel/diag/hwhud.c` | Hardware panel presentation of USB diagnostics |
| `Makefile` | Hardware panel build option |
| `tests/usb.mk` | Descriptor, ring, HID and QEMU input targets |
| `tests/unit/usb_hid_test.c` | HID parser and decoder fixtures, including the 240-field keyboard |
| `tests/usb_hub.mk` | Hub and xHCI transfer/lifecycle targets |
| `tests/ehci.mk` | EHCI host and QEMU targets |
| `tests/boot/run-ehci-hid-test.py` | EHCI input and final interrupt-binding assertions |
| `tests/usb_storage.mk` | Storage protocol and persistence targets |
| `tests/usb_hotplug.mk` | Root port attachment and removal targets |
| `tests/drivers/usb/hid/generic.mk` | Generic HID fixtures and negative controls |