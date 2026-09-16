# c/kernel/{mm,cpu,sched,core,module,pci,audio} — 2026-09-16

## 方法与覆盖

先读 `docs/CODE_AUDIT.md`（776 行，含 2026-08-04 修复批、2026-08-05 终端卡死专案、TLS/crypto 批）与 `CLAUDE.md` 的 OPEN BUG/known 条目，确认已知问题清单（见文末）。随后通读本分区全部 .c/.h（28,692 行）。

**逐行深读**（页表/分配器/锁/中断/refcount/错误分支）：
- mm：`pmm.c`(1065)、`kheap.c`(683)、`vmm.c`(1230)、`fault.c`(714)、`reclaim.c`(759)、`swap.c`(522)、`rmap.c`(323)、`oom.c`(588)、`pcache.c`(1309)、`vma.c`(814)、`shm.c`(365)、`mmsys.c`(488)、`mmguard.inc`、`mm.h`、`mmguard.h`
- cpu：`spinlock.c`、`tlb.c`、`smp.c`(481)、`percpu.c`、`interrupts.c`(394)、`lapic.c`、`ioapic.c`、`idt.c`、`acpi.c`(415)、`prot.c`、`cpufeat.c`、`acpi_integrity.c`、`apic_model.c`、`gdt.c`；`cpu_platform.c`/`smp_topology.c`/`smp_boot_model.c` 略读
- sched：`sched.c`(1863)、`uthread.c`(829)；`kbench.c` 略读
- core：`ktime.c`(1420)、`settings.c`(1790)、`rng.c`
- module：`modload.c`、`modelf.c`、`ksyms.c`
- pci：`pci.c`(516)、`pci_msi.c`(573)、`pci_init.c`
- audio：`mixer.c`(749)、`capture.c`(464)、`pcm.c`、`snd.c`、`snd.h`；另核读 `sync/wait.h` 的 `wait_event_timeout` 与 `sync/work.c` 的 softirq

本仓注释文化极强，以下每条发现都先对照过所在文件的自述注释（发现 1 恰恰是该文件注释声称已被防住的情形）。历史注释中大量 "BKL" 字样指 2026-09-10 已移除的全局 entry lock（`spinlock.c:15-16` 明示），不影响现状。

## 发现

### [high] [CONFIRMED] kheap magazine 快速路径使双重释放检测失效 —— 同一块两次 kfree 后双属主/重复入 bin，静默堆破坏

- 位置：`c/kernel/mm/phys/kheap.c:605-608`（kfree 快速路径）、`453-462`（mag_push 无去重）、`468-488`（mag_drain_all_locked 二次出队）、对照注释 `598-604`
- 代码：
```c
/* kfree(), 605 行 */
    if (!blk_low(h) && !blk_free(h)) {
        int cls = mag_class(blk_size(h));
        if (cls >= 0 && mag_push(cls, h)) return;   /* 块进 magazine，F_FREE 不置位 */
    }
    uint64_t f = spin_lock_irqsave(&kheap_lock);
    if (blk_free(h)) {                              /* 612 行：双释放检查 */
        kprintf("[kheap] double free of %p (%d bytes) -- refused\n", ...);
```
```c
/* mag_push(), 453 行：无任何“已入队”标记检查 */
    if (m->n[cls] < MAG_DEPTH) { m->blk[cls][m->n[cls]++] = b; took = 1; }
```
- 触发与后果：对一块 16..512 字节、非 low 域的块做两次 `kfree`：
  1. 第一次走快速路径入 magazine（`F_FREE` 保持 0——注释自述这是刻意设计）；
  2. 第二次 kfree 读到 `F_FREE` 仍为 0，**再次走快速路径**，同一块被推入 magazine 两次。文件注释（598-604 行）声称“第二次 kfree 仍会找到已分配块并走慢速路径、在那里被拦下”——与实际控制流相反；
  3. 此后 `mag_drain_all_locked()`（OOM 前的兜底，530-539 行必然先于任何重试分配执行）把同一指针弹出两次：第一次 `bin_push(coalesce(b))` 将其（可能与邻居合并后）放入 bin 并置 `F_FREE`；第二次再 `bin_push` 一次——**同一内存以两个块的身份同时挂在空闲链上**，且 `st_live` 被扣减两次（无符号下溢）。之后两次 `kmalloc` 各拿到一份，得到两个互相重叠的属主。全程无任何一行日志。
  并发版同理：两核同时 `kfree` 同一指针，双双通过 `!blk_free(h)` 检查。
- 修复建议：头部尚有闲置标志位（`0x8`，`F_FREE/F_LAST/F_LOW` 之外），加 `F_MAG` 位：快速路径入队前置位、`mag_pop` 出队时清除；`kfree` 见该位即按双释放拒绝。或者在 `mag_push` 前慢速持 `kheap_lock` 做一次廉价查重。

### [medium] [CONFIRMED] capture 引擎把“worker 线程创建失败”永久闩死 —— 首次 OOM 后整条录音路径静默失效

- 位置：`c/kernel/audio/capture.c:337-340`；对照 `sched.c:587-592`（`thread_create` 失败时静默返回 void）；对照 mixer.c 同型的已声明取舍 `mixer.c:452-459`
- 代码：
```c
    if (!g_cap_engine_up) {
        thread_create(kcapture_thread, "kcapture");   /* void：失败不可检测 */
        g_cap_engine_up = 1;                          /* 无论成败都置位 */
    }
```
- 触发与后果：第一次 `snd_cap_open` 恰逢 `thread_create` 内部 `kmalloc(sizeof(struct thread))` 或栈分配失败（`sched.c:589-592` 直接 return）时，`kcapture` 永远不存在，但 `g_cap_engine_up` 已闩为 1，此后所有 open 不再重试创建。后果：中断照常 `sem_post`（无人消费）、`snd_cap_read` 每次 `wait_event_timeout` 空等 500 ms 后返回 0 字节、无错误码、无一行日志；同时 `snd_cap_open` 仍然成功返回句柄——调用方拿到的是一台“永远录到 0 字节”的麦克风。mixer.c 对同型缺陷有注释明示接受（"no reliable OOM retry is claimed"），capture.c 没有任何说明，且 mixer 的失败模式有 `g_running`/underruns 计数器可见，capture 的完全不可见。
- 修复建议：仿照本仓其它 weak-hook 的做法给 `thread_create` 增加返回值，或失败时不置 `g_cap_engine_up` 并让本次 open 返回 `SND_E_NOMEM`。

### [medium] [CONFIRMED] tlb_flush_all 超时从“记录后放弃”改为 fail-stop 死机，而全部调用方注释仍描述旧的降级行为 —— 长轮询 + 多线程同 CR3 下可真实触发

- 位置：`c/kernel/cpu/tlb.c:180-186`（fail-stop）；与 `c/kernel/mm/mmsys.c:247-267`、`c/kernel/sched/uthread.c:180-197`（历史注释）矛盾
- 代码：
```c
/* tlb.c:181 */
    int ack=__atomic_load_n(&g_tlb_ack,__ATOMIC_ACQUIRE);
    if (ack != others) {
        if (ack < others)
            __atomic_fetch_add(&g_tlb_late,(unsigned long)(others-ack),...);
        /* An excess ACK is also a broken transaction, not permission to reuse. */
        tlb_failstop(me,ack,others,n);          /* _Noreturn：cli;hlt 全机停 */
    }
```
```c
/* mmsys.c:249 —— 仍在描述旧行为 */
        /* That flush is BOUNDED and can give up: ... tlb_late_count() records
         * it, and that core keeps the stale entry until its next CR3 switch. */
        unsigned long late0 = tlb_late_count();
        vmm_unmap_range_in(cr3, start, end - start);
        if (tlb_late_count() != late0) sched_tlb_gen_bump();
```
- 触发与后果：两条链路叠加。(a) 行为契约：`SYS_MUNMAP` 路径（以及 `vmm_unmap_range_in`/`vmm_protect_range_in` 的注释）全部按“最多丢一次 shootdown、用 `sched_tlb_gen_bump()` 兜底、一个 tick 内收敛”设计并写了对应代码；现在任何一核未在 `TLB_WAIT_SPINS`(5×10⁷) 内 ack，机器直接 `cli;hlt`，“late 计数 + 代际 bump”整条兜底链路成为死代码。(b) 生存性：本分区外存在比 5×10⁷ 次更长的 IF=0 非自旋锁轮询（`c/drivers/block/nvme.c:106-136` 的 2×10⁸ 次提交轮询、`c/drivers/virtio/virtio.c:156-164` 同量级、`lapic.c` 的 `LAPIC_IPI_WAIT_SPINS`=10⁸）——多线程进程一个线程陷在这种轮询里、另一线程 `munmap` 时，`vmm_space_busy_elsewhere` 命中同 CR3 → 发起方等 ack 超时 → **全机停机**，而按 mmsys.c 注释的设计这里应当只是“晚一个 tick 的陈旧 TLB 项”。
- 修复建议：二选一并对齐注释——要么恢复“超时 → `g_tlb_late` 计数 + 发起方 `sched_tlb_gen_bump()`”的降级路径（fail-stop 仅留给 `ack > others` 这类真正的事务破坏）；要么把 fail-stop 定为最终语义，删改 mmsys.c/uthread.c/vmm.c 里所有“can give up / bounded give-up”描述，并给长轮询驱动加 `tlb_service()` 探针。

### [low] [CONFIRMED] modelf/ksyms 对未 NUL 终止的节名字符串表无界走读 —— 越过 shstr/img 末尾读相邻内核堆

- 位置：`c/kernel/module/modelf.c:33-37`（`m_streq` 无界）、`231-234`（`sec_name` 返回未终止指针）、`388-395`（`symval` 的 `strtab + st_name` 交给 `resolve`→`ksym_lookup` 继续无界走）；`c/kernel/module/ksyms.c:160-164`（`streq` 无界）
- 代码：
```c
static const char *sec_name(const char *shstr, uint32_t shstr_len, uint32_t off)
{
    if (!shstr || off >= shstr_len) return "";
    return shstr + off;              /* 不保证 off 处或表尾有 NUL */
}
...
        if (m_streq(sec_name(ss, sslen, sh[i].sh_name), "logit_drivers")) {
```
- 触发与后果：`hdrs()` 只校验 `sh_offset+sh_size <= imglen`，不校验字符串表以 NUL 结尾（ELF 规范要求，但 modelf.c 的自述威胁模型明确把“截断/半写的 .ko”当普通输入）。一个节名恰好是表中最后若干字节且不含 NUL 时，`m_streq`/`streq` 越过 `shstr_len`（乃至 `imglen`，经 `symval` 的 `resolve(nm)` 路径）继续比较，读进 kmalloc 块之后的堆字节；若后续字节恰好拼出导出名可造成错误符号解析（错误重定位 → 模块首跑即崩，难定位）。后果限于越界读与解析错误：镜像本就是 root 放的，且 arena 内读不会 fault——按本仓“半写的 .ko 不是攻击，是 Tuesday”的标准仍应修。
- 修复建议：`sec_name`/`symval` 改为带回界的比较（`m_streqn(strtab+off, shstr_len-off, name)`），查找函数传入剩余长度。

### [low] [CONFIRMED] snd_report/snd_cap_report 的“buffer ms”诊断在合法几何下 32 位回绕 —— 打印值错误

- 位置：`c/kernel/audio/mixer.c:518`、`c/kernel/audio/capture.c:437`
- 代码：
```c
            (g_dev->period_bytes / (g_dev->channels * 2u)) * 1000u * g_dev->periods / g_dev->rate,
```
- 触发与后果：`valid_device_geometry()` 允许 `period_bytes` 到 `SND_MAX_PERIOD_BYTES`(256 KiB)、`periods` 到 256、`channels`=1：period_frames=131072，`131072*1000*256 = 3.36e10` 溢出 `unsigned`(32 位)，回绕后除以 4000 Hz 打出 ~872,415 ms 而非 ~8,389 ms。纯诊断行，但 boot log 的这个数字正是排查音频延迟时要读的。
- 修复建议：先乘后除改序（`period_frames * periods * 1000u / rate` 中间值 3.4e7 不回绕），或转 u64。

### [low] [CONFIRMED] ktime.c CPU accounting 槽位永不回收 —— 第 65 个 pid 之后整机 CPU 时间静默记入 idle

- 位置：`c/kernel/core/ktime.c:709-727`（`CPUACC_MAX 64`、`acc_slot`）
- 代码：
```c
static struct cpuacc *acc_slot(int pid, int create)
{
    int free_i = -1;
    for (int i = 0; i < CPUACC_MAX; i++) {
        if (g_acc[i].pid == pid) return &g_acc[i];
        if (free_i < 0 && g_acc[i].pid == 0) free_i = i;
    }
    if (!create || free_i < 0) return 0;
```
- 触发与后果：槽位只在 `time_host_reset()`（host 测试）清零；guest 内没有任何退出/回收路径。累计 64 个不同 pid 后（长时间运行 + 反复开关应用即可），`account_sample` 对新进程返回 NULL → `g_acc_idle += dt`——新进程的 CPU 时间全部记进 idle，`SYS_RUSAGE`/`time_cpu_ns` 对其返回 -1，无任何提示。
- 修复建议：pid 死亡时挂一个回收钩子（proc reap 通知），或退化为 LRU：满时替换最旧槽位并接受计数断层。

### [low] [SUSPECTED] pcache_file_open 空闲槽回收窗口内，旧句柄的 `pcache_file_ref` 会被身份重置覆盖丢失

- 位置：`c/kernel/mm/cache/pcache.c:474-491`
- 代码：
```c
            pf[i].recycling=1; pf[i].generation++;
            spin_unlock_irqrestore(&pc_lock,fl);
            purge(i,&c_evict);                    /* 锁外：refs 仍可为并发 ref++ 抬升 */
            fl=spin_lock_irqsave(&pc_lock);
            ...
            pf[i].refs=1; pf[i].hiwater=0;        /* 并发 ref++ 被无条件覆盖 */
```
- 触发与后果：回收窗口（锁已放、`recycling=1`）内，若任何路径对旧身份的 fh 调 `pcache_file_ref`（如另一核正在 `vma_space_clone`/`vma_reserve_file`，持有的是重回收前取到的句柄），该引用先使 `refs` 变 1，随后被 `pf[i].refs=1` 覆盖——多出的引用永久丢失，对应 VMA 退出时会 `pcache_file_put` 到 0 以下被拒（`refs > 0` 检查），真正后果是该文件条目可能比应有生命周期提前进入 idle/被 retire，映射中的页靠 VMA 自身引用兜底、不至 UAF。树内当前句柄纪律（VMA 持引用期间条目必有 refs）使此窗口需要“持有已 put 的句柄”这一内核自身 bug 才打开，故列 SUSPECTED/低。
- 修复建议：`recycling` 期间让 `pcache_file_ref` 拒绝（返回不增），或回收完成后 `refs` 做增量而非赋值。

## 已知问题（未重复上报）

以下条目经与 `CLAUDE.md`（OPEN BUG/known 段）和 `docs/CODE_AUDIT.md` 核实为已知；其中多数在 HEAD 已修复，本审计复核确认修复在位：

1. **munmap 缺跨核 TLB shootdown**（CLAUDE.md "OPEN BUG, verified 2026-08-28"，源出 `uthread.c:197-203` 与 `mmsys.c:229`）——**HEAD 已修**：`vmm.c:733-855` 的 `unmap_drain()` 在帧归还前做 `tlb_flush_all()`、批间 `hold[64]`，`mmsys.c SYS_MUNMAP` 配对 `sched_tlb_gen_bump()`。CLAUDE.md 该条目文本已过时（仍称 OPEN），建议更新。
2. **`tlb_flush_all` 无界等待/静默放弃**（CODE_AUDIT 2026-08-05 修复项）——现为有界 + fail-stop；其行为契约问题见本报告 medium #2。
3. **`pci_find` 只扫 bus0/func0**（CODE_AUDIT 遗留清单）——已由 registry 查找 + `scan_bus` 桥递归 + `g_bus_seen` 防环解决。
4. **smp PERCPU_MAXCPU 溢出计数**（CODE_AUDIT 内核节中危）——已修：`smp.c:415` `if (g_online >= PERCPU_MAXCPU) break;`，AP 不再自行 `g_online++`。
5. **lapic `ipi_wait` 无超时**（CODE_AUDIT 低）——已有界（`LAPIC_IPI_WAIT_SPINS`）。
6. **RNG 静默退化为 rdtsc**（CODE_AUDIT 中危）——已有 boot 告警（`rng.c:136-137`）+ `rng_strong()` 供 TLS 拒绝弱熵。
7. **`thread_create` kmalloc 返回值未检查**（CODE_AUDIT 中危）——已检查（`sched.c:589-592`）；但其 void 返回引出本报告 medium #1（capture）。
8. **kheap `ALIGN16`/`frames*FRAME_SIZE` 溢出**（CODE_AUDIT 低）——已有防护（`alloc_domain` 尺寸上限检查、`grow` 倍增回绕守卫）。
9. **futex 虚拟键不覆盖跨进程共享段**（跨进程须轮询）——`uthread.c:596-652` 长注释明示，`sem_open(pshared)/sem_open()` 按约返回 ENOSYS。设计声明，非新发现。
10. **SYS_SHM_MAP 句柄是全局小整数、非 capability**——`mmsys.c:328-337` 注释明示为 ABI 最弱点。
11. **mixer.c `thread_create` 失败不可检测**——`mixer.c:452-458` 注释自承（本报告 capture.c 条目是该模式的未声明副本）。
12. **vmm_clone/cow 对 `MM_PTE_ADDR`/`MM_PTE_FLAGS` 掩码、NX 携带、shm-fork、PROT_NONE 继承等历史缺陷**——各修复点均带负面控制（`VMM_FORK_REASSEMBLE`、`SHM_FORK_COPY` 等）与测试，复核在位，不重复上报。
