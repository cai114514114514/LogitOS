---
title: AetherScript 2 — making the language actually ours
status: design
date: 2026-08-05
---

# AetherScript 2 — From "clox in Python clothes" to Aether's Own Language

This is a **language design document**, not an implementation spec. It fixes the
criterion for originality, names the four pillars that follow from it, and slices
the work into milestones (M27–M30) that each get their own implementation spec
later. Nothing here is scheduled until the milestone spec is written.

## 0. Honest inventory: what is borrowed today

`c/apps/as/` is ~4.9 kLOC of C plus a self-hosted compiler (`fsroot/as/lib/asc.as`).
Its lineage splits cleanly into three layers.

**Runtime core — a near-structural copy of clox** (*Crafting Interpreters*):

| Element | Where | Status |
|---|---|---|
| `Value{type, union{i,f,obj}}`, `Obj{type,marked,next}` intrusive alloc list | as.h:20-47 | clox verbatim |
| Single-pass Pratt parser, `ParseRule{prefix,infix,prec}`, `parse_precedence` | compiler.c:169-179 | clox verbatim (same names) |
| `Compiler{enclosing, locals[256], scope_depth, upvalues[256]}`, `Local{name,depth,is_captured}` | compiler.c:15-26 | clox verbatim |
| Upvalues: open/closed + list sorted by descending stack address | as.h:66-71 | clox verbatim |
| Copy-down inheritance (`OP_INHERIT` copies parent methods) | as.h:91-94 | clox verbatim |
| Mark-sweep with a gray worklist; `OP_INVOKE` fast path | as.h:217-224, as.h:154 | clox verbatim |

**Surface syntax — Python.** INDENT/DEDENT (lexer.h:9-11), `def`/`elif`/`for x in
range()`/`.append()`, f-strings, comprehensions, `import`/`from`. compiler.c:172
literally annotates the bitwise precedence tier as "Python order".

**Genuinely ours.** A3 indirection (`ObjPtr` typed pointers, `syscall`,
`peek/poke` — as_native.c), the `.la` container + `AS_BC_VERSION` gate
(as.h:166, as_bc.c), and the `asc.as` self-hosting chain with its byte-identical
fixpoint proof (2026-06-10-m21-p3-selfhost.md).

Summary: **the semantic core is Lox, the skin is Python, and only "it can touch
hardware" is ours.**

## 1. The criterion (this document's one load-bearing decision)

Renaming `def` to `fn`, or swapping indentation for braces, is **not** originality
— it is a reskin, and it would make the language resemble someone *else's*
braces. We adopt a single test instead:

> **A construct earns its place only if it falls out of a constraint or an
> opportunity that is specific to Aether OS.**

Every proposal below is judged by it, and so is every future one. The corollary
is the operational definition of success:

> **An AetherScript 2 program should be impossible to port to CPython or Lua on
> Linux without rewriting its core** — not because we obfuscated it, but because
> it is built out of abstractions only an OS author can offer.

The leverage that makes this possible: every other scripting language must treat
the OS as a black box and reach it through the POSIX least common denominator.
**We own both sides.** We can add a syscall, add a PCB field, change the loader.

Today's `as_native.c` does the exact opposite — it injects ~40 `SYS_*` integers
as globals and makes scripts hand-marshal (`syscall(SYS_PIPE, addr(buf))`,
lib/sys.as:60-68). That is the language reaching for the OS through a keyhole
*we ourselves cut*. Reverse the direction.

## 2. The four pillars

### P1 — Ports: OS objects as first-class values

**Constraint it answers.** Aether has real processes, pipes, an fd table
(proc.h:24), a window manager, and an event queue — yet at the script level they
are all bare integers passed to `syscall()`. Resource lifetime is manual
(`alloc`/`dealloc`, lib/sys.as:12-18) and fd hygiene is a known hazard (CLAUDE.md
records the `wm_launch` fd 0/1/2 collision).

**The abstraction.** A **port** is a typed, owned, kernel-backed endpoint. Unix
said "everything is a file" and handed you an integer; we say **everything is a
port** and hand you a value that knows its own type, its own capability (P2), and
its own lifetime. Files, pipes, child-process stdio, window event queues, sockets,
and timers are all ports.

- New object types: `O_PORT` (kernel handle + kind + cap), `O_PROC`, `O_REGION`.
- Ports are **iterable** (reuse `OP_ITER`, as.h:156) — `for line in f:`,
  `for ev in win.events():`.
- **Deterministic release**, not GC-timed: a port closes when its owning scope
  ends. GC finalization is the backstop for leaks, never the primary path.
- Pipelines are a language value, not string soup:
  `run("ls","/usr") |> run("grep","as") -> "out.txt"` builds a pipeline object
  and executes it with one fork/dup2/exec pass.

**The payoff that proves it.** AetherScript replaces the hand-written C `/bin/sh`
(c/apps/coreutils) as the system shell and as `init`'s script language. A shell
is the hardest possible test of "OS objects as values"; passing it is not a demo.

### P2 — Capabilities and bounded memory

**Constraint it answers.** `syscall()` is an unguarded global and `peek64(addr)`
faults on any wrong address. Every `.as` script today is effectively root, on an
OS that runs a browser, a TLS stack, and untrusted network content.

**The abstraction.** A **capability** is an unforgeable language value
(`O_CAP`). It cannot be constructed by script code — only

1. **granted by the kernel** at `execve` from a per-process set, or
2. **attenuated** from a capability you already hold
   (`c2 = c.fs.scope("/usr")` — strictly weaker, never stronger).

Kernel side this is small and centralized: a `caps` bitmap plus a path-prefix
table on `struct proc` (proc.h:17-27), checked at the top of `syscall_dispatch`
(syscall.c:53). Language side, every port-opening operation consumes a
capability. Raw `peek`/`poke` survive but require `CAP_RAW`, which the default
grant withholds.

**Bounded memory.** `region(n)` replaces `alloc(n)`: an `O_REGION` carries its
length, `r[i]` and the slice `r[a:b]` are bounds-checked, and a violation raises
a catchable language error (M22.4 `try/except` already exists) instead of a #PF.
`addr()` on a region is itself `CAP_RAW`-gated.

This is the pillar that is **structurally impossible to accuse of being borrowed**,
because it does not live in the language alone — it is a contract co-designed
between `c/apps/as` and `c/kernel/exec`.

### P3 — Tasks: the VM and the scheduler, joined

**Constraint it answers.** A blocking syscall blocks the whole thread; the VM has
exactly one `Frame` stack (vm.c:12). GUI apps are therefore poll loops.

**The abstraction.** A **task** is a VM-level coroutine with its own frame and
value stack. Any port operation that would block yields to the VM scheduler
instead. Concurrency is **structured**: tasks are spawned into a scope that
cannot exit until they finish, and a task's error propagates to the scope owner.
When every task is blocked, the VM makes one kernel call to wait on the whole
port set — which means **a new multi-handle wait syscall, designed for the
language.** Language-driven kernel work again.

### P4 — Our own IR, and native `.aex` output

**Constraint it answers.** Single-pass Pratt straight to bytecode is clox's
defining trait and its ceiling: with no AST there is no constant folding, no dead
code elimination, no type inference, no register allocation.

**The abstraction.** Insert AST → **AetherIR** → codegen. `.la` stays as the
portable artifact. The prize is `as -c --native`, emitting a real **`.aex`**
ring-3 binary — legitimate precisely because we own the ABI, the ELF/aex loader
(`kernel/aex.c`), and the page tables. Note the sequencing: switching to a
register VM would just be borrowing Lua instead of Lox; going native is the move
only an OS author can make.

## 3. Syntax: what changes, and what deliberately does not

Each addition below is required by a pillar. Nothing changes for aesthetics.

| Addition | Pillar | Why it cannot be a library call |
|---|---|---|
| `\|>` pipeline operator | P1 | Needs compile-time knowledge of both stages to wire fds before either runs |
| `-> path` / `<- path` redirect | P1 | `>` is taken by comparison; new `T_ARROW`, `T_LARROW` tokens |
| `with r = port(...):` scope | P1 | Deterministic close needs a compiler-emitted scope-exit opcode |
| `grant` / capability attenuation | P2 | Unforgeability requires the value to be VM-constructed only |
| slice `r[a:b]` on regions | P2 | Bounds live in the object; needs `OP_SLICE`, not a native |
| `task` / task scope | P3 | The compiler must know a suspension point to split frames |

**Explicitly unchanged**, because no pillar requires it and changing it would be
pure reskin: indentation-based layout, `def`, `class`/`super`, f-strings,
comprehensions, `import`/`from`, operator precedence. Python-shaped surface
syntax on Aether-shaped semantics is a *choice*, and we state it as one: the
reader's first five minutes should be free, so the novelty budget is spent on
ports, capabilities, and tasks.

## 4. Migration reality

- **The self-hosting tax is real.** `asc.as` compiles itself to a byte-identical
  fixpoint. Every opcode add or reorder now means editing *two* compilers and
  bumping `AS_BC_VERSION` (as.h:166). Therefore: **batch changes per milestone**,
  one version bump each, and re-run the fixpoint gate at every milestone's end.
  No opportunistic opcode additions between milestones.
- **The clox core is an asset, not a debt.** GC, closures, classes, `.la`, and
  self-hosting all work. The strategy is to **grow an original layer on top**
  (new object types, new opcodes, new kernel contracts), never a rewrite.
- **`lib/sys.as` and `lib/gui.as` become compatibility shims.** They keep working
  over `syscall()` while the built-in port types take over; they are deleted only
  when nothing imports them.
- **Kernel changes stay narrow**: `caps` on `struct proc`, one check at
  `syscall_dispatch` entry, one multi-handle wait syscall (P3). No new subsystem.

## 5. Milestones

| M | Name | Contents | Gate |
|---|---|---|---|
| **M27** | Ports | `O_PORT`/`O_PROC`, `\|>`, `-> / <-`, `with` scopes, iterable ports | `.as` shell runs the `test-shell` suite that `/bin/sh` passes today |
| **M28** | Capabilities | `O_CAP`, kernel grant set, `syscall_dispatch` check, `region` + slices, `CAP_RAW` gating | A script without `CAP_FS` provably cannot read `/etc`; every OOB access raises instead of faulting |
| **M29** | Tasks | Per-task frame/value stacks, VM scheduler, structured task scope, multi-handle wait syscall | A GUI app written as `for ev in win.events():` with a concurrent network fetch, no poll loop |
| **M30** | AetherIR | AST + IR + folding/DCE, `as -c --native` → `.aex` | A `.as` program runs as a native ring-3 process, Dock-launchable, no VM |

Each milestone ends with: `make test-as` + `make test-as-gcstress` +
`make test-as-os` green, **and** the `asc.as` self-compile fixpoint re-proved.

## 6. When we get to call it ours

Three concrete tests, all objective:

1. **`/bin/sh` is AetherScript**, and `init` boots through a `.as` script.
2. A representative AetherScript 2 program **cannot be ported to CPython/Lua on
   Linux without a rewrite**, because ports, capabilities, and structured tasks
   have no counterpart there.
3. The language's three load-bearing abstractions — **port, capability, task** —
   appear in neither clox nor Python. What remains shared with clox (a mark-sweep
   GC, a stack VM) is shared the way every OS shares paging: it is the correct
   engineering, not the identity.

## 7. Non-goals (locked)

- No rewrite of the existing VM/GC/class machinery.
- No syntax churn for its own sake (see §3's unchanged list).
- No static type system. Optional annotations may return with M30's IR if — and
  only if — they earn codegen wins; they are not an identity project.
- No register-VM conversion. It trades one borrowed design for another; M30 goes
  native instead.
- No package manager, no async/await keyword pair (P3 is structured, not
  future-based), no FFI beyond the existing `CAP_RAW` path.
