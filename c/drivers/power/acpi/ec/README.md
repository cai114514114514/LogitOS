# Embedded Controller driver

This driver adds a real EC OperationRegion backend to uACPI. Byte transactions
use firmware-derived ports and the standard read, write and query commands;
no conventional port pair is assumed.

## Supported binding

Discovery evaluates `PNP0C09` devices. `_STA` must describe a present, enabled,
functioning controller. `_CRS` must contain exactly two one-byte, fixed-location,
16-bit-decoded IO descriptors, in the specified data-then-control order. The
ports must be distinct and must not overlap another EC binding. `_GPE` must be
a global integer GPE with no existing handler, runtime/wake owner or hardware
enable. The EC uses an edge-triggered native uACPI handler.

If ECDT identifies the same namespace node, its byte-wide SystemIO GAS entries,
GPE and available integer `_UID` must agree. Length and path termination are
bounded. An inconsistent ECDT is rejected instead of choosing whichever source
happens to work in the test fixture.

The implemented subset is deliberately specific:

- `_GLK` nonzero is rejected: shared firmware/SMI ownership requires a Global
  Lock transaction protocol that is not implemented here.
- FixedIO and 10-bit IO decoding are rejected because their aliases cannot be
  reserved by this driver's resource model.
- GPIO interrupts, package-valued `_GPE`, already-owned GPEs, SystemMemory EC
  registers and other resource descriptor forms are rejected.
- ECDT is checked against the loaded namespace; early ECDT bootstrap access
  needed while the namespace itself is loading is not implemented.
- Four boot-lifetime controller slots are available. There is no runtime unload
  or platform-wide IO-port allocator; the driver relies on the supported ACPI
  private-resource description and checks collisions between its own bindings.

An unsupported controller leaves an explicit `ec_error` in the power snapshot.
Independent ACPI devices and shutdown remain available. A discovered EC or a
nonzero `ec_count` is not a claim that every battery/thermal method succeeded;
method error fields still govern whether a measurement is usable.

## Ordering and transactions

`power_ec_initialize` runs after namespace loading and before namespace device
initialization. It maps ports, creates the transaction mutex, installs an
initially disabled GPE handler, and installs the EC address-space handler. The
pinned uACPI installation API executes `_REG(EmbeddedControl,1)` at this stage,
so `_INI` may access EC Fields. `power_ec_activate` enables GPE delivery only
after namespace initialization has finished, preventing `_Qxx` from racing
`_INI`. It also schedules any already-latched SCI query rather than relying on
a new interrupt edge that may never arrive.

The interrupt callback only samples status and submits deferred work. Read,
write and query commands serialize through one mutex. Each input write waits
for IBF clear, then gives IBF at least one microsecond to become visible before
checking completion. A read consumes the data port only after OBF is set.
Handshakes have a 250 ms deadline and a finite poll count, including when the
clock stops. Transfers are bounded to eight bytes within the 256-byte EC space.

A query command is sent only for SCI_EVT. A returned zero is a spurious/no-event
result and never invokes `_Q00`. Nonzero results execute the matching `_Qxx` on
the existing sleepable worker. The transaction mutex is released before AML,
because query methods can access EC Fields themselves. The GPE stays disabled
until queued queries finish; query errors, queue exhaustion and query storms
remain visible and leave delivery disabled/masked.

Reads publish output only after the complete transfer. A write may have
completed a prefix when it fails. Timeout, bus failure or unexpected output
quarantines that transport until reboot; it never retries a possibly completed
write. Empty IBF/OBF alone does not prove that the EC command parser has returned
to idle. Published handler contexts and resources therefore have boot lifetime;
an ambiguous partial installation retains them rather than freeing live
OperationRegion or interrupt context.

## Evidence

`make BUILD=build/drivers/expansion/power test-power-ec-aml` runs ASan/UBSan
transport and full-interpreter gates. The separate model uses literal commands
and status bits, delayed IBF assertion and delayed OBF production. It exercises
little-endian accesses, range rejection, partial writes, failed handshakes,
quarantine, a stopped clock and query-zero semantics.

The full ACPI fixture puts the EC at nondefault ports, runs real `_REG` and
`_INI` Field writes, and obtains battery capacity and thermal temperature through
real AML Field reads. Changing model memory changes the reported measurement.
A modeled SCI enters uACPI's real GPE dispatcher, schedules QR_EC, runs `_Q42`,
and observes another EC Field write. A notification present before activation
is also consumed after `_INI`. Refusal cases cover shared ownership, IO aliases,
port overlap, occupied/package GPEs and malformed/mismatching ECDT.

Both negative controls are prerequisites: removing the IBF visibility delay
must produce early writes; moving handler installation after `_INI` must leave
its EC-backed initialization write unexecuted. These are protocol/interpreter
tests, not evidence from an actual notebook EC. The existing QEMU poweroff gate
checks integration on a machine without this physical EC.

Sources checked for this implementation:

- [ACPI 6.5, Embedded Controller Interface, especially 12.3, 12.6, 12.7 and 12.11](https://uefi.org/specs/ACPI/6.5/12_Embedded_Controller_Interface_Specification.html)
- [Linux v6.12 EC driver](https://github.com/torvalds/linux/blob/v6.12/drivers/acpi/ec.c)
- [Pinned uACPI address-space API](https://github.com/uACPI/uACPI/blob/f5f6cc29f85dd5db0dd79c47700c8f3ebce18be4/include/uacpi/opregion.h)
- [Pinned uACPI GPE API](https://github.com/uACPI/uACPI/blob/f5f6cc29f85dd5db0dd79c47700c8f3ebce18be4/include/uacpi/event.h)
