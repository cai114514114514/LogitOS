---
name: logitos-add-a-syscall
description: Add or change a LogitOS system call end to end (ABI number, dispatcher, capability policy, libc wrapper, generated ABI files, tests). Use when editing include/abi/ or c/kernel/exec/syscall.c.
---

# Add a system call

Follow `SYS_FTRUNCATE` (number 189) as the worked example.

1. **Contract and number:** pick a free number in `include/abi/logit_abi.h` after reading its reservation
   comments. Write down argument meaning, pointer direction, lengths, result and who owns resources.
2. **Dispatch and implementation:** add the case in `c/kernel/exec/syscall.c` and implement the operation
   in its subsystem. Copy user memory only through the checked copy helpers and define buffer ownership
   across sleeps.
3. **Policy:** update `syscall_cap_class()` when the call needs a capability category, and review
   `include/abi/agent_policy.h` for what agent workers may call.
4. **Caller interface:** declaration and wrapper in libc or the application library
   (`ftruncate()`: `c/apps/libc/include/unistd.h`, `c/apps/libc/src/io.c`); translate failures into `errno`.
5. **Generated consumers:** describe the call in `include/abi/logit_calls.abi`, add the binding in
   `c/apps/as/common/system_constants.def`, regenerate, and run `make check-abi`.
6. **ABI headers reach the guest compiler:** headers under `include/abi/` are installed into the guest
   sysroot, where TCC 0.9.27 has no `_Static_assert`; use the negative-array-size form that
   `include/abi/logit_boot.h` uses, and run `make test-sysroot-headers`.
7. **Tests:** a host test of the backend, a guest test across the syscall boundary, both with a
   negative control; `make test-libc` when a C wrapper changed. Build both the ISO (`make`) and the
   application disk (`make build/disk.img`).

Details: `docs/architecture/system-calls.md`, `docs/reference/syscall-table.md`,
skill `logitos-write-a-test`.
