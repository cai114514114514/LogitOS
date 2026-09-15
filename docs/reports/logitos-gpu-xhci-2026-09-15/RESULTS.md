# LogitOS xHCI 与 GTX 1050 bring-up 结果（2026-09-15）

本轮把 xHCI 从“控制器命令 HCE 超时”推进到可运行的 QEMU 来宾热插拔闭环；GTX 1050
则从纯 boot framebuffer 观察器推进到可审计的 GP107 原生加速 bring-up 边界。两项的
完成度不同：**xHCI 已通过来宾功能测试；GTX 1050 尚未执行 GPU 命令，不能称为已完成
GPU 加速。**

最终组合产物位于 `build-gpu-xhci-final/`。运行本目录的 `python3 verify.py` 可以重新核对
产物哈希、双 USB 控制器结果、显卡门禁以及内核链接。

## xHCI：已完成的修复

- 修复事件环发布顺序：控制器可在 ERSTBA 高 32 位写入时立即取 ERST，所以现在先准备
  DCBAA、命令环和事件环，执行 DMA 写屏障并精确回读 PCI Bus Master Enable，再按
  `ERSTBA -> ERDP -> Run` 发布。
- ERST 发布后用同设备 `USBSTS` 读取冲刷 posted write，并拒绝同步 `HSE/HCE`。
- `CMD_RS` 后的每一次状态采样都先检查 `HSE/HCE`，包括 `HCH` 刚清除的最终样本。
- 失败路径会停机、清 BME；只有 DMA 确认不可达后才释放，否则隔离设备并保留 DMA，避免
  控制器继续访问已释放内存。

同一份最终 ISO 在 QEMU TCG 下分别连接 xHCI 和 EHCI 根端口，均完成：启动后挂入 USB
磁盘、读取 SCSI capacity 与 MBR、发布 `usb0p1`、拔出、设备删除和 block class 下线。

## GTX 1050：已完成的安全 bring-up

- 精确匹配 12 个 GTX 1050/1050 Ti PCI ID，继续拒绝相邻 MX150、Quadro 等 Pascal ID。
- 固定 Nouveau 源提交 `587858367581b9c55c3690f4e63382ad622719d4` 和 linux-firmware
  提交 `1522c78ab870b3c051d8a3a1d24ecbbc12b23be5`。
- 固化 22 个 GP107 前置固件的路径、长度和 SHA-256：ACR 4、SEC2 两套 ABI 共 6、GR 12。
- 严格执行 `PCI/BAR 检查 -> 22 个固件全部校验 -> BAR0 映射 -> 只读 BOOT0/BOOT1/
  PMC_ENABLE`；任一失败均保持 CPU boot framebuffer。
- 只接受 BOOT0 chipset `0x137`。当前阶段 MMIO 写、BME、DMA、IRQ 和 GPU 命令计数均为
  零，`fill/copy` 明确返回失败。
- 新对象已进入最终内核链接；BIOS 1024x768 和 UEFI 1280x800 的 test-only QEMU stdvga
  门禁确认被动显示路径仍可启动且桌面可见。

22 个固件暂未打进 fsroot，因为驱动尚未实现 Falcon 上传。当前默认镜像会在
`firmware-missing` 处安全回退；即使用户安装了精确固件，也只会只读确认 GP107，然后停在
`mmu-fifo-channel-ce` blocker。

## 验证结果

| 范围 | 结果 |
| --- | --- |
| xHCI lifecycle | 18/18；9 个顺序、状态、回滚变异负控各精确失败 |
| xHCI transfer core | 21/21；4 个负控精确失败 |
| USB root hotplug host | 12/12；无 CSC 与未等待 detach 负控精确失败 |
| xHCI guest | PASS；启动后 MBR 磁盘 online，再 offline |
| EHCI guest 回归 | PASS；启动后 MBR 磁盘 online，再 offline |
| GTX 1050 bring-up host | 38/38；缺固件仍映射 BAR、坏哈希仍进入 MMIO 两个变异负控精确失败 |
| GTX 1050 passive host | 56/56；宽泛 ID 和 D3 接管两个负控精确失败 |
| test-only display guest | BIOS 与 UEFI 均 PASS；结果明确 `physical_gtx1050_verified=false` |
| 内核集成 | `nvidia_pascal_accel.o` 已链接，四个公开入口符号存在 |

主要证据在 `evidence/`：`usb-hotplug-result.json`、`test-nvidia-pascal-host.log`、
`test-xhci-lifecycle.log`、`test-xhci-xfer.log`、两份 USB serial 和两份 synthetic display
结果。

## 最终产物

| 产物 | SHA-256 |
| --- | --- |
| `build-gpu-xhci-final/logit.iso` | `4c7f651fea2dd10b19ff4fe72bbd4ca2ae17dcbd0dbbc1ed934c590f7d91403e` |
| `build-gpu-xhci-final/disk.img` | `7a364ff778e8dcab65f4220988e5c8b86c209653133c26a8731d18acd9495a02` |

## 显卡尚缺的部分

真正提交 Pascal Copy Engine 命令前，还需要 GP100 MMU、instance memory、RAMFC/USERD、
runlist/PBDMA、GPFIFO `0xc06f`、CE `0xc0b5/0xc1b5`、fence、fault 与 timeout quarantine。
第一个硬门槛应是在独立显存 BO 上执行离屏 copy canary 并由 CPU 回读；在解析 scanout 映射
或安全接管 display flip 前，不能把 Multiboot LFB 的 PCI 地址当成 GPU VA 提交。

本轮证据来自 host fixture、QEMU TCG 和 test-only stdvga，尚未在实体 GTX 1050、X79 或
i7-14700KF 机器启动验证。
