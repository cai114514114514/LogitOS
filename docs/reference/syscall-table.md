# System call table

The LogitOS ABI declares 197 syscall numbers, including the historical spawn entry and a reserved GUI receipt entry. This reference lists each number, its arguments, raw return value, and purpose. Most declarations live in `include/abi/logit_abi.h`; `include/abi/aex_agent.h`, `include/abi/pty.h`, and `include/abi/fs_ref.h` supply the remaining entries.

## Calling convention

User programs enter the kernel with `int $0x80`. The convention is documented in `include/abi/logit_abi.h` and implemented by `_sys()` in `c/apps/logit.h`.

| Register | Meaning |
|---|---|
| `rax` | Syscall number on entry; result on return |
| `rdi` | First argument |
| `rsi` | Second argument |
| `rdx` | Third argument |

`c/boot/isr.asm` saves general registers and x87/SSE state. `c/kernel/cpu/irq/interrupts.c` routes vector 128 to the dispatcher and intercepts `SYS_SIGRETURN`, which restores both saved states. The main dispatcher is in `c/kernel/exec/syscall.c`; window and desktop operations continue through `c/kernel/gui/wm.c`.

Arguments below appear in register order. `()` means no arguments. A record name followed by `*` denotes a pointer to that C struct. Buffer sizes are bytes unless another unit is stated. String arguments use a terminating NUL unless accompanied by a length.

Returns are raw kernel values. Interpret them using the call’s contract: mappings use zero for allocation failure, DNS returns an IPv4 value, and scheduling queries can return negative priorities. Dispatcher policy can return `-1` before a subsystem handler runs.

## Packed arguments and records

Three argument registers accommodate small fields through packed integers and larger requests through pointers to records. `include/abi/logit_calls.abi` records wrapper signatures and field positions. `tools/gen_abi.py` generates the unpack macros in `include/abi/logit_pack.h` and compile-time assertions for record layouts.

In the legacy packed drawing calls, `x` and `y` are signed 16-bit coordinates, while `w` and `h` are unsigned 16-bit sizes. Drawing geometry uses window content points. Packed RGB colors occupy 24 bits. Formulas in the tables show field positions; mask each field to its width before combining it with the others.

Drawing records for numbers 211–220 and 222 use 24.8 fixed point for point coordinates and dimensions: 256 represents one point. Each record’s leading `size` field must equal the kernel’s record size, as checked in `c/kernel/gui/wm_draw2.h`. Bitmap source rectangles use pixels, row strides use bytes, and path matrix coefficients have the formats specified in `include/abi/logit_abi.h`.

## Numbers 1–41: basic services, files, networking, and drawing

File and network dispatch in this range lives in `c/kernel/exec/syscall.c`. Drawing, launch arguments, system information, and HTTP calls pass through `c/kernel/gui/wm.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 1 | `SYS_WRITE` | `fd, buf, len` | Bytes written; negative error | Write through a descriptor; descriptors 1 and 2 support serial fallback when no file description is installed. |
| 2 | `SYS_EXIT` | `code` | Terminates execution | End the process and initiate thread teardown. |
| 3 | `SYS_GUI_CREATE` | `title, (w<<16) \| h` | `0`; `-1` error | Create the caller’s application window. |
| 4 | `SYS_GUI_CLEAR` | `color` | `0`; `-1` error | Clear the window canvas. |
| 5 | `SYS_GUI_RECT` | `(x<<16) \| y, (w<<16) \| h, color` | `0`; `-1` error | Fill a rectangle. |
| 6 | `SYS_GUI_TEXT` | `(x<<16) \| y, color, str` | `0`; `-1` error | Draw a NUL terminated text string. |
| 7 | `SYS_GUI_FLUSH` | `()` | `0`; negative error | Report changes covering the window canvas for presentation. |
| 8 | `SYS_POLL_EVENT` | `logit_event*` | `1` event; `0` no event; `-1` error | Consume a queued window event. |
| 9 | `SYS_GET_ARG` | `buf, max` | Bytes copied excluding NUL; `-1` error | Read the application launch argument. |
| 10 | `SYS_GET_TIME` | `logit_time*` | `0`; `-1` error | Read calendar time from a cached RTC snapshot. |
| 11 | `SYS_READ_FILE` | `path, buf, max` | Bytes read; `-1` error | Read file contents from the beginning. |
| 12 | `SYS_YIELD` | `()` | `0` | Yield the CPU. |
| 13 | `SYS_SYSINFO` | `buf, max` | Text length excluding NUL; `-1` error | Obtain formatted system information. |
| 14 | `SYS_FILE_COUNT` | `()` | Entry count; negative error | Count entries in the root directory. |
| 15 | `SYS_FILE_NAME` | `index, buf, max` | Entry size; `-1` error | Copy a root directory entry’s name. |
| 16 | `SYS_WRITE_FILE` | `path, buf, size` | Bytes written; negative error | Create or overwrite a file. |
| 17 | `SYS_DELETE_FILE` | `path` | `0`; negative error | Delete a file or empty directory. |
| 18 | `SYS_MKDIR` | `path` | `0`; negative error | Create a directory. |
| 19 | `SYS_DIR_COUNT` | `dir` | Entry count; negative error | Count entries in a directory. |
| 20 | `SYS_DIR_NAME` | `dir, index, buf` | File size; `-2` directory; `-1` error | Copy an entry name into a buffer of at least 64 bytes. |
| 21 | `SYS_NET_INFO` | `logit_netinfo*` | `1` NIC available; `0` unavailable; `-1` error | Read IPv4 configuration and the MAC address. |
| 22 | `SYS_NET_PING` | `ip` | `0` submitted; negative error | Submit an ICMP echo request to a packed IPv4 address. |
| 23 | `SYS_NET_PING_RTT` | `()` | RTT in milliseconds; `-1` pending | Read the most recent ping result. |
| 24 | `SYS_NET_DNS` | `name` | `0`; `-1` error | Start an IPv4 DNS query. |
| 25 | `SYS_NET_DNS_RESULT` | `()` | IPv4 address; `0` pending; `0xFFFFFFFF` failed | Read the query result from the low 32 bits. |
| 26 | `SYS_HTTP_GET` | `url` | `0`; negative error | Fetch synchronously through the HTTP implementation, including redirect handling. |
| 27 | `SYS_HTTP_STATUS` | `()` | `0` idle; `1` busy; `2` done; negative error | Read the HTTP fetch state. |
| 30 | `SYS_GUI_TEXT_MONO` | `(x<<16) \| y, (cell<<24) \| color, str` | `0`; `-1` error | Draw monospace text using an 8-bit cell width and 24-bit color. |
| 36 | `SYS_HTTP_BODY` | `buf, max` | Bytes copied; `-1` error | Copy the last HTTP response body. |
| 37 | `SYS_TEXT_MEASURE` | `str, len, (px<<2) \| face`; or `query*, selector, 0` | Width in points; query status; `-1` error | Measure text or request text metrics or caret positions. |
| 38 | `SYS_GUI_TEXT_RUN` | `logit_run*` | `0`; `-1` error | Draw text with a byte length, size, face, and color. |
| 39 | `SYS_RES_FETCH` | `src, buf, max` | Bytes copied; `-1` error | Fetch resource bytes from a supported data URL or a URL resolved against the current HTTP page. |
| 40 | `SYS_GUI_BLIT` | `logit_blit*` | `0`; negative error | Scale and draw an RGBA bitmap into the window. |
| 41 | `SYS_GUI_CLIP` | `(x<<16) \| y, (w<<16) \| h` | `0`; `-1` error | Set the drawing clip; two zero dimensions clear it. |

Serial TTY writes use `tty_write()` in `c/kernel/exec/fd/file.c`. It expands LF to CRLF and holds the console message lock through each newline or a chunk of at most 160 UART bytes. A final partial line is queued immediately. `c/drivers/char/serial.c` manages the transmit queue; timer interrupts in `c/kernel/cpu/irq/interrupts.c` also drain it.

The header comment for `SYS_HTTP_GET` describes starting a fetch. The current handler calls `http_get()` synchronously. HTTP status and body calls access the shared response object in `c/net/http/http.c`.

`SYS_TEXT_MEASURE` accepts two negative length selectors. `LOGIT_TEXT_METRICS_QUERY` selects `logit_text_metrics_query`; `LOGIT_TEXT_CARETS_QUERY` selects `logit_text_carets_query`. These queries return `1` on success, `0` for a size or version mismatch, and `-1` for malformed input or a failed operation. The width form returns zero for an invalid length or text size. These branches live in `c/kernel/gui/text_measure_dispatch.inc`.

For `SYS_GUI_CLIP`, exactly one zero dimension creates an empty clip. Both dimensions must be zero to reset it, as implemented in `c/kernel/gui/wm.c`.

## Numbers 50–81: processes, descriptors, and client socket handles

Process and descriptor dispatch lives in `c/kernel/exec/syscall.c`. Lifecycle handlers use `c/kernel/exec/proc.c` and `c/kernel/exec/load/exec.c`; file description operations use `c/kernel/exec/fd/file.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 50 | `SYS_FORK` | `()` | Child PID in parent; `0` in child; negative error | Create a child from the caller’s execution state. |
| 51 | `SYS_EXECVE` | `path, argv, envp` | Transfers execution on success; negative error | Replace the process image and supply argument and environment vectors. |
| 52 | `SYS_WAITPID` | `pid, int *status, opts` | Reaped PID; `0` pending with nonblocking option; negative error | Wait for a child; `pid == -1` selects any child and `opts & 1` requests a nonblocking check. |
| 53 | `SYS_GETPID` | `()` | PID; `-1` error | Read the caller’s process ID. |
| 54 | `SYS_OPEN` | `path, flags` | Descriptor; negative error | Open a path resolved against the process working directory. |
| 55 | `SYS_CLOSE` | `fd` | `0`; `-1` invalid descriptor; `-2` flush failure | Close a descriptor and report a failed final writeback. |
| 56 | `SYS_READ` | `fd, buf, len` | Bytes read; `0` EOF; negative error | Read through a descriptor backend. |
| 57 | `SYS_LSEEK` | `fd, off, whence` | New offset; negative error | Change a file description’s position. |
| 58 | `SYS_DUP2` | `oldfd, newfd` | `newfd`; negative error | Install a duplicate at a chosen descriptor number. |
| 59 | `SYS_PIPE` | `int *fds` | `0`; negative error | Create a pipe and write its read and write descriptors into two integer slots. |
| 60 | `SYS_GETCWD` | `buf, max` | Length excluding NUL; `-1` error | Read the process working directory. |
| 61 | `SYS_CHDIR` | `path` | `0`; negative error | Change the process working directory. |
| 62 | `SYS_DUP` | `fd` | New descriptor; negative error | Duplicate an open file description. |
| 63 | `SYS_SPAWN` | `path, argv` | `-1` | Historical spawn declaration; see Limitations. |
| 64 | `SYS_SETNB` | `fd` | `0`; `-1` error | Set the file description’s nonblocking flag. |
| 65 | `SYS_RENAME` | `old_path, new_path` | `0`; negative error | Rename a filesystem entry. |
| 66 | `SYS_OPEN_PATH` | `path` | `0`; negative error | Ask the desktop to open a path using its associated application. |
| 67 | `SYS_IMG_DECODE` | `logit_imgreq*` | `0`; `-1` error | Decode an image file into the supplied RGBA buffer and return dimensions in the record. |
| 68 | `SYS_CPU_INDEX` | `()` | Logical CPU index | Read the CPU currently executing the caller. |
| 69 | `SYS_KHEAP_STRESS` | `iters, size, seed` | Allocation and verification failure count; `-1` invalid input | Exercise kernel allocation and freeing with tagged blocks. |
| 70 | `SYS_UI_DARK` | `set` | Current theme, `0` or `1`; negative error | Query with a negative value or set the system theme. |
| 71 | `SYS_GUI_ICON` | `(x<<16) \| y, (id<<16) \| px, color_or_name` | `0`; `-1` error | Draw a symbol; `LOGIT_ICON_APP` selects an application name string in the third argument. |
| 72 | `SYS_GUI_GLASS` | `(x<<16) \| y, (w<<16) \| h, (radius<<32) \| (tr<<24) \| (tg<<16) \| (tb<<8) \| ta` | `0`; `-1` error | Draw a glass region using an 8-bit radius and tint channels. |
| 73 | `SYS_GUI_RRECT` | `(x<<16) \| y, (w<<16) \| h, (radius<<24) \| color` | `0`; `-1` error | Fill a rounded rectangle using an 8-bit radius. |
| 74 | `SYS_FSYNC` | `fd` | `0`; negative error | Flush a file description’s pending contents. |
| 75 | `SYS_MONOTONIC_MS` | `()` | Unsigned milliseconds since boot | Read the monotonic millisecond clock. |
| 76 | `SYS_SOCK_OPEN` | `host, (port<<16) \| flags` | Handle ≥ 0; negative error | Begin a client connection with optional TLS and ALPN flags. |
| 77 | `SYS_SOCK_POLL` | `handle` | State and readiness bits; negative error | Read a client connection’s state. |
| 78 | `SYS_SOCK_SEND` | `handle, buf, len` | Bytes accepted, possibly zero or short; negative error | Queue bytes for a client connection. |
| 79 | `SYS_SOCK_RECV` | `handle, buf, max` | Bytes received; `0` pending; negative value on closure or error | Receive available client connection bytes. |
| 80 | `SYS_SOCK_ALPN` | `handle, buf, max` | Copied protocol length; negative error | Copy the negotiated ALPN name with a terminating NUL. |
| 81 | `SYS_SOCK_CLOSE` | `handle` | `0`; negative error | Close a client connection handle. |

Numbers 76–81 use process-owned handles in `c/net/core/sock.c`. Numbers 137–149 use file descriptors backed by `c/net/core/lsock.c`. Each interface has its own transfer, close, and readiness operations.

`SYS_WAITPID` writes the child’s stored exit code into `status`. `c/kernel/exec/proc.c` implements the nonblocking option and signal interruption of a blocking wait.

## Numbers 82–106: time, audio, memory, and desktop services

Time dispatch begins in `c/kernel/exec/syscall.c`. Audio handlers live in `c/kernel/audio/snd.c`; memory handlers live in `c/kernel/mm/mmsys.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 82 | `SYS_SCREEN_INFO` | `what` | Selected value; `-1` unknown selector | Query logical dimensions, scale percentage, or framebuffer dimensions. |
| 83 | `SYS_CLOCK_GETTIME` | `clock_id, logit_timespec*` | `0`; `-1` error | Read the selected clock in seconds and nanoseconds. |
| 84 | `SYS_NANOSLEEP` | `logit_timespec *req, logit_timespec *rem` | `0`; `-1` invalid request | Sleep for an interval and write zero to a valid optional remainder record on completion. |
| 85 | `SYS_CLOCK_INFO` | `logit_clockinfo*, set_source` | `0`; `-1` error | Query timing information and optionally select a clock source; a negative selector queries. |
| 86 | `SYS_SND_INFO` | `logit_sndinfo*` | `1` device present; `0` absent; negative error | Read output audio device information. |
| 87 | `SYS_SND_OPEN` | `logit_sndfmt*` | Playback handle ≥ 0; negative error | Open an audio playback stream. |
| 88 | `SYS_SND_WRITE` | `handle, buf, bytes` | Bytes accepted; negative error | Supply playback samples. |
| 89 | `SYS_SND_AVAIL` | `handle` | Available buffer bytes; negative error | Query playback queue capacity. |
| 90 | `SYS_SND_CLOSE` | `handle, drain` | `0`; negative error | Close playback, optionally draining queued samples. |
| 91 | `SYS_SND_STATE` | `handle, logit_sndstate*` | `0`; negative error | Read playback counters and state. |
| 92 | `SYS_MMAP` | `len, prot, hint` | Base address; `0` allocation failure | Reserve anonymous memory whose pages are populated on access. |
| 93 | `SYS_MUNMAP` | `addr, len` | `0`; `-1` error | Release a mapped address range. |
| 94 | `SYS_MEMINFO` | `logit_meminfo*`; or `0, diagnostic, arg` | `0` for a copied record; diagnostic result; negative error | Read memory accounting or invoke a memory diagnostic. |
| 95 | `SYS_PROCS` | `logit_procinfo *buf, max` | Entries copied; `-1` error | Read process table records. |
| 96 | `SYS_KILL` | `pid, sig, flags` | `0` accepted; negative error | Request deferred termination, or send or probe a signal when `LOGIT_KILL_SIGNAL` is set. |
| 97 | `SYS_CLIP_SET` | `flavour \| (flags<<16), buf, len` | Bytes stored; negative error | Copy a representation into the kernel clipboard. |
| 98 | `SYS_CLIP_GET` | `flavour, buf, max` | Bytes copied; negative error | Read a representation; text truncation respects UTF-8 character boundaries. |
| 99 | `SYS_CLIP_INFO` | `what, flavour` | Selected value; negative error | Query clipboard types, length, ownership, serial, or capacity. |
| 100 | `SYS_NOTIFY` | `title, body, level` | Notification ID ≥ 1; `0` dropped; `-1` error | Queue a desktop notification. |
| 101 | `SYS_GUI_WIN_MIN` | `(w<<16) \| h` | `0`; `-1` error | Set the minimum content size in points. |
| 102 | `SYS_GUI_WIN_STATE` | `what, arg` | Query value or command result; `-1` error | Query geometry or control window state, keyboard reporting, panels, and titlebar appearance. |
| 103 | `SYS_SETTING_GET` | `key, buf, max` | Bytes copied excluding NUL; negative error | Read a stored setting or its schema default. |
| 104 | `SYS_SETTING_SET` | `key, value, flags` | `0`; negative error | Set a string value; flag bit 0 requests an immediate commit. |
| 105 | `SYS_SETTING_ENUM` | `index, logit_setting*, prefix` | `0`; negative error or end | Enumerate schema entries with a null prefix, or entries matching a prefix string. |
| 106 | `SYS_SETTING_CTL` | `op, a, b` | Value or status selected by operation | Commit, reset, reload, inspect, or diagnose the settings store. |

Clipboard storage belongs to the kernel and survives the originating process; its copy rules are in `c/kernel/gui/clipboard.c`. Notifications use the bounded queue in `c/kernel/gui/notify.c`. Settings operations use the schema and store in `c/kernel/core/settings.c`.

## Numbers 107–138: threads, metadata, signals, and local IPC

Thread operations use `c/kernel/sched/uthread.c`. Metadata calls route to `c/kernel/exec/load/meta.c`; signal calls route to `c/kernel/exec/signal/ksignal.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 107 | `SYS_THREAD_CREATE` | `logit_thread_spec*` | Thread ID > 0; negative error | Start a thread with the supplied entry, stack, argument, and TLS state. |
| 108 | `SYS_THREAD_EXIT` | `retval` | Terminates execution | End the calling thread; the final thread performs process teardown. |
| 109 | `SYS_THREAD_JOIN` | `tid, retval*` | `0`; negative error | Wait for a joinable sibling and optionally copy its 64-bit result. |
| 110 | `SYS_THREAD_DETACH` | `tid` | `0`; negative error | Arrange resource release when a thread exits. |
| 111 | `SYS_THREAD_SELF` | `()` | Thread ID; negative error | Obtain the calling thread’s ID. |
| 112 | `SYS_SET_TLS` | `fsbase` | `0` when setting; current base for `-1`; negative error | Set or query the calling thread’s FS base. |
| 113 | `SYS_FUTEX` | `word*, (timeout_ms<<32) \| val, op` | Wait: `0` or negative error; wake: count or negative error | Wait on an aligned 32-bit word or wake waiters; zero timeout waits indefinitely. |
| 114 | `SYS_GETRANDOM` | `buf, len, flags` | Bytes copied; `-1` error; null-buffer query returns `0` or `1` | Obtain DRBG bytes or query whether hardware entropy successfully seeded it. |
| 115 | `SYS_THREAD_INFO` | `what` | Selected count; `-1` unknown selector | Query thread population, slots, creation, and reclamation counters. |
| 120 | `SYS_STAT` | `path, logit_stat*, len` | `0`; negative error | Read metadata after following symlinks. |
| 121 | `SYS_LSTAT` | `path, logit_stat*, len` | `0`; negative error | Read metadata for the named entry, including a symlink itself. |
| 122 | `SYS_FSTAT` | `fd, logit_stat*, len` | `0`; negative error | Read descriptor metadata. |
| 123 | `SYS_GETDENTS` | `logit_dirreq*` | Entries copied; `0` end; negative error | Enumerate directory records and advance the request cursor. |
| 124 | `SYS_CHMOD` | `path, mode` | `0`; negative error | Change permission bits subject to ownership checks. |
| 125 | `SYS_UMASK` | `mask` | Previous mask | Set the creation mask; a negative argument queries it. |
| 126 | `SYS_SYMLINK` | `target, linkpath` | `0`; negative error | Create a symbolic link storing the supplied target text. |
| 127 | `SYS_READLINK` | `path, buf, max` | Bytes copied; negative error | Copy symlink target bytes. |
| 128 | `SYS_LINK` | `oldpath, newpath` | `0`; negative error | Create a hard link. |
| 129 | `SYS_CHOWN` | `path, uid, gid` | `0`; negative error | Change ownership through the root credential check. |
| 130 | `SYS_SIGACTION` | `signo, logit_sigaction *act, logit_sigaction *old` | `0`; negative error | Install or query a signal disposition; either record pointer may be null. |
| 131 | `SYS_SIGPROCMASK` | `how, set*, old*` | `0`; negative error | Update or query the process signal mask using 64-bit sets. |
| 132 | `SYS_SIGRETURN` | `()`; signal frame at `rsp` | Restores execution; negative error or termination for an invalid frame | Restore registers, floating-point state, and the saved signal mask. |
| 133 | `SYS_SIGPENDING` | `out*` | `0`; negative error | Copy the pending blocked-signal set into a 64-bit word. |
| 134 | `SYS_ALARM` | `seconds` | Previous remaining seconds, rounded up; negative error | Schedule an alarm signal; zero cancels it. |
| 135 | `SYS_SIGSUSPEND` | `mask*` | `SIG_E_INTR` after waking; other negative error | Temporarily install a mask and wait for a signal. |
| 136 | `SYS_SIGQUERY` | `what` | Selected diagnostic value; `-1` unknown selector | Query signal delivery and state counters. |
| 137 | `SYS_CONNECT` | `fd, address*, len` | `0`; negative error | Connect a local socket or establish a blocking IPv4 stream connection. |
| 138 | `SYS_SOCKETPAIR` | `domain, type, int *sv` | `0`; negative error | Create connected local socket descriptors in two integer slots. |

For `SYS_STAT`, `SYS_LSTAT`, and `SYS_FSTAT`, `len` supplies the caller’s buffer size and must be at least eight bytes. `c/kernel/exec/load/meta.c` copies the supported prefix and records the copied length in the output. `SYS_STATVFS` uses the same size convention. `SYS_READLINK` copies the target’s bytes directly.

`SYS_CONNECT` selects `logit_sockaddr_un` or `logit_sockaddr` from the address family. The IPv4 branch in `c/kernel/exec/syscall.c` and `c/net/core/lsock.c` extends the local-socket contract preserved in the ABI header comment.

## Numbers 140–167: socket descriptors, credentials, capabilities, and protection

Socket descriptor operations use `c/net/core/lsock.c`. Credential operations use `c/fs/vfs/vfs_cred.c`; capability spawning uses `c/kernel/exec/load/exec.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 140 | `SYS_SOCKET` | `domain, type, protocol` | Descriptor; negative error | Create a socket description. |
| 141 | `SYS_BIND` | `fd, address*, len` | `0`; negative error | Bind an IPv4 address or local socket path. |
| 142 | `SYS_LISTEN` | `fd, backlog` | `0`; negative error | Establish a listener with backlog clamped to 1–8. |
| 143 | `SYS_ACCEPT` | `fd, peer*, timeout_ms` | Accepted descriptor; negative error | Accept a connection and optionally copy peer information. |
| 144 | `SYS_GETSOCKNAME` | `fd, address*` | `0`; negative error | Read the bound address using the socket’s address family. |
| 145 | `SYS_SETSOCKOPT` | `fd, (level<<16) \| optname, value` | `0`; negative error | Set a supported option using a scalar value. |
| 146 | `SYS_SHUTDOWN` | `fd, how` | `0`; negative error | Shut down the selected connection direction. |
| 147 | `SYS_RECVFROM` | `fd, logit_dgram*` | Bytes received; negative error | Receive a datagram and fill source address fields when bytes are returned. |
| 148 | `SYS_SENDTO` | `fd, logit_dgram*` | Bytes sent; negative error | Send bytes to the destination in the record. |
| 149 | `SYS_SOCKSTAT` | `what` | Selected counter; `-1` unknown selector | Query socket and transport diagnostics. |
| 150 | `SYS_GETUID` | `()` | User ID | Read the process user ID. |
| 151 | `SYS_GETEUID` | `()` | User ID | Read the same stored user ID as `SYS_GETUID`. |
| 152 | `SYS_GETGID` | `()` | Group ID | Read the process primary group ID. |
| 153 | `SYS_GETEGID` | `()` | Group ID | Read the same stored group ID as `SYS_GETGID`. |
| 154 | `SYS_SETUID` | `uid` | `0`; negative error | Change the user ID through the root credential check. |
| 155 | `SYS_SETGID` | `gid` | `0`; negative error | Change the group ID through the root credential check. |
| 156 | `SYS_GETGROUPS` | `n, list*` | Group count; negative error | Copy supplementary 32-bit group IDs; zero capacity queries the count. |
| 157 | `SYS_SETGROUPS` | `n, list*` | `0`; negative error | Set supplementary groups through the root credential check. |
| 158 | `SYS_SETSESSION` | `uid, gid` | `0`; negative error | Set login session credentials and adopt user settings if preparation succeeds. |
| 159 | `SYS_GETSESSION` | `which` | Session user or group ID | Read the group ID with selector `1`, otherwise the user ID. |
| 160 | `SYS_CAP_SPAWN` | `path, argv, logit_capreq*` | Child PID; negative error | Launch a child with a capability grant bounded by the caller’s grant. |
| 161 | `SYS_CAP_QUERY` | `buf, max` | Capability bitmap | Read the caller’s grant and optionally copy its filesystem prefix. |
| 162 | `SYS_MMAP_FILE` | `logit_mmap_file_req*` | Base address; `0` generic failure; `-1` writable request | Map file pages through an existing descriptor, with read and optional execute protection. |
| 163 | `SYS_POWEROFF` | `()` | Powers off; negative error on refusal or failure | Drain filesystem activity, sync storage, and request platform shutdown. |
| 164 | `SYS_REBOOT` | `()` | Resets; negative error on refusal or failure | Prepare the filesystem, quiesce devices, and attempt platform reset methods. |
| 165 | `SYS_WAIT_EVENT` | `logit_event *event, timeout_ms` | `1` available; `0` no event; `-1` error | Wait for a window event; a null pointer waits for availability while preserving the event. |
| 166 | `SYS_MPROTECT` | `addr, len, prot` | `0`; negative error | Change protection on a reserved memory range. |
| 167 | `SYS_RUSAGE` | `cmd, a, b` | Value or status selected by command | Read CPU usage or resident frames, or set or query the calling thread’s CPU limit. |

`SYS_SETSESSION` manages login credentials. Process sessions and process groups use numbers 198–200.

After draining filesystem activity and syncing storage, `SYS_REBOOT` calls `dev_quiesce_all()` from `c/drivers/core/device.c`. With confirmed DMA quiescence, reset attempts proceed through ACPI, the board reset path, an 8042 pulse, and a triple fault. Unconfirmed DMA adds a board reset attempt before ACPI. `board_reset()` in `c/drivers/board/board.c` supplies the CF9 attempt when a supported PCH is present. `c/kernel/init/power.c` implements the ordering.

`SYS_WAIT_EVENT` uses a positive timeout in milliseconds; zero or a negative value waits indefinitely. Window closure also wakes the wait. The handler in `c/kernel/gui/wm.c` returns zero when it completes without an event.

`SYS_RUSAGE` uses these selectors from `include/abi/logit_abi.h`.

| Selector | Second argument | Result |
|---|---|---|
| `RUCTL_GET_NS` | Zero for the calling thread; nonzero for the process | CPU nanoseconds |
| `RUCTL_SET_LIMIT_S` | Limit in seconds; a negative value clears it | `0` or negative error |
| `RUCTL_GET_LIMIT_S` | Unused | Limit in seconds; `-1` means unlimited |
| `RUCTL_GET_RSS_FRAMES` | Unused | Resident user frames in the caller’s address space |

CPU accounting and limit operations live in `c/kernel/sched/sched.c`; resident-frame dispatch lives in `c/kernel/exec/syscall.c`.

## Numbers 168–203: capture, polling, shared memory, modules, and agents

Audio capture uses `c/kernel/audio/snd.c`. Polling and event descriptor creation use `c/kernel/exec/fd/kpollsys.c`; shared memory uses `c/kernel/mm/mmsys.c`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 168 | `SYS_SND_CAP_OPEN` | `logit_sndfmt*` | Capture handle ≥ 0; negative error | Open capture and return the selected format in the record. |
| 169 | `SYS_SND_CAP_READ` | `handle, buf, bytes` | Bytes delivered; negative error | Read captured audio samples. |
| 170 | `SYS_SND_CAP_AVAIL` | `handle` | Captured bytes available; negative error | Query queued capture data. |
| 171 | `SYS_SND_CAP_CLOSE` | `handle` | `0`; negative error | Close an audio capture stream. |
| 172 | `SYS_SND_CAP_STATE` | `handle, logit_sndstate*` | `0`; negative error | Read capture counters and state. |
| 173 | `SYS_POLL` | `logit_pollfd *fds, nfds, timeout_ms` | Ready entry count; `0` timeout; negative error | Wait for descriptor readiness and fill each entry’s result bits. |
| 174 | `SYS_EVENTFD` | `initval, flags` | Descriptor; negative error | Create a counter descriptor for event loop wakeups. |
| 175 | `SYS_TIMERFD` | `fd, logit_itimer*, flags` | New descriptor when `fd < 0`; otherwise `0`; negative error | Create, rearm, or disarm a timer descriptor. |
| 176 | `SYS_SHM_OPEN` | `name, pages, (mode<<4) \| flags` | Handle ≥ 0; negative error | Open or create a named segment, or create an unnamed segment with a null name. |
| 177 | `SYS_SHM_MAP` | `handle, len, (prot<<32) \| page_offset` | Base address; `0` failure | Map a segment range using a page offset and byte length. |
| 178 | `SYS_SHM_UNLINK` | `name` | `0`; negative error | Remove a segment’s name. |
| 179 | `SYS_SHM_CLOSE` | `handle` | `0`; negative error | Release a segment handle; mappings hold their own references. |
| 182 | `SYS_MODULE_LOAD` | `path` | Module ID ≥ 1; negative error | Load a kernel module through the root credential check. |
| 183 | `SYS_MODULE_UNLOAD` | `id` | `MOD_E_NOUNLOAD` for root; permission error otherwise | Request module unloading; see Limitations. |
| 184 | `SYS_MODULE_LIST` | `logit_modinfo *out, max` | Total loaded count; negative error | Copy up to `max` records; a null buffer with zero capacity also prints the table. |
| 185 | `SYS_MODULE_SYM` | `name, address*` | `1` exported; `0` absent; negative error | Query an export and optionally copy its address; requires root. |
| 186 | `SYS_SCHED` | `cmd, pid, value` | Nice value, weight, or negative error | Query or change process scheduling priority; PID zero selects the caller. |
| 187 | `SYS_PTRACE` | `request, pid, arg` | `0`; negative error | Control tracing or exchange registers and memory through request payloads. |
| 188 | `SYS_GUI_FLUSH_RECT` | `(x<<16) \| y, (w<<16) \| h` | `0`; `-1` error | Report a changed content rectangle; a rectangle empty after clipping selects the whole canvas. |
| 189 | `SYS_FTRUNCATE` | `fd, len` | `0`; negative error | Change the length of a writable file description. |
| 190 | `SYS_AGENT_SPAWN` | `path, aex_agent_spawn*` | Child PID; `-1` failure; `AEX_SPAWN_CHANNEL_CONSUMED` after transfer-stage failure | Launch an agent worker and transfer its IPC channel. |
| 191 | `SYS_AGENT_SELF` | `aex_agent_identity*, len` | `0`; `-1` error | Read the calling process’s bound agent identity. |
| 192 | `SYS_AGENT_PEER` | `fd, aex_agent_identity*, len` | `0`; `-1` error | Read the identity bound to an IPC peer. |
| 193 | `SYS_CREATE_FILE` | `path, buf, size` | Bytes written; negative error | Atomically create a new file at an absent name. |
| 194 | `SYS_PTY_OPEN` | `int *fds` | `0`; `-1` error | Create PTY master and slave descriptors in two integer slots. |
| 195 | `SYS_PTY_CTL` | `fd, op, arg` | Value or status selected by operation; negative error | Control terminal attributes, window size, queues, signal events, and foreground process group. |
| 196 | `SYS_CPU_COUNT` | `()` | Online logical CPU count | Read the current online CPU population. |
| 197 | `SYS_FSREF` | `logit_fsref*, len` | `0`; negative error | Resolve a persistent file identity into a currently accessible path. |
| 198 | `SYS_SETSID` | `()` | New session ID; `-1` error | Create a process session and process group for the caller. |
| 199 | `SYS_SETPGID` | `pid, pgid` | `0`; `-1` error | Change permitted group membership; PID zero selects the caller, and PGID zero selects the target PID. |
| 200 | `SYS_GETPGID` | `pid` | Process group ID; `-1` error | Read a process group; PID zero selects the caller. |
| 201 | `SYS_GUI_MENU_SET` | `blob, len` | `0`; `-1` error | Publish the window’s validated menu blob; zero length withdraws it. |
| 202 | `SYS_STATVFS` | `path, logit_statvfs*, len` | `0`; negative error | Read capacity information for the filesystem containing a path. |
| 203 | `SYS_OPEN_PATH_ARG` | `path, arg` | `0`; negative error | Ask the desktop to open a path; an AEX application receives the supplied launch argument. |

`SYS_POLL` uses zero timeout to probe readiness and a negative timeout to wait indefinitely. Event descriptor reads and writes transfer an eight-byte counter; timer descriptor reads return an eight-byte expiration count. The contracts are in `include/abi/logit_abi.h`, with descriptor behavior in `c/kernel/exec/fd/file.c`.

Shared memory handles belong to the calling process. Closing a handle releases its reference; mapped regions retain references through their VMAs, as implemented in `c/kernel/mm/mmsys.c`.

Agent request and identity records are defined in `include/abi/aex_agent.h`. `SYS_AGENT_SELF` and `SYS_AGENT_PEER` require the exact identity record size. `SYS_AGENT_SPAWN` compares the requested application identity and image hash with the parsed image, loads that image snapshot, and installs the transferred channel at descriptor 3. `AEX_SPAWN_CHANNEL_CONSUMED` means the caller’s original channel descriptor has been consumed. This path is in `c/kernel/exec/load/exec.c`.

`SYS_FSREF` requires its second argument to equal the record size, validates the record’s version and reserved field, and resolves the identity through current VFS access checks. Its record is defined in `include/abi/fs_ref.h`; handling spans `c/kernel/exec/load/meta.c` and `c/fs/vfs/vfs.c`.

## Numbers 210–223: drawing records and theme control

Drawing record calls route through `c/kernel/gui/wm_draw2.c`. “Drawing status” below means zero on success or a negative drawing error. Theme control returns the current theme value.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 210 | `SYS_GUI2_INFO` | `what` | Version and features for `0`; automatic stack closure count for `1`; negative error | Discover drawing capabilities or inspect frame state diagnostics. |
| 211 | `SYS_GUI_PATH` | `logit_path2*, logit_paint2*, logit_stroke2*` | Drawing status | Fill a path with a null stroke pointer, or stroke it with the supplied record. |
| 212 | `SYS_GUI_CLIP_PUSH` | `logit_clip2*` | Drawing status | Push a clip intersecting the current clip. |
| 213 | `SYS_GUI_CLIP_POP` | `()` | Drawing status | Pop a clip level. |
| 214 | `SYS_GUI_GROUP_BEGIN` | `logit_group2*` | Drawing status | Begin an opacity group. |
| 215 | `SYS_GUI_GROUP_END` | `()` | Drawing status | Composite and close the current group. |
| 216 | `SYS_GUI_BLIT2` | `logit_blit2*` | Drawing status | Draw a bitmap using source, destination, filtering, and opacity fields. |
| 217 | `SYS_GUI_TEXT_RUN2` | `logit_run2*` | Drawing status | Draw a text run using fixed-point placement. |
| 218 | `SYS_GUI_SHADOW2` | `logit_shadow2*` | Drawing status | Draw a shadow from its geometry and effect parameters. |
| 219 | `SYS_GUI_GLASS2` | `logit_glass2*` | Drawing status | Apply a glass effect through a drawing record. |
| 220 | `SYS_GUI_BACKDROP` | `logit_backdrop2*` | Drawing status | Register a window backdrop region and mark its canvas pixels. |
| 221 | `SYS_GUI_BACKDROP_CLEAR` | `region` | Drawing status | Clear region 0–7, or all regions with `-1`. |
| 222 | `SYS_GUI_ICON2` | `logit_icon2*` | Drawing status | Draw a symbol with fixed-point geometry and a weight selection. |
| 223 | `SYS_UI_DARK_AT` | `on, x, y` | Current theme, `0` or `1`; negative error | Set the theme with a reveal origin in content points; a negative `on` queries it. |

`SYS_GUI2_INFO(0)` combines the version in the upper 16 bits with feature bits below it. The router assembles those bits from the drawing handlers. Selector `1` reports clip and group levels automatically closed at a flush, using the counter in `c/kernel/gui/wm_draw2_state.c`.

`SYS_UI_DARK_AT` takes integer point coordinates. `c/kernel/gui/wm.c` converts them to a screen reveal origin.

## Numbers 224–225: scheduler observation and GUI receipt reservation

Scheduler observation dispatches through `c/kernel/exec/syscall.c` to `observe_syscall()` in `c/kernel/sched/observe/observe_control.c`. Request and export records are defined in `include/abi/logit_sched_observe.h`.

| Number | Name | Arguments | Return | Description |
|---:|---|---|---|---|
| 224 | `SYS_SCHED_OBSERVE` | `command, request*, bytes` | `0` success; `1` stop pending; negative error | Start, stop, inspect, or export an observation run. |
| 225 | `SYS_GUI_RECEIPT` | Reserved; see Limitations | `-1` | Reserve an entry for GUI input receipts. |

Every observation request begins with `sched_observe_header`. Its `size` and the third syscall argument must equal the command’s request size, and `version` must equal `SCHED_OBSERVE_VERSION`. Addresses embedded in records use 64-bit integer fields. The handler copies and validates the request before dispatch.

| Command | Request record | Result |
|---|---|---|
| `SCHED_OBSERVE_BEGIN` | `sched_observe_begin` | `0` after allocating pools and publishing the run; negative error |
| `SCHED_OBSERVE_STOP` | `sched_observe_header` | `0` when stopped or reclaimed; `1` while draining or closing; negative error |
| `SCHED_OBSERVE_STATUS` | `sched_observe_status` | `0` after copying state and counters; negative error |
| `SCHED_OBSERVE_READ` | `sched_observe_read` | `0` after copying records and updating the request; negative error |
| `SCHED_OBSERVE_PROBE` | `sched_observe_header` | Provider result; see Limitations |
| `SCHED_OBSERVE_MODEL` | `sched_observe_header` | Provider result; see Limitations |

`SCHED_OBSERVE_BEGIN` and `SCHED_OBSERVE_STOP` require user ID zero. Status and read requests allow root or the process and user that started the run. A begin request supplies a positive run ID greater than every earlier run ID, input and group capacities from 1 through `SCHED_OBSERVE_MAX_CAPACITY`, and a valid manifest for the selected baseline. `SCHED_BASELINE_TRACE_ONLY` permits an empty manifest. Allocation uses the quiet path in `c/kernel/sched/observe/observe_platform.c`.

A read request selects one CPU’s trace stream, input records, group archive records, or metadata. Each reader starts with cursor zero and continues with `next_cursor`. The handler returns the copied record count in `records_copied`; output flags describe gaps, retries, and the end of a stopped stream.

Stopping closes input admission, drains pending outcomes and group consumers, and asks each producer CPU to seal its ring. Scheduler, interrupt, and timer hooks advance these phases. After the run stops, the reader confirms each stream, including each producer CPU’s trace stream, with an empty read carrying `SCHED_READ_CONFIRM_EXPORT` and a cursor one past `final_sequence`. Pools remain allocated until every export is confirmed and all active readers finish, allowing failed copies to be retried.

## Dispatch policy

`c/kernel/exec/syscall.c` checks capability bits for the filesystem and network calls enumerated in its classification switch. Descriptor operations acquire references to file descriptions held by the process. Agent workers pass an additional syscall policy check; `c/kernel/cpu/irq/interrupts.c` applies that policy to signal return.

Subsystem handlers enforce credential and object checks. Examples include ownership checks for metadata, root checks for module and power operations, and the grant ceiling for capability spawning.

Within numbers 1–225, numbers without a syscall declaration are 28–29, 31–35, 42–49, 116–119, 139, 180–181, and 204–209. Unknown numbers return `-1` through the fallback in `c/kernel/gui/wm.c`.

## Limitations

- `SYS_SPAWN` has a declaration without an implementation and returns `-1`. `include/abi/logit_abi.h` preserves this history; capability spawning uses `SYS_CAP_SPAWN`.
- `SYS_GUI_RECEIPT` reserves number 225 without a request contract or handler and returns `-1` through the window manager fallback.
- `SYS_EXECVE` refuses a process with multiple live threads. The check is in `c/kernel/exec/syscall.c`.
- Module unloading returns `MOD_E_NOUNLOAD` because driver registrations can retain pointers into module storage. `c/kernel/module/modload.c` records the reason.
- File mappings reject writable protection with `LOGIT_MMAP_FILE_E_WRITE`, as implemented in `c/kernel/mm/mmsys.c`.
- `SYS_READ`, `SYS_READ_FILE`, `SYS_SOCK_SEND`, and `SYS_SOCK_RECV` transfer at most 64 KiB per call. `SYS_SENDTO` and `SYS_RECVFROM` reject larger payload requests. These checks are in `c/kernel/exec/syscall.c`. Playback writes and capture reads transfer at most 8192 bytes per call in `c/kernel/audio/snd.c`; `SYS_GETRANDOM` clamps output to 4096 bytes in `c/kernel/core/rng.c`.
- `SYS_POLL` accepts at most 32 entries, and observation reads accept at most 64 records. The limits are defined in `include/abi/logit_abi.h` and `include/abi/logit_sched_observe.h`.
- `SYS_NANOSLEEP` clamps its computed nanosecond duration to one hour. Timer descriptors use the 100 Hz tick clock, giving their millisecond fields 10 ms clock granularity. See `c/kernel/exec/syscall.c`, `c/kernel/exec/fd/file.c`, and `c/drivers/timer/pit.h`.
- `SYS_BIND` ignores its length argument. `SYS_CONNECT` checks length in the IPv4 branch; its local branch copies a fixed address record. These branches are in `c/kernel/exec/syscall.c`.
- Scheduling priorities overlap negative error values. `SYS_SCHED` callers must interpret the result using the command and target state; definitions are in `include/abi/logit_abi.h`.
- `SYS_REBOOT` halts if DMA remains unconfirmed and platform reset attempts return. `c/kernel/init/power.c` blocks the subsequent CPU reset methods because devices could continue writing into the next boot’s memory.
- `SCHED_OBSERVE_PROBE` and `SCHED_OBSERVE_MODEL` return `SCHED_OBSERVE_ENOSYS` because their providers are unregistered. Production input queues have yet to integrate the observation ledger’s arrival and group APIs. The recorder covers at most 32 CPUs, counts NMI and machine-check entries without recording them, and reports partial coverage of intervals with interrupts disabled. See `c/kernel/sched/observe/observe_control.c`, `c/kernel/sched/observe/input_ledger.h`, `c/kernel/sched/observe/coalesced_inputs.h`, and `c/kernel/sched/observe/flight_recorder.h`.

## See also

- [System calls](../architecture/system-calls.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [Shared memory](../kernel/shared-memory.md)
- [Signals](../kernel/signals.md)
- [Sockets](../networking/sockets.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_abi.h` | Main syscall numbers, argument contracts, records, selectors, and return constants. |
| `include/abi/logit_calls.abi` | Wrapper signatures and packed field positions. |
| `include/abi/logit_pack.h` | Generated packed argument accessors. |
| `tools/gen_abi.py` | Wrapper generation, layout assertions, and unpack macro generation. |
| `include/abi/aex_agent.h` | Agent launch and identity records, channel transfer result, and exclusive file creation number. |
| `include/abi/pty.h` | PTY controls and process session and group syscall numbers. |
| `include/abi/fs_ref.h` | Persistent file reference request and syscall number. |
| `include/abi/logit_sched_observe.h` | Observation commands, request records, export formats, cursors, states, and errors. |
| `include/abi/logit_app_icon.h` | Application icon selector. |
| `c/apps/logit.h` | Native syscall entry wrapper and application helpers. |
| `c/boot/isr.asm` | Interrupt register and x87/SSE state preservation. |
| `c/kernel/cpu/irq/interrupts.c` | Syscall routing, signal return interception, observer hooks, and serial timer drain. |
| `c/kernel/exec/syscall.c` | Main dispatch, capability classification, worker policy, user copies, and subsystem forwarding. |
| `c/kernel/exec/proc.c` | Fork, waiting, termination, process queries, capability queries, sessions, and groups. |
| `c/kernel/exec/load/exec.c` | Image replacement, capability spawning, and agent spawning. |
| `c/kernel/exec/load/meta.c` | Metadata, directory enumeration, capacity, and file reference calls. |
| `c/kernel/exec/fd/file.c` | Descriptor backends, serial TTY writes, positioning, flushing, event counters, and timer descriptors. |
| `c/kernel/exec/fd/pty.c` | PTY allocation and terminal controls. |
| `c/kernel/exec/fd/kpollsys.c` | Poll request marshalling and event and timer descriptor creation. |
| `c/kernel/exec/signal/ksignal.c` | Signal dispositions, masks, pending sets, alarms, and diagnostics. |
| `c/kernel/exec/signal/ksigframe.c` | Signal frame restoration and invalid frame handling. |
| `c/kernel/exec/ptrace.c` | Tracing requests and register and memory exchange. |
| `c/kernel/sched/uthread.c` | Thread lifecycle, TLS, futexes, and thread counters. |
| `c/kernel/sched/sched.c` | CPU accounting, CPU limits, priority controls, and observation hooks. |
| `c/kernel/sched/observe/observe_control.c` | Observation request validation, permissions, lifecycle, export, and reclamation. |
| `c/kernel/sched/observe/observe_platform.c` | Observation allocation, clocks, CPU masks, caller identity, and user copies. |
| `c/kernel/sched/observe/flight_recorder.h` | Recorder hooks, interrupt coverage, and export interface. |
| `c/kernel/sched/observe/input_ledger.h` | Input arrival, outcome, and ledger export interfaces. |
| `c/kernel/sched/observe/coalesced_inputs.h` | Input group construction, completion evidence, and archive export interfaces. |
| `c/kernel/mm/mmsys.c` | Mappings, protection, shared memory, and memory diagnostics. |
| `c/kernel/audio/snd.c` | Playback and capture syscall handlers. |
| `c/kernel/core/rng.c` | Random byte generation and hardware seed query. |
| `c/kernel/core/settings.c` | Settings lookup, update, enumeration, persistence, and controls. |
| `c/kernel/gui/wm.c` | Window, event, drawing, launch, theme, menu, and HTTP dispatch. |
| `c/kernel/gui/text_measure_dispatch.inc` | Width, metrics, and caret queries. |
| `c/kernel/gui/wm_draw2.h` | Drawing handler interface and record size validation. |
| `c/kernel/gui/wm_draw2.c` | Drawing record dispatch and feature discovery. |
| `c/kernel/gui/wm_draw2_path.c` | Path and paint handling. |
| `c/kernel/gui/wm_draw2_state.c` | Clips, groups, bitmap drawing, and automatic stack closure accounting. |
| `c/kernel/gui/wm_draw2_text.c` | Text record handling and coordinate conversion. |
| `c/kernel/gui/wm_draw2_fx.c` | Shadow and glass record handling. |
| `c/kernel/gui/wm_draw2_icon.c` | Icon record handling. |
| `c/kernel/gui/wm_backdrop.c` | Backdrop registration and clearing. |
| `c/kernel/gui/clipboard.c` | Clipboard storage and queries. |
| `c/kernel/gui/notify.c` | Notification submission and queue behavior. |
| `c/net/core/sock.c` | Client socket handles, transfer, readiness, and ALPN. |
| `c/net/core/lsock.c` | Descriptor socket operations and IPv4 connection handling. |
| `c/net/core/unix.c` | Local sockets and listener backlog handling. |
| `c/net/transport/tcp.c` | TCP listeners and backlog handling. |
| `c/net/ip/icmp.c` | Ping submission and RTT state. |
| `c/net/http/http.c` | Synchronous HTTP fetching, shared response state, and resource retrieval. |
| `c/fs/vfs/vfs.c` | Exclusive file creation, file reference resolution, and metadata permission checks. |
| `c/fs/vfs/vfs_cred.c` | Credentials, supplementary groups, and login session identity. |
| `c/kernel/module/modload.c` | Module loading, listing, symbol queries, and unload refusal. |
| `c/kernel/init/power.c` | Filesystem drain, storage synchronization, shutdown, and reset ordering. |
| `c/drivers/core/device.c` | Device quiescing and DMA confirmation before reboot. |
| `c/drivers/board/board.c` | Board reset entry and CF9 reset forwarding. |
| `c/kernel/diag/kprintf.c` | Console message lock shared with serial TTY writes. |
| `c/drivers/char/serial.c` | Serial transmit queue, timer drain, and blocking flush before shutdown or reset. |
| `c/drivers/timer/pit.h` | Tick frequency and millisecond clock granularity. |