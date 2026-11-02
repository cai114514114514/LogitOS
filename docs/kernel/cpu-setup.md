# CPU setup

LogitOS establishes floating point and protection state before entering the C kernel, then installs descriptor tables for interrupts and privilege transitions. Each CPU has its own Global Descriptor Table (GDT) and Task State Segment (TSS), while all CPUs load a shared Interrupt Descriptor Table (IDT). CPUID supplies capability and platform information; register readback reports which protections are active on the executing CPU.

## Initialization order

The bootstrap processor (BSP) enters `logit_native_start` in `c/boot/long.asm` in 64-bit mode. It switches to a kernel stack, configures CPU state, and calls `cpu_early_init` before entering `kernel_main`.

| Stage | Work |
|---|---|
| `c/boot/long.asm` | Establish x87 and SSE state and enable supported NX and SMEP protections. |
| `c/crypto/cpu_report.c` | Initialize the feature cache, select the AES-GCM implementation, initialize serial output, and report CPU information. |
| `c/kernel/init/kmain.c` | Call `idt_init`, `gdt_init`, `pic_remap`, and `pit_init`, then run the SIMD self-test. After physical memory initialization, configure LAPIC virtual wire delivery and platform clocks. Initialize PAT and machine check handling before framebuffer setup. |
| `c/kernel/cpu/smp/smp.c` | Calibrate the LAPIC timer and initialize BSP power policy before starting application processors (APs). |
| `c/boot/ap_trampoline.asm` | Establish paging and floating point and protection state on each AP, then enter C. |
| `c/kernel/cpu/smp/smp.c` | Initialize the AP’s local APIC, load the shared IDT, construct its GDT and TSS, initialize cache and power settings, capture topology, arm its timer, and publish readiness. The BSP then admits the CPU to the online count. |

The assembly setup makes floating point available to C code. Early feature detection fills the BSP’s capability cache before AP startup.

## GDT and TSS

`gdt_init` in `c/kernel/cpu/gdt.c` calls `percpu_bsp_init`. Descriptor construction lives in `c/kernel/cpu/smp/percpu.c`; APs use the same construction through `percpu_ap_init`. Each `struct cpu` in `c/kernel/cpu/smp/percpu.h` embeds a GDT, its register image for the descriptor table, and a TSS.

The selectors are defined in `c/kernel/cpu/gdt.h`:

| Selector | Value | Descriptor |
|---|---:|---|
| `SEL_KCODE` | `0x08` | Kernel code, privilege level 0, 64-bit mode |
| `SEL_KDATA` | `0x10` | Kernel data, privilege level 0 |
| `SEL_UCODE` | `0x1b` | User code, privilege level 3, 64-bit mode |
| `SEL_UDATA` | `0x23` | User data, privilege level 3 |
| `SEL_TSS` | `0x28` | TSS descriptor occupying two GDT slots |

`gdt_flush` in `c/boot/gdt_flush.asm` loads GDTR and reloads the data segment selectors. CS holds selector `0x08`, the kernel code descriptor used during boot. `tss_flush` loads the task register with `ltr`.

The TSS supplies `rsp0` for interrupts and traps that transition from ring 3 to ring 0. Construction initializes it to the top of a 16 KiB stack assigned to that CPU. During scheduling, `c/kernel/sched/sched.c` calls `percpu_tss_set_rsp0` with the incoming thread’s kernel stack top before switching contexts.

Construction also clears the TSS, sets its descriptor limit to `sizeof(struct tss) - 1`, and sets `iomap_base` to `sizeof(struct tss)`. The I/O permission bitmap offset lies beyond the descriptor limit.

The loaded GDT identifies the executing CPU. `this_cpu` reads GDTR with `sgdt`, derives an index into `g_cpus` from the table address, and verifies the resulting address. Early setup falls back to the BSP entry. The comment in `c/kernel/cpu/smp/percpu.c` records why this uses GDTR: LAPIC MMIO reads had proved unreliable in the `int 0x80` syscall path under QEMU TCG.

## IDT and interrupt entry

`idt_init` in `c/kernel/cpu/irq/idt.c` constructs the installed entries in a table containing 256 slots and loads IDTR. APs call `idt_load` to load the same table address.

The gates installed by `idt_init` use kernel code selector `0x08`. Vector 128 has descriptor privilege level 3, allowing user programs to invoke `int 0x80`; the remaining installed gates have privilege level 0.

| Vectors | Entry purpose |
|---|---|
| 0–17, 19–31 | CPU exception stubs |
| 18 | Dedicated machine check entry |
| 32–47 | Remapped legacy hardware IRQs |
| 65 | Legacy e1000 interrupt stub |
| 128 | System call entry |
| 240 | TLB shootdown IPI |
| 241 | Parallel presentation IPI |
| 243 | Compositor wakeup and device vector cleanup IPI |
| 255 | Local APIC spurious interrupt |

Vector 242 is installed for the delivery probe when `LOGIT_RAPTOR_SMP_QEMU_TEST` is defined.

The common entry path in `c/boot/isr.asm` saves general registers and creates a 512-byte FXSAVE image aligned to 16 bytes beneath the register frame. It passes both frame addresses to `interrupt_handler`, then restores floating point state with FXRSTOR before returning through `iretq`.

The saved image also supports signals. `c/kernel/cpu/irq/interrupts.c` passes it to signal handling, and `c/kernel/exec/signal/ksigframe.c` copies it into the user signal frame. Signal return copies the saved state back into the image consumed by the interrupt epilogue.

Subsystems install additional gates through `idt_install_gate`. The device IRQ layer in `c/drivers/core/irq.c` installs its gates by obtaining the shared table address with `sidt`. Its entry stubs also preserve floating point state around C callbacks.

Device vector retirement drains active dispatches and requires every CPU in the online snapshot to check its local LAPIC pending and in-service bits before reuse. Dispatch remains pinned through end of interrupt. Vector 243 requests cleanup on other CPUs, and further cleanup follows completion of an old dispatch. These checks protect a new vector owner from a delayed delivery belonging to the previous device.

## Floating point state

The BSP and AP assembly establish these settings in `c/boot/long.asm` and `c/boot/ap_trampoline.asm`:

| Register setting | Purpose |
|---|---|
| Clear `CR0.EM` | Enable hardware floating point execution |
| Set `CR0.MP` | Configure floating point wait behavior |
| Set `CR0.NE` | Use native x87 exception delivery |
| Set `CR4.OSFXSR` | Enable SSE and FXSAVE/FXRSTOR support |
| Set `CR4.OSXMMEXCPT` | Enable SIMD exception delivery |
| Clear `CR4.OSXSAVE` | Establish the state contract used by the FXSAVE interrupt ABI |

Both paths execute `fninit` and load MXCSR with `0x1F80`, masking SIMD exceptions. Clearing inherited OSXSAVE state makes the interrupt state contract consistent across firmware handoffs and CPUs.

AES-NI and PCLMULQDQ operate on XMM registers covered by this save format. `crypto_simd_init` in `c/crypto/aead/aes_dispatch.c` selects the AES implementation. The accelerated backend in `c/crypto/aead/aes_ni.c` requires both instruction capabilities; the portable backend supplies the fallback.

## Protection features

The boot assembly checks CPUID leaf availability and capability bits before enabling NX and SMEP.

| Feature | Capability check | Enabled state |
|---|---|---|
| NX | Leaf `0x80000001`, EDX bit 20 | `EFER.NXE`, bit 11 |
| SMEP | Leaf 7, subleaf 0, EBX bit 7 | `CR4.SMEP`, bit 20 |

NX gives bit 63 of a page-table entry its execute-disable meaning. The BSP enables it before creating user mappings. The AP trampoline enables it before turning on paging because the shared page tables can already contain NX entries. SMEP makes supervisor instruction fetches from user pages fault.

`cpu_prot_nx`, `cpu_prot_smep`, and `cpu_prot_smap` in `c/kernel/cpu/prot.c` read EFER or CR4. `cpu_prot_report` combines the cached BSP capability bits with register state from the executing CPU. It prints support and enablement after BSP GDT setup and during each AP’s startup.

The ELF loader in `c/kernel/exec/load/elf.c` derives page permissions from segment flags and unions permissions where segments share a page. It rejects loadable segments requesting both write and execute permission and applies `PTE_NX` to pages lacking execute permission when `cpu_prot_nx_usable` permits it.

An older comment in that loader says `cpu_prot_nx_usable` returns zero pending repairs to page-table address masking. The current implementation in `c/kernel/cpu/prot.c` returns the active NX state. `c/kernel/mm/mm.h` documents the correction: `MM_PTE_ADDR` extracts the physical address field, and `MM_PTE_FLAGS` retains flags including NX when permissions are copied.

## Feature detection

`cpu_features_init` in `c/kernel/cpu/cpufeat.c` fills a cached `struct cpu_features`. A table maps feature identifiers to CPUID leaves, subleaves, registers, bits, and printable names. Queries for feature leaves are bounded by the advertised maximum basic or extended leaf, protecting detection against unrelated responses to unsupported queries.

The cache includes vendor and brand strings, instruction extensions, protection capabilities, and clock features. For CPUs advertising XSAVE, leaf `0x0d` supplies the supported state mask and save-area sizes. XGETBV runs when CPUID reports OSXSAVE active. `cpu_avx_usable` requires XSAVE, OSXSAVE, AVX, and the XCR0 bits covering SSE and AVX state.

The decoder uses static storage and runs before heap and timer services exist. Its compilation path for other architectures produces a feature set with capability bits clear, allowing the source to serve host tools. During kernel startup, this cache describes the BSP.

## Platform identity and topology

`c/kernel/cpu/cpu_platform.c` decodes CPU signatures, topology, cache geometry, APIC state, machine check capabilities, and invariant TSC support. `struct cpu_platform_ops` in `c/kernel/cpu/cpu_platform.h` supplies CPUID and optional MSR callbacks, allowing the decoder to consume hardware results or test fixtures.

Family, model, and stepping supply diagnostic labels for Sandy Bridge-EP, Ivy Bridge-EP, and selected Raptor Lake-S signatures. The Raptor Lake-S label includes signatures shared with its refresh. Feature inventory uses capability bits. Reads of power and thermal MSRs that are specific to the model additionally check Intel family 6 models `0x2d` and `0x3e` through `c/kernel/cpu/power/hardware.h`.

Topology decoding prefers leaf `0x1f`, then leaf `0x0b`. Legacy leaf 1 and leaf 4 data supply a fallback for processors with uniform SMT. Validation checks level numbering, counts, shifts, APIC identity, and termination. Hierarchy levels above the core level extend the package boundary so that core identities remain distinct across those domains.

SMP startup samples each logical CPU. Leaf `0x1a` identifies Intel performance and efficiency core types, while topology leaves describe the executing core’s SMT arrangement. `c/kernel/cpu/smp/smp_topology.c` groups validated package and core identities. Invalid topology samples receive an unknown core class and an identity keyed by the firmware APIC ID.

CPUID topology counts describe addressable processor IDs. Firmware enumeration and completed AP startup establish the online count. Cache sharing fields likewise describe addressability; cache sizes come from leaf 4 geometry with multiplication overflow checks.

## Interrupt routing and timer quirks

`c/kernel/cpu/irq/lapic.c` follows the xAPIC or x2APIC mode handed over by firmware, selecting MMIO or MSR access. AP initialization checks that each CPU’s mode matches the system mode. AP startup delays use the monotonic clock because instruction timing varies across processors and emulators.

After physical memory initialization, the BSP calls `lapic_virtual_wire`. This clears task priority, configures LINT0 for ExtINT delivery from the legacy PIC, and configures LINT1 for NMI delivery.

`time_platform_init` in `c/kernel/core/ktime.c` then attempts HPET initialization through `c/drivers/timer/hpet.c`. HPET setup disarms inherited comparator interrupts and clears legacy replacement routing before enabling the counter. Together, these steps configure the PIT delivery path after firmware handoff.

Before AP startup, `lapic_timer_calibrate` measures the timer after division by 16 against a calibrated TSC. It seeks five valid samples over intervals of at least 10 ms within 15 attempts. Timestamp brackets around register accesses reject endpoints taking more than 10 microseconds, including endpoints delayed by System Management Interrupts. At least three valid samples are required; the selected estimate is the middle entry of the sorted samples.

AP timer periods are 1.6 ms plus 0.2 ms per CPU index. Staggering the periods spreads arrivals at the shared scheduler lock. The fallback uses the earlier fixed reload counts and reports the calibration failure.

## Cache attributes and machine checks

`pat_init` in `c/kernel/cpu/power/pat.c` checks PAT and MSR support, then writes the PAT layout `0x0007040600070106`. This assigns write-back memory to entry 0 and write combining to entry 1. The update brackets the MSR write with cache and TLB flushes and checks readback.

The BSP initializes PAT before framebuffer setup; APs initialize it before publishing readiness. `c/kernel/gui/fb/fb.c` picks the framebuffer mapping’s attribute for write combining through the PAT availability result.

`mce_init` in `c/kernel/cpu/power/mce.c` checks MSR, MCE, and MCA support on each CPU. It reports existing valid bank records, enables bank controls, clears bank status, and sets `CR4.MCE`.

Vector 18 enters `mce_entry`, which preserves general registers and floating point state around `mce_handle`. A machine check can interrupt a logging lock owner, so diagnostics first use bounded COM1 polling in `c/kernel/cpu/power/hardware.c`. Bank address and miscellaneous registers are read when their validity bits are set.

The handler panics for uncorrected errors, lost error information, corrupt processor context, missing valid bank records, or a cleared restart IP validity bit. For an accepted event, it clears bank status and the machine check in progress bit before returning.

## Power policy and diagnostics

`pstate_init` in `c/kernel/cpu/power/pstate.c` checks EIST support and firmware enablement, then derives ratio bounds from platform MSRs and any loaded firmware table. Startup requests the highest permitted ratio, including turbo when available and enabled.

`c/kernel/cpu/power/firmware.c` implements parsing of ACPI `_PSS`, `_PCT`, `_PPC`, and `_CST` data. The idle policy in `c/kernel/cpu/power/cpuidle.c` accepts supported firmware MWAIT hints with exit latency at most 200 microseconds. It also requires MONITOR/MWAIT extensions, interrupt break support, and an Always Running APIC Timer to preserve the AP’s wake source. Scheduler idle and polling waits call `cpuidle_enter`; its fallback executes `sti; hlt; cli`.

Early serial reports in `c/crypto/cpu_report.c` include feature names, platform identity, topology, cache geometry, APIC state, XSAVE geometry, and the selected AES-GCM implementation. The MCA message “enumerated, not enabled” belongs to this early inventory stage; later `mce_init` calls configure machine check handling.

After PIC and PIT setup, `cpu_simd_selftest` checks XMM register contents across enabled timer interrupts and compares cryptographic results with the portable backend and a known answer. `CPU_SIMD_SELFTEST_OK` requires both phases to observe at least four elapsed timer ticks.

After the virtual wire and HPET setup, another probe reports PIT ticks over a nominal 50 ms interval, bounded by an iteration ceiling. `c/kernel/cpu/power/thermal.c` captures ratio, temperature, current throttling status, and microcode revision during CPU startup, then reports the samples after AP bring-up. Package temperature uses the advertised package sensor capability and a nonzero temperature target.

`make HWHUD=1`, defined in `Makefile`, enables the hardware panel in `c/kernel/diag/hwhud.c`. Its CPU and clock lines display activity on the sampled CPU, PIT tick rate, and calibrated LAPIC timer frequency.

## Limitations

- AVX remains disabled under the FXSAVE interrupt ABI. Enabling it requires extending interrupt and signal state preservation to cover YMM state.
- Boot setup does not enable SMAP. `c/kernel/cpu/prot.c` records the direct accesses to user memory that need conversion to accessors bracketed by STAC and CLAC.
- TSS IST entries and the installed gates’ IST selectors remain zero, so exceptions have no dedicated IST stacks.
- HFI and Thread Director supply capability inventory. Scheduling uses a uniform policy across online CPUs.
- `cpu_power_load_acpi` and `pstate_tick` have no production callers. Current startup uses ratios derived from MSRs and the HLT idle path; frequency control based on load remains unwired.
- The SIMD self-test precedes virtual wire setup and HPET route cleanup. Blocked PIT delivery can produce `CPU_SIMD_SELFTEST_FAIL` before the later PIT probe.
- PAT, LAPIC calibration, and CPU power sources mark X79-VG4 hardware validation as pending.

## Source map

| File | What it contains |
|---|---|
| `c/boot/long.asm` | BSP entry, floating point state, NX and SMEP setup |
| `c/boot/ap_trampoline.asm` | AP mode transitions and CPU register setup |
| `c/boot/gdt_flush.asm` | GDT and task register loading |
| `c/boot/isr.asm` | Common interrupt entry and floating point preservation |
| `c/kernel/init/kmain.c` | BSP initialization order and PIT delivery probe |
| `c/kernel/cpu/gdt.c` | BSP descriptor initialization and protection report |
| `c/kernel/cpu/gdt.h` | Selectors, descriptor structures, and TSS layout |
| `c/kernel/cpu/smp/percpu.c` | Descriptor construction, initial stacks, and CPU lookup |
| `c/kernel/cpu/smp/percpu.h` | Per-CPU state structure |
| `c/kernel/cpu/irq/idt.c` | Shared IDT construction and gate installation |
| `c/kernel/cpu/irq/interrupts.c` | Interrupt dispatch and signal integration |
| `c/kernel/cpu/irq/lapic.c` | APIC access modes, virtual wire setup, vector state, and timer calibration |
| `c/kernel/cpu/prot.c` | Protection register readback and SMAP rationale |
| `c/kernel/cpu/prot.h` | Protection interfaces and NX permission bit |
| `c/kernel/cpu/cpufeat.c` | Cached CPUID feature decoding |
| `c/kernel/cpu/cpufeat.h` | Feature identifiers and cache interface |
| `c/kernel/cpu/cpu_platform.c` | Platform, topology, cache, and MSR decoding |
| `c/kernel/cpu/cpu_platform.h` | Platform records and hardware callbacks |
| `c/kernel/cpu/smp/smp.c` | AP initialization, timer periods, and topology capture |
| `c/kernel/cpu/smp/smp_topology.c` | Topology validation and aggregation |
| `c/kernel/cpu/power/hardware.c` | Privileged operations, idle instructions, and emergency serial output |
| `c/kernel/cpu/power/hardware.h` | Power interfaces and model checks for MSR access |
| `c/kernel/cpu/power/pat.c` | PAT programming and readback |
| `c/kernel/cpu/power/mce.c` | Machine check initialization, entry, and bank handling |
| `c/kernel/cpu/power/pstate.c` | EIST ratio initialization and control |
| `c/kernel/cpu/power/cpuidle.c` | Idle policy selection and entry |
| `c/kernel/cpu/power/firmware.c` | ACPI frequency and idle table parsing |
| `c/kernel/cpu/power/thermal.c` | Ratio, temperature, throttling, and microcode reports |
| `c/kernel/core/ktime.c` | Platform clock initialization |
| `c/drivers/timer/hpet.c` | HPET setup and cleanup of inherited interrupt routes |
| `c/kernel/gui/fb/fb.c` | Framebuffer cache attributes |
| `c/kernel/diag/hwhud.c` | Hardware panel diagnostics |
| `c/kernel/sched/sched.c` | TSS stack updates and idle entry |
| `c/kernel/exec/load/elf.c` | ELF page permissions and NX use |
| `c/kernel/exec/signal/ksigframe.c` | Floating point state in signal frames |
| `c/kernel/mm/mm.h` | Page-table address and permission masks |
| `c/drivers/core/irq.c` | Device interrupt stubs, gates, and vector retirement |
| `c/crypto/cpu_report.c` | Early CPU reports and SIMD interrupt self-test |
| `c/crypto/aead/aes_dispatch.c` | AES implementation selection and comparison |
| `c/crypto/aead/aes_ni.c` | Accelerated AES backend capability checks |
| `Makefile` | Hardware panel build option |

## See also

- [Kernel initialisation](init.md)
- [Local APIC and I/O APIC](apic.md)
- [SMP boot](smp-boot.md)
- [Virtual memory](virtual-memory.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Security model](../architecture/security-model.md)
- [Power and platform drivers](../drivers/power-and-platform.md)
