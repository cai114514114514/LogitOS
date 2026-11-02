# Keyboard, mouse, serial and VGA

LogitOS provides PS/2 keyboard and mouse input, COM1 serial communication and VGA text output through `c/drivers/char`. Keyboard and mouse handlers enqueue desktop events for processing by the window manager. The serial console connects UART input to file operations and signal handling. Kernel diagnostic output passes through a shared formatter to VGA, COM1 and the kernel log.

## Driver responsibilities

| Component | Hardware interface | Kernel interface |
|---|---|---|
| Keyboard | Port `0x60`, IRQ1 | Key presses, releases, modifiers and held-key queries |
| Mouse | Ports `0x60` and `0x64`, IRQ12 | Pointer coordinates, three button levels and wheel movement |
| Serial | COM1 at `0x3F8` | Queued transmission and polling for received bytes |
| VGA text | 80 × 25 cells at `0xB8000` | Character output, colors, clearing and scrolling |

Each driver has a matching header in `c/drivers/char`. Desktop input handling lives in `c/kernel/gui/wm.c`; serial console file operations live in `c/kernel/exec/fd/file.c`.

The default `make run` configuration connects COM1 to the host terminal with `-serial stdio` and uses VirtIO GPU for the graphical display. These settings are defined in `Makefile`.

## Initialization and interrupt routing

`kernel_main()` initializes COM1 before printing its opening diagnostics. It then initializes the interrupt descriptor table, remaps the PIC and configures the timer. Later, it initializes and renders the desktop before calling `mouse_init()`. Mouse coordinates use the framebuffer dimensions. This sequence is in `c/kernel/init/kmain.c`.

The PIC configuration enables IRQ0 for the timer, IRQ1 for the keyboard, IRQ12 for the mouse and IRQ2 for the slave-controller cascade. During SMP initialization, the kernel attempts to route the timer, keyboard and mouse through the I/O APIC to the bootstrap processor. Successful routing masks the PIC. A safe routing rejection retains the PIC path; an unsafe routing state halts initialization. See `c/drivers/timer/pic.c` and `c/kernel/cpu/smp/smp.c`.

The I/O APIC driver discovers controllers through ACPI, selects the controller covering each global system interrupt and applies ACPI interrupt source overrides to legacy routes. The routing and verification code is in `c/kernel/cpu/irq/ioapic.c`.

The interrupt dispatcher calls `keyboard_handle()` for IRQ1 and `mouse_handle()` for IRQ12, then acknowledges the selected interrupt controller. Their prefix and packet state belongs to handlers on the bootstrap processor. Keyboard state queries use atomic loads for readers on other CPUs. See `c/kernel/cpu/irq/interrupts.c` and `c/drivers/char/keyboard.c`.

## PS/2 keyboard

`keyboard_handle()` reads one scancode byte from port `0x60`. The decoder uses scancode set 1 and fixed US QWERTY tables. An `0xE0` prefix selects the extended mapping, distinguishing right Ctrl, right Alt, command keys and navigation keys from base scancodes with the same byte values.

The translation rules in `c/drivers/char/keyboard.c` include:

| Input | Result |
|---|---|
| Printable key | Character from the base or Shift table |
| Ctrl with a letter | Control code from 1 through 26 |
| Extended navigation key | ABI navigation key code |
| Forward Delete | `0x7F` |
| Function key | `KEY_F1` through `KEY_F12` |
| Release of a mapped character key | Character from the unshifted table |
| Release of a mapped special key | Its special key code |

Command contributes `EV_MOD_SUPER` to the event modifier mask. Character translation follows the Shift and Ctrl rules.

Releases identify character keys through the unshifted table. A letter pressed as `'A'` with Shift held is released as `'a'`; Ctrl+S produces control code `0x13` on press and `'s'` on release. The event contract is documented in `include/abi/logit_abi.h`.

Modifier tracking assigns a bit to each physical left or right modifier. Holding both Shift keys and releasing one keeps Shift active. `kbd_mods()` combines those states into an event mask and merges the USB HID modifier snapshot. Mouse events sample this combined mask through the window manager. The USB state queries are implemented in `c/drivers/usb/usb_hid.c`.

The decoder records make and break state before translating characters. `kbd_key_held()` checks mapped ASCII keys and extended arrow keys, then consults USB HID state. It also accepts uppercase ASCII letters by converting them to lowercase. This provides continuous key state between keyboard repeats. `WINS_KEY_HELD` exposes the query to the focused application through `c/kernel/gui/wm.c`.

## PS/2 mouse

`mouse_init()` enables the auxiliary controller port, enables its interrupt and clock, restores mouse defaults, negotiates wheel support and starts data reporting. Controller accesses use bounded polling, with pending output drained around configuration changes. The implementation is in `c/drivers/char/mouse.c`.

Wheel negotiation sends sample rates 200, 100 and 80, then requests the device ID. An ID of 3 selects four-byte IntelliMouse packets; the initial packet length is three bytes. The driver then selects a sample rate of 100. Reading the returned ID determines the packet length used by the decoder.

`mouse_handle()` reads one byte per interrupt. At a packet boundary, it accepts a first byte with the synchronization bit set and both overflow bits clear. After collecting a complete packet, it reconstructs signed horizontal and vertical movement, extracts the three button levels and decodes the signed wheel byte when present.

The first complete packet starts from the framebuffer center and applies that packet’s movement. Horizontal movement adds to the screen coordinate; vertical movement is subtracted because PS/2 Y increases upward. Coordinates are clamped to the framebuffer bounds before delivery through `wm_mouse_event()`.

Button levels accompany every packet. The window manager derives press and release transitions and tracks interactions such as dragging. The wheel byte retains its decoded sign; `EV_WHEEL` defines positive movement as scrolling down. See `c/kernel/gui/wm.c` and `include/abi/logit_abi.h`.

## Desktop event delivery

`wm_key()`, `wm_key_up()`, `wm_mods_changed()` and `wm_mouse_event()` capture the current modifier mask and enqueue input. The window manager drains the queue and processes events in thread context alongside its rendering work.

This arrangement puts window manipulation and input-triggered application launches in the window manager thread. Comments in `c/kernel/gui/wm.c` describe the motivating failures: input interrupts could change geometry during rendering or enter allocation and disk paths while interrupting related kernel work.

The queue has 512 slots and holds up to 511 events. It coalesces adjacent pointer motion when button levels are stable and wheel movement is zero. Keys, modifier changes, wheel movement and button transitions preserve ordering boundaries. When full, it can evict an old motion event to admit one of those events. Each drain returns at most 16 events, allowing rendering and other desktop work between batches. See `c/kernel/gui/input/input_queue.c` and `c/kernel/gui/input/input_queue.h`.

For key presses, the window manager handles desktop shortcuts and overlays, resolves keyboard focus and passes application input through the input method. Application delivery uses `EV_KEY`. A window enables releases and modifier-key events with `WINS_WANT_KEYUP`; releases arrive as `EV_KEY_UP`. The definitions are in `include/abi/logit_abi.h`.

## COM1 serial console

`serial_init()` configures COM1 for 115200 baud, eight data bits, no parity and one stop bit, with the FIFO enabled and cleared. Earlier documentation described 38400 baud; the current divisor is 1, replacing the earlier divisor of 3. See `c/drivers/char/serial.c`.

Transmission uses a 65,536-byte RAM ring. `serial_putc()` submits a byte and attempts to drain one burst if the transmitter is ready. Initialization reads the UART’s interrupt-identification register and selects bursts of 16 bytes for working 16550A FIFOs or one byte otherwise. Repeated initialization preserves the software ring.

`serial_tx_tick()` gives queued output another opportunity to leave after producers become idle. It tries the transmit lock and writes at most one burst when the UART is ready. `c/kernel/cpu/irq/interrupts.c` calls it on every core’s timer interrupt.

`serial_tx_stats()` reads cumulative accepted, sent and dropped byte counts, plus ring occupancy and capacity. The sent count advances when bytes are written to the UART transmit register. The fields are read individually with atomic loads. `serial_dropped()` returns the cumulative loss count. These interfaces are declared in `c/drivers/char/serial.h`.

### Receive handling

`serial_getc()` polls the line-status register and returns a byte or `-1` when input is unavailable.

On timer entries that pass the nesting check, the interrupt dispatcher calls `ksig_tick()`. Each call reads up to 16 UART bytes into a ring with 1024 slots and 1023 usable entries. Reading stops when the ring fills. Ctrl+C is consumed and posted as `LOGIT_SIGINT` when a foreground PID has been recorded. This processes console signals while an application is executing between reads. See `c/kernel/cpu/irq/interrupts.c` and `c/kernel/exec/signal/ksignal.c`.

`ksig_tty_getc()` consumes queued input and polls the UART when the queue is empty. It also handles Ctrl+C obtained through that polling path. A console-input lock coordinates readers with the timer’s receive work. Poll waiters are awakened after bytes enter the queue.

### File operations

The serial console file returned by `file_open_tty()` uses the `F_TTY` backend in `c/kernel/exec/fd/file.c`:

| Operation | Behavior |
|---|---|
| Read with positive length | Records the caller as the foreground PID |
| Wait for input | Calls `sched_poll_wait()` and checks for an interrupting signal |
| Successful read | Returns one echoed byte |
| Carriage-return input | Returns newline and echoes carriage return followed by newline |
| Backspace or Delete input | Echoes backspace, space, backspace and returns a backspace byte |
| Write | Expands newline to carriage return followed by newline |
| Poll | Registers on the console wait queue, reports queued input as readable and output as writable |

Console writes acquire the kernel console message lock for each line or chunk of up to 160 output bytes. An expanded carriage-return/newline pair counts as two bytes and stays within one chunk. A final partial line is submitted immediately, keeping prompts visible. This lets kernel diagnostics appear between user lines or chunks.

## Output locking and panic reporting

The serial transmit lock protects ring updates and each UART readiness check with its possible burst write. Receive register access uses another lock. The helpers in `c/drivers/core/io_lock.h` save and restore the caller’s interrupt state while providing exclusion across CPUs.

`kprintf()`, `serial_puts()` and console write chunks participate in the message lock defined in `c/kernel/diag/kprintf.c`. During regular output, that lock covers formatting or string traversal and submission to the transmit ring, with interrupts disabled. Direct `serial_putc()` calls acquire the transmit lock for each call.

Earlier output held the message lock across UART readiness waits. Queued transmission moves those waits out of regular output submission, reducing the time spent with interrupts disabled. This change and its motivation are documented in `c/kernel/diag/kprintf.c` and `c/drivers/char/serial.c`.

`kprintf()` sends characters to VGA, COM1 and the kernel log. Boot narration uses this path so it remains available in the log. `serial_puts()` sends its text to COM1. See `c/kernel/diag/kprintf.c` and `c/kernel/init/kmain.c`.

While the boot framebuffer console is active, the formatter also calls `bootcon_putc()` to paint text there. `kernel_main()` calls `bootcon_stop()` after rendering the first desktop frame. The framebuffer console is implemented in `c/kernel/diag/bootmark.c`.

The panic path calls `kdiag_stop_other_cpus()` before `klog_panic_takeover()`. The former sends halt requests to peer CPUs; takeover enables lock bypasses in the formatter, serial driver, VGA driver and kernel log. This lets reporting proceed when the failed execution path held a console lock. See `c/kernel/diag/panic.c`, `c/kernel/diag/kdiag.c` and `c/kernel/diag/klog.c`.

After serial panic takeover, `serial_putc()` uses a synchronous drain with bounded readiness polling. `serial_flush_blocking()` drains queued output and then polls the transmitter-empty status, which covers the final byte in the shift register. Its readiness waits occur between transmit-lock transactions. Power-off and reset paths invoke it before platform operations that would interrupt transmission, including the PCH reset in `c/drivers/board/board.c`. See `c/drivers/char/serial.c` and `c/kernel/init/power.c`.

## VGA text output

`c/drivers/char/vga.c` writes volatile 16-bit cells at `0xB8000`. Each cell holds a character byte and a color attribute. The initial colors are light grey on black. `vga_set_color()` selects the attribute used by subsequent character writes and clearing operations.

The driver maintains its cursor position in software:

| Character or condition | Action |
|---|---|
| Newline | Advance one row and reset the column |
| Carriage return | Reset the column |
| Tab | Advance the column to the next multiple of four |
| Column reaches 80 | Wrap to the next row |
| Row reaches 25 | Scroll upward and clear the bottom row |

Tab advances the column arithmetically. Scrolling copies existing cells upward and fills the bottom row with spaces in the current color.

The VGA device lock protects cursor updates, color changes, clearing and scrolling. `vga_clear()` fills all cells with spaces and resets the cursor; `vga_puts()` calls the character writer for each byte. Public declarations and the color enumeration are in `c/drivers/char/vga.h`.

## Limitations

- The PS/2 keyboard driver provides no keyboard initialization routine, layout selection or Caps Lock handling. See `c/drivers/char/keyboard.c`.
- Mouse setup consumes acknowledgement bytes without validating their values. Controller waits are bounded but provide no timeout status to their callers. See `c/drivers/char/mouse.c`.
- The desktop input queue can drop events under overload. Counters distinguish discarded motion from discarded key, modifier, button or wheel events. See `c/kernel/gui/input/input_queue.c`.
- Earlier documentation described serial loss as a timeout on each byte. Current regular transmission drops new bytes when the RAM ring fills. Blocking drains abandon remaining queued bytes after 1,000,000 consecutive failed readiness checks, or one failed check if the drain started with the UART marked stuck. A later `serial_putc()` queues a loss notice when at least 96 bytes of ring space are available. The final transmitter-empty wait is also bounded, and `serial_flush_blocking()` returns no completion status. See `c/drivers/char/serial.c`.
- UART interrupts are disabled. The serial console uses polling and directs Ctrl+C to one recorded PID, without foreground process-group selection. See `c/drivers/char/serial.c` and `c/kernel/exec/signal/ksignal.c`.
- The message lock groups participating callers; direct `serial_putc()` calls can interleave with their output. See `c/drivers/char/serial.c` and `c/kernel/diag/kprintf.c`.
- VGA output requires a compatible text display at `0xB8000`. The driver provides no display-mode setup, ANSI escape parser or Unicode decoder. See `c/drivers/char/vga.c`.

## Source map

| File | What it contains |
|---|---|
| `c/drivers/char/keyboard.c` | Scancode translation, modifiers, held-key state and desktop input calls |
| `c/drivers/char/keyboard.h` | Keyboard handler and state-query declarations |
| `c/drivers/char/mouse.c` | Controller setup, wheel negotiation and packet decoding |
| `c/drivers/char/mouse.h` | Mouse initialization and interrupt-handler declarations |
| `c/drivers/char/serial.c` | COM1 setup, transmit ring, receive polling, draining and panic takeover |
| `c/drivers/char/serial.h` | Serial interfaces and transmit statistics |
| `c/drivers/char/vga.c` | Text cells, colors, software cursor and scrolling |
| `c/drivers/char/vga.h` | VGA colors and output interfaces |
| `c/drivers/core/io_lock.h` | Device locking and interrupt-state preservation |
| `c/drivers/timer/pic.c` | PIC configuration and acknowledgement |
| `c/drivers/usb/usb_hid.c` | USB modifier and held-key snapshots |
| `c/drivers/board/board.c` | Serial flushing before PCH reset |
| `c/kernel/init/kmain.c` | Driver initialization order and boot-console handover |
| `c/kernel/init/power.c` | Serial flushing before power-off and reset |
| `c/kernel/cpu/irq/interrupts.c` | PS/2 dispatch, timer receive work and periodic serial draining |
| `c/kernel/cpu/irq/ioapic.c` | Controller discovery, legacy routing and route verification |
| `c/kernel/cpu/smp/smp.c` | I/O APIC routing setup and PIC fallback |
| `c/kernel/gui/wm.c` | Input capture, queue draining, focus and application delivery |
| `c/kernel/gui/input/input_queue.c` | Motion coalescing, ordering and overflow handling |
| `c/kernel/gui/input/input_queue.h` | Queue structures, capacity and drain budget |
| `c/kernel/exec/fd/file.c` | Serial console reads, writes, message grouping and polling |
| `c/kernel/exec/signal/ksignal.c` | Receive queue, foreground PID and Ctrl+C handling |
| `c/kernel/diag/kprintf.c` | Formatting, message locking and output sinks |
| `c/kernel/diag/klog.c` | Kernel log and console panic takeover |
| `c/kernel/diag/bootmark.c` | Boot framebuffer console |
| `c/kernel/diag/panic.c` | Panic reporting sequence |
| `c/kernel/diag/kdiag.c` | Peer-CPU halt requests |
| `include/abi/logit_abi.h` | Application events, modifiers and window services |
| `Makefile` | QEMU display and serial configuration |

## See also

- [Text and input](../architecture/text-and-input.md)
- [GUI events, clipboard and notifications](../kernel/gui-events-and-services.md)
- [USB](usb.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [Kernel diagnostics](../kernel/diagnostics.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)