# X79-VG4 实体机首启与全设备驱动：设计

日期：2026-10-31　分支：`hw/x79-vg4`（主仓库 `~/LogitOS` 内，不新建 `LogitOS-wt` 工作区）

## 目标

让 LogitOS 第一次在实体机上启动，并让这台机器上的每一个设备都由 LogitOS 自己的驱动接管。
成功的标准是实体机上的证据（串口日志、屏幕照片、读回校验），不是 QEMU 或宿主模型。
`TRANSPARENCY.md` 的规则不变：没在这台机器上跑过的路径一律写“未验证”。

## 目标机器（来自 Windows 清单，2026-10-31）

| 部件 | 型号 / PCI ID | 备注 |
|---|---|---|
| 主板 | INTEL X79-VG4 V2.2，AMI BIOS 4.6.5（2024-06-21），UEFI 模式 | 国产板，南桥为 6 系列桌面 PCH |
| CPU | Xeon E5-2670 v2，10 核 20 线程 | 在 32 个 CPU 槽位以内 |
| 内存 | 2 × 8 GiB DDR3-1600 无缓冲 | 两条都报告为 `Node0_Bank0` |
| 硬盘 | Netac SSD 120GB，SATA，512/512 扇区 | GPT：ESP 300 MB、MSR 16 MB、C:（Windows） |
| SATA | `8086:1c02`，AHCI 模式，00:1f.2 | 现有 AHCI 驱动按类别匹配 |
| USB | EHCI `8086:1c26`、`8086:1c2d`，各接一个 `8087:0024` 速率匹配集线器 | 没有 xHCI，只有 USB 2.0 |
| 键盘 / 鼠标 | `24ae:4056`、`18f8:1286`，都是 USB 复合设备 | 挂在速率匹配集线器之后 |
| 声卡 | HDA `8086:1c20`，00:1b.0 | 驱动现在写死 C600 的 `1d20` |
| SMBus | `8086:1c22` | 驱动只认 C600 的 `1d22` |
| 网卡 | Realtek `10ec:8168` rev 0c，子系统 `10ec:0123` | 大概率 RTL8168G；现有驱动 351 行，从未上过硬件 |
| 显卡 | GTX 1050 `10de:1c81` | 沿用 GOP 帧缓冲；HDMI 声音 `10de:0fb9` 没有驱动 |
| 其他 | LPC `8086:1c4e`、PCI 桥 `8086:244e`、CPU 根端口与 uncore `8086:0eXX` | uncore 不需要驱动 |
| 串口 | COM1 | 用户正在买 USB 转 RS232 线 |

## 安装方式（用户选择：从 C 盘分出一个分区）

- **启动介质是 U 盘，Windows 的 ESP 不动。** `tools/mkesp.py` 新增 GPT 模式：输出带保护 MBR 和一个 ESP 分区的整盘镜像，里面放 `\EFI\BOOT\BOOTX64.EFI` 和 `\LOGIT.ELF`。旧的“超级软盘”布局保留为选项，因为部分 AMI 固件不认这种无分区表的 U 盘。开机按启动菜单键选 U 盘，不按就照常进 Windows。
- **根文件系统在 Netac 上新建的分区里。** 用户在“磁盘管理”里把 C: 压缩出 24 GiB，留成未分配空间。新增 `tools/windows/install-root.ps1`，必须以管理员身份运行，按顺序做：
  1. 找到唯一一块未分配空间，在那里建一个不格式化、不分配盘符的 GPT 分区，类型用 LogitOS 专用的类型 GUID；
  2. 校验：这个分区必须是新建的、没有文件系统、大小足够、所在磁盘编号正确，然后把要写的磁盘、分区编号和大小打印出来，等用户输入 `YES`；
  3. 写入 `release.img`，读回并比较 SHA-256；
  4. 任何一步对不上就停下，不写任何东西。
  这个脚本永远不会碰 ESP、MSR 和 C: 分区。根盘选择逻辑已经会遍历 GPT 分区，找第一个带 LogitFS 超级块的分区（`c/drivers/block/blkdev.c`）。
- 以后更新内核只需要重写 U 盘。只有根文件系统格式变了，才重新运行 `install-root.ps1`。

## 工作拆分：省 Claude 额度

按用户的规则：较弱的模型写代码，强模型审计。Claude 和 GPT 一起干，负责调度，并在每条线路合并前做最终检查、完成合并。

- **写代码**：Codex `gpt-6.1-sol`（high），`workspace-write` 沙箱。每条线路一个 agent，在仓库内的临时 worktree `~/LogitOS/.hw/wt/<线路>` 里工作，最多 3 条并行。
- **审计**：Codex `gpt-6-astra`（xhigh），只读。拿到这条线路的 diff 和测试输出，按“会变红的负控是否存在、是否违反 TRANSPARENCY、有没有改到别人的文件”给出问题清单。写代码的 agent 按清单最多修两轮。
- **调度**：Mac 上的一个后台脚本 `hw-run`，结构和文档流水线一样，状态写在 `~/LogitOS/.hw/`（加进 `.gitignore`，结束后删掉）。Claude 只看每条线路的一页摘要，然后负责提交。
- **并行但不打架**：
  - 每条线路有自己独占的文件清单，提示词里写死；
  - 每条线路用自己的 `BUILD=build-hw-<线路>` 目录；
  - agent 不执行任何 git 写操作，由调度脚本按线路的文件清单 `git add` 并提交；
  - 两条线路都要改的共享文件（`Makefile`、`tests/*.mk`）不让 agent 直接改：每条线路新增自己的 `tests/hw-<线路>.mk`，最后由 Claude 在 Makefile 里统一 include。

## 线路（第一阶段：不需要真机，现在就能做）

| 线路 | 内容 | 独占文件 | 验收（宿主模型 / QEMU） |
|---|---|---|---|
| T1 启动介质 | `mkesp.py` GPT 模式；`install-root.ps1`；U 盘写入说明 | `tools/mkesp.py`、`tools/windows/`、`tests/hw-boot.mk` | OVMF 从 GPT U 盘镜像启动。脚本的校验逻辑写成不碰磁盘的纯函数，在 Mac 上用 PowerShell 7（`pwsh`）对模拟的磁盘清单跑负控：有文件系统、有盘符、空间不够、目标是 ESP 或 C: 时都必须拒绝。如果 Mac 上没有 `pwsh`，先问用户能不能装。真正写盘只在用户的机器上进行 |
| T2 平台 ID | SMBus 按 Intel PCH SMBus 家族接受 `1c22`（寄存器布局与 `1d22` 相同，在说明里写明依据），文件改成更通用的名字；HDA 接受 `1c20`；SPD 处理两条都标 `Node0_Bank0` 的情况 | `c/drivers/platform/`、`tests/hw-platform.mk` | 宿主模型用 `1c22` 读两条 DDR3-1600 无缓冲 SPD；`1d22` 回归通过；未知 ID 仍被拒绝 |
| T3 RTL8168G | 照 Linux r8169 的 8168G 启动流程补全：识别芯片版本、MAC OCP / ePHY 初始化、PHY 初始化（不依赖固件补丁）、链路检测、MSI 或 INTx、64 位 DMA；未知芯片版本一律拒绝 | `c/drivers/net/rtl8169.c`（可以拆文件）、`tests/hw-rtl.mk` | 寄存器模型覆盖初始化、收发、链路断开 / 恢复、关机；未知版本的负控变红 |
| T4 USB 输入 | 速率匹配集线器 `8087:0024` 之后的低速 / 全速键盘和鼠标：拆分事务、复合设备里的多个 HID 接口、启动协议回退 | `c/drivers/usb/`（不含 `xhci*`）、`tests/hw-usb.mk` | QEMU EHCI + usb-hub + 键盘鼠标；宿主模型模拟复合设备两个接口 |
| T5 显示 | EFI 加载器在 GOP 模式里选显示器的原生分辨率（读 EDID，没有就选最大的 32 位模式），把选择的原因打到串口；GTX 1050 保持零 MMIO | `c/boot/efi/loader.c`、`c/kernel/gui/fb/`、`tests/hw-display.mk` | OVMF 多个分辨率；没有 EDID 时回退的负控 |
| T6 诊断 | 新增 `hwreport`：每次启动把 PCI 列表、驱动绑定、CPU 拓扑、内存、根盘、各里程碑写到串口和 `/var/log/boot-<n>.log`；用 `hwreport` 应用在屏幕上查看同一份内容 | `c/kernel/diag/hwreport*`、`tests/hw-diag.mk` | QEMU 输出包含全部里程碑标记；根盘只读时也不会卡住 |

暂不做：GTX 1050 的 HDMI 声音（`10de:0fb9`）、NVIDIA 硬件加速、Windows ESP 里的启动项、USB 根文件系统。它们放到第三阶段。

## 第二阶段：真机点亮梯子（串口线到了以后）

每一级都记录实体证据，写进 `docs/hw/X79-VG4.md`（串口日志原文、照片、镜像哈希）。哪一级失败，就针对那一级开线路修，修完从那一级重测：

| 级 | 里程碑 | 证据 |
|---|---|---|
| L0 | U 盘的 `BOOTX64.EFI` 在这块板上运行 | 串口 `[efi]` 行 / 屏幕文字 |
| L1 | 退出启动服务，内核帧缓冲点亮 | `[efi] ebs ok`、屏幕 |
| L2 | 20 个逻辑 CPU 全部上线，时钟中断和 IPI 正常 | `hwreport` 拓扑 |
| L3 | AHCI 找到 Netac，挂载新分区里的 LogitFS | `[blk]`、`[fs] mounted` |
| L4 | 键盘、鼠标可用 | 屏幕操作 |
| L5 | 桌面出现，`LOGIT_BOOT_OK` | 屏幕 + 串口 |
| L6 | 网卡拿到 DHCP 地址并能 ping 通、访问 HTTP | 串口 |
| L7 | 板载声卡发声 | 用户确认 |
| L8 | SMBus 读出两条内存的 SPD | `hwreport` |
| L9 | 重启和关机正常，回到 Windows 一切照旧 | 用户确认 |

## 风险

- 写分区的那一步可能伤到 Windows：用前面说的多重校验、读回比较和人工确认 `YES` 控制。在动真盘前，建议用户在 Windows 里先做一次系统映像备份。
- AMI 固件可能不认 U 盘：GPT 和超级软盘两种 U 盘镜像都提供。
- 板载声卡的编解码芯片型号未知（这类板常见 ALC662 / ALC887）：T2 只负责让驱动认出控制器，编解码芯片在 L7 用真机日志确认后再适配。
- 网卡芯片版本只能在真机上读到：T3 会覆盖 8168G 前后的几个相近版本，碰到列表外的版本直接拒绝，并把版本号打到日志里。
- 第一阶段的测试结果全是模型证据，这一点在提交说明和文档里如实写明。
