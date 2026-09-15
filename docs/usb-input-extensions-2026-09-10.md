# USB input coverage, 2026-09-10

This change extends the generic USB HID path used by physical devices. It does
not add a host controller driver. UHCI, OHCI and EHCI-only computers remain
unsupported; USB hubs and device hotplug are also outside this change.

## Behavior

| Device shape | Previous behavior | Current behavior |
| --- | --- | --- |
| Composite keyboard/mouse, separate interfaces | Only the first interface bound | Each interface owns its own driver state, endpoint polling and teardown |
| Keyboard/mouse sharing one report-ID interface | The keyboard role suppressed mouse decoding | Both decoders receive each report and accept only their own fields |
| NKRO variable-bit keyboard | Non-modifier bits were ignored | Pressed keyboard usages are collected, bounded to 32 simultaneous keys |
| Absolute USB tablet | Coordinates were added as relative deltas | Each axis uses its descriptor's logical range and Absolute/Relative bit |
| USB Shift/Ctrl/Alt/Super | Key characters could change, while GUI modifier flags stayed PS/2-only | Atomic USB modifier snapshots join the common keyboard state before key/click enqueue |
| Firmware-selected boot protocol | Parsed descriptor was assumed to match the wire | Boot-capable devices explicitly select report protocol; refused mode changes leave the interface unbound |

The HID parser remains bounded to 96 fields, eight report IDs and a 4096-bit
input report. Report descriptors may be fetched up to 4096 bytes. Multitouch,
gestures, consumer/media key actions, keyboard LEDs, GUI key-release events,
and arbitrary gamepad controls are not implemented here. Basic US key mapping
continues to follow the existing desktop convention.

## Evidence

`make BUILD=build-driver-input` produced a linked kernel and bootable ISO.
`make BUILD=build-driver-input test-usb-input-extensions` runs the production
interface binder, HID class driver and descriptor parser against a host
transport fixture. It verifies independent composite interfaces, shared
report-ID roles, modifier publication/removal, independent/nonzero tablet axis
ranges, stable repeated coordinates, NKRO and protocol refusal.

Two production-path negative controls were compiled and observed failing the
same positive assertions:

- First-interface-only binding: `FAIL both composite interfaces bind: got 1, want 2`.
- Absolute coordinates treated as deltas: `FAIL tablet X range normalized: got 1279, want 639`.

They are prerequisites of the positive host target. Logs are written to
`build-driver-input/usb-input-first-negctl.log` and
`build-driver-input/usb-input-abs-negctl.log`.

The guest command was:

```sh
python3 tests/boot/run-usb-tablet-test.py build-driver-input/logit.iso build/disk.img
```

QEMU had **no i8042/PS/2 controller**, with `qemu-xhci`, `usb-kbd` and
`usb-tablet`. A ring-3 `events.as` application received pointer coordinates
`(512,291)` then `(622,342)`: observed delta `(110,51)` for requested screen
delta `(110,50)`, within the asserted one-pixel quantization tolerance. The
shifted key arrived as `EV 1 65 0 1 0 0`; click and release both kept
`(622,342)` with Shift set; releasing Shift produced `EV 1 97 0 0 0 0` once.
Command, serial and QEMU error logs are in
`build-driver-input/usb-tablet-evidence/`.

The established relative USB guest regression also passed:

```sh
bash tests/boot/run-usb-test.sh build-driver-input/logit.iso build/disk.img
```

It observed both descriptors, MSI-X delivery, eight repeated `k` events during
a hold, shifted `A`, relative movement including downward Y, button edges and
the existing positive-down/negative-up wheel convention.

These are QEMU hardware-emulation measurements. The composite and NKRO cases
have production-path **host fixture** coverage only. No physical computer,
wireless receiver, tablet or NKRO keyboard has been exercised, so no make/model
compatibility claim follows from this evidence.

## References

- [USB-IF HID 1.11 specification](https://www.usb.org/sites/default/files/hid1_11.pdf): sections 6.2.2.5 (Input flags), 6.2.2.7 (report IDs and logical ranges), 7.2.6 (Set Protocol), Appendix B (boot formats).
- [USB-IF USB 2.0 documents](https://www.usb.org/document-library/usb-20-specification): section 9.6.5, interface descriptor and interface numbering.
- [QEMU USB HID implementation](https://github.com/qemu/qemu/blob/master/hw/usb/dev-hid.c): descriptor-backed keyboard/mouse/tablet emulator used for guest validation.
