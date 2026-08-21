# The porting line: a C toolchain that compiles itself on LogitOS

Status: workflows 1–3 in flight (2026-08-21). Every number below is measured on
this machine on 2026-08-20/21 unless it says otherwise; nothing here is a plan
written from memory.

## What the line is for

M18's stated goal is "run software not written for LogitOS". Everything ported
so far was ported by cross-compiling on the host and packing an `.aex`. A
machine that cannot compile a program is a machine that cannot be developed
*on*, and every real Unix program assumes a `cc` exists. The deliverable is a C
compiler that runs on LogitOS and **compiles itself**, with the one gate that
cannot be talked around:

> tcc built on LogitOS compiles tcc's source into tcc2; tcc2 compiles the same
> source into tcc3; **tcc2 and tcc3 are byte-identical**, and both are
> byte-identical to the tcc cross-built on the host from the same source
> against the same libc objects.

A compiler can compile a wrong program that runs. It cannot compile a copy of
itself that is byte-identical to itself *and* wrong.

## Why TCC first and not GCC — measured, not taste

| | host, measured |
|---|---|
| `cc1` | 35.7 MB dynamically linked; 50–60 MB with gmp/mpfr/mpc/isl static |
| `cc1` undefined symbols | 515: **isl 222, mpfr 71, mpc 21, gmp 20** (334 from four from-scratch bignum/polyhedral libraries, 3.35 MB of .so), libc+libm 157 of which **54 are missing here** |
| `tcc` 0.9.27 | 36,151 lines, 11 x86-64 TUs; **libc gap list EMPTY, proven by linking** (0 undefined symbols against mini-libc + libm); emits ELF directly, **needs no assembler and no linker** |

GCC is not blocked by the libc (54 names, 9 real — `madvise sbrk syscall
getpagesize getentropy arc4random secure_getenv mallinfo2 wcswidth`) and is not
blocked by the filesystem any more (see walls). It is blocked by needing
binutils (`as`, `ld`) *and* four libraries this tree would have to port first.
TCC is blocked by nothing this tree does not already have, and its self-host is
the gate that proves the whole path — libc, crt0, syscalls, ELF loader, file
system, shell — before GCC is attempted on top of it. TCC is a step, not a
substitute.

## The walls, and their state

All four were measured before anything was changed. Two were real and are
raised; one was a misreading; one was never a wall.

| wall | before | after (committed) |
|---|---|---|
| filesystem image | 64 MiB | **512 MiB** (`TOTAL_BLOCKS` 16384→131072) |
| inodes | 256, **26 free**; a full cull recovers only 100 because 69 tiny `.as` files and 41 media fixtures eat the table, and a C library's headers alone are 64 | **8192** — superblock already carried the counts, so **version stays 4**, old images mount |
| largest loadable program | "12 MiB, measured" — **that was the largest thing anybody had tried** | measured ceiling **224 MiB**; the loader now streams over `vfs_pread` and peak kernel heap went 249,856→16,384 KiB |
| signal frame vs red zone | feared | **never a wall** — `ksigframe.c:116` skips 128 bytes |

Two more silent limits found while measuring, being fixed in workflow 3:
`/bin/sh` drops the 33rd argument and truncates a 600-character line with no
message; the kernel's `copy_uvec` drops argument 49+ silently; a 60-byte
filename is accepted and invisible (`VFS_NAME_MAX` 60 vs logitfs 59).
`cc1`'s own argv for a real file is 32 tokens / 436 bytes — **already at the
shell's cap**.

## Decomposition into workflows (file ownership is disjoint)

### 1 — `port-1-tcc-target`: tcc as a LogitOS program
Owns `third_party/tcc/**`, `tests/tcc.mk`, `c/kernel/exec/{aex,elf}.*`.

- Build `tcc.aex` from the 10 x86-64 TUs (not `tccrun.c`: it is the `-run`
  JIT and the only file needing `<sys/ucontext.h>`), `CONFIG_TCC_STATIC`,
  against `$(LIBC_OBJS) $(LIBM_OBJ)` + `crt0_cli` at 0x50000000.
- **Patch the vendored defaults**: `ELF_START_ADDR` → 0x50000000. tcc's
  default 0x400000 is *shared kernel low memory* on this machine (CLAUDE.md
  M18); a default that needs `-Wl,-Ttext` to be safe is a default every user
  forgets once. Also `CONFIG_TCCDIR=/usr/lib/tcc`, sysinclude `/usr/include`,
  libpath `/usr/lib` — the paths workflow 2 installs to.
- **Teach `aex.c` to execute a bare ELF.** Today both reject sites
  (`aex.c:72`, `:276`) accept only the `AEX1` magic. tcc emits bare ELF; so
  does `ld`; so does every toolchain a port will bring. A bare ELF gets the
  AEX view synthesised (body = file, default stack hint, **no CRC — and no
  fake one**). An ELF whose entry is outside the private user region is
  refused *by name* — that is the 0x400000 case, and the kernel is the last
  line that can say so.
- Gates: `tcc -c hello.c` on the device **byte-identical** to the host tcc's
  `-c` (deterministic, needs no sysroot); a bare ELF runs without `mkaex`;
  the 0x400000 binary is refused, not faulted.

### 2 — `port-2-sysroot`: what a compiler needs on the disk
Owns `tools/mkfs.py`, `tools/mksysroot.py`, `tests/sysroot.mk`.

- `mkfs.py` gains `add_tree()` — sorted, symlink-refusing, name/path-length
  refusing, deterministic (an image must be a function of its inputs;
  `test-fs-format` and the scoreboard assume it).
- `/usr/include/**` ← mini-libc headers (`uonly/sched.h` flattens in: on the
  disk there is no kernel header to collide with); `/usr/lib/tcc/include/` ←
  tcc's own `stddef.h stdarg.h …` (its `va_arg` calls into libtcc1, so it
  must be *these*); `/usr/lib/libc.a` ← `llvm-ar` over the objects;
  `/usr/lib/crt0.o`; `/usr/lib/tcc/libtcc1.a` ← `lib/*.c` compiled for the
  target **with clang** (a sysroot that needs the compiler it serves is
  circular).
- **The header trap**: mini-libc's headers were written to be compiled by
  clang. tcc 0.9.27 has no `__int128`, no vector types, a subset of
  `__builtin_*`. Every header is parsed by the host tcc before it ships; a
  header tcc cannot parse is a sysroot that fails at the first `#include`.
- Gate: linking `hello.o` against the packed `libc.a` is byte-identical to
  linking against the build tree's objects; dropping one object from the
  archive fails the link *by symbol name*.

### 3 — `port-3-truncations`: the silent drops
Owns `sh.c`, `exec.c`, `vfs_path.*`, logitfs's name-length check.

Each fix is two parts and the second is the one that matters: raise the limit
to what a link line needs (60+ tokens, 2 KiB), **and** make exceeding it a loud
refusal that runs nothing — *a truncated `rm` is a different `rm`*. The
60-byte crack becomes one definition, owned by the on-disk format. Gates assert
the **absence** of silent success: after `touch` returns 0, `ls` must list it.

### 4 — `port-4-selfhost` (after 1–3 land)
tcc on the device compiles hello against the sysroot and runs it; then
compiles its own source; then the fixpoint above. Also measured, because
yesterday's number was ugly: tcc performs **1,055 opens for one 6,500-line
file, 892 of them failed `-I` probes**, and on LogitOS every probe is a full
VFS resolution with a disk-backed directory read. If compile time on the
device is dominated by that, the answer is a negative dentry cache in `c/fs`
— named there, not built in this line.

### Then GCC — the honest remainder
With 1–4 green the question changes from "can it fit" to "what does it still
need": binutils (`as`, `ld` — or tcc's own ELF writer as the linker), gmp,
mpfr, mpc, isl (334 symbols), and the 9 real libc names above, of which `sbrk`
and `madvise` have no cheap substitute (no `SYS_BRK`, no `SYS_MADVISE`). C++
(`cc1plus`, 38 MB, libstdc++, exceptions, RTTI) is a further step and is not
in this line.

## What this line does NOT claim

- That tcc-compiled code is as fast as clang's. It is not; tcc does no
  optimisation. The claim is *correct* and *self-hosting*.
- That the sysroot is a POSIX sysroot. It is mini-libc's surface, which is
  what it is (`flock` → `ENOSYS`, `statvfs` → `ENOSYS`, argued per file).
- That GCC is close. The measurement says what stands between here and there,
  and it is four libraries and binutils, not the kernel.
