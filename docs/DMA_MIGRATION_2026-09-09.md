# DMA 第二轮：实现与验收

实现已接入普通内核构建。内核堆与旧 `pmm_alloc/contig` 保持低区；本轮没有建设 IOMMU、高地址内核堆、动态加载器。浏览器和第三方源代码未由本任务修改。测试在 `tests/`，沿用仓库核心 GPL 与外围 MIT 许可划分，许可证仍在 `LICENSES/`。

## 可重跑入口

在仓库根目录执行：

```sh
make -j4 BUILD=/tmp/logitos-dma-check WIDEVERIFY=1 DMA_CAPTURE_VERIFY=1 test-dma-os
```

它运行 DMA/PMM、真实驱动模型、固件与内存矩阵、块设备、virtio、网络、USB、音频、输入法及正常解绑验收；对应负向控制是正向目标的前置依赖。最终生成 `$(BUILD)/dma-acceptance.json`，包含结果、实测地址、资源计数与镜像/源码指纹。使用既有 `build/` 产品应用；可用 `WIDE_BASE_BUILD`、`DMA_BASE_BUILD` 指向其他已构建的产品目录。所有 guest 使用磁盘副本或 QEMU snapshot，UEFI ESP/VARS 也独立复制。

两个宏只增加测试探针。普通构建不需要宏就启用 DMA 新路径。`WIDEVERIFY` 的块测试会临时改写并恢复测试磁盘尾部，因此只能给验收 guest 挂私有磁盘；生产内核没有这些测试选择器。

## 公共接口与生命周期

`c/drivers/core/dma.h` 将设备地址定义为 `dma_addr_t` 包装类型。coherent 句柄分别保存 CPU physmap 指针、物理页、设备地址、长度及释放信息。描述符显式取设备地址。低 RAM 的 supervisor physmap 别名在启动阶段建立，旧低地址 identity 接口保持兼容。

`pmm_alloc_contig_masked` 检查完整分配区间的地址范围、对齐和边界，优先 4 GiB 以上，再回退到可寻址 RAM。coherent 分配还检查设备段长度。流式映射逐页解析内核 RAM，逐页增加引用与固定计数，生成真实 SG；回退缓冲由每个映射独立持有，单次流式映射上限 1 MiB。多段回退按每段约束拆分，不把第一帧地址当作整个虚拟区间连续的依据。

块层取消全局 1 GiB DMA 判定与共享 bounce。内核缓冲可以异步映射；用户指针仍先经过同步 usercopy 的有界独立暂存，不能由其他进程上下文直接异步完成 usercopy。

句柄状态为 READY、DEVICE_OWNED、COMPLETED、QUIESCED、QUARANTINED。提交获得新 cookie；旧 cookie、超过映射长度的完成、设备持有时释放均拒绝。只有已确认的硬件停止才允许回收。TO_DEVICE 不回写原缓冲，FROM/BIDIRECTIONAL 只同步有效长度。

`blk_dev_offline`、现有 PCI `.remove`、USB slot/controller、GPU framebuffer 借用撤销、HDA 播放与捕获注销均已接线。GPU 等待在途 CPU 写入排空后才释放 backing；HDA 最后捕获消费者关闭会停止 DMA，再次打开重新提交。IRQ 热路径保持预分配缓冲。

**失败边界：**块设备若硬件停止无法确认，先隔离对象，再 fail-stop。固定页只能阻止页被释放，无法阻止原调用者复用栈或堆缓冲；此时返回普通错误会允许迟到 DMA 破坏调用者的新数据。因此这一极端失败不声称能继续运行系统。固定缓冲设备可以保留隔离对象并拒绝新提交。

## 设备能力表

| 驱动 | 地址范围依据 | 迁移对象 | 实测边界 |
|---|---|---|---|
| NVMe | 64 位 PRP/队列地址 | Admin/I/O SQ/CQ、PRP list、Identify、每请求 SG | 两种固件 8 GiB 真设备模型读写、跨重启及解绑 |
| AHCI | `CAP.S64A` 决定 32/64 位 | CLB/FIS、命令表、真实 PRDT SG | 两种固件 8 GiB，根盘和独立控制器，解绑隔离 |
| virtio transport/blk/net/gpu/rng | 已支持的 modern transport 64 位物理地址 | 全部队列、请求与状态、GPU 嵌套 backing/cursor | 两种固件 8 GiB，八个队列均高于 4 GiB |
| virtio balloon | DMA 队列 64 位；PFN 仍 32 位 | staging 与捐赠物理页所有权分开 | inflate/deflate 计数恢复；未确认归还不释放 |
| e1000 | 64 位 ring/data 地址字段 | RX/TX rings 和固定缓冲 | 两种固件 8 GiB，真实 HTTP 收发、正常解绑 |
| RTL8139 | 32 位 | RX 环与四个 TX 缓冲 | 实测 DMA `0x40000000`，不截断高地址 |
| RTL8169/8168 系列 | PCIe、设备 ID 和明确识别的 C 及后续 XID 才启用 64 位；旧版/未知为 32 位 | 全驱动 rings、data、失败回收 | 真实驱动代码配寄存器/DMA 仿真；**无实体硬件验证、无 QEMU RTL8169 模型** |
| xHCI | `HCCPARAMS1.AC64` | DCBAA、scratchpads、contexts、各环与固定端点缓冲 | 禁 PS/2 的键鼠，两种固件 8 GiB；slot/controller 回收 |
| HDA | `GCAP.64OK` | CORB/RIRB、播放/捕获 BDL 与 PCM rings | WAV 内容、真实捕获写入、停止、重启及解绑 |
| ATA PIO | CPU 访问 | 不生成 DMA 地址 | 保留原 CPU 路径 |

RTL8169 能力划分核对了 [Linux 上游 r8169 实现](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/realtek/r8169_main.c)；没有仅凭 64 位寄存器布局就启用旧 PCI DAC。

## 验证结果

- DMA core：62 项，含非连续页、跨页偏移、完整 mask 区间、SG 段长/边界、并发独立 bounce、重叠固定页、部分失败回收与 stale completion。4 个缺陷注入分别命中 CPU_POINTER、HIGH_ADDRESS、DIRECTION、EARLY_FREE 断言。
- virtio：130 项与 5 个负控；覆盖 GPU 嵌套地址、CPU 写入排空、reset 失败、balloon PFN 和正常卸载。
- 其他驱动：150 个 ASAN/UBSAN 场景，11 个地址、生命周期和 RTL8169 能力负控。NVMe/AHCI 生产函数夹具分别 18/13 项及 3 个负控。夹具结果与下述真实 guest 结果分别保留。
- BIOS/UEFI × 512 MiB、2 GiB、8 GiB 六组：宽地址、首次触页、fork/COW、权限、swap、回收、8 GiB 稀疏保留、裸 ELF/AEX 静态 PIE 两基址与主/子线程 TLS 全部通过。
- NVMe/AHCI 根盘共 8 次启动：512 KiB 带跨页偏移数据逐字节写读、原尾部恢复，以及文件跨重启验证通过。virtio-blk 同样通过高页 512 KiB 写读。
- e1000、RTL8139、virtio-net：两种固件 8 GiB 均接收完整 32768 字节 HTTP，FNV1a `fb2a9dc5`。解析器额外有 18 个正负 fixture，保留原始日志，拒绝错误 hash、缺失字段和半数字拼接。
- HDA 输出：WAV 中 48000 帧测试信号与连续样本校验。捕获：每轮 32768 字节由 `0xA5` 哨兵变为已知静音，用户 guard 不变；正常关闭后观察 100 ms，LPIB、IRQ 和 `0xC7` 哨兵均不变；再次打开仍工作；两种固件均正常解绑且后续 open 返回 NODEV。
- GPU 桌面和 cursor、RNG 实际完成长度、balloon 确认归还及计数、禁 PS/2 的 USB 键鼠均通过。
- TextEdit/输入法：两种固件 8 GiB 的 ASCII 负控、你好候选，以及完整中文输入/保存逐 UTF-8 字节校验通过。普通构建也独立验证了 virtio、桌面与输入法。
- 两种固件的终止型解绑测试：11 类已绑定驱动实际执行 `.remove` 后，coherent/bytes/mappings/pins/direct/bounce/quarantine **全部为 0**，PMM audit/bugs 为 0。

## 地址与计数样本

512 MiB BIOS 的 GPU 队列：CPU `0xffff800001889000`，DMA `0x1889000`，证明低内存时也使用独立别名。

8 GiB UEFI 的 512 KiB 块数据：CPU `0xffff80010058e025`，DMA `0x10058e025`。NVMe、virtio-blk、AHCI 各完成 2621440 高页字节（保存、写、读、恢复、再读）。wide-memory 结束时：coherent=30、bytes=4227072（设备持久缓冲）、mappings=0、pins=0、direct=0、bounce=0、quarantine=0/0/0，completed_high=7872512。

其他 8 GiB BIOS 样本：e1000 ring `0x1003f9000`；xHCI `0x100443000`；HDA capture ring `0x10044e000`。CPU 地址分别是 physmap 别名，不能作为设备地址使用。

## 工件与限制

本次统一构建与日志：`/tmp/logitos-dma-20260909/acceptance/`、`acceptance-final.log`。普通构建：`/tmp/logitos-dma-20260909/release/`；普通构建设备与输入法证据分别在 `release-virtio/`、`release-ime/`。源码及镜像指纹和普通构建结果见 `/tmp/logitos-dma-20260909/delivery.json`；统一入口自动汇总见 `acceptance/dma-acceptance.json`。

所有设备运行证据来自 QEMU；RTL8169 只有真实驱动代码的模型验证。IOMMU、高地址内核堆、DMA 用户零拷贝、动态加载器没有包含在本轮。现有低区堆和旧 PMM 接口限制保留。

最终全仓库 `test-mk-wired` 仍被并行浏览器工作新增、尚未接线的 `tests/flex_parser.mk` 阻挡；本轮 `dma.mk`、`dma_drivers.mk` 及其负控均已在 Make 依赖图中可达。没有为修复这个范围外检查而修改浏览器测试文件。
