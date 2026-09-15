# 通用 x86_64 PC 驱动扩展（2026-09-10）

本轮目标是型号尚未确定的实体老电脑。实现采用真实硬件的接口与协议；目前证据来自生产函数测试和 QEMU 模拟设备，**尚无任何物理电脑的本轮验收结果**。不能把这些结果解释为任意 PC 已可安装使用。

## 本轮实现

| 路径 | 具体增量 | 支持范围与限制 |
|---|---|---|
| Intel 有线网卡 | 新增独立 `e1000e.c`，PCIe DMA 停止、D0、NVM 等待、MDIO 所有权、PHY 协商、收发环 | 82574 `8086:10D3/10F6`；模拟设备验收覆盖 10D3。不覆盖 I217/I219、PCH、igb/igc |
| AMD 旧 PCI 网卡 | 新增 `pcnet.c`，32 位 DMA、描述符所有权、收发与 STOP 后释放 | PCI ID `1022:2000`，另核对 Am79C970A/PCnet-PCI II 硅片版本；不把同 ID 的 FAST 系列误认成已支持 |
| 网卡公共中断 | 所有已绑定网卡通过设备模型注册中断，先建立路由再启用硬件中断；失败卡继续轮询 | 启动期接线；暂用 INTx，未声称各旧驱动都具备 MSI-X 队列配置或运行时重新绑定能力 |
| RTL8139 修复 | 溢出中断保留接收环读写进度，确认状态后正常排空；停止把软件 CAPR 归零而留下硬件 CBR | 已有驱动的修复；真实中断曾揭示旧错误，轮询成功本身不能证明中断路径正常 |
| NVMe | 从 bus 0/function 0 扫描改为 PCI 设备模型；BAR/门铃范围、页大小、命令集、namespace 格式校验；Number of Queues 标准协商 | 桥后设备可发现，按控制器实际授权数量建队列；目前仅 512 字节逻辑扇区、无 metadata/PI 的 namespace |
| AHCI/SATA | BIOS/OS 交接失败即停止接管；IDENTIFY 区分 512e 与 4Kn | 512e 可使用；4Kn 明确拒绝，避免用 512 字节单位错误寻址 |
| USB HID | 按接口绑定复合设备；同接口多 Report ID；NKRO；USB 修饰键；绝对 tablet 按逻辑范围映射 | 仍依赖 xHCI；最多 8 个设备、每设备 8 个接口、每 HID 8 个报告、32 个同时按键，超出范围不构成完整支持 |
| PCI 配置访问 | 原生 8/16 位写入；修正非零起始 MCFG 的 bus-zero 基址计算 | 避免 Command 写入误清邻接 W1C Status；仍是单个 segment 0 ECAM 窗口，未实现 ACPI 多根总线发现 |
| PCI INTx | 实体机恢复 level/active-low，仅 TCG 保留历史 edge 兼容；同 GSI 共用向量、逐设备分发；解绑等待活动回调，最后退出屏蔽线路并等待 EOI 完成 | ACPI `_PRT`/链路设备 AML、多个 I/O APIC 等仍需扩展，不能凭 IRQ Line 字段声称覆盖所有主板 |
| VirtIO-SCSI | 新增 SCSI 容量查询、READ/WRITE(16)、真实缓存屏障、分区/根盘接入 | 辅助虚拟化覆盖，单控制器 target 0–7、LUN 0、512 字节逻辑扇区；不算实体 SATA/NVMe 新芯片支持 |
| VirtIO 传输 | 增加按指定 PCI function 初始化的接口，避免同 ID 设备重新选择第一个实例 | 公共传输能力；本轮最终未加入 VirtIO 输入驱动 |

## 已收集的分组证据

- Intel 82574 与 AMD PCnet：各取得 DHCP 地址、两次 128 KiB HTTP 下载，正文长度、FNV、SHA-256 与服务端实际 GET 一致；DMA 接收环多次回绕且错误计数为 0。
- NVMe：设备置于 PCIe Root Port 后，日志为 `01:00.0`；分别通过 BIOS、自建 UEFI loader/OVMF 启动，各两次冷启动。控制器只授权一组 I/O 队列时，驱动从请求四组收敛至一组。实际写入、修改、flush 和冷启动逐字节回读通过。
- AHCI 512e：两次冷启动、写入和逐字节回读通过。QEMU `ide-hd` 拒绝创建 4Kn 设备，此项 guest 验证显式 SKIP；4Kn 拒绝路径只由生产 IDENTIFY 函数夹具证明。
- USB：禁用 PS/2，以真实模拟 xHCI + USB keyboard/tablet 路径进入窗口系统，验证位移、点击、Shift+A、带修饰键鼠标点击、释放 Shift 后 a；原 USB relative mouse 回归通过。复合设备、多报告、NKRO 由生产 binder/HID 协议夹具验证。
- 共享 INTx：两个 EDU 模拟设备在 `00:06.0`、`00:0a.0`，guest 确认共用 GSI 10 / vector 96；17 项断言覆盖双设备收发、先解绑一台、最后屏蔽、重新启用 PCI 中断源后触发旧线路、重用向量不受影响。只分发第一个成员的负控命中第二设备失败；跳过线路屏蔽的负控使重用向量实际收到 8 次中断，因此没有被 PCI 源已经关闭掩盖。
- 每个正向测试均有必跑负控；控制恢复旧错误后必须命中相应失败断言，编译错误或无输出不计为有效负控。

分组原始日志位于 `build-driver-network/`、`build-driver-storage/`、`build-driver-input/`；共享 INTx 分组证据位于 `build-driver-intx/{positive,first,mask}/result/serial.log`。最终统一构建验收另行记录，避免把中间构建混作最终制品。

## 最终集成验收

三个代理完成分组实现与交叉复审，整合后的普通 BIOS ISO、UEFI ESP、内核和应用盘均已构建。为保持已构建输入稳定，最终验收对 ISO、ESP 和应用盘使用 Make 的 `-o`，避免其他并行应用修改在测试中途重新生成介质。

- `test-driver-os` 与 `test-mk-wired` 同次运行退出 0：[统一日志](../build-driver-integration/validation/final-driver-os.log)。覆盖 PCI 配置、共享 INTx、USB HID、存储、DMA、网卡匹配的 host 正例/负控，以及两种新增网卡、BIOS/UEFI NVMe、AHCI 512e、USB tablet、两种 VirtIO-SCSI 的 guest 验收。共享 INTx 的 guest 正例和两个负控使用单独的测试内核，普通 ISO 无该测试 hook。
- 既有 DMA 驱动门禁单独运行退出 0：[DMA 日志](../build-driver-integration/validation/final-dma-drivers.log)。包含本轮 RTL8139 overflow 修复的生产 ISR 检查及旧错误负控；该门禁已补入 `test-driver-host` 聚合入口。
- 最终普通 ISO 的 **e1000e、PCnet、e1000、VirtIO-net、RTL8139 五卡**均有动态 vector 96、实际 IRQ 调度、DHCP 与两次 128 KiB 完整下载。前两卡在统一日志，后三卡在[旧网卡集成日志](../build-driver-integration/validation/legacy-nics.log)。RTL8139 最终发生 4 次 overflow/recovery event，仍完整收到两份正文；不是“无溢出”结论。
- 共享 INTx host 33 项通过，4 个负控真实触发预期断言；guest 17 项通过，2 个硬件负控分别验证丢失第二成员和未屏蔽旧路由。Make 接线检查：268 个片段，267 个可达，1 个明确声明的独立 wrapper 例外。
- **显式跳过项**：AHCI 原生 4Kn 的 QEMU guest。模拟器拒绝该设备配置；本轮只有生产 IDENTIFY 函数对 4Kn 的拒绝测试，不能将其写成 4Kn 支持。

| 普通构建产物 | 用途 | 大小 |
|---|---|---:|
| [logit.iso](../build-driver-integration/logit.iso) | BIOS 内核启动介质 | 15,034,368 B |
| [esp.img](../build-driver-integration/esp.img) | 自建 UEFI loader 与内核 | 16,777,216 B |
| [disk.img](../build-driver-integration/disk.img) | LogitFS 应用数据盘 | 536,870,912 B |
| [kernel.elf](../build-driver-integration/kernel.elf) | 普通内核 | 6,825,808 B |

完整 SHA-256、门禁与证据路径见[制品清单](../build-driver-integration/validation/manifest.json)。[硬件清单示例](../build-driver-integration/validation/hardware-inventory-example.json)由最终 e1000e guest 串口日志生成。本轮没有向物理磁盘写入镜像，也没有实体电脑兼容性验收。

## 可重复入口

```sh
make BUILD=build-driver-integration test-driver-host
make -j6 BUILD=build-driver-integration build-driver-integration/logit.iso build-driver-integration/disk.img build-driver-integration/esp.img
make BUILD=build-driver-integration test-driver-os
make BUILD=build-driver-integration test-mk-wired
```

新测试片段通过 `tests/dma.mk` → `tests/driver_expansion.mk` 进入根 Makefile；独立 `BUILD` 必须使用命令行赋值。`.iso` 是内核启动介质，`.img` 数据盘包含应用，不能只拿 ISO 的构建结果证明桌面应用已重建。

拿到那台电脑的串口启动日志后，可执行：

```sh
python3 tools/driver_inventory.py /absolute/path/to/serial.log
python3 tools/driver_inventory.py --json /absolute/path/to/serial.log
```

该工具只读取日志，列出 PCI 地址、ID、类别、绑定驱动与后端启动标记；不访问或修改宿主机设备。注册表 `driver=-` 不自动等于完全缺驱动，部分旧 ATA/framebuffer 路径要结合后端日志判断。

## 仍影响老电脑覆盖的缺口

1. UHCI/OHCI/EHCI 主机控制器、USB hub、USB mass storage，以及完整设备热插拔。xHCI 上的 HID 扩展不能弥补这些缺口。
2. 更常见的主板集成网卡需要按芯片另行支持，例如 Intel PCH/I217/I219；RTL8169/8168 原有实现仍需实际芯片和 PHY 验证。Wi-Fi、蓝牙未补齐。
3. 多 PCI segment/独立 ACPI root bus、完整 ACPI `_PRT` 路由、多 I/O APIC、IOMMU 与 suspend/resume 仍未完整支持。
4. NVMe/AHCI 的部分等待仍使用有界循环次数，物理硬件的最坏等待时间需要按规范时间基准进一步验证。4Kn 通用块层映射未实现。
5. 图形仍主要依靠固件 framebuffer 或 VirtIO GPU；本轮不包含 Intel/AMD/NVIDIA 原生 modesetting 或 3D 驱动。HDA 等既有音频驱动不因本轮改动自动获得真机验证。
6. 中断生命周期测试覆盖已经进入 ISR 的回调与 EOI，以及释放后重新触发被屏蔽的旧线路；未覆盖尚未进入 ISR 的 LAPIC pending-IRR 在向量复用时的所有状态。仍需专项验证及物理机验收。

## 资料依据

- [Intel 82574 原始数据表](https://www.mouser.com/pdfdocs/82574datasheet.pdf)
- [AMD Am79C970A 原始数据表](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/19436.pdf)
- [Linux PCI host bridge / MCFG 说明](https://www.kernel.org/doc/html/v5.18/PCI/acpi-info.html)
- [Linux x86 原生 PCI 配置访问](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/pci/direct.c)
- [QEMU x86 CPUID 的 TCG 标识实现](https://raw.githubusercontent.com/qemu/qemu/master/target/i386/cpu.c)
- [OASIS VirtIO 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [USB 输入改动与验收说明](usb-input-extensions-2026-09-10.md)
