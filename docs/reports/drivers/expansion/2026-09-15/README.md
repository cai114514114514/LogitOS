# Driver expansion: USB, Wi-Fi, ACPI EC and Polaris resources

This round extends four existing driver paths. Implementation is divided by
responsibility in `usb/hid`, `net/wifi/{management,security,data,adapter}`,
`power/acpi/ec` and `gpu/amd/polaris/resource`. No physical machine was available.

## Implemented behavior

### USB touchpad configuration

The production USB HID probe now reads descriptor-defined Feature reports,
checks full parallel contact capacity, preserves unrelated report bits while
selecting touchpad Input Mode, and reads the mode back before arming input.
Input/Output/Feature bit cursors are independent. Vendor certification fields
retain their report extent without exhausting the actionable-field table.
Short reports, wrong IDs, stalled/ignored mode changes and unsupported contact
capacity are refused. Hybrid contact-frame assembly and I2C-HID remain absent.

### Wi-Fi connection lifecycle and kernel adapter

The WPA2 station supports pairwise/group rekey, retransmission without key
reinstallation, preserved replay windows, beacon timeout and transport loss.
The kernel netdev adapter consumes real station Ethernet frames, distinguishes
registration from authenticated link readiness, and stops old transport callbacks
after detach. The single-radio transport owner must still implement actual
receive/transmit, tuning, random bytes and timing; AX200/AX210 firmware boot,
DMA queues, radio calibration and physical association are not implemented.

### ACPI Embedded Controller

The EC implementation discovers declared controller ports and provides bounded,
serialized read/write/query transactions to real uACPI OperationRegions.
Timeouts quarantine the binding rather than blindly replaying writes. The AML
tests exercise EC-backed battery and thermal fields and deferred query methods.
Discovery starts in a dedicated thread after scheduler initialization and WM
lock release. Boot diagnostics expose EC binding count and errors separately
from general AML readiness. `_REG` precedes `_INI`; GPE delivery and pending
query processing start only after namespace initialization completes.
Shared-global-lock, GPIO/package-GPE and unsupported port layouts
remain explicit refusals; suspend/resume and CPU power-state programming are
not added in this round.

### RX 580 / Polaris resource discovery

After root VFS mount and device binding, the real boot path revalidates PCI/BAR
state, reads the posted VBIOS copy through BAR0, parses declared ATOM firmware
reservations and reads bounded firmware files from `/lib/firmware/amdgpu`.
Each security-key candidate has separate file-format results. Format validation
does not authenticate firmware. This round does not install firmware on the
shared disk or grant VRAM ownership from the parsed table.

The boot report preserves `ownership-missing` and `security-key-unknown`.
Complete VRAM ownership transfer is still required before the existing SMU/SDMA
runtime can be activated automatically. This is a working resource-discovery
consumer, not proof of RX 580 acceleration, shaders or performance.

## Validation

All commands below exited successfully. Host fixtures validate protocol and
state behavior; QEMU checks validate emulated devices and boot regressions.
Neither proves physical-board operation.

| Gate | Observed result |
| --- | --- |
| USB generic HID | 95 checks; real class bind/poll/remove and input consumers |
| Existing USB HID/input extensions | Descriptor, keyboard/mouse, composite and absolute tablet regressions passed |
| Wi-Fi station / kernel adapter | 312 / 53 checks; ASan/UBSan |
| Existing netif | 55 checks |
| EC transport / real AML | 51 / 165 checks; ASan/UBSan |
| Existing power AML | 162 checks |
| Polaris resource discovery / boot framebuffer | 4370 / 62 checks; ASan/UBSan |
| Make test wiring | 369 fragments, 368 reachable, 1 documented standalone wrapper |

Watched prerequisite controls: stale touch anchor, omitted touchpad mode request,
bad Wi-Fi MIC (4 failures), GTK reinstallation (2), fabricated link (3), omitted
EC IBF delay (13), late EC handler installation (1), wrong Kelvin conversion (3),
omitted AMD firmware reservation (4), and four separate boot-framebuffer controls
(one failure each). The old netif and USB gates retain their own controls.

The final ISO passed these guest checks:

- USB keyboard and absolute tablet with PS/2 absent: pointer displacement,
  shifted key, click/release and stable coordinates.
- BIOS QEMU ATI VGA: bound driver and retained 1024×768 scanout, 221 sampled
  colours. This model is not a Polaris GPU.
- e1000e: DHCP, two exact 128 KiB HTTP transfers with FNV/SHA256 validation,
  dynamic vector 96 and actual IRQ delivery.
- Three power boots: QEMU exits itself on S5, the file survives byte-for-byte
  without journal replay, and reboot produces two boot banners. The second
  boot makes no power request and now requires successful AML/EC startup logs.
- The preceding ISO, with no startup discovery call, fails both new discovery
  assertions in a retained network-only boot log. The final network-only boot
  reports `AML ready=1 ... error=0` and `EC bound=0 error=0` without a power
  syscall. Zero ECs is the QEMU board's actual inventory, not an EC hardware test.

`artifacts.json` identifies the final ISO, kernel ELF and shared disk input;
`sources.json` records scoped source/test hashes. The shared disk was used only
through snapshots or private copies. `before-boot-discovery/` retains the earlier
ISO identity and serial log for the missing-consumer comparison.

Validation bookkeeping: two overlapping power runs initially shared one output
file; an older run reported no third-boot banner into that mixed output. It is
retained as `power-confounded-attempt.log` and is not acceptance evidence. Both
processes ended before the final independent run, whose successful three boots
are retained in `power-guest.log` and `power-b1/b2/b3.log`.

The tested ISO is pinned by hash. After its successful build, another task edited
four OpenLogit source files (`core/openlogit.c`, `canvas/state.c`,
`canvas/sprites.c`, `scene/scene2d.c`). Those later edits are outside this round's
source manifest and were not folded into or claimed as tested by this ISO.

Reproduction commands (repository root):

```sh
make BUILD=build/drivers/expansion/usb test-usb-hid-generic test-usb-input-extensions test-usb-hid
make BUILD=build/drivers/expansion/verify/wifi test-wifi-adapter-host test-netif
make BUILD=build/drivers/expansion/verify/power test-power-ec-aml test-power-aml-host
make BUILD=build/drivers/expansion/verify/amd test-polaris-resources-host test-amd-bootfb-host
make BUILD=build/drivers/expansion/verify/wired test-mk-wired
make -j4 BUILD=build/drivers/expansion/kernel all
python3 tests/boot/run-usb-tablet-test.py build/drivers/expansion/kernel/logit.iso build/disk.img
python3 tests/boot/run-amd-bootfb.py --firmware bios --iso build/drivers/expansion/kernel/logit.iso --out build/drivers/expansion/kernel/amd-bootfb-guest
python3 tests/boot/run-pcnet-test.py --self-test
python3 tests/boot/run-pcnet-test.py --iso build/drivers/expansion/kernel/logit.iso --disk build/disk.img --output build/drivers/expansion/kernel/e1000e-guest --device e1000e --driver e1000e
PWTEST_KEEP_WORK=1 bash tests/boot/run-power-test.sh build/drivers/expansion/kernel/logit.iso build/disk.img
```
