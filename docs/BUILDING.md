# Building LogitOS

`make` builds the kernel ISO. Applications are on the separate disk image;
build both before checking a change spanning the kernel and userland:

```sh
gmake all build/disk.img
gmake run
```

GNU Make 4.4 or newer automatically uses up to 12 jobs, bounded by the host's
CPU count, for ordinary product builds. `JOBS=6` changes that default. Explicit
`-j1`, `-j6`, inherited `MAKEFLAGS`, and recursive jobservers take precedence.
Test targets and mixed product/test invocations retain their existing scheduling
because several older harnesses share output paths.

Apple's bundled Make 3.81 and GNU Make before 4.4 retain their original parallel
settings. Their parse-time `MAKEFLAGS` cannot reliably distinguish plain `make`
from an explicit `make -j1`, so guessing would override the caller's request.
Use an explicit job count with those versions:

```sh
make -j12 all build/disk.img
make -j12 run
```

## Independent builds and cache

Pass `BUILD` as a Make command-line assignment, not an environment prefix:

```sh
gmake BUILD=build-check all build-check/disk.img
```

When `ccache` is installed and on `PATH`, product C compilation uses it
automatically. The compiler remains `CC`; linking and host probes keep their
normal commands. Without ccache, the same build runs directly through `CC`.

The default cache is `.cache/ccache`, outside every build directory. Both
`make clean` and `make clean-scratch` leave it available for the next build.
Set `CCACHE_DIR` to use another cache, or pass `CCACHE=` to disable caching.
Cache population is gradual: a first compilation with new sources or flags
still needs to run the compiler. Inspect the project cache with:

```sh
CCACHE_DIR="$PWD/.cache/ccache" ccache --show-stats
```

## What causes recompilation

Product C objects use the compiler's `.d` files to track the headers and text
fragments they actually include. A missing `.d` forces its object to rebuild;
keeping an old object without its dependency file cannot silently disable
header tracking. Multi-source host probes retain their broader dependency lists.

Product compiler domains also track the compiler identity and their configured
flags in `BUILD/.config/`. Kernel, userland, browser, Preview and other variants
are distinct: a browser option does not invalidate unrelated kernel objects.
The configuration check is read-only during Make parsing, so `make -n` does
not consume a requested flag change. Interrupted and partial builds retain the
outdated objects' rebuild requirement, including on Make 3.81, which compares
file times at whole-second precision on this host.

Studio, Preview and Terminal compile their main source as separate object
targets. Changing a linked library therefore requires another link, without
also compiling an unchanged main source.

## Build-system checks

```sh
make test-build-incremental test-build-config test-build-jobs test-mk-wired
```

Each build-mechanics gate runs its negative control first. The checks exercise
real compiler dependency files, partial builds, compiler/flag changes, failed
compiles and Make's jobserver. They measure rebuild decisions and actual worker
concurrency; they do not claim a fixed wall-clock speedup or guest runtime gain.

Disk packing still preserves user directories, validates a private snapshot,
rejects images held by QEMU, and replaces the image atomically. Compilation
parallelism does not make it safe to write a disk used by a running VM.

The packer and its checked snapshot now leave unused zero blocks as holes in
the host file. The raw disk remains 512 MiB to the guest, with the same bytes
at every offset. On the 2026-09-23 image, 19,285 blocks were used and the host
allocated about 75 MiB instead of 512 MiB. Both write paths retain `fsync`;
the snapshot still runs journal recovery and filesystem validation before any
new image is accepted. Run `make test-disk-sparse test-snapshot-sparse
test-disk-profile test-fs-format` to check byte identity, disk allocation,
preserved state and the on-disk format. See the
[measurement](reports/build-speed-2026-09-23.md) for the timing boundary.
