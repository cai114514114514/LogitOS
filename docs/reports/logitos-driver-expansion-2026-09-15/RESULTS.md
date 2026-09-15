# LogitOS 通用 PC 驱动扩充结果（2026-09-15）

> 这是本轮早期的历史快照，其中的 xHCI HCE 失败已经被后续修复取代。当前结果、最终
> 产物与复验入口见 `../logitos-gpu-xhci-2026-09-15/RESULTS.md`。

本轮继续按“广撒网”推进，但只接入有明确硬件边界和失败门禁的支持。最终可复验产物位于
`build-drivers-storage-usb/`；运行本目录的 `python3 verify.py` 可重新核对产物哈希、来宾结果和
关键主机门禁。

## 本轮完成

- **ACPI 完整性底座**：校验 ACPI 2.0 扩展 RSDP、RSDT/XSDT 签名和所有 SDT 校验和，并拒绝
  越过 Multiboot2 tag 边界的表。它为 X79/C600 与较新的 Z790 固件枚举提供了可信入口。
- **HPET 时钟源**：新增 ACPI HPET 发现、GAS/MMIO/硬件身份校验和只读主计数器时钟源。
  PIT 继续负责调度 tick 和紧急回退；时钟源与折算点在同一个 seqlock 事务中发布，避免 SMP
  读者看到撕裂的时间元组。
- **USB 运行时插拔**：EHCI 和 xHCI 根端口接入 CSC 处理及 10 ms 丢中断观察器；连接、替换、
  断开和 USB Mass Storage 下线都有生命周期处理。detach 会等待在途 class callback，并把
  可能阻塞的 teardown 移出自旋锁。
- **Intel 82540 网卡族**：精确加入 `8086:1015`、`1016`、`1017`、`101e`；它们与现有
  `8086:100e` 同属 82540 MAC 类型。82574/e1000e、igb 和需要额外 TX FIFO 修复的 82547
  仍拒绝绑定。
- **Apple Silicon 测试机兼容**：DMA 和 xHCI host harness 移除了 Rosetta/x86 汇编依赖，
  可在当前 arm64 macOS 原生执行整套驱动主机门禁。

## 验证结果

| 范围 | 结果 |
| --- | --- |
| 全量 `test-driver-host` | PASS；最终日志覆盖 PCI、IRQ、DMA、存储、USB、NIC、X79、Xeon E5、14700KF、NVIDIA 被动帧缓冲、ACPI、HPET |
| ACPI 完整性 | 15/15；损坏校验和 2 个负控、错误根签名 1 个负控均精确失败 |
| HPET host | 18/18；GAS、block ID、revision 三类负控均精确失败 |
| HPET guest | 无设备时拒绝；有设备时启用、RTC/PIT 交叉检查通过、PIT 回退后恢复 HPET、4 核 400000 次读取无观测回退 |
| 时钟源并发 | 208/208；旧的 source-before-seqlock 顺序负控精确失败 |
| USB root hotplug host | 12/12；无 CSC 和未等待 detach 两类负控均精确失败 |
| EHCI hotplug guest | 启动后挂入真实 MBR USB 磁盘，发布 `usb0p1`；拔出后 block class 下线 |
| USB BOT / Storage | 117/117、124/124 |
| EHCI / USB hub / xHCI transfer core | 56/56、32/32、21/21 |
| NIC 注册与路径 | `net_drv` 136；82540 新增四行负控精确失败；e1000 PCH2 154/154 |
| X79 / Xeon E5 | 31/31、45/45；CPU 锁诊断保留 30/31 |
| i7-14700KF 平台 / SMP | 106/106、15/15 |
| x2APIC / PCI INTx / device model / MSI | 39/39、43/43、44/44、49/49 |
| GTX 1050 / Pascal | 被动 boot framebuffer 56/56；宽泛 MX150 匹配和 D3 探测负控均失败 |

完整日志：

- `evidence/test-driver-host-final.log`
- `evidence/supplemental-host.log`
- `evidence/test-time-host.log`
- `evidence/test-hpet-guest.log`
- `evidence/test-mk-wired.log`

## 产物身份

| 产物 | SHA-256 |
| --- | --- |
| `build-drivers-storage-usb/logit.iso` | `2f108cef9c783d0a074dd086600af7c83850a13843bb6073a05aa65934cb5849` |
| `build-drivers-storage-usb/disk.img` | `be19e1c03e92b2c6b432a0dac95e8714aba4abbbb2358aa3cd5fdba805f590d8` |

EHCI 成功记录在 `build-drivers-storage-usb/usb-hotplug/guest/ehci/result.json`；同一组产物的
xHCI 失败记录在 `build-driver-audit-final/usb-hotplug-xhci-current/xhci/failure.json`。

## 明确边界

xHCI 的运行时插拔代码已经接入并通过 host 并发/传输门禁，但当前 QEMU 来宾在 Enable Slot
命令处超时，`USBSTS=0x1000`（HCE），因此 **xHCI 热插拔仍不受支持**，也没有接入绿色 CI
汇总；EHCI 是本轮唯一获得来宾上线、读盘和下线闭环的 USB 控制器。

GTX 1050 当前只使用固件交付的被动 boot framebuffer。没有原生 Pascal modesetting、2D/3D
加速、视频解码或 HDMI 音频。X79/C600、未知型号 Xeon E5、Z790/i7-14700KF 和 GTX 1050
尚未在用户实体电脑上启动验证；这里的结论来自 host 门禁和 QEMU TCG，不能代替真机支持声明。
