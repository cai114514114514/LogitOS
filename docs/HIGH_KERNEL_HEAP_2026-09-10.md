<!-- SPDX-License-Identifier: MIT -->
# 高地址内核堆：2026-09-10

普通构建的 `kmalloc` 已改用 supervisor physmap CPU 指针，优先分配 4 GiB 以上的物理连续页。512 MiB 机器同样使用高虚拟 alias。模块可执行 image 改走独立的 `kmalloc_low`：物理地址低于 1 GiB，继续使用可执行 identity alias，满足对低地址内核导出的 rel32/32 位重定位约束。模块状态和装载输入缓冲仍使用普通堆。

两个域具有独立空闲链表；16 字节块头的空闲标志位保存域信息，拆分和合并保留它。低域既不进入每 CPU magazine，也不因扩容清空普通域缓存。普通域原有拆分、合并、失败时排空缓存的行为保留。低域耗尽返回失败，不通过杀死高内存用户进程尝试解决低地址短缺。

AP 启动汇编在进入 64 位、装好最终页表后才读入 RSP，因此 AP 栈、内核线程栈、用户陷入栈和 fork 栈都继续使用普通堆。trampoline、启动参数和页表保留既有低区路径。panic 的回溯和扫描先验证可读 RAM 范围及溢出；`pmm_physmap_low_ready()` 在低区 alias 全部建立、刷新 CR3 后才发布，支持高映射失败时的低区降级回溯。

最终结果位于 `/tmp/logitos-highheap-20260910/final/`：

- `highheap-acceptance.json`：**30 个阶段通过**，记录的 **692 个源码、构建及测试输入未变化**；7 种构建的 **21 个内核/启动镜像哈希**均有记录。普通 ELF 不含压力测试入口，BKL 源码/ELF 检查通过。
- `heap-results/`：BIOS/UEFI × 512 MiB、2 GiB、8 GiB，共 6 组全部通过。检查真实 CPU/物理地址、supervisor/W/NX 权限、四核并发、高栈回溯、32 次上下文切换、堆缓冲 DMA 映射与完成后的解除固定。
- `module-results/`：普通镜像的同一 6 组配置均执行了实际装载模块的回调，验证低区代码调用内核导出、重定位的数据指针和高堆状态读写。它是 CPU 功能测试模块，不配置其匹配的 PCI host bridge。
- `panic-results/`、`low-map-only-results/`：两种固件的正常 8 GiB 启动及禁用高物理映射的降级启动，均从高虚拟栈输出完整 panic，并由该镜像的符号表解析到 `kdiag_write`、VFS、文件层和系统调用路径。

8 GiB 压力测试的实测值如下。每组分配 20 个 `64 MiB - 16` 字节块，写入并核对全部 **167,772,120 个 64 位字**，所有数据块位于 4 GiB 以上物理 RAM。

| 固件 | 实际校验字节 | arena 字节 | 其中低域 | 其中物理 >4 GiB | live_bytes 前 → 后 |
|---|---:|---:|---:|---:|---:|
| BIOS | 1,342,176,960 | 1,363,148,800 | 4,194,304 | 1,358,954,496 | 11,586,416 → 11,552,496 |
| UEFI | 1,342,176,960 | 1,363,148,800 | 4,194,304 | 1,358,954,496 | 11,586,800 → 11,552,928 |

例如 BIOS 的真实系统调用栈为 CPU `0xffff80014d5587d0`、物理 `0x14d5587d0`。512 MiB 的模块状态缓冲为 CPU `0xffff8000030cf770`，模块代码仍在 `0x3216010`。降级 BIOS 启动报告 `high_pages=0`，栈为 `0xffff8000097b3f20`，仍能完成回溯。

`live_bytes` 包含 magazine 中暂存的小块。客体允许后台 GUI 带来 8 MiB 的活动量变化，并记录实际前后值；精确资源平衡由隔离的主机测试检查。真实 PMM/堆 fixture 的 **399 个断言**通过，最终普通域与低域各持有 4 MiB arena，live/cache 均为零，空闲块和块头字节完全平衡。原有堆重叠、失败注入、20,000 次模糊工作负载、泄漏及 8 线程 ticket-lock 测试通过。

控制是正向测试的前置步骤：强制普通堆使用低页、低域从普通缓存取块、拆分丢失域、低域扩容错误清空普通缓存，均触发对应主机断言。panic 的四个控制覆盖未就绪 alias、错误要求完整高映射、frame 范围、扫描跨入保护页；正向 **33 个断言**通过。客体还实际恢复旧 CPU identity 指针、强制低物理页、误把模块代码放进普通高堆；后者明确返回重定位范围错误 `-7`。

`regression/dma-acceptance.json` 记录上一轮完整宽内存/静态 PIE/DMA 回归通过：两种固件的内存矩阵、NVMe/AHCI 读写、virtio 队列与数据、网络真实收发、GPU/光标、RNG、balloon、禁 PS/2 的 USB 输入、HDA 播放及捕获、停止和资源回收。RTL8169 继续以真实驱动配寄存器/DMA 仿真验证，未增加实体硬件结论。

普通内核的 TextEdit/IME 在两种固件、8 GiB、4 CPU 下均保存出相同的 66 字节 UTF-8 内容：`你好，我爱中国。你好吗？女孩输入法二维码西安`；ASCII 控制与 `你好` 也通过。另通过 1/2/4/8 CPU 等待测试、线程/TLS/互斥/回收、52 项信号检查，以及四核 120 次 fork+exec。堆并发测试为 T1=5839 ms、T4=6188 ms，4 个不同 CPU，数据损坏计数 0，满足原有严格门槛。

相同 MM/FS 程序、相同磁盘，交替测量三次：

| 内核 | 客体耗时样本 ms | 中位数 ms |
|---|---|---:|
| 本轮前，已移除 BKL | 357 / 354 / 353 | 354 |
| 最终高堆内核 | 375 / 389 / 361 | 375 |

本组新内核中位数高约 5.9%，**不能宣称本轮提速**。采样期间有其他 QEMU，起止负载和 PID 已记录；这些样本不足以把差异归因于堆迁移。已验证的性能改动是低域操作不再排空普通每 CPU 缓存；BKL 的多核并行门槛继续通过。环境为 Apple Silicon、16 逻辑 CPU、128 GiB RAM、Apple Clang 21、QEMU 11.0.0，客体使用四核 TCG；没有实体硬件吞吐结论。

本轮仍使用连续物理 arena，单次大分配可能因碎片失败；`kfree` 归还块供堆复用，arena 仍由堆持有，尚不归还 PMM。模块可执行 image、页表和原低区 PMM 接口保留低地址约束。浏览器和第三方源码未由本轮修改，未新增第三方依赖或变更许可证位置。

重跑统一入口（独立目录）：

```sh
make BUILD=/tmp/logitos-highheap-check \
  HIGHHEAP_APPS_DISK=/tmp/logitos-bkl-20260910/release/parallel/bkl-disk.img \
  HIGHHEAP_BASELINE=/tmp/logitos-highheap-20260910/before \
  test-highheap-all
```

共享 `build/` 在本轮已不存在，因此该入口从上一轮验收磁盘恢复应用字节到新的私有目录，记录源磁盘及恢复文件的哈希，并复用仓库的文件系统读写工具。若已有完整应用构建，可省略 `HIGHHEAP_APPS_DISK` 并设置 `WIDE_BASE_BUILD=/path/to/apps-build`。`HIGHHEAP_BASELINE` 仅供性能比较；普通内核构建无须任何 HIGHHEAP 验证开关。

实现集中在 `c/kernel/mm/phys/kheap.c`、`kheap.h`、`pmm.c`、`physmap.h`、`c/kernel/diag/panic.c` 和 `c/kernel/module/modload.c`。测试入口为 `tests/highheap.mk`，fixture 和控制留在 `tests/unit/`，客体驱动与报告留在 `tests/boot/`。下一阶段可在这些地址约束上实现内核 `PT_INTERP` 支持。
