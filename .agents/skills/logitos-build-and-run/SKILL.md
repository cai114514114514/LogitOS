---
name: logitos-build-and-run
description: Build LogitOS and boot it in QEMU. Use when you need a kernel ISO, the application disk, or a running guest to check a change, and before any guest test.
---

# Build and run LogitOS

1. **Pick your own build directory** when other agents share the checkout: pass it on the command line,
   for example `make BUILD=build-mytask`. The root `Makefile` sets `BUILD := build`, so a command-line
   assignment decides where artifacts go; two tasks in one directory overwrite each other.
2. **Build what your change touches.**
   - Kernel or loader: `make BUILD=… ` (the default target builds the ISO).
   - Applications, libc or anything packed into the root filesystem: `make BUILD=… $BUILD/disk.img`
     (with the default directory, `make build/disk.img`). The ISO alone does not rebuild applications.
3. **Boot it:** `make BUILD=… run`. Apple Silicon runs the x86-64 guest through QEMU's TCG, so guest
   time is the only honest clock for durations (see `AGENTS.md` §5).
4. **Read the serial log.** `LOGIT_BOOT_OK` marks a boot that mounted the root and probed devices;
   `[fs] mount FAILED`, `LOGIT_FB_FAIL` and `[efi] REFUSING TO BOOT:` are the usual early failures.

Toolchain: LLVM/LLD, NASM, Rust, Python 3, Make and QEMU.

Details: `docs/getting-started/building.md`, `docs/getting-started/running-in-qemu.md`,
`docs/reference/make-targets.md`. Ask the docs MCP server (`search_docs`) for a specific target.
