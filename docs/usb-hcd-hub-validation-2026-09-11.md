# USB HCD, hub and xHCI transport validation

The USB class layer now dispatches through `usb_hc_ops`, with a controller on
each device and explicit parent, downstream port, depth, route and nearest
high-speed transaction translator (TT). This allows an xHCI controller and EHCI
controllers to share the same hub, HID and mass-storage class implementations.
The current xHCI backend still has one hardware instance (`g_xhci`); the generic
registry has eight controller slots. This change does not provide multiple
independent xHCI instances.

USB2 hub boot enumeration powers and resets downstream ports, checks status,
waits for recovery, and enumerates connected children before normal operation.
Selected interface protocol determines single/multiple-TT mode. A full/low-speed
device below a high-speed hub retains the nearest TT and its downstream port,
including through an intervening full-speed hub. The xHCI slot context uses the
hub's slot ID; USB CLEAR_TT_BUFFER uses its USB address and clears both directions
for a failed control transaction. SuperSpeed hubs and dynamic hotplug are outside
this implementation.

The xHCI backend implements synchronous bulk transfers with accurate residual
lengths, including zero-byte and short IN transfers. A 32 KiB class request is
staged through eight 4 KiB DMA chunks. Endpoint recovery stops/resets the endpoint,
advances the dequeue past failed TDs and preserves quarantined DMA on an
unconfirmed stop. CLEAR_FEATURE is followed by backend DATA0 restoration; a
healthy endpoint needs Configure Endpoint Drop/Add because Stop Endpoint and Set
TR Dequeue alone preserve its toggle. Control completion retires the complete
Setup/Data/Status TD, and an error on the Data stage is recognized without waiting
for an unexecuted Status stage.

Controller removal closes admission and drains active users, then removes
children before parents. IRQ and periodic-timer callbacks on one controller are
serialized so the same borrowed interrupt buffer cannot be decoded concurrently.
`USB_DEV` retains its original fields and appends `hc`, `parent`, `parent_port`
and `depth`, allowing equal USB addresses on different controllers to be identified.

## Production host tests

Command:

```sh
make BUILD=build-x79-usb test-usb-hub-host test-xhci-xfer-host \
  test-usb-input-extensions test-dma-drivers
```

The hub fixture compiles the real enumeration, hub and class-binding code with
fake HCD/clock/PCI leaves. Its 32 checks pass. The xHCI fixture compiles the real
transport/ring code with simulated MMIO/completions and non-identity DMA; its 21
checks pass. Both run with address/undefined-behavior sanitizers. The existing
HID extension and driver DMA gates also pass.

| Negative control | Observed assertion failures |
| --- | ---: |
| Do not enumerate hub children | 5 |
| Drop inherited high-speed TT topology | 5 |
| Clear only one direction of a control TT transaction | 1 |
| Ignore bulk residual length | 2 |
| Do not retire the whole completed control TD | 3 |
| Omit Set TR Dequeue during recovery | 1 |
| Omit the Drop part of healthy-endpoint DATA0 reset | 1 |

These controls change production behavior while using the same assertions. The
positive and negative results are in `/tmp/x79-usb-final-host.log`; generated
fixtures/results are below `build-x79-usb/{usb-hub,xhci-xfer}-{host,negctl}`.

## QEMU input validation

The final `build-x79-usb/logit.iso` passed three runs with no i8042 controller:

| Topology | Actual guest evidence |
| --- | --- |
| xHCI → full-speed USB hub → keyboard + absolute tablet | Two children, parent address 1 and downstream ports 1/2; ring-3 input, Shift+A, click/release, then unshifted a; observed tablet displacement 110/51 for target 110/50 with one-pixel tolerance |
| xHCI → direct keyboard + absolute tablet | Same input and coordinate checks |
| xHCI → direct keyboard + relative mouse | Real MSI-X vector 97; report-descriptor decoding, autorepeat, both axes, buttons and wheel reach a ring-3 application |

Commands after `make BUILD=build-x79-usb -j6`:

```sh
USB_HUB_TEST=1 python3 tests/boot/run-usb-tablet-test.py \
  build-x79-usb/logit.iso build/disk.img
python3 tests/boot/run-usb-tablet-test.py \
  build-x79-usb/logit.iso build/disk.img
bash tests/boot/run-usb-test.sh build-x79-usb/logit.iso build/disk.img
```

Hub and direct-tablet artifacts are in `build-x79-usb/usb-hub-evidence` and
`build-x79-usb/usb-tablet-evidence`. Terminal results are retained in
`/tmp/x79-usb-{hub,direct,mouse}-final.log`.

QEMU's `usb-hub` model is full-speed USB1.1. That guest verifies actual xHCI hub
routing and downstream input, but cannot verify a high-speed hub's split
transactions or the integrated X79 rate-matching hub. Those topology/TT paths have
production host-fixture evidence here, and need testing on the user's board.
Separate EHCI and USB MSC gates cover their own backend and transport behavior;
this report does not claim physical X79, USB boot/rootfs, UAS, isochronous or
hotplug validation.

## Implementation references

- [Intel C600/X79 chipset datasheet](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/c600-series-chipset-datasheet.pdf): EHCI and integrated rate-matching hubs motivate downstream full/low-speed support.
- [Intel xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf), section 4.6.8.1: halted endpoint recovery, TT cleanup, dequeue updates and healthy-endpoint reinitialization.
- [QEMU USB documentation](https://www.qemu.org/docs/master/system/devices/usb.html) and [QEMU hub implementation](https://github.com/qemu/qemu/blob/master/hw/usb/dev-hub.c): full-speed hub model and descriptor behavior.
- [Linux USB hub implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/core/hub.c): CLEAR_TT_BUFFER direction and single/multiple-TT selection.
