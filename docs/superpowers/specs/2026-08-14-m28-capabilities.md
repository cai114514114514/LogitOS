---
title: M28 — Capabilities, and the grant chain the machine actually admits
status: design (implementation spec)
date: 2026-08-14
supersedes: the M28 row of 2026-08-05-aetherscript-2-language-design.md §5
---

# M28 — Capabilities

This is the implementation spec for the language-design document's second pillar.
It **locks the opcode batch**, because `asc.as` compiles itself to a byte-identical
fixpoint and the design document forbids opportunistic opcode additions between
milestones. Everything below is a decision with the reason attached; where a
decision departs from the design document, it says so and argues it.

Six read-only surveys of the tree preceded this. Their most useful product was not
the confirmations — it was one finding that **invalidates the design document's
stated grant model**, and it is §1.

## 0. A standing note about line numbers

The design document was written on 2026-08-05 and cites `syscall.c:53` as the
place a capability check belongs. **Nine days later that line is inside an
unrelated `kheap_stress()` helper**; `syscall_dispatch` now begins at
`c/kernel/exec/syscall.c:96` and its real insertion point is around `:135-137`.
`proc.h:17-27` still bounds `struct proc`, but contains none of the fields the
document describes there.

Every citation below was re-derived against the tree on 2026-08-14 and will drift
too. Re-locate before editing; do not trust a line number in a document over the
code.

## 1. The finding that breaks the stated model

The design document says a capability is *"granted by the kernel at `execve` from
a per-process set."* On this machine that sentence has no referent.

**Every AetherScript program executes the same binary.** `c/apps/as/as.c`'s `main`
is the one entry point, and the kernel only ever observes
`execve("/bin/as", ["as", "<script>.as"], envp)`. Fifty `.as` files in
`fsroot/as/` share one exec path. A grant keyed on the executed binary therefore
**cannot distinguish two scripts**, and — the part that matters — it does not fail
when it can't: it silently hands every script whatever `/bin/as` itself holds,
which is the entire per-script model defeated with no error anywhere.

The obvious repair is worse. `execve`'s `argv` and `envp` are copied verbatim from
the *calling* process's syscall registers (`c/kernel/exec/exec.c:59-74, 245-251`),
so any grant riding on them is **caller-forgeable**, which is precisely the
property a capability may not have. `auxv` is kernel-written and therefore not
forgeable — but `crt0_cli.asm` never reads the 14 `AT_*` pairs the kernel already
pushes, so a grant put there would be silently read as whatever is at that stack
offset. Neither channel identifies *which script*, only *which binary*.

### 1.1 D1 — the parent attenuates; the kernel enforces the ceiling

**Locked.** The grant does not come from the binary. It comes from the parent's
own held set, narrowed at spawn time, and the kernel's only rule is:

> **A child's set must be a subset of the caller's set. Never a superset. The
> kernel checks the inclusion and nothing else.**

- The chain's root is `proc_spawn` (`c/kernel/exec/exec.c:309`), where the kernel
  itself launches init's shell. That process holds the full set **by
  construction** — it is the kernel granting, which is what the design document
  asked for, at the one place where the phrase is meaningful.
- Everything below narrows. `ash.as` running a script hands it a subset; a script
  spawning a child hands *it* a subset.
- `/bin/as` being one binary stops mattering, because identity was never what was
  being checked. **Monotone decrease is checkable without knowing who anyone is.**

This is ~20 lines of kernel: a bitmap AND plus a prefix-containment test. It needs
no policy table, and there is no analog for a policy table in this tree to borrow
from anyway. It is also the design that survives M30, when a `.as` program becomes
its own `.aex` and the per-binary story would have started working — because by
then the chain is already right and nothing has to change.

**Consequence, stated plainly:** an attenuation is only as good as the fds already
open across it. See D10.

## 2. The object model

### 2.1 D2 — there is no `O_REGION`. `region()` returns an `ObjBuf`.

**Locked, and this departs from the design document**, which names `O_REGION` as a
new object type.

`O_BUF` already exists (`as.h:60`), already carries `nbytes`, is already GC-owned,
GC-accounted and finalized, and `as.h:198-209` **already documents it as the
length-carrying replacement for `alloc`/`dealloc`**. `op_LEN` already has a working
`O_BUF` arm (`vm.c:1127`), so `len(buf)` works today.

The only real gap, verified by reading it: **`op_INDEX_GET` and `op_INDEX_SET`
contain zero `IS_BUF` branches** (`vm.c:1059-1120`). A buffer cannot be indexed.
That is one missing arm in each of two opcodes — not a new object type.

Adding `O_REGION` beside `ObjBuf` would grow a second bounded, GC-owned raw-memory
type doing an overlapping job through a different code path. That is the same
mistake Open Logit exists to prevent, and this codebase has a name for it: a
fourth path. `region(n)` is a name for `buffer(n)` with the bounds contract stated;
`ObjBuf` grows nothing but arms.

**Element width is one byte.** It matches `alloc(n)`'s byte count and the byte view
`mem2str`/`mem2cstr` already take. Typed sub-views remain `ObjPtr`'s job, which is
`CAP_RAW`-gated and unbounded by design (D6's note).

### 2.2 D3 — slices are read-only views, and they hold their parent

`r[a:b]` yields a new `ObjBuf` **viewing** the parent's bytes, with a strong GC
edge back to the parent so the parent cannot be swept out from under a live slice
(the shape `ObjBoundMethod` already uses to hold its receiver). A copy would be
the safe-looking choice and is wrong: it makes `r[0:n]` on a large buffer an
allocation, which is exactly what a script slices to avoid.

**Slice assignment (`r[a:b] = ...`) is NOT in M28.** Reason, and it is the
self-hosting tax: `compiler.c` has **two** bracket-index sites — the rvalue path
(`:502-507`) and the lvalue chain (`:838-864`) — each mirrored in `asc.as`. A
slice-lookahead handled in only one produces a language where read-slicing and
slice-assignment silently disagree. Read-only halves the surface and defers the
divergence risk to a milestone that can spend its whole batch on it.

## 3. The two boundaries, and which is which

### 3.1 D4 — the VM is the TCB for `CAP_RAW`; the kernel is the TCB for the rest

`peek`/`poke` do not reach the kernel. `as_ll_peek`/`as_ll_poke` (`as_ll.c:10-30`)
are plain `volatile` dereferences in the process's own address space. **There is no
syscall to intercept**, so "checked at the top of `syscall_dispatch`" cannot
enforce `CAP_RAW` at all, and a design that assumes it does leaves the pillar's
headline capability unenforced.

So `CAP_RAW` is enforced **per call, in `as_native.c`'s `peek_w`/`poke_w`** — not
lower, because `as_ll_*` is real on the host too and an ungated test against
`peek8` SIGSEGVs the host binary instead of printing a clean FAIL; and not once at
registration, because a check that runs at `as_install_indirection()` time passes
once and then permits every access for the rest of the run.

State the honest consequence: **`CAP_RAW` bounds what a script may do; it does not
bound a script that has already subverted the VM.** It is effective because a
script cannot execute machine code — `peek`/`poke` are the only escape from the
VM's control — so gating them is exactly as strong as the VM is correct. Anything
kernel-mediated has a real boundary underneath it. This one does not, and saying
so is part of the design.

### 3.2 D5 — `syscall()` is gated as hard as `peek`

`syscall(n, ...)` (`as_native.c:107-198`) reaches `open`, `pipe`, `execve` and
`fork` — everything the five port constructors reach — with **no port and no
capability involved**. A check placed only in `as_port.c`'s natives is bypassable
in one line.

Worse for any table-driven approach: `syscall()` takes a bare integer, and a script
can supply one that has no symbolic name anywhere in the tree
(`fsroot/as/examples/setcheck.as:19-21` hardcodes `103` for exactly this reason).
Of the kernel's 137 syscalls only 60 are named in `as_native.c`. **A capability
audit built from the AetherScript side systematically undercounts the surface**,
which is why the kernel check is the boundary and the language check is a
convenience that makes failures catchable.

`syscall()` therefore requires `CAP_RAW`. It is the universal bypass of every
language-level check, and it is classed with the other universal bypass.

### 3.3 D6 — the category bit at dispatch; the path prefix after resolution

Two checks, in two places, each for a stated reason:

- **The category bit** (`CAP_FS` at all, `CAP_NET` at all) at `syscall_dispatch`
  (`syscall.c:~135`, after `sched_tlb_gen_check()` and before `syscall_do()`). One
  place, one table lookup.
- **The path prefix** *after* symlink resolution, not at the syscall gate.
  `proc_resolve()` (`proc.c:232-261`) is **lexical only and symlink-blind**; a
  prefix check on its output looks correct, passes every obvious test, and is
  silently bypassable by a symlink whose real target escapes the prefix. And
  `SYS_SYMLINK` stores its target completely unresolved by design
  (`meta.c:227-230`), so a scoped process can plant the escape hatch itself.

This contradicts the design document's "one check at `syscall_dispatch`". The
document is wrong on this point, and the reason it is wrong is worth keeping: a
check placed where the path is cheap to read runs before the path means anything.

**Two guards inherited from the neighbouring code must NOT be copied.** The
existing kill-check gates carry `!syscall_is_bkl_free(...)` (`syscall.c:109,118`);
cloning that shape exempts `SYS_KHEAP_STRESS` from capability enforcement without
anyone deciding to. And `SYS_SIGRETURN` is intercepted at `interrupts.c:161`, one
line *before* `syscall_dispatch` is called, so it structurally never reaches the
gate — fine today, and it must be written down or it becomes a hole the moment
something is added beside it.

### 3.4 D7 — capabilities are orthogonal to uid, and root does not bypass

`vfs_cred.c` gives root a blanket bypass of every uid/gid check. Capabilities do
not compose with that. A capability bounds a **program**, not a **user**, and a
root bypass makes the system decorative for exactly the process most worth
bounding — init's own shell. Locked: enforcement applies at every uid.

## 4. The opcode batch, locked

One opcode. **`OP_SLICE`**, appended at the end of the enum.

Everything else M28 needs is a native call and costs the self-hosting tax nothing:
`region(n)` is shaped exactly like today's `alloc(n)`, and attenuation
`c.fs.scope("/usr")` is a method call on an object — the design document's syntax
table lists both loosely, and neither needs a token, a `ParseRule`, or a keyword.

Bounds-checked `r[i]` needs **no** new opcode: it is an `IS_BUF` arm in
`op_INDEX_GET` and one in `op_INDEX_SET`.

`AS_BC_VERSION` goes 4 → 5. Once, at the end.

### 4.1 The colon, which is the subtle one

`T_COLON` already means different things at ~5 independent parsing sites (block
opener for `if`/`while`/`def`/`class`/`try`/`with`/`for`, and dict `key:value`)
with no shared dispatch. `r[a:b]` adds a sixth **inside brackets**. The lookahead
must be written once and used by both bracket sites, even though M28 only needs
the rvalue one — because the lvalue site exists and will be reached in M29.

### 4.2 D8 — close the table nothing checks, while the enum is already open

`vm.c`'s computed-goto `dispatch[]` (`vm.c:813-829`) is positionally matched to the
`OpCode` enum **by hand**, with no sentinel, no `_Static_assert`, and no coverage
from `check-asops`. The enum has no `OP__COUNT` at all. A label in the wrong
position compiles cleanly and **silently misdispatches every opcode after it**.
The table already contains `&&op_BAD /* OP_GET_ATTR: no handler */` — a deliberate
hole held in place purely by hand-counting.

Meanwhile `object.c:321` has exactly the protection this table lacks, for
`ObjType`, and it is the only `_Static_assert` in the whole of `c/apps/as`.

M28 adds `OP__COUNT` and the matching `_Static_assert`, and teaches
`tools/gen_as_opcodes.py --check` to read `dispatch[]`. In scope because the enum
is being opened anyway, and because this is the one hand-mirrored table the
existing check cannot see.

**Not in scope:** implementing `gen_as_opcodes.py --write`. Its `--write` mode is a
stub that exits (a fact its own docstring contradicts, along with claiming
`AS_BC_VERSION` is frozen at 3 when it is 4). A tool that edits two compilers
automatically is larger and riskier than the edit it saves; M27 added five opcodes
by hand. Extending `--check` is the better spend.

## 5. D9 — where the held set lives, and what the host gets

There is no `g_caps` analog in `vm.c` today. The set is C state set once at
`as_run()` startup, the same shape as `as_set_args()`, reachable from both opcode
handlers and natives.

**On the host the default is DENY.** Not "grant everything" — that would leave
every existing host test running ungated and the gate itself untested. A host test
grants explicitly, so a test that forgets to fails closed. This is also the answer
to "how does a host test obtain a capability at all": through a C entry point, not
through script code, which preserves unforgeability precisely because script code
cannot reach it.

**Unforgeability via `.la` is already free**, and it is worth recording why so that
nobody "completes" it: `as_bc.c`'s loader tag enum (`K_NIL..K_FN`, `as_bc.c:29`) is
closed and can never produce a capability constant. Adding a `K_CAP` tag to "finish
serialization support" would silently make capabilities forgeable by hand-writing a
`.la` file.

## 6. D10 — fd inheritance, or attenuation is theatre

`proc_start` (`as_port.c:328-350`) does `fork` + `execvp` with **no `O_CLOEXEC` and
no close-others pass**, and no explicit `envp`. Every fd the parent holds rides into
every child spawned by `run()` / `|>` / `with p = run(...)`, whatever the child's
own grant says. Containment can look correct at the `open()` call site and be void
at the process boundary.

In scope: close everything not explicitly wired. **Risk, named:** `test-as`'s
`proc_chain_survives_gc` and `redir_both`, and `ash.as`'s coreutils children, may
rely on inheritance. Those tests are the gate for this change, not an obstacle to
it — if they fail, the inheritance was load-bearing and that is a finding.

Two more provenance holes from the same survey, both in scope because each defeats
a check that would otherwise look present:

- `port(fd)` (`as_port.c:432-439`) wraps **any** bare integer 0..65535 with no
  provenance check, and infers its `kind` from the fd number alone
  (`fd<=2 ? PK_TTY : PK_FILE`, `:437`). A capability class chosen from `kind` is
  spoofable by choosing an fd number. Class borrowed ports off `owns == 0` and the
  kernel's answer, never off the language's guess.
- Capability bookkeeping added at `open()` must be mirrored at **every** release
  path — `as_value_release`, `as_port_close`, `as_port_drop`, `fin_port`,
  `fin_proc` (`as_port.c:78-141`, `object.c:280-287`) — or a raise inside a `with`
  skips whatever was only added to the normal exit. Release paths may never raise,
  so the bookkeeping there is best-effort by construction.

## 7. Gates

Each milestone ends with `test-as` + `test-as-gcstress` + `test-as-os` green **and**
the `asc.as` fixpoint re-proved. M28 adds:

| Gate | Asserts | Modelled on |
|---|---|---|
| `test-as-cap` (host) | attenuation is monotone: no derived set exceeds its parent, over a generated lattice of sets and prefixes | `test-as` |
| `test-as-cap-negctl` | `-DAS_CAP_NO_CHECK` — a denied script reads `/etc` and the target **must fail** | `test-as-port-negctl` |
| `test-as-region-negctl` | `-DAS_REGION_NO_BOUNDS` — an OOB index does not raise and the target **must fail** | `test-as-port-negctl` |
| `test-as-os` addition | a script spawned without `CAP_FS` cannot read `/etc`, on the real kernel | `run-as-test.sh` |
| `check-asops` extension | `dispatch[]` matches the enum | `check-asops` |

**A harness trap that will otherwise eat the headline test:**
`run-selfhost-compile.sh` **silently SKIPs** — does not fail, does not count — any
example whose direct interpretation errors. A capability-denial example is supposed
to error. It would be dropped from the byte-identical comparison and read as a
pass. The harness must be taught to expect that one, explicitly.

And: `check-asops` / `check-abi` are prerequisites of nearly every `test-as*`
target **but not of `test-ash`** (`test-ash: $(ASC)` only), and of neither `all`
nor `$(ISO)`. A plain `make run` boots happily with a badly drifted `asc.as`.

## 8. Explicitly out of scope, with the leak named

Not "we didn't think about it" — decided, so that the gap is on the record:

- **`SYS_PROCS`** returns every live process's pid, ppid, name and cwd, ungated. A
  capability-confined script can still enumerate the whole process table.
- **Clipboard (`CLIP_*`) and notifications (`NOTIFY`)** are existing, ungated
  cross-process channels. **A filesystem-scoped script can exfiltrate through the
  clipboard regardless of its `CAP_FS` prefix.** This is true today and stays true
  after M28.
- **`SYS_SETTING_SET`** changes machine-wide persistent configuration from any
  process with no gate.
- **`ObjPtr` stays unbounded.** It is `CAP_RAW`-gated and that is its whole
  contract; retrofitting bounds onto it is a different milestone.
- **`alloc`/`dealloc` are not deleted.** They become `CAP_RAW`-gated legacy beside
  `region()`. Their footprint is one script and five host tests; deleting them is a
  migration, and migrations do not belong in the batch that changes the opcode set.
- **Revocation.** A capability is checked at acquisition. Nothing revokes one
  mid-run, and the GC finalizer backstop means a port opened under a since-narrowed
  grant keeps its fd until collection. Deterministic revocation needs a synchronous
  sweep and is not M28.
