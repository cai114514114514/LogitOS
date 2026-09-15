# Generic USB HID input

`generic.c` is linked into `usb_hid.c`: descriptor-based binding, actual interrupt
polling and interface removal all use this code. It does not need device IDs.

- Joystick/Game Pad Application collections: eight absolute axes normalized to
  -32767..32767, 4/8-way hats with explicit null state, 32 variable buttons.
  Report IDs retain independent snapshots. Existing desktop/game consumers receive
  left-stick/D-pad arrow levels and button 1..4 Space/Enter/Escape/Tab edges.
  Remaining raw axes/buttons are decoded; no userspace raw-gamepad ABI is claimed.
- Touch Pad Application collections: up to ten contacts in a complete parallel
  frame, matched by Finger collection and Contact Identifier. Tip Switch and
  Touch Valid (formerly Confidence) filter contacts. A new finger anchors, one
  continuing finger moves the pointer, two continuing fingers scroll. Three or
  more fingers do not synthesize motion. Physical button levels survive separate
  report IDs. Lift, invalid frames and unplug clear appropriate gesture state.
- Descriptor limits: 96 fields, eight report IDs, 4096 input bits, 16 collection
  levels, eight global Push levels. Input reports must fit one configured USB
  interrupt packet. Full 32-bit usages retain their embedded page.

`feature.c` now performs descriptor-directed Input Mode negotiation for full
parallel touchpads. Input, Output and Feature reports maintain separate bit
cursors. GET_REPORT preserves unrelated bits, SET_REPORT selects mode 3 at the
declared field offset, and a second GET_REPORT confirms the result before the
interrupt endpoint is armed. Short reads, wrong IDs, stalled/ignored requests
and contact capacity exceeding a complete parallel frame refuse binding.
Vendor certification fields retain their report extent without consuming the
bounded table of actionable fields. Feature-only collections do not bind as
input devices. Devices without a mode Feature retain the existing input path.

Scan Time serial/hybrid contact-frame assembly, I2C-HID transport,
XInput/vendor protocols, force feedback, tap-to-click
and raw multitouch delivery to applications remain unsupported. Refusal of an unsupported partial
frame is preferable to combining fingers from different scans into a drag.

Primary protocol references:
[USB-IF HID 1.11, 6.2.2](https://www.usb.org/sites/default/files/documents/hid1_11.pdf)
and [USB-IF Usage Tables 1.3, Generic Desktop and Digitizers](https://www.usb.org/sites/default/files/hut1_3_0.pdf).
The configuration usage and mode values follow the
[Precision Touchpad configuration collection](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchpad-configuration-collection).

Run `make -f tests/drivers/usb/hid/generic.mk BUILD=build/drivers/usb CC=clang test-usb-hid-generic`.
The ASan/UBSan fixture binds and polls the production class driver into WM/held-key
spies; its prerequisite negative control inherits a stale finger anchor and must
fail the real WM position assertion. Host fixtures do not establish physical
controller, gamepad or touchpad compatibility. QEMU tablet/keyboard gates cover
emulated USB transport and existing pointer/keyboard regressions.
