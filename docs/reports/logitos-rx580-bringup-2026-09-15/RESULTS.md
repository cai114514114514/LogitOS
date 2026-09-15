# RX 580 / Polaris 驱动基础 — 2026-09-15

本轮明确转向 RX 580 所在的 Polaris10-class 路径（PCI 1002:67df）。**这是实卡驱动基础的部分交付，尚未实现 RX 580 硬件加速。** 不将之前 RV100 的成功计入 RX 580。

三个代理分别负责固件解析、SDMA 包编码、寄存器资料审核，并继续承担探测与真实驱动接线测试。主线完成只读探测、内核接线、官方文件验收和镜像回归。

## 本轮已实现

1. **Polaris 独立只读探测**：从 BAR5 读取 8 个实际寄存器，记录显存容量、MC 范围、SMU/队列启动后续需要参考的 VM/SDMA 原始状态。它使用 64 位显存容量字段，分别保存 CPU BAR0 aperture，避免 4/8 GiB 截断或误认成 256 MiB。完整校验身份、D0、电源配置、内存解码、BAR 类型/区间及启动 framebuffer 归属；接受 GOP 扫描线 padding。拒绝时保留 CPU 画面。
2. **真实驱动接线**：amd_bootfb → amd_accel_prepare → polaris_probe_readonly；输出 `[amd-polaris] ... commands=0 acceleration=unavailable`。读取寄存器成功依旧不注册呈现器、不返回加速成功。idle/enable 位仅是状态快照。
3. **SDMA 3.1 包构建**：构建 copy / fill / fence 的内存包，校验已映射 GPU 范围描述、40 位上界、DWORD 对齐、长度、重叠、输出容量及溢出；失败不改变既有流。描述符本身不建立映射，尚无硬件提交。
4. **固件结构解析与检查工具**：支持 SDMA v1.0/v1.1 文件头、有界小端读取、payload 和 jump table 范围。新工具 `tools/check-polaris-firmware.c` 直接复用内核解析器检查文件；不安装、不加载微码。

BAR0/2/5 布局、40 位地址和 SDMA 包规则依据固定 [Linux v6.12 GMC](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c)、[设备初始化](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c)、[SDMA 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c)。67df 同时覆盖若干 Polaris 产品，不能单凭这个 ID 推断具体品牌、2048SP 与否或容量。

## 验收证据

| 项目 | 结果与边界 |
|---|---|
| 固件解析 ASan/UBSan | 132 检查，0 失败 |
| SDMA 包编码 ASan/UBSan | 208 检查，0 失败；独立字面包 oracle |
| 只读探测 ASan/UBSan | 89 检查，0 失败；假寄存器、4/8 GiB 和 padded pitch 样本 |
| 真实驱动 wrapper 集成 ASan/UBSan | 259 检查，0 失败；真实源文件、假 PCI/MMIO 输入 |
| 合计新增正向检查 | 688 检查通过 |
| 强制前置负控 | v2 冒充 v1：1 失败；v1.1 缺扩展：1；count-minus-one：3；显存 32 位截断：3；误注册加速呈现器：3 |
| 既有主机回归 | AMD bootfb 59、RV100 604、fb 集成 21、Intel bootfb 31 检查均通过，各自负控生效 |
| 构建 | 独立 BUILD 当前源码内核/ISO 成功；ELF 中可见全部 Polaris 模块符号 |
| Make 接线 | test-mk-wired 通过 |
| 最终镜像 QEMU 回归 | RV100 桌面 GPU 呈现通过；Rage128 BIOS bootfb 通过；均不代表 RX 580 |

官方 linux-firmware 固定提交 `1522c78ab870b3c051d8a3a1d24ecbbc12b23be5` 的两份 `polaris10_sdma.bin` / `polaris10_sdma1.bin` 实际通过解析；URL、字节数和 SHA256 保存在 [firmware-provenance.json](firmware-provenance.json)。两者各 12692 字节，头版本 1.1，IP 3.1，ucode version 58、3109 DWORD、jump table 3072/32；feature 分别 31/0，digest_size 原值分别 5/0。

**实文件发现并修正的问题**：最初只支持 v1.0，合成测试通过但官方文件被拒绝；保留 [原失败日志](initial-real-firmware-rejection.log)。补齐 v1.1 的 52 字节扩展后两份文件通过。截断真实文件到 48 字节则明确拒绝，见 [负向文件检查](real-firmware-negative.log)。这证明结构兼容及截断拒绝，未验证 CRC、摘要、签名、GPU 微码加载或执行。`digest_size` 保留元数据；不根据猜测剥离末尾内容。[官方头结构](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h)

## 复现

从仓库根目录执行，使用独立 BUILD：

```sh
make -j4 BUILD=build-rx580-bringup all
make BUILD=build-rx580-host test-polaris-probe-host test-polaris-integration-host test-polaris-sdma-host test-polaris-firmware-host test-mk-wired
make BUILD=build-rx580-host POLARIS_FIRMWARE_DIR=build-rx580-firmware-input test-polaris-firmware-files
python3 tests/boot/run-amd-rv100-present.py --iso build-rx580-bringup/logit.iso --out build-rx580-bringup/final-rv100-regression
python3 tests/boot/run-amd-bootfb.py --iso build-rx580-bringup/logit.iso --firmware bios --out build-rx580-bringup/final-bootfb-regression
```

真实固件检查需要调用者提供目录中的两份文件；构建不自动联网、不将固件打包到内核。当前本机检查输入位于 build-rx580-firmware-input。报告不复制固件，保留来源和哈希。

## 尚缺的实卡路径

- 显存分配、MC/GART 地址映射和缓存可见性，区分 CPU 地址、PCI aperture 和 GPU 地址。
- Polaris 的 SMU 固件加载与电源/时钟交接；Linux 选择 SMU 加载而非简单逐字写 SDMA 寄存器。[加载选择](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c)、[SMU 加载流程](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/smumgr/smu7_smumgr.c)
- Ring 启动、提交、fence 完成、超时复位，以及屏幕外测试的真实像素读回。
- 上述环节完成后才能接入桌面呈现；着色器/3D 合成还需另行实现。

本轮没有连接 RX 580，没有物理寄存器读值、硬件提交或性能数据。QEMU 回归使用其真实 RV100/Rage128 模型，没有通过伪造 PCI ID 充当 Polaris。新 ISO 只包含内核，测试未挂载应用磁盘；不能据此声称浏览器或完整应用体验已验收。

产物：[测试 ISO](../../build-rx580-bringup/logit.iso)、[内核符号](kernel-symbols.txt)、[源码/产物哈希](source-and-artifacts.json)、[主机验收日志](polaris-host.log)、[RV100 回归证据](rv100-regression/result.json)。
