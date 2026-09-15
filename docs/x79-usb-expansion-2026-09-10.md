# X79 / 16 GiB DDR3：第二轮硬件扩展

用户确认目标平台为 X79 主板、16 GB DDR3；主板品牌、修订版、CPU、网卡与显卡型号尚未确定。因此设备匹配仍以 PCI class/prog-if 和实际 USB 描述符为准，不从“X79”名称推断外围芯片型号。

Intel 的 C600/X79 数据表描述了 EHCI 与集成 rate-matching hub。上一轮只有 xHCI 时，这条 USB2 路径尚不能服务。本轮三代理分别负责 EHCI 后端、公共 USB/HCD/Hub、USB Mass Storage，主代理负责块层接入、16 GiB 验收与整合。

## 实现范围与验收边界

实施日期为 2026-09-10 至 2026-09-11；结果以实际 guest 日志为准。

| 组件 | 本轮工作 | 验收边界 |
|---|---|---|
| EHCI | PCI class `0c.03.20`，控制/bulk/周期输入，BIOS 交接、DMA 地址约束与停止确认 | 模拟器不能等同 X79 实体芯片 |
| USB 公共层 | 多控制器、按设备选择后端、拓扑与 TT 信息、逐控制器使用引用 | 保留已有 xHCI/HID/DMA 与解绑语义 |
| USB2 Hub | 开机供电、端口复位、子设备枚举、hub 路由与 TT 配置 | 开机枚举；完整运行时热插拔不在本轮范围 |
| USB 存储 | 类 `08/06/50` 的 Bulk-Only Transport，容量、读写、flush、错误恢复 | 附加块设备；启动根盘选择阶段保持现有顺序 |
| 晚枚举分区 | `blk_probe_partitions` 发布新介质分区，成功扫描一次，失败可重试 | 已注册整盘、启动期串行发现，不是挂载后在线重分区 |
| 16 GiB 平台 | 固件可用容量、高页分配、COW/swap、跨页 DMA 与冷启动持久性 | QEMU 提供 16 GiB；不代表测试 DDR3 电气稳定性 |

QEMU 的 `usb-kbd`/`usb-tablet` 在 `usb_version=2` 时具有 high-speed 描述符，可检验 EHCI 的真实模拟周期传输。内置 `usb-hub` 仅具有 full-speed 描述符，不能证明 EHCI→high-speed Hub→低/全速设备的整个 TT 路径。xHCI 下 full-speed Hub 的真实输入、TT 配置的生产函数夹具与物理机测试必须分别报告。

## 已落地的公共接入

- 晚到块设备分区发布接入生产块层；原根盘保持不变，分区读写使用真实偏移。块层 88 项检查通过；恢复重复发布的负控命中同一注册表断言。
- 修正旧 DMA harness 的 `ram == '8G'` 条件：16G 与 16384M 都必须验证高物理地址及完成字节数。容量证据必须来自 guest，并拒绝缺失、重复或被截断的内存映射。11 项检查通过；恢复 8G-only 条件的负控确实失败。
- `tools/driver_inventory.py --profile x79` 按 PCI prog-if 区分 EHCI/xHCI/UHCI/OHCI，并保留 USB 拓扑记录。profile 是用户指定目标，不能当作主板识别结果。

## 16 GiB 实测发现并修正的启动问题

第一次 OVMF 启动停在 `[physmap] PMM metadata does not fit unoccupied low RAM; PMM disabled`。旧分配器要求约 15 MiB 元数据紧挨内核，并全部落在一个 AVAILABLE 描述符里；UEFI 把相邻的可回收内存分成多个描述符，导致已有可用 RAM 仍被拒绝。

`pmm.c` 现在按实际 AVAILABLE 区间寻找低地址连续空间，允许跨相邻描述符、绕过固件空洞和仍在使用的启动信息。内核和元数据分别保留，避免搬迁元数据时占用两者之间的全部 RAM。实际生产 PMM 的 15 项 ASan/UBSan 检查通过；恢复单描述符条件、恢复固定内核尾地址的两个负控各命中一项明确失败。原有 physmap 的普通/NX 关闭变体各 4721 项、消费路径 8199 项及对应负控也通过。

重测还遇到 DMA 测试的全局快照被并发 4 KiB 传输影响：`mappings=0/1 pins=0/2`，而后续全局资源均回到零。采用共享工作区当时新增的有界 idle 快照 helper 后重跑；该 helper 不释放任何资源，持续保留 mapping/pin/direct/bounce 的四个控制均被拒绝。此处属于验收器修正，不宣称修复了 NVMe 驱动泄漏。两次失败的原始日志分别保留在 `build-x79-platform/x79-memory-before-pmm` 和 `x79-memory-before-idle-snapshot`。

| 已完成的模拟器验收 | 结果及证据 |
|---|---|
| 16G / SandyBridge，BIOS 与 UEFI 内存及 PIE | 两种启动均通过；`build-x79-platform/x79-memory`，包含真实高页、COW、交换回读、页缓存、512 KiB 跨页 DMA 和用户程序 |
| NVMe，16G / SandyBridge，BIOS 与 UEFI | 每种启动两次冷启动，真实写入、flush、逐字节持久性检查均通过；`build-x79-platform/x79-storage/nvme-*` |
| SATA/AHCI，16G / SandyBridge，BIOS 与 UEFI | 每种启动两次冷启动，真实写入、flush、逐字节持久性检查均通过；`build-x79-platform/x79-storage/ahci-*` |

这些测试使用私有磁盘镜像；没有写入实体电脑或宿主机磁盘分区。SandyBridge 是 QEMU 的 CPU 配置，不是对用户 CPU 型号的识别。

## USB 实际传输与回归

| 路径 | 验证结果 |
|---|---|
| EHCI → high-speed 键盘、鼠标 | 禁用 PS/2 后，按键、长按重复、鼠标移动、左右按钮与双向滚轮进入 ring-3；要求实际独占 EHCI 向量的 IRQ 计数增加。无 USB 设备的负控能启动系统和程序，却无法满足输入断言。最终普通 ISO 证据：`build-x79-ehci/release-hid`、`release-hid-negative` |
| 两个 EHCI 控制器分别接键盘、鼠标 | 最终普通 ISO 实测两套 HCD 同时工作，完整输入到 ring-3；fixture 关闭默认 NIC，使各自 IRQ 计数不会混入共享网卡向量。最终证据：`build-x79-ehci/release-hid-dual-exclusive` |
| xHCI → full-speed Hub → 键盘、tablet | 实际 Hub 的 8 端口中枚举两台子设备；Shift+A、修饰键点击、释放修饰键、绝对坐标转换进入用户程序。证据：`build-x79-usb/usb-hub-evidence` |
| xHCI → USB BOT 存储，16G / SandyBridge | 两次冷启动，每次 15 项 guest 检查、66560 字节分区读写/flush/边界检查；调用方及实际 xHCI DMA staging 均在 4 GiB 以上。证据：`build-x79-storage/xhci-16g` |
| EHCI → USB BOT 存储，16G / SandyBridge | 同一测试内核，两次冷启动同组 15 项检查；调用方缓冲区高于 4 GiB，真实 qTD 使用 `0x40001040` 的 DMA32 中转。证据：`build-x79-storage/ehci-16g` |
| xHCI + EHCI 同时各挂一块 USB 盘 | 两次冷启动，每次 31 项检查、合计 133120 字节；`usb0p1` 与 `usb1p1` 各自的媒体标识、不同数据模式及 guard 均正确，避免仅重复验证第一块盘。最终证据：`build-x79-storage/dual-final-16g` |

新增主机夹具直接执行生产控制器/协议代码：EHCI 44 项、Hub/core 32 项、xHCI 传输 21 项、BOT 117 项、存储类 124 项。对应负控覆盖不枚举子设备、丢 TT 路由、只清一个控制方向、忽略实际传输长度、不回收控制 TD、不更新 dequeue、不重置 toggle、错误 CSW/tag/residue/阶段、假 flush、跳过固件交接及未确认停止即释放。负控均先真实触发指定失败，再运行正常实现；不能用主机模拟代替上表的 guest 证据。

前轮网卡、PCI/共享 INTx、存储、USB HID、DMA 与异步块层主机回归通过。普通镜像另外完成 PCIe root port 后 NVMe 的 BIOS/UEFI 各两次冷启动持久性检查，以及 e1000e 的 DHCP、实际 IRQ、两份 128 KiB 数据的 FNV/SHA256 校验。

## 普通交付镜像

输出目录：`build-x79-integration/`。`logit.iso` 为 BIOS 引导内核镜像，`esp.img` 为本项目 UEFI 引导镜像；应用文件在独立的 `disk.img`。该盘通过捕获的正式 Makefile 磁盘清单打包，复用 `build-driver-integration` 已构建应用，含 348 个文件、无测试附加程序。没有重编或宣称验收其他代理的应用改动。

`validation/manifest.json` 记录这四个文件的尺寸和 SHA256。普通 `kernel.elf` 已检查不包含 `[widecheck]`、`USB_MSC_GUEST_CHECK` 或 `USB_MSC_GUEST_RESULT` 测试写盘入口标识。上表写盘测试用的是独立的 opt-in 测试内核和私有测试盘。

当前能力边界：公共 USB 注册表支持多个控制器，EHCI 有 4 个实例槽；xHCI 硬件实现仍只有一个实例。Hub 做启动期发现，USB 存储支持 512 字节逻辑块的 SCSI-transparent BOT 附加盘；UAS、USB 存储作为启动根盘、完整热插拔、SuperSpeed Hub、UHCI/OHCI、等时传输尚未实现。EHCI→真实 high-speed Hub→低/全速外设的 split-TT 端到端仍待实体硬件验证；不把 QEMU full-speed Hub 的 xHCI 结果当作该路径通过。

## 可重复入口

```sh
make BUILD=build-x79-platform test-x79-host
make -j6 BUILD=build-x79-platform WIDEVERIFY=1 WIDE_BASE_BUILD=build-driver-integration wide-memory-image wide-memory-disk
make BUILD=build-x79-platform WIDEVERIFY=1 WIDE_BASE_BUILD=build-driver-integration test-x79-memory
make BUILD=build-x79-usb-acceptance test-x79-usb
make BUILD=build-x79-platform test-mk-wired
python3 tools/driver_inventory.py --profile x79 --json /absolute/path/to/serial.log
```

`WIDEVERIFY` 只用于独立测试镜像；其磁盘尾部 scratch 检查仅能对 harness 创建的私有镜像运行。普通发布内核不包含该验证入口。

## 资料

- [Intel C600 / X79 原始数据表](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/c600-series-chipset-datasheet.pdf)
- [Intel X79 产品规格](https://www.intel.com/content/www/us/en/products/sku/64015/intel-x79-express-chipset/specifications.html)
- [QEMU USB HID 模型](https://raw.githubusercontent.com/qemu/qemu/master/hw/usb/dev-hid.c)
- [QEMU USB Hub 模型](https://raw.githubusercontent.com/qemu/qemu/master/hw/usb/dev-hub.c)
