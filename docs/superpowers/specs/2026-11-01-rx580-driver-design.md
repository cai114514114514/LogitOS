# RX 580（Polaris）完整驱动：设计

日期：2026-11-01　基线：`hw/x79-vg4` `44c004e0d`　目标机：X79-VG4（Xeon E5-2670 v2，UEFI/GOP），AMD Radeon RX 580，显示器 MSI MAG 274Q X24（EDID 首选 2560×1440）

**透明度：** 下文的寄存器和流程都引自 Linux v6.12 的 amdgpu/radeon 源码和 AMD 寄存器头文件（每处注明文件与函数）；没有一项在这张卡上运行过。真机事实（PCI ID、VBIOS 版本、BAR5 大小、GOP 用的是哪个 CRTC、显示器接口）都要等 A0 的只读转储。

## 1. 目标与用户决定

- 「完全驱动」= 三个阶段，按 A → B → C 实施，每阶段在真机上验证后才进入下一阶段（用户 2026-11-01）。
- 第三方组件（用户 2026-11-01）：只新增一个——**AMD Polaris 微码包**（linux-firmware `amdgpu/polaris10_*.bin` 等），固定到官方提交并校验 SHA256，放在根盘；只用于 B、C。
- **AtomBIOS 命令表解释器自己写**（用户 2026-11-01 更正：「不要 2 了，必须得要，但是 2 可以自己写」）。Linux 的 `atom.c` 只作为格式与语义的参考，不引入代码。
- 其余代码全部自己写，不从 amdgpu 搬代码。实现流程：Sonnet 写，Claude 审查、跑门、提交（用户 2026-11-01）。

## 2. 现状（调研，`c/drivers/gpu/amd`，约 5.5k 行）

| 部分 | 状态 |
|---|---|
| `bootfb.c` 被动认领 AMD 显示设备 | 真机可用，只读 |
| `polaris/device.c` 只读探测（仅 `1002:67df`） | 真机可用 |
| `polaris/resource/*` VBIOS 与固件盘点 | 会运行，但把整个 BAR0 映射成 UC，覆盖扫描输出的 WC（P12 正在修） |
| SMU 微码加载、SDMA、GART、呈现（`native/runtime/desktop/present`） | 只在主机模型里测过，内核里没有调用者 |
| DCE 11.2 显示引擎、IH 中断环、ATOM 解释器、GFX8 | 没有 |
| 设备 ID | 只认 `67df`；RX 580 2048SP 是 `6fdf`（Linux 视为 Polaris10，`amdgpu_drv.c` pciidlist） |

## 3. 阶段 A：显示引擎（DCE 11.2）

不需要任何 AMD 微码：Polaris 上 DC 和 IH 都不加载固件（`amdgpu_dm.c load_dmcu_fw` 对 POLARIS10/11/12 返回 0；`tonga_ih.c` 无 request_firmware）。分四段，每段可以单独合并、单独上真机。

### A0 只读转储与设备身份（先做，几十行）

- 接受 `1002:6fdf`（以及 Linux 列为 Polaris10 的其余 ID），把散落在约 10 处的 `67df` 判断收拢成一个 `polaris_device_known()`。
- 找到 GOP 用的 CRTC：在 6 个 CRTC 偏移（`vid.h`：0、0x200、0x400、0x2600、0x2800、0x2a00 双字）上读 `CRTC_CONTROL.CRTC_MASTER_EN`、`GRPH_ENABLE`、`GRPH_CONTROL`（线性 32bpp）、`GRPH_PRIMARY_SURFACE_ADDRESS(_HIGH)`、`GRPH_PITCH`，并核对它与 GOP 帧缓冲（地址差、pitch）一致；不一致就拒绝接管。
- 解码时序：`CRTC_H/V_TOTAL`（存的是 total−1）、`H/V_BLANK_START_END`（有效宽 = BLANK_START − BLANK_END）、`H/V_SYNC_A`；刷新率用 `CRTC_STATUS_FRAME_COUNT`（24 位，场同步起点加一）对着 TSC 实测 0.5 秒（`dce110_timing_generator.c`；PLL 像素时钟在 TMDS 路径上读不回，`dce_clock_source.c`）。
- 显示寄存器自己检查 BAR5 上界（CRTC5 的光标寄存器到字节 0x111b8，而 `native.c` 只要求 ≥ 0x5494）。
- 结果打到串口和 HUD：`[amd-dce] crtc=N mode=WxH@Hz pitch=… surface=… bar5=…`。

### A1 硬件光标（约 300–400 行）

- 序列（`dce_v11_0.c` 2364–2534）：置 `CUR_UPDATE.CURSOR_UPDATE_LOCK` → 写 `CUR_SURFACE_ADDRESS_HIGH` 再写低位（MC 地址，4 KiB 对齐）→ `CUR_SIZE=(w−1)<<16|(h−1)` → `CUR_POSITION`/`CUR_HOT_SPOT` → `CUR_CONTROL`（`CURSOR_EN=1`，`CURSOR_MODE=2` 预乘 ARGB）→ 解锁。热点在软件里减掉，负坐标用 `CUR_HOT_SPOT` 裁剪。
- 光标表面：64×64（16 KiB），线性，pitch == 宽度（`dm_check_cursor_fb`）。位置：显存里一块已证明不属于 GOP 扫描输出、也不在 VBIOS 保留区的 4 KiB 对齐区域。A 阶段没有 B 的保留清单时，用「GOP 报告的 FrameBufferSize 尾部超出 pitch×height 的部分」，并在开机时核对；不够就不启用硬件光标，退回 P11 的软件叠加。
- 接在 `fb_cursor_image()`/`fb_cursor_move()` 后面（今天只转给 virtio-gpu，`fb.c` 659–668），启动行沿用 `[wm] pointer: display cursor plane (motion does not composite)`。

### A2 轮询 vblank 与双缓冲翻页（约 250–350 行）

- `GRPH_FLIP_CONTROL` 设为场消隐锁存（`IMMEDIATE_EN=0`），写 `GRPH_PITCH`、`GRPH_PRIMARY_SURFACE_ADDRESS_HIGH`、低位（写低位触发更新）；完成 = `GRPH_UPDATE.GRPH_SURFACE_UPDATE_PENDING` 归零（`dce_v11_0.c` 258–281；`evergreen.c evergreen_page_flip_pending`）。
- vblank：轮询 `CRTC_STATUS_FRAME_COUNT`，并用 `CRTC_STATUS_POSITION` 不动来判定计数器停止，避免死等（`dce110_tg_is_in_vertical_blank`）。
- 给合成器一个帧时钟 `amd_dce_frame_wait()`，这是调度器设计里「按显示器刷新率计帧」的硬件来源。
- 第二块扫描缓冲（1280×1024×4 = 5 MiB，切换到 2K 后 14.7 MiB）需要显存所有权；A2 只在 A0 证明 GOP 尾部够大时启用，否则等 B 的保留清单。

### A3 原生分辨率：VBIOS 获取 + 自写 ATOM 解释器 + 开机早期模式切换（约 3–4k 行，全部自写）

- **VBIOS 来源**（`amdgpu_bios.c` 的顺序）：
  1. 引导加载器在 `ExitBootServices` 之前，经 `EFI_PCI_IO_PROTOCOL.RomImage/RomSize` 把显卡 ROM 拷进一块内存，并通过启动信息交给内核（`x86-stub.c` 43–105 的做法）；
  2. 内核里的 ACPI `VFCT` 表；
  3. 都没有就不做 A3。
  BAR0 永远不作为 VBIOS 来源（P12 的教训）。
- **解释器（自写）**：`c/drivers/gpu/amd/atom/`。按 Linux `atom.c` 的操作码与寻址语义（寄存器/参数空间/工作空间/帧缓冲暂存/PLL/MC、`IIO` 子程序、`CALL_TABLE`、条件跳转、`DELAY`、`SWITCH`）自己实现；每个操作码都有主机测试，未知操作码与越界访问拒绝执行；每张命令表有执行步数和墙钟超时（参考原版 20 秒 `ATOM_CMD_TIMEOUT_SEC`）。只实现 A3/B 实际要执行的命令表所用到的操作码，用真实 VBIOS 映像在主机上统计后确定范围。
- **模式切换顺序**（`dce_v11_0_crtc_mode_set` 2702–2741，`atombios_crtc.c`）：只针对 A0 找到的那个 CRTC 和它的编码器——`BlankCRTC` → `SetPixelClock`（Polaris 为 v7）→ `SetCRTC_UsingDTDTiming`（用 EDID 首选时序）→ 编码器/发送器表 → 如果是 DP，做链路训练（只有 DP 需要；HDMI 不需要）→ 重设 `GRPH_*` 指向新的扫描缓冲 → `UpdateCRTC_DoubleBufferRegisters` → 解除消隐。行缓冲和水位按 `dce_v11_0` 的公式用 VBIOS 启动频率计算。
- **时机**：在内核早期、`fb_init()` 之前完成，所以合成器从一开始就按 2560×1440 建立，不需要运行中改分辨率。失败（任一命令表报错或超时、链路训练失败、读回不对）就回到 GOP 原来的地址和模式，桌面照常以 1280×1024 起来，并在 HUD 上标出原因。
- **HDMI 与 DP 都支持**（用户 2026-11-01：「能都做吗」）。A0 从 VBIOS 对象表（connector/encoder/transmitter 对象）和热插拔状态判断显示器接在哪个口：
  - HDMI：发送器表以 TMDS 模式输出，不需要链路训练；RX 580 为 HDMI 2.0，像素时钟上限 600 MHz。
  - DP：经 `ProcessAuxChannelTransaction` 命令表走 AUX 通道读写 DPCD；链路训练（时钟恢复 + 通道均衡，`DPEncoderService`/发送器表，参考 Linux `atombios_dp.c` 与 DP 1.4 规范第 3.5 节）；按 DPCD 能力选链路速率与通道数。约多 600–1000 行。
- **刷新率**：首选 EDID 首选时序；接口带宽不够时，选 EDID 里该接口可达的最高刷新率。

### A 阶段的门

- 主机模型：一张模拟的 DCE 寄存器文件（记录每次读写）。断言光标序列（锁 → 高位 → 低位 → 尺寸 → 使能 → 解锁）、翻页只写 `GRPH_*`、从不清 `CRTC_MASTER_EN`、从不碰 PLL（A1/A2）；时序解码对几组已知模式给出精确的宽高与刷新率；`polaris_device_known()` 接受 `67df`/`6fdf`。
- ATOM：用一份真实 Polaris VBIOS 映像（测试资源，不进内核）在主机上执行 `SetCRTC_UsingDTDTiming`、`SetPixelClock`，断言写出的寄存器序列与参数；超时和非法操作码被拒绝。负对照：去掉超时、去掉失败回滚。
- QEMU 没有 Polaris，A 阶段的真实证据只能来自真机：A0 的转储、光标出现、`[wm] pointer: display cursor plane`、实测刷新率、2560×1440 点亮。

## 4. 阶段 B：资源提供方，让现有 SMU + SDMA 上线（需要微码）

- 微码包进根盘：固定提交、SHA256、`/lib/firmware/amdgpu/`；内核已有 `sha256()`。
- 资源提供方：用 A3 拿到的 VBIOS 解析 `VRAM_UsageByFirmware`、`firmware_info`；从活硬件读出所有 CRTC 的扫描输出、光标表面；授予驱动独占的显存 arena；读安全密钥；然后调用已有的 `polaris_desktop_start()`。
- 现有 SDMA 呈现要补 pitch 感知的 `LINEAR_SUB_WINDOW` 拷贝；硬件光标（A1）必须先于 B，因为注册 `native_present` 后软件叠加会自动关闭（P11 的 `fb_overlay_available()`）。
- 是否接管内存控制器（GMC）在 B 的设计里单独决定。

## 5. 阶段 C：GFX8（复用 B 装好的 CP/RLC 微码）

- C1：RLC 重启、CP_RB0、解除 ME/PFP/CE 停机、clear-state、环和 IB 测试、EOP fence（`gfx_v8_0.c` 4047–4284），带一条 GRBM/SRBM 软复位路径。
- C2：CP DMA 填充/拷贝。
- C3：计算 dispatch 与 3–5 个手写 GCN3 内核（填充、拷贝、混合、模糊），模板参考 Linux 里 Carrizo 的手写 dispatch（`gfx_v8_0.c` 1331–1648）。
- 通用 3D 与着色器编译器不在本设计内（属于以后的 GFX 3D）。
- 真正的性能还需要 DPM 调频（SMU7），列为 C 之后的阶段 D，届时必须同时设置显示间隙和最小 vblank。

## 6. 规模（估计，A 阶段实施前按转储结果复核）

| 阶段 | 驱动代码 | 测试 |
|---|---|---|
| A0–A2 | 约 1.2k | 约 1.5k |
| A3 | 约 3.5–4k（解释器约 1.5k + 模式切换约 1.5k + DP AUX/链路训练约 0.6–1k） | 约 1.8k |
| B | 约 2k | 约 1.2k |
| C1–C3 | 约 5–6k | 约 3k |

## 7. 风险

- 写错 `GRPH_*` 或光标地址：花屏或黑屏，可以恢复到 GOP 地址；所有写入前先快照，失败回滚。
- A3 模式切换失败可能黑屏到重启：只在 `fb_init()` 之前做，带超时和回滚，并提供启动参数关闭 A3。
- 显存所有权：A 阶段只用核对过的 GOP 尾部，不确定就不启用。
- 2048SP 与 8 GB/4 GB 版本的差异、VBIOS 命令表版本：以 A0 的转储为准。

## 8. 未决

1. 显示器接口：已决定 HDMI 与 DP 都做，开机时自动判断（用户 2026-11-01）。
2. B 是否接管 GMC。
3. C3 的着色器二进制：手工编码，还是构建期用 `llvm-mc -mcpu=gfx803` 汇编（后者会用掉剩下的第三方名额，默认手工编码）。
