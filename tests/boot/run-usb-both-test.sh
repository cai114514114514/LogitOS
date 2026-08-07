#!/usr/bin/env bash
# COEXISTENCE: a machine with BOTH input stacks live at the same time.
#
# An xHCI controller with a usb-kbd and a usb-mouse, AND the PS/2 controller.
# Both drivers are bound, both are armed, and both feed the same window-manager
# input ring (wm_key / wm_mouse_event, which only enqueue -- that is what makes
# a second producer safe at all).
#
# The requirement is not "input works". It is that every physical event reaches
# the application EXACTLY ONCE. Two input stacks that both claimed the same
# keystroke would show up as every character arriving twice, which is precisely
# what a USB driver that does not disable firmware's PS/2 emulation produces on
# real hardware -- and which no "does input work?" test would ever catch.
#
# The counting lives in tests/qmp/qmp_ps2_only.py --with-usb; it types three
# distinct characters and requires one delivery each, and clicks once and
# requires one press.

set -u
ISO="${1:?usage: run-usb-both-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-usb-both-test.sh <iso> <disk.img>}"
exec python3 "$(dirname "$0")/../qmp/qmp_ps2_only.py" "$ISO" "$DISK" --with-usb
